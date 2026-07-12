# Windows gating audit — 2026-07-04

Static audit of every fork divergence that touches code Windows compiles,
performed at `a045dfc780` on `macos-optimizations`. For each divergence the
verdict is one of: **upstream-on-non-Apple** (a compile-time gate keeps the
exact upstream path), **equivalence-argued** (shared behavior change whose
correctness on native drivers follows from the Vulkan spec / platform-neutral
reasoning, written down here or in a code comment), or **needs-real-HW**
(behavior is believed correct but has never executed on a real Windows
Vulkan ICD — this machine cannot host one; VMs expose no ICD).

Honest scope: this audit delivers *compile-correct + CI-green +
statically-audited*. It is not a substitute for running on real Windows
hardware; the needs-real-HW list at the end is mirrored in README "Future
vectors".

## A. Compile-time platform gates (non-Apple keeps upstream semantics)

| # | Site | Gate | Non-Apple / Windows behavior |
|---|---|---|---|
| A1 | `hw/xbox/nv2a/pgraph/vk/surface.c` (`target_is_active`, ~1347) | `#ifdef __APPLE__` narrows the upload-surface full-GPU-sync | `#else target_is_active = true` — upstream full-finish semantics; the WHPX-visible ordering race documented in the comment cannot occur |
| A2 | `surface.c` ~1085 (scratch image) | Apple skips the scratch allocation at `surface_scale == 1` | always allocates (upstream) |
| A3 | `vk/instance.c` ~540 | Apple relaxes device features (`geometryShader`, `occlusionQueryPrecise`, … off) | `#else` requires the strict upstream feature set |
| A4 | `vk/instance.c` 55/167/233 | MoltenVK portability-subset/enumeration extensions | not requested; standard Vulkan init |
| A5 | `pgraph.c get_default_renderer` | Vulkan is default on Apple only | non-Apple order identical to upstream (OpenGL, then Vulkan) — verified against `upstream/master` |
| A6 | `pfifo.c:477`, `ui/xemu.c` (vblank thread) | `QOS_CLASS_USER_INTERACTIVE` bumps | compiled out; no scheduling change |
| A7 | `ui/xemu.c` MVK_CONFIG `setenv` block (~1595) | `#ifdef __APPLE__` | absent — MoltenVK config vars are meaningless off macOS |
| A8 | Metal present / MetalFX / IOSurface (`ui/xemu-metal.m`, `vk/metalfx_upscale.m`, `ui/xui/metal-helpers.mm`) | Objective-C sources added only under `host_os == 'darwin'` in meson | not compiled at all; GL presentation path as upstream |
| A9 | `hw/xbox/mcpx/apu/vp/vp.c` | vDSP (Accelerate) FIR path | `#else` portable C path (autovectorized) |
| A10 | `hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h` | `DSP56K_JIT_SUPPORTED = __APPLE__ && __aarch64__` | fork inline JIT absent; interpreter or upstream external JIT engine per `dsp_want_external_jit_engine()` — precedence key (`audio.dsp_jit.enabled`) is inert when unsupported |
| A11 | `tcg/aarch64/tcg-target-has.h:65` | `TCG_TARGET_HAS_fpu 0` under `_WIN32` | helper-based hard FPU (bit-equivalent), avoiding the llvm-mingw `qemu_build_not_reached` link failure |
| A12 | `ui/xemu-input.c:522` | `XEMU_INPUT_PIPE` FIFO harness | `#ifdef _WIN32` stub — facility absent by design |
| A13 | `ui/thirdparty/fpng/fpng.cpp` | ARM CRC32 intrinsics | gated on `__ARM_FEATURE_CRC32` |
| A14 | `util/cutils.c`, `util/miniz`, `system/exit-with-parent.c`, `include/qemu/osdep.h` `__APPLE__` hits | — | upstream-inherited QEMU portability; zero fork-changed `__APPLE__` lines (verified via `git diff upstream/master...HEAD`) |

## B. Shared behavior divergences (active on Windows by design)

These are the fork's portable renderer/audio changes; upstream xemu does not
have them on any platform. Windows gets them via the same C paths.

| # | Divergence | Verdict + reasoning |
|---|---|---|
| B1 | In-pass occlusion queries, bulk `vkCmdResetQueryPool` at CB begin, per-CB primer query, 4096-query pool, `REPORTS_FULL` capacity guard (`draw.c`, `reports.c`) | Equivalence-argued: all constructs are core-spec-valid (reset outside a pass, begin/end inside one pass, zero-sample query outside any pass). The primer exists for a MoltenVK-specific encoder rule and is harmless elsewhere (one empty query per CB). **needs-real-HW** for report-value soak on native drivers |
| B2 | Deferred zpass report delivery at slot reclaim + 5 ms idle budget + non-blocking `REPORTS_SUBMIT` (`reports.c`) | Platform-neutral logic; `XEMU_REPORTS_SYNC=1` restores legacy synchronous drains everywhere. **needs-real-HW** for guest-poll timing under WHPX |
| B3 | Byte-exact vertex-conflict refinement (`vertex.c`, `XEMU_VTX_EXACT=0` hatch) | Equivalence-argued: strictly-additive refinement over page-granular tracking; any unproven conflict still takes the conservative finish (architecture-contract Invariant 4). memcmp semantics are host-neutral |
| B4 | Redundant-bind skips: descriptor sets, vertex/index buffers, push constants, dynamic-state caches (`draw.c`) | Equivalence-argued from Vulkan pipeline-layout compatibility and CB-scoped state rules (reasoning in code comments); caches reset at CB begin |
| B5 | PFIFO untimed idle wait (`pfifo.c`) | Platform-neutral: the lost-wakeup proof is the enumerated kick-site lock discipline, not a platform property |
| B6 | Surface-expiry host-time throttle (`surface.c`, `QEMU_CLOCK_REALTIME`) | Platform-neutral; monotonic clock on all POSIX + Windows (`get_clock()` uses the monotonic source under both) |
| B7 | `vk_wait_for_fence_or_die` 5 s abort (`command.c`) | Deliberate cross-platform behavior change: converts silent driver hangs into diagnosable aborts. 5 s is far beyond any legitimate fence wait observed (max ~8 ms in captures) |
| B8 | Deferred zeta-shape eviction + quarantine reuse (`surface.c`, `XEMU_ZETA_SHAPE_READBACK=1` hatch) | Reuse: **equivalence-proven** — a migrated image keeps its attachment layout; first GPU touch is ordered against all earlier same-queue submissions by the draw render pass's explicit `VK_SUBPASS_EXTERNAL` dependency (attachment stages/accesses) or the attachment-stage src masks of the transfer transitions; proof comment at `get_any_compatible_invalid_surface`. Destruction: was **formally invalid on every platform** (see C below), fixed by `a045dfc780` |
| B9 | nsprof + Phase-1 attribution counters (`nsprof.c`, default-off) | Portable C, `getenv`-gated, zero-cost when unset (three flag stores per draw otherwise) |
| B10 | APU fixes (reset-hold runstate gate `d6da581d4b`, engine-aware mixbuffer bulk copy) | Platform-neutral; engine-gated paths select by `DSPOps`, not platform |

## C. Defect found and fixed by this audit

`prune_invalid_surfaces` destroyed quarantined `VkImage`s as soon as they
were not referenced by the *recording* CB — but `pgraph_vk_finish`
pipelines (submits slot N, waits only slot N−1), so the just-submitted CB
routinely still executes while the next records. Destruction could race a
pending submission on **every** platform; the window opened when
flight-slot pipelining replaced upstream's synchronous finish. Fixed in
`a045dfc780`: evictions are stamped with the highest submission index that
may reference them; a retirement watermark (updated at every slot-fence
observation) gates destruction; `surface_flush` drains all slots first.
Validated at fps parity on the F5 savestate scene (46.6–47.2 vs
46.83 ± 0.24, identical finish mix, zero errors).

## D. Needs-real-HW validation list (mirrored in README Future vectors)

None of this can be exercised on this machine (VMs expose no Vulkan ICD):

1. Occlusion rework end-to-end on native drivers (AMD/NVIDIA/Intel):
   report values under deferred delivery, `XEMU_REPORTS_SYNC=1` as the
   bisect hatch (B1/B2).
2. Primer-query cost and validation-layer cleanliness on native drivers (B1).
3. Zeta quarantine/reuse soak on native drivers — equivalence is proven on
   paper, a soak on real HW is the confirmation (B8).
4. Byte-exact vertex refinement under WHPX guest-write timing
   (`XEMU_VTX_EXACT=0` hatch) (B3).
5. Fence-or-die 5 s timeout margin on slow/loaded Windows systems (B7).
6. Performance: no macOS number transfers; Windows needs its own savestate
   A/B baseline before any perf claim.
7. Windows PGO (2026-07-05 parity audit): CI's Windows legs are
   win64-cross — x86_64 uses GCC (LLVM `.profdata` is incompatible in
   both format and flag wiring) and arm64 uses llvm-mingw (format-
   compatible but no committed Windows profile, and the win64-cross
   branch has no PGO plumbing). An honest Windows PGO story requires a
   training run of the bench corpus on real Windows hardware. The
   Linux build.sh branch has the clang PGO wiring (same two-stage flow
   as Darwin) for when a Linux training run exists.
8. DSP56K JIT runtime proof on non-Apple aarch64: the gate now admits
   POSIX aarch64 hosts (Apple keeps MAP_JIT + write-protect pairing;
   others take the plain-RWX mmap path with graceful interpreter
   fallback when refused). Compile is CI-covered by the
   ubuntu-22.04-arm leg; a `XEMU_DSP_JIT_DIFF=1` + `_STATS=1` run
   (`checked>0, failures=0`) in a linux/arm64 VM or on real hardware
   is the acceptance gate before claiming it works there
   (`audio.dsp_jit.enabled = true` selects the engine).

## E. Re-verification

```sh
grep -n "target_is_active" hw/xbox/nv2a/pgraph/vk/surface.c        # A1: both branches
grep -n "ifdef _WIN32" ui/xemu-input.c tcg/aarch64/tcg-target-has.h # A11/A12
grep -n "DSP56K_JIT_SUPPORTED" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h
grep -n "host_os == 'darwin'" hw/xbox/nv2a/pgraph/vk/meson.build ui/meson.build
grep -n "evict_submit_seq\|retired_submit_count" hw/xbox/nv2a/pgraph/vk/*.c hw/xbox/nv2a/pgraph/vk/renderer.h
git diff upstream/master...HEAD -- util/cutils.c util/miniz include/qemu/osdep.h | grep -c "__APPLE__"  # expect 0
```

---

# Windows gating audit — addendum 2026-07-11 (`ed2568277b`)

Extends the audit through two batches since `7e2e6e7256` (four
correctness fixes, the xbox unit suite + first CI test step, harness
hardening, `XEMU_INV_PROF`/`XEMU_TB_RANGE_INV` counters, push-model
present, PGO retrain). Same verdict vocabulary. Three CI matrices are
green today (macOS x86_64/arm64 + Windows x86_64/arm64 cross + Linux
x86_64/aarch64, debug+release) — compile/package proof; the below is the
semantic layer. **No divergence in this batch fails the bar; two of the
four correctness fixes are cross-platform *benefits*.**

## F. New shared behavior divergences (active on Windows/Linux by design)

| # | Divergence | Verdict + reasoning |
|---|---|---|
| F1 | **Texture/sampler LRU eviction gated on submission retirement** (`hw/xbox/nv2a/pgraph/vk/texture.c`; sampler `submit_time` at `renderer.h:299-302`) | **Equivalence-argued + cross-platform BENEFIT.** `texture.c` is in the core vk meson list (`meson.build:19`, above the `host_os=='darwin'` fence) so it builds on every platform; `grep -c '__APPLE__\|IOSurface\|Metal' texture.c` = 0. The old pre-evict guard protected an entry only while referenced by the *currently-recording* CB, but `pgraph_vk_finish` pipelines (waits only slot N−1), so the just-submitted CB still executes with stale stamps and `post_evict` destroys immediately — a **use-after-free / VUID violation on every driver** (MoltenVK's deferred encode only widens the window). New guard `submit_time >= retired_submit_count` (`texture.c:2435`, sampler `:2519`) mirrors the §B8/§C surface watermark. Two forward-progress valves (`ensure_cache_headroom()` slot-fence drain, `texture.c:1660`; finalize drain, `:2603`) exist because release strips `lru_evict_one`'s assert. Windows/Linux native-driver users get a real correctness fix. **needs-real-HW** for native-driver soak (D9). GL untouched. |
| F2 | **x87 FPCR-cache invalidation when the sticky-SSE bracket rewrites FPCR** (`target/i386/tcg/fpu_helper.c:3892`) | **Regression-risk none; cross-platform BENEFIT on aarch64 hosts.** Compiles on all aarch64 (`XEMU_SSE_HOSTFP && __aarch64__`, `:3758/:3865`) incl. Windows ARM64; x86_64 branch (`:3918`, MXCSR) untouched and stays dark (`mode=0` default off-aarch64, `:3833`). The store `cached_fpuc_rc = 0xFFFF` is harmless everywhere (forces a clean `gen_flcr` re-fire; unread where inline x87 is off). Correctly aarch64-scoped: aarch64 shares ONE FPCR between NEON and the `double`-based x87 `__hard` helpers (`:293-318`), so a leaked FZ/RC corrupts x87 doubles; x86_64 writes MXCSR — a *separate* register from the x87 control word — so no leak is possible there. Beneficiaries are all aarch64 hosts running the default-on sticky path (`hard_fpu` default true, `config_spec.yml:400`): **macOS arm64 AND Linux arm64**. **needs-real-HW** (D10) only to confirm inline x87 is the live path on Windows ARM64 (see §G note). |
| F3 | **DSP JIT chain-unpatch icache flush** (`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`) | **Equivalence-argued; BENEFIT on Linux arm64.** `DSP56K_JIT_SUPPORTED = __aarch64__ && !_WIN32` (`dsp56k_jit_arm64.h:24`) — POSIX aarch64 only. `chain_unpatch_incoming` rewrites branch sites in *other* blocks' live code but omitted the `jit_clear_icache` its sibling `retro_chain_patch_pending` (~`:9221`) already does — stale I-cache could execute a chained branch into a block being invalidated (audio-glitch class). Linux arm64 gets the fix (JIT gate widened at `2f6c6d691c`). **needs-real-HW** (D11 ≡ #8): the DIFF harness disables chaining so cannot cover this path; Linux-arm64 acceptance still owes a `XEMU_DSP_JIT_DIFF=1`+`_STATS=1` run. |
| F4 | **`XEMU_INV_PROF` SMC/invalidation-churn counters + `XEMU_TB_RANGE_INV` cure** (`accel/tcg/tb-maint.c`, `cpu-exec.c`, `translate-all.c`, `target/i386/tcg/translate.c`, `accel/tcg/xemu-inv-prof.h`) | **Regression-risk none; all-platform dev-tooling.** Header: declarations unconditional, definitions/sites `#if defined(XBOX)` (universal). Only plain `uint64` globals + latched `getenv` + `atexit` — nothing POSIX-only; single vCPU writer. Both default OFF, `unlikely()`-gated; default is byte-identical whole-page invalidation with zero added cost. `XEMU_TB_RANGE_INV=1` re-applies upstream's exact per-TB byte-range overlap filter (a correct subset, cannot miss a needed invalidation) but is measured neutral-to-negative (25× notdirty-trap explosion) — a dark A/B knob, not a shipped opt. Benefits Windows/Linux identically. |
| F5 | **Push-model present + refuter + `present_wait` counter** (`nv2a.h`, `pgraph.c`, `vk/{display,renderer}.c`, `ui/xemu.c`, `nsprof.[ch]`) | **Regression-risk none off-Apple; macOS-only benefit.** Shared surface `nv2a_get_present_frame_pushed()` (`pgraph.c:461`) returns false when the renderer op is unset; the vk op (`renderer.c:362`) + slot/publish machinery (`display.c:31-212`) are `HAVE_IOSURFACE_SHARING`-gated (the existing Apple-only present gate) and fall through to `return false` off-Apple, so the `renderer.c:401` registration compiles everywhere. Default OFF (`XEMU_PUSH_PRESENT`, Metal + interp-off). **GL presentation entirely untouched (no push op registered).** Apple correctness proven by the `XEMU_PUSH_PRESENT_REFUTE` byte-identical checker (0 mismatches over soaks). |

## G. Windows ARM64 sticky-SSE / inline-x87 note (F2 depth)

`XEMU_SSE_HOSTFP` is defined on Windows ARM64 (aarch64), so the sticky
bracket compiles and default-activates (`hard_fpu` default true).
`g_use_hard_fpu_inline = hard_fpu` is set with **no `_WIN32` gate**
(`translate.c:4678`). `TCG_TARGET_HAS_fpu=0` under `_WIN32`
(`tcg-target-has.h:66`, §A11) is a *link-time* workaround for llvm-mingw's
inability to constant-fold the generic-middle-end `qemu_build_not_reached`
FP branches (`tcg.c:2626-2671` gate `tcg_op_supported` for the FP ops); it
is **not** a runtime disable of the fork's inline emission. The aarch64
backend's `tcg_out_op` FP cases (`tcg-target.c.inc:3260`, `4008-4025`) are
present unconditionally and the `tcg_gen_*` FP emitters
(`tcg-op-fp.c:14-58`) emit without a support check, so inline x87 —
including `cached_fpuc_rc` — most likely runs on Windows ARM64 and F2
covers the leak there identically. The residual (whether
`tcg_op_supported=0` reroutes anything at runtime) is unresolvable
statically with no Windows-ARM64 host → **needs-real-HW D10**. The *fix*
regresses nothing either way; the pre-existing sticky-SSE feature
(`d76ddd7b20`) owns the receipt.

## H. Tests + CI portability (this batch)

- **Portable arms, all platforms:** `xbox-mcpx-dsp`,
  `xbox-mcpx-dsp-corpus-interp` (`tests/xbox/dsp/meson.build:48`),
  `xbox-nv2a-swizzle` (`swizzle/meson.build`; macOS `objcopy
  --redefine-sym` non-portability replaced by `-D` token-paste renames).
- **JIT arms, aarch64 POSIX only:** `corpus-jit` / `jit-diff-sync` /
  `jit-differential` gated `if host_machine.cpu_family() == 'aarch64' and
  host_os != 'windows'` (`tests/xbox/dsp/meson.build:57`) — `DSP56K_JIT_
  SUPPORTED` parity.
- **CI executes tests only on the macOS arm64 legs** (`build-macos.yml`,
  `if: matrix.arch == 'arm64'`, debug+release). `build-linux.yml` uses
  `dpkg-buildpackage` (`:140`) with no accessible meson build dir;
  `build-windows.yml` is win64-cross — **neither runs tests.** The
  `ubuntu-22.04-arm` runner (`build-linux.yml:14`) *would* exercise the
  aarch64 JIT arms if a meson-configure+`meson test --suite xbox` job were
  added — the cheapest path to Linux-arm64 JIT runtime coverage (folds
  needs-real-HW #8/D11 into CI). Owed: **CI push** (no local cross-build:
  no docker/colima).

## I. needs-real-HW list — additions (mirror README Future vectors)

9. **Texture/sampler eviction retirement gate** — report-value/soak on
   native AMD/NVIDIA/Intel Vulkan (equivalence proven; soak confirms) (F1).
10. **FPCR-cache fix on Windows ARM64** — `XEMU_SSE_HOST=2` differential on
    real Windows-ARM64 hardware to confirm inline x87 is the live path so
    F2 covers the leak; no regression either way (F2/§G).
11. **DSP JIT chain-unpatch icache fix on Linux arm64** —
    `XEMU_DSP_JIT_DIFF=1`+`_STATS=1` run (`checked>0, failures=0`); folds
    into #8. Cheapest via the CI arm runner (§H).

## J. Re-verification (addendum)

```sh
grep -c "__APPLE__\|IOSurface\|Metal" hw/xbox/nv2a/pgraph/vk/texture.c   # F1: expect 0
grep -n "cached_fpuc_rc = 0xFFFF" target/i386/tcg/fpu_helper.c            # F2: aarch64 branch only
grep -n "hard_fpu" config_spec.yml                                       # default: true
grep -n "DSP56K_JIT_SUPPORTED" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h  # aarch64 && !_WIN32
grep -n "defined(XBOX)" accel/tcg/xemu-inv-prof.h                        # F4: sites XBOX-gated
grep -n "HAVE_IOSURFACE_SHARING\|get_present_frame_pushed" hw/xbox/nv2a/pgraph/vk/renderer.c  # F5
grep -n "matrix.arch == 'arm64'\|meson test --suite xbox" .github/workflows/build-macos.yml
grep -n "dpkg-buildpackage\|ubuntu-22.04" .github/workflows/build-linux.yml  # H: no test, arm runner
```
