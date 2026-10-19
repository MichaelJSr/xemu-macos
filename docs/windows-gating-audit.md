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
hardware; the needs-real-HW list at the end is mirrored in
[`roadmap.md`](roadmap.md) ranked item 5 (the README "Future vectors"
section moved there in the 2026-07-18 README split).

## A. Compile-time platform gates (non-Apple keeps upstream semantics)

| # | Site | Gate | Non-Apple / Windows behavior |
|---|---|---|---|
| A1 | `hw/xbox/nv2a/pgraph/vk/surface.c` (`target_is_active`, ~1674) | `#ifdef __APPLE__` narrows the upload-surface full-GPU-sync | `#else target_is_active = true` — upstream full-finish semantics; the WHPX-visible ordering race documented in the comment cannot occur |
| A2 | `surface.c` ~1209 (scratch image) | Apple skips the scratch allocation at `surface_scale == 1` | always allocates (upstream) |
| A3 | `vk/instance.c` ~540 | Apple relaxes device features (`geometryShader`, `occlusionQueryPrecise`, … off) | `#else` requires the strict upstream feature set |
| A4 | `vk/instance.c` 55/167/233 | MoltenVK portability-subset/enumeration extensions | not requested; standard Vulkan init |
| A5 | `pgraph.c get_default_renderer` | Vulkan is default on Apple only | non-Apple order identical to upstream (OpenGL, then Vulkan) — verified against `upstream/master` |
| A6 | `pfifo.c` (`pfifo_thread`), `ui/xemu.c` (vblank thread; `qemu_main` under icount), `tcg-accel-ops-mttcg.c` (vCPU), `tcg-accel-ops-rr.c` (vCPU under icount) | `QOS_CLASS_USER_INTERACTIVE` bumps | compiled out; no scheduling change |
| A7 | `ui/xemu.c` MVK_CONFIG `setenv` block (~1897) | `#ifdef __APPLE__` | absent — MoltenVK config vars are meaningless off macOS |
| A8 | Metal present / MetalFX / IOSurface (`ui/xemu-metal.m`, `vk/metalfx_upscale.m`, `ui/xui/metal-helpers.mm`) | Objective-C sources added only under `host_os == 'darwin'` in meson | not compiled at all; GL presentation path as upstream |
| A9 | `hw/xbox/mcpx/apu/vp/vp.c` | vDSP (Accelerate) FIR path | `#else` portable C path (autovectorized) |
| A10 | `hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h` | `DSP56K_JIT_SUPPORTED = __aarch64__ && !_WIN32` (was `__APPLE__ && __aarch64__` when audited; Linux arm64 since) | fork inline JIT absent; interpreter or upstream external JIT engine per `dsp_want_external_jit_engine()` — precedence key (`audio.dsp_jit.enabled`) is inert when unsupported |
| A11 | `tcg/aarch64/tcg-target-has.h:65` | `TCG_TARGET_HAS_fpu 0` under `_WIN32` | helper-based hard FPU (bit-equivalent), avoiding the llvm-mingw `qemu_build_not_reached` link failure |
| A12 | `ui/xemu-input.c:522` | `XEMU_INPUT_PIPE` FIFO harness | `#ifdef _WIN32` stub — facility absent by design |
| A13 | `ui/thirdparty/fpng/fpng.cpp` | ARM CRC32 intrinsics | gated on `__ARM_FEATURE_CRC32` |
| A14 | `util/cutils.c`, `util/miniz`, `system/exit-with-parent.c`, `include/qemu/osdep.h` `__APPLE__` hits | — | upstream-inherited QEMU portability; zero fork-changed `__APPLE__` lines (verified via `git diff upstream/master...HEAD`) |

## B. Shared behavior divergences (active on Windows by design)

These are the fork's portable renderer/audio changes; upstream xemu does not
have them on any platform. Windows gets them via the same C paths.

| # | Divergence | Verdict + reasoning |
|---|---|---|
| B1 | In-pass occlusion queries, bulk `vkCmdResetQueryPool` at CB begin, per-CB primer query, 4096-query pool, `REPORTS_FULL` report guard (`reports.c`) and `QUERIES_FULL` query guard (top of `begin_pre_draw`, `draw.c`; moved there 2026-10-10 from `begin_draw`, where a trip handed MoltenVK `framebuffers[-1]`) | Equivalence-argued: all constructs are core-spec-valid (reset outside a pass, begin/end inside one pass, zero-sample query outside any pass). The primer exists for a MoltenVK-specific encoder rule and is harmless elsewhere (one empty query per CB). **needs-real-HW** for report-value soak on native drivers |
| B2 | Deferred zpass report delivery at slot reclaim + 300 µs FIFO-idle budget (`REPORTS_IDLE_BUDGET_NS`) + non-blocking `REPORTS_SUBMIT` (`reports.c`) | Platform-neutral logic. **needs-real-HW** for guest-poll timing under WHPX |
| B3 | Byte-exact vertex-conflict refinement (`vertex.c`) | Equivalence-argued: strictly-additive refinement over page-granular tracking; any unproven conflict still takes the conservative finish (architecture-contract Invariant 4). memcmp semantics are host-neutral |
| B4 | Redundant-bind skips: descriptor sets, vertex/index buffers, push constants, dynamic-state caches (`draw.c`) | Equivalence-argued from Vulkan pipeline-layout compatibility and CB-scoped state rules (reasoning in code comments); caches reset at CB begin |
| B5 | PFIFO untimed idle wait (`pfifo.c`) | Platform-neutral: the lost-wakeup proof is the enumerated kick-site lock discipline, not a platform property |
| B6 | Surface-expiry host-time throttle (`surface.c`, `QEMU_CLOCK_REALTIME`) | Platform-neutral; monotonic clock on all POSIX + Windows (`get_clock()` uses the monotonic source under both) |
| B7 | `vk_wait_for_fence_or_die` 60 s abort, warning at 5 s (`command.c`) | Deliberate cross-platform behavior change: converts silent driver hangs into diagnosable aborts. A loaded macOS host once stalled a startup upload past 5 s; legitimate waits are ~8 ms in captures |
| B8 | Deferred zeta-shape eviction + quarantine reuse (`surface.c`) | Reuse: **equivalence-proven** — a migrated image keeps its attachment layout; first GPU touch is ordered against all earlier same-queue submissions by the draw render pass's explicit `VK_SUBPASS_EXTERNAL` dependency (attachment stages/accesses) or the attachment-stage src masks of the transfer transitions; proof comment at `get_any_compatible_invalid_surface`. Destruction: was **formally invalid on every platform** (see C below), fixed by `a045dfc780` |
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

## D. Needs-real-HW validation list (mirrored in roadmap.md ranked item 5)

None of this can be exercised on this machine (VMs expose no Vulkan ICD):

1. Occlusion rework end-to-end on native drivers (AMD/NVIDIA/Intel):
   report values under deferred delivery (B1/B2).
2. Primer-query cost and validation-layer cleanliness on native drivers (B1).
3. Zeta quarantine/reuse soak on native drivers — equivalence is proven on
   paper, a soak on real HW is the confirmation (B8).
4. Byte-exact vertex refinement under WHPX guest-write timing (B3).
5. Fence-or-die margin on slow/loaded Windows systems (B7: warns at 5 s, aborts at 60 s).
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
| F1 | **Texture/sampler LRU eviction gated on submission retirement** (`hw/xbox/nv2a/pgraph/vk/texture.c`; sampler `submit_time` at `renderer.h:301-303`) | **Equivalence-argued + cross-platform BENEFIT.** `texture.c` is in the core vk meson list (`meson.build:19`, above the `host_os=='darwin'` fence) so it builds on every platform; `grep -c '__APPLE__\|IOSurface\|Metal' texture.c` = 0. The old pre-evict guard protected an entry only while referenced by the *currently-recording* CB, but `pgraph_vk_finish` pipelines (waits only slot N−1), so the just-submitted CB still executes with stale stamps and `post_evict` destroys immediately — a **use-after-free / VUID violation on every driver** (MoltenVK's deferred encode only widens the window). New guard `submit_time >= retired_submit_count` (`texture.c:2463`, sampler `:2536`) mirrors the §B8/§C surface watermark. Two forward-progress valves (`ensure_cache_headroom()` slot-fence drain, `texture.c:1676`; finalize drain, `:2628`) exist because `lru_evict_one`'s assert fires in release too (QEMU `#error`s on `NDEBUG`), i.e. exhaustion is an abort, not a silent miss. Windows/Linux native-driver users get a real correctness fix. **needs-real-HW** for native-driver soak (D9). GL untouched. |
| F2 | **x87 FPCR-cache invalidation when the sticky-SSE bracket rewrites FPCR** (`target/i386/tcg/fpu_helper.c:3892`) | **Regression-risk none; cross-platform BENEFIT on aarch64 hosts.** Compiles on all aarch64 (`XEMU_SSE_HOSTFP && __aarch64__`, `:3758/:3865`) incl. Windows ARM64; x86_64 branch (`:3918`, MXCSR) untouched and stays dark (`mode=0` default off-aarch64, `:3833`). The store `cached_fpuc_rc = 0xFFFF` is harmless everywhere (forces a clean `gen_flcr` re-fire; unread where inline x87 is off). Correctly aarch64-scoped: aarch64 shares ONE FPCR between NEON and the `double`-based x87 `__hard` helpers (`:293-318`), so a leaked FZ/RC corrupts x87 doubles; x86_64 writes MXCSR — a *separate* register from the x87 control word — so no leak is possible there. Beneficiaries are all aarch64 hosts running the default-on sticky path (`hard_fpu` default true, `config_spec.yml:400`): **macOS arm64 AND Linux arm64**. **needs-real-HW** (D10) only to confirm inline x87 is the live path on Windows ARM64 (see §G note). |
| F3 | **DSP JIT chain-unpatch icache flush** (`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`) | **Equivalence-argued; BENEFIT on Linux arm64.** `DSP56K_JIT_SUPPORTED = __aarch64__ && !_WIN32` (`dsp56k_jit_arm64.h:24`) — POSIX aarch64 only. `chain_unpatch_incoming` rewrites branch sites in *other* blocks' live code but omitted the `jit_clear_icache` its sibling `retro_chain_patch_pending` (~`:9168`) already does — stale I-cache could execute a chained branch into a block being invalidated (audio-glitch class). Linux arm64 gets the fix (JIT gate widened at `2f6c6d691c`). **needs-real-HW** (D11 ≡ #8): the DIFF harness disables chaining so cannot cover this path; Linux-arm64 acceptance still owes a `XEMU_DSP_JIT_DIFF=1`+`_STATS=1` run. |
| F4 | **`XEMU_INV_PROF` SMC/invalidation-churn counters + `XEMU_TB_RANGE_INV` cure** (`accel/tcg/tb-maint.c`, `cpu-exec.c`, `translate-all.c`, `target/i386/tcg/translate.c`, `accel/tcg/xemu-inv-prof.h`) | **Regression-risk none; all-platform dev-tooling.** Header: declarations unconditional, definitions/sites `#if defined(XBOX)` (universal). Only plain `uint64` globals + latched `getenv` + `atexit` — nothing POSIX-only; single vCPU writer. Both default OFF, `unlikely()`-gated; default is byte-identical whole-page invalidation with zero added cost. `XEMU_TB_RANGE_INV=1` re-applied upstream's exact per-TB byte-range overlap filter (a correct subset, cannot miss a needed invalidation); killed on a 25× notdirty-trap proxy without an fps A/B, it was a dark A/B knob, not a shipped opt. **2026-10-10:** the cure half was removed; the counters remain and the verdict stands. Benefits Windows/Linux identically. |
| F5 | **Push-model present + refuter + `present_wait` counter** (`nv2a.h`, `pgraph.c`, `vk/{display,renderer}.c`, `ui/xemu.c`, `nsprof.[ch]`) | **Regression-risk none off-Apple; macOS-only benefit.** Shared surface `nv2a_get_present_frame_pushed()` (`pgraph.c:617`) returns false when the renderer op is unset; the vk op (`renderer.c:362`) + slot/publish machinery (`display.c:31-212`) are `HAVE_IOSURFACE_SHARING`-gated (the existing Apple-only present gate) and fall through to `return false` off-Apple, so the `renderer.c:401` registration compiles everywhere. On by default on the Metal backend (interpolation on or off); falls back to pull on GL, headless, or before the first publish. **GL presentation entirely untouched (no push op registered).** Apple correctness proven by the `XEMU_PUSH_PRESENT_REFUTE` byte-identical checker (0 mismatches over soaks). |

## G. Windows ARM64 sticky-SSE / inline-x87 note (F2 depth)

2026-10-15: inline x87 has run on no host since `3eb1b2de10`, and its
fork layers and `cached_fpuc_rc` were removed, so F2, this section, I.10
and the J grep for `cached_fpuc_rc` are historical.

`XEMU_SSE_HOSTFP` is defined on Windows ARM64 (aarch64), so the sticky
bracket compiles and default-activates (`hard_fpu` default true).
`g_use_hard_fpu_inline = hard_fpu` is set with **no `_WIN32` gate**
(`translate.c:4853`). `TCG_TARGET_HAS_fpu=0` under `_WIN32`
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
- **CI executes tests on the macOS arm64 legs AND (since 2026-07-11) on
  Linux** (`build-macos.yml` `if: matrix.arch == 'arm64'` debug+release;
  `build-linux.yml` additive `test` job, x86_64 + aarch64, plain
  `build.sh --debug` tree + `meson test --suite xbox`). The packaging
  `build` job still goes through `dpkg-buildpackage` with no accessible
  meson dir — tests own a separate build tree. `build-windows.yml`
  (win64-cross) still runs no tests. First green run: CI 29177917607
  (2026-07-11) — the `ubuntu-22.04-arm` leg ran all three aarch64 JIT
  arms OK including the engagement assert, retiring the runtime half of
  needs-real-HW #8/D11 via CI (chain-unpatch caveat: see #11).

## I. needs-real-HW list — additions (mirror roadmap.md ranked item 5)

9. **Texture/sampler eviction retirement gate** — report-value/soak on
   native AMD/NVIDIA/Intel Vulkan (equivalence proven; soak confirms) (F1).
10. **FPCR-cache fix on Windows ARM64** — `XEMU_SSE_HOST=2` differential on
    real Windows-ARM64 hardware to confirm inline x87 is the live path so
    F2 covers the leak; no regression either way (F2/§G).
11. **DSP JIT chain-unpatch icache fix on Linux arm64** —
    The DSP JIT corpus arms now run green in CI on the arm runner
    (§H, first green 2026-07-11), which covers translation/execution/
    bit-exactness there. Still open: the chain-UNPATCH icache path needs
    P-space self-modification of an already-chained block, which the
    game-content-free corpus never triggers — an in-game
    `XEMU_DSP_JIT_DIFF=1`+`_STATS=1` run on Linux arm64 remains owed.

## J. Re-verification (addendum)

```sh
grep -c "__APPLE__\|IOSurface\|Metal" hw/xbox/nv2a/pgraph/vk/texture.c   # F1: expect 0
grep -n "cached_fpuc_rc = 0xFFFF" target/i386/tcg/fpu_helper.c            # F2: aarch64 branch only
grep -n "hard_fpu" config_spec.yml                                       # default: true
grep -n "DSP56K_JIT_SUPPORTED" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h  # aarch64 && !_WIN32
grep -n "defined(XBOX)" accel/tcg/xemu-inv-prof.h                        # F4: sites XBOX-gated
grep -n "HAVE_IOSURFACE_SHARING\|get_present_frame_pushed" hw/xbox/nv2a/pgraph/vk/renderer.c  # F5
grep -n "matrix.arch == 'arm64'\|meson test --suite xbox" .github/workflows/build-macos.yml
grep -n "dpkg-buildpackage\|Test xbox suite" .github/workflows/build-linux.yml  # H: test job present
```

---

# Windows real-hardware report — 2026-08-09 (v0.13.1, first field run + local repro)

The needs-real-HW list finally got its first data point. A user on real
Windows hardware (AMD Ryzen 5 5500, Zen 3, x86_64) reported the released
`v0.13.1` Windows x86_64 build boots to a **black screen with working
boot audio** — the guest runs, nothing renders. This section records what
the report plus a local reproduction attempt established. **Honest scope:
the reporter's exact failure (Vulkan on a real desktop GPU) could NOT be
reproduced on the dev Mac — no real Windows GPU is available here; the only
Windows target is a Parallels VM whose paravirtual GPU can't initialize
this renderer at all. Findings below are: two code-confirmed Windows
defects, one strongly-localized hypothesis for the reporter, and the wall
that blocks closing it locally.**

## K. The default renderer on Windows is VULKAN (not OpenGL)

`config_spec.yml` sets `display.renderer` **default: VULKAN** on every
platform. `nv2a_context_init` (`pgraph.c:436`) only calls
`get_default_renderer()` (the OpenGL-first-on-non-Apple fallback) when the
*configured* renderer is unavailable. So a fresh Windows install with no
`xemu.toml` runs the **Vulkan** renderer, not OpenGL. The
`get_default_renderer` OpenGL-first ordering (§A5) is a fallback, not the
default — it fires only if Vulkan fails to register entirely. **The
reporter is on the Vulkan path.**

## L. CONFIRMED defect: Vulkan requires strict device features on non-Apple

`instance.c:524-560` — the `#else` (non-Apple) branch marks
`geometryShader`, `occlusionQueryPrecise`,
`shaderTessellationAndGeometryPointSize`, `fillModeNonSolid`, `depthClamp`,
`shaderClipDistance` all **required: true**. If the selected physical
device lacks any one, `pgraph_vk_init_device` prints
`Error: Device does not support required feature <name>` and xemu exits.
This is the **upstream-inherited** requirement (the fork only *added* the
relaxed `#ifdef __APPLE__` branch at `50ccd17cfd`); the audit called it
A3. **First runtime confirmation (2026-08-09):** the Parallels Win11 VM
(paravirtual D3D12→Vulkan device, `Parallels Display Adapter (WDDM)`,
driver 26.1.99) aborts here on `shaderTessellationAndGeometryPointSize`.
Real desktop AMD/NVIDIA/Intel GPUs support all six, so the reporter's
machine passes this check — this abort is a **VM-GPU artifact, not the
reporter's bug** — but it is a real landmine for anyone on a
feature-poor Windows Vulkan ICD (VMs, Dozen/vkd3d layers, some iGPUs), and
the exit is total (no automatic OpenGL fallback on a *feature* rejection,
only on a missing-renderer registration).

## M. CONFIRMED defect: OpenGL renderer crashes after PFIFO start (VM)

Forcing `renderer = 'OPENGL'` on the VM gets past renderer init — the
PFIFO thread starts (`pfifo heartbeat iters=1`) — then the process
**crashes deterministically**: WER `Application Error`, `xemu.exe`,
exception `0xc00000ff`, identical fault offset across every run, dispatched
through `ntdll.dll` (debug build; the frame is almost certainly a
compiled-in assert/abort, WER LocalDumps did not capture a usable dump
under the scheduled-task session). **Caveat:** the VM's OpenGL is
`Parallels using Apple M2 Ultra (Compat)` — GL 4.1 over Metal over an
Apple GPU, driven through Prism x86→ARM translation. A real AMD OpenGL
driver is an entirely different stack, so this crash may not reproduce on
the reporter's machine and is likely **not** their bug either (they run
Vulkan). Recorded because it is a genuine crash in the fork's Windows GL
path and the only stack-adjacent signal obtained.

## N. Strongly-localized HYPOTHESIS for the reporter (unconfirmed on real HW)

Symptom = audio + black + process stays alive. That means the renderer
initialized (guest CPU + APU running), so on the reporter's real GPU the
§L feature check **passed** and Vulkan is live. The black screen is
therefore in the **present path**, not init. The non-Apple Vulkan present
(`display.c`) renders into an external-memory `VkImage` and shares it into
a GL texture via `VK_KHR_external_memory_win32` →
`glImportMemoryWin32HandleEXT` (the GL window samples that texture). If
that VK↔GL interop yields a black/garbage texture on their driver combo,
the game area is black while Vulkan and audio run normally; in a **release
build the guarding `assert(glGetError()==GL_NO_ERROR)` is compiled out**,
so the failure is silent. The auto-hiding ImGui menu bar would leave the
whole window looking black. This flow reads as structurally intact and is
largely upstream, so the break — if it is a fork regression vs. stock
xemu-on-Windows — most likely lives in the 1737-line `display.c` rework,
but **no specific defect was pinned, and it was not reproduced on real
hardware.** Do not present this as proven.

## O. The wall (why this can't be closed on the dev Mac)

Reproducing the reporter's case needs a Vulkan device that (a) supports the
§L strict feature set AND (b) runs the real Windows VK↔GL interop present
path. The dev Mac has neither: no real Windows GPU, and the only Windows
environment (Parallels) exposes a paravirtual GPU that fails §L before the
present path is ever reached. Options to actually close it, in order of
signal: (1) get the reporter's `xemu.log` (shows selected renderer +
whether init completed + where output stops — zero effort for them); (2)
have the reporter set `renderer = 'OPENGL'` in `xemu.toml` and report —
renders ⇒ Vulkan present path confirmed as culprit + gives them a working
config; black/crash ⇒ deeper; (3) reporter runs stock upstream xemu — works
⇒ fork regression, isolates to fork-changed Windows code; (4) build a
patched Windows xemu that relaxes §L to exercise the present path on the
VM's Vulkan (heavy; the paravirtual GPU may still not render Xbox geometry
correctly). Reporter-facing triage doc: `scratchpad/windows-triage-for-reporter.md`.

## P. Reproduction assets (2026-08-09, dev Mac)

- Fixtures + released dbg Windows build staged in a local triage folder on the Mac
  and inside the guest at `C:\xemu-triage\` (bootrom/BIOS/eeprom, cloned
  `hdd-triage.qcow2`, `xemu-dbg/xemu.exe` = `xemu-0.13.1-dbg-windows-x86_64.zip`).
- Guest launch is via interactive-token scheduled tasks (`xemudesk`/`xemugl`)
  so xemu gets a real GPU window station (session-0 `prlctl exec` wedges at
  window/GL-context creation — an artifact, not the bug).
- macOS-side receipts (renderer axis exoneration): `scratchpad/gltest/`
  (fork GL renderer boots Azurik on macOS; official x86_64 slice under
  Rosetta 2 boots — TCG/x87/ISA axis clear).

## Q. ROOT CAUSE + FIX (2026-08-09, resolved)

The reporter confirmed **stock upstream xemu renders on their Windows Vulkan
GPU while this fork is black** — a clean differential proving a fork
regression in fork-changed VK code that MoltenVK tolerates but a native ICD
enforces. Three parallel fork-vs-`upstream/master` diffs of the non-Apple VK
path found it.

**Primary bug — illegal coalesced cross-stage UBO descriptor write.**
`pgraph_vk_update_descriptor_sets` (`hw/xbox/nv2a/pgraph/vk/shaders.c`) wrote
the two set-0 uniform buffers — binding 0 (`VSH_UBO_BINDING`, **VERTEX**
stage) and binding 1 (`PSH_UBO_BINDING`, **FRAGMENT** stage) — as a single
`descriptorCount=2` `VkWriteDescriptorSet` at `dstBinding=0`, relying on the
Vulkan §14.2.3 consecutive-binding overflow. That overflow is only legal
when every spanned binding has identical `stageFlags`; these differ, so the
write violates **VUID-VkWriteDescriptorSet-descriptorCount-10776** and
**VUID-VkWriteDescriptorSet-dstArrayElement-00321**. MoltenVK tolerates it
(each element lands in its Metal argument-buffer slot → macOS renders); a
native driver may leave binding 1 (`PshUniforms`) unwritten, so the fragment
shader reads a stale/zero uniform buffer and **every fragment resolves to
black** while guest + audio run — the exact field symptom, on every draw
(persistent). Upstream wrote the two UBOs as two separate `descriptorCount=1`
writes.

**Runtime-confirmed, driver-independently.** A standalone Vulkan program
(`scratchpad/vkrepro/repro.c`) issuing the exact write under the Khronos
validation layer fires both VUIDs for the coalesced form and is clean for the
two-write form — confirming the violation is real and the fix removes it,
independent of any driver's tolerance.

**Secondary bug — display descriptor use-after-free.** `update_descriptor_set`
(`display.c`) cached "skip if unchanged" on a raw `SurfaceBinding*` never
cleared on eviction; after `g_free`/`g_malloc` address reuse, a destroyed
`VkImageView` stays bound → black on native, tolerated by MoltenVK
(ARC-retained `MTLTexture`). (Agent-B sync/barrier divergences — pipelined
`pgraph_vk_finish`, narrowed aux→main `wait_stage` — are real but
`wait_stage=VERTEX_INPUT` still gates the shader stages; not fixed, revisit
only if the two above prove insufficient.)

**Fix — branch `fix-windows-vulkan-black`** (CI green on macOS/Linux/Windows;
merged for v0.13.2, branch deleted 2026-10-10):
(1) split the UBO write into two per-stage writes (primary; universal);
(2) gate the display descriptor skip-cache to Apple only (native rewrites
every present); (3) relax `shaderTessellationAndGeometryPointSize` to
optional on non-Apple — still enabled when available (no fidelity change on
capable GPUs), but its absence no longer hard-aborts init on weak/paravirtual
Windows Vulkan ICDs.

**Validation state.** Primary fix is validation-layer-proven locally. The
end-to-end render was NOT reproduced on real Windows GPU hardware here: the
only local Windows target is the Parallels VM, whose Vulkan is Microsoft
Dozen (`vulkan_dzn.dll`, Vulkan→D3D12→Metal on Windows-on-ARM) — it crashes
inside the driver during resource setup (0xc0000005), before rendering, even
with the feature relaxed. The reporter's real AMD driver is far more
conformant (reaches rendering → black, not crash), so the definitive
confirmation is the reporter testing the patched build; commits 1+2 are what
fix their screen (their GPU has the relaxed feature).

## R. Deep cross-platform audit (2026-08-09) — additional fixes

A follow-up sweep (VK renderer / Windows / Linux-POSIX / TCG host-arch, four
parallel diffs vs `upstream/master`) found more of the same
MoltenVK-tolerated-or-arm64-only class. Confirmed and fixed for v0.13.2:

- **Dynamic-state reuse across command buffers** (`vk/draw.c`) — line width
  / depth bias / blend constants could be skipped against a value cached in
  a prior CB (undefined CB-scoped state, VUID-vkCmdDraw-None-07833/34/35).
  All native GPUs incl. the reporter's; MoltenVK retains state across CBs.
  Fixed by poisoning the conditional caches at CB begin.
- **Texture `bufferOffset` misalignment** (`vk/texture.c`) — the staging
  bump allocator left `VkBufferImageCopy.bufferOffset` unaligned to the
  texel block size (VUID-...-bufferOffset-00193) → texture corruption on
  native. Fixed by rounding up to 16.
- **GS `gl_PointSize` capability** (`vk/{instance,shaders}.c`, `glsl/geom.*`)
  — §L's feature relaxation was incomplete: the GS still declared the
  `GeometryPointSize` capability, moving a clean early failure to a late
  pipeline-creation failure on drivers with `geometryShader` but not the
  point-size feature. Fixed by omitting the `gl_PointSize` write (and thus
  the capability) when the device lacks the feature.
- **Inline x87 FPU, Windows ARM64** (`target/i386/tcg/translate.c`) —
  inline FP ops were emitted despite `TCG_TARGET_HAS_fpu=0`, tripping the
  codegen assert (debug) / UB (release). This **corrects §A11/§G**, which
  wrongly claimed Windows ARM64 fell back to the helper FPU — it did not
  until this fix (`g_use_hard_fpu_inline` now gated on `TCG_TARGET_HAS_fpu`).
- **Inline x87 directed rounding, x86_64 hosts** (`translate.c`) — `FIST`
  under RC ±∞ and `FRNDINT` under any non-nearest mode silently rounded to
  nearest (the x87 CW is never programmed on i386; only MXCSR is). Fixed by
  a softfloat-helper fallback for those cases on non-aarch64 hosts. Affects
  Windows/Linux x86_64 (the reporter's platform) for directed-rounding FP.
- **Sub-page dirty-tracking default** (`accel/tcg/xemu-subpage-fast.c`) —
  default-on contradicted its documented "Apple 1, elsewhere 0" and ran
  unvalidated on non-Apple aarch64. Gated the default to `__APPLE__`.

Audited clean (important negatives, verified in-tree): the two-descriptor-set
split, texture descriptor coalescing, descriptor bind-skip across clears, the
pipelined `pgraph_vk_finish` and narrowed aux→main `wait_stage` (per-slot
partitioned; wait gates all later stages), deferred eviction / LRU retirement
watermark, the Linux OPAQUE_FD external-memory path (no leak/double-close),
the DSP56300 arm64 JIT non-Apple W^X/icache path, audio/APU portable
`#else`/SSE2/scalar paths, xpage chaining / can_do_io elision / lockless MMIO
(all host-arch-independent). Remaining suspected-latent items (not fixed,
don't fire on desktop): compute workgroup size 256 unclamped, `BUFFER_COMPUTE`
256 MiB cap, surface-upload compute-descriptor partition guard.

---

# Windows real-hardware report — 2026-10-18 (v0.15.1 + windows branch, NVIDIA TITAN Xp, i7-4770K)

The first run of [`windows-test-plan.md`](windows-test-plan.md) on real
hardware, 2026-10-17 to 18, driven by the agent on the owner's PC and
mostly unattended. Results are in the kit's `results/RESULTS.md`; fixes
and optimizations went to `windows` (`ad7188d177..18638975a3`). **Honest
scope: one x86_64 CPU and one NVIDIA Pascal driver.** "Settled" below
means settled on this configuration; AMD and Intel GPUs, Windows ARM64
and Linux stay as open as before. Plan step ids (2.3, 3.11, ...) name
the evidence.

## S. Machine and builds

- Windows 10 Pro 22H2 (10.0.19045); Intel Core i7-4770K (Haswell, 4C/8T,
  not hybrid, no SHA-NI), 32 GB; 2x NVIDIA TITAN Xp (Pascal; xemu picks
  GPU0, which drives both monitors), driver 561.09, Vulkan 1.3.280 (the
  SDK 1.4.363 loader replaced the system one during 2.3); 3840x2160@60 +
  1920x1080@180 at 100 %; Balanced power plan (High performance around
  benchmark batches). Same box and driver as the 2026-09-07 session.
- Builds: v0.15.1 release (sha256[:16] `e68e784be6fdbd7f`) and its -dbg
  build, v0.14, upstream v0.8.137, CI builds of the branch, and native
  MSYS2 GCC 16 builds.

## T. Needs-real-HW items

| Item | Status | Evidence |
|---|---|---|
| D1 occlusion rework, report values under deferred delivery (B1/B2) | Partly | The anchors render (2.3) and the 3.11 soak is clean; report values were not compared against a reference, and B2's guest-poll timing ran under TCG only (no WHPX) |
| D2 primer-query cost, validation cleanliness (B1) | Partly | 3.10a, five scenes on SDK 1.4.363: one VUID (device-level layers, not query-related), 0 after `699bbd0d38` (3.10a-r2). The `QUERIES_FULL` guard ran for the first time once `54671ac359` stopped `XEMU_MAX_QUERIES` shrinking the report pool: ~18 trips/flip at 64, renders, clean exit (3.10b-r2). Primer cost not measured |
| D3 zeta quarantine/reuse soak (B8) | Settled (NVIDIA) | 3.11 soak; 3.10c F1→F6→F1 walk-ins ×3 and movie-e2; 0 VUIDs (3.10a-r2) |
| D4 byte-exact vertex refinement under guest-write timing (B3) | Partly | 3.10c walk-ins and the 3.11 walks: screenshots PASS, no errors, TCG only; 3.10d offscreen frames await the Mac comparison |
| D5 fence-or-die margin on a loaded system (B7) | Partly | No `vkWaitForFences` warning in 2.3, 3.10c, 3.11 or the windowed benches, some under native builds on half the CPUs; fence_wait 0.1-0.6 ms/flip at v0.15.1 rates, 1-5 ms/flip at the optimized build's 30-40 fps. A fast GPU: slow or GPU-bound systems open |
| D6 Windows baselines before any perf claim | Settled (this box) | Det: instructions and core cycles per frame from ETW counters (`c783af41ca`), §V. Windowed (4.2-4.4): A/A floor 0.1%; v0.15.1 heavy-f1 10.7 fps quiet; the headline A/Bs in §V |
| D7 Windows PGO | Settled (native GCC) | 7.4: the `.gcda` flow works natively: an `XEMU_PGO=generate` build, the plan's corpus (5 loadvm sessions, a walk, 2 det runs: 1187 profiles), a `use` build. Det heavy-f1 against the same source without PGO, quiet: core cycles −3.1 %, instructions −3.9 % (4/4), goldens identical. Not in CI: CI's cross GCC cannot read a native profile; a CI `generate` binary trained here with `GCOV_PREFIX` is the way |
| D8 / I.11 DSP56K JIT on non-Apple aarch64 | Open | Not reachable from x86_64 |
| I.9 texture/sampler retirement gate (F1) | Settled (NVIDIA) | 3.11 soak; 0 VUIDs (3.10a-r2) |
| I.10 FPCR cache on Windows ARM64 | Open | Historical since §G's 2026-10-15 note; plan 3.9 needs ARM64 hardware |
| B5 PFIFO untimed idle wait | Settled (this box) | 3.11: heartbeat `iters` still rising at the end (4.44M) |
| B6 surface expiry on monotonic time | Settled (NVIDIA) | 3.10c walk-ins, 3.11 |
| Lockless MMIO and DMA_PUT doorbell, first non-VM soak (roadmap 5) | Settled (Windows x86_64) | 3.11, 30/30 cycles over heavy-f1, W2, stream-e2 (walking), F5, transition-f1 (walking): every screenshot PASS, the frame counter rising every cycle, no fence warnings, errors or asserts, exit 0. Linux open |
| `XEMU_SSE_HOST=2` on real x86_64 silicon (roadmap 5) | Settled | 3.8: no divergence; det heavy-f1 f300/f1200 and stream-e2 f600/f1200 equal the default (softfloat) hashes; 5 min live walk. The host path is now the x86_64 default (`57cfe4fb51`) |
| §K/§L Vulkan default, strict non-Apple features | Settled (NVIDIA) | No `preferred_physical_device`: Vulkan init passes and picks GPU0 (2.3). `VK_KHR_external_semaphore_win32` is no longer enabled (`6e2accff54`): windowed benches render |
| §N/§Q Win32 external-memory VK→GL present | Settled (NVIDIA) | "External memory sharing: enabled", DXGI presenter in flip mode with tearing, every scene renders (2.3, 3.10a, the windowed benches). AMD, the v0.13.1 reporter's vendor, unchecked |
| §M OpenGL renderer | Settled (NVIDIA) | heavy-f1 renders on v0.15.1 and the fixed build, exit 0 (2.4 rerun); the VM crash does not reproduce |
| v0.14+ Windows-only paths (plan §0) | Settled | 3.1 logs, 3.2 headless offscreen and null, 3.3 QMP, 3.4 input pipe (0 of 100 writer failures), 3.5 `XEMU_RAM_SHM`, 3.6 gdbstub, 3.7 det goldens, 3.12 refuters (SUBPAGE, XPAGE, TLB_WCLEAN, TB_REVAL: 0 violations), 3.15 busy QMP port |
| 2026-09 "6-10 s after launch" native crash | Settled | The SDL race in §U, not `-O3`: 7.3 clean, 6 of 6 launches at native `-O2` and `-O3`, and 6 of 6 on the CI `-O3` build (cross GCC + LTO); x86_64 builds now default to `-O3` (`6a58566a54`) |

## U. Defects found and fixed

| Defect | Found | Fix |
|---|---|---|
| **P0** startup crash, either renderer, 7-9 s after launch, 3 of ~25 launches: SDL 3.4.18's `WIN_CreateWindow` pumps messages before it sets the new window's `internal`, so an activation of xemu's main window then sends `WIN_OpenClipboard` through NULL. The GL renderer's three hidden gloffscreen windows are created in every renderer mode. Upstream v0.8.137: 0 of 5 | 2.4, 3.6 (-dbg dumps) | `058431f83b` (wrap `diff_files` patch, CI builds); `d1b99ddb42` (native builds link it statically) |
| **P1** every `XEMU_HEADLESS=null` quit crashed: the DXGI presenter cleanup called `glBindFramebuffer` through an unresolved epoxy pointer | 3.2, 3.3, 3.5 | `e40df3fae7` |
| x86_64 hard FPU: `frndint`, `fist`, `fistp` and the int64 forms rounded to nearest under any guest RC. **Corrects §R**: its 2026-08 "softfloat fallback" never ran under `hard_fpu` (that helper is the long-double one). Every Windows det hash moves | optimization pass, reading the hard helpers | `cfcc8e68ab` |
| x86_64 hard FPU: the vCPU thread's x87 control word was never set, and Windows starts threads at `0x27f` (53-bit): guest extended-precision math ran at double precision, single precision rounded twice (53-bit, then a cast that also clamped the exponent range), and the arithmetic ignored the guest's RC. Azurik runs PC double, RC nearest, so no golden moved | optimization pass (a `_beginthreadex` probe) | `deb98ee93f` (the guest's PC and RC in the host control word) |
| The Windows main loop busy-waited every sub-millisecond timer deadline: 95% of a logical CPU, and under host load the vCPU starved (windowed heavy-f1 once 3.9 fps) | per-thread probe | `e8e260186d` (high-resolution waitable timer) |
| Minimized, flip-model Present returns at once, so the UI loop spun unpaced | 3.13a | `9778d93ad5` (a vblank sleep; minimized process CPU 176 % → 158 % of one core) |
| The no-console `xemu.log` was fully buffered (lost its tail in a crash), and a stderr the parent had redirected to a file was replaced by `xemu.log` | 3.1b, 3.1-r2 | `a150d08571` + `7d5a2e6cc6`; `06d375729f` |
| `VUID-VkDeviceCreateInfo-enabledLayerCount-12384`: validation layers passed as device layers | 3.10a | `699bbd0d38` |
| `XEMU_MAX_QUERIES` shrank the report pool too, so `REPORTS_FULL` always fired first and the `QUERIES_FULL` guard never ran | 3.10b | `54671ac359` |
| `XEMU_TB_REVAL_REFUTE` armed only on incremental restores, so 3.12 printed nothing | 3.12 | `2568477fd8` |
| The OS info line read `OS_Version:  22H2`: `RegGetValueW` got WCHAR sizes and wrote 4 bytes into a 2-byte variable | logs | `47b506ad18` |
| nsprof and heartbeat reports went out one character per write (mingw printf on an unbuffered stderr) | 3.1b follow-up | `c25a93771b` |
| The Vulkan SPIR-V cache ignored Settings > Cache shaders to disk (its own `XEMU_SPIRV_CACHE` hatch) | review | `a922602c75` |

§A6 changed: 3.13b measured the cost of normal priority under host load
(§V), and `f96f00ba9d` now runs the vCPU, PFIFO and APU threads at
`THREAD_PRIORITY_ABOVE_NORMAL` on Windows (`XEMU_VCPU_QOS=0` leaves all
three at normal); the main loop stays at normal. Harness fixes:
`07fb7a811d`, `a0aff4866d`, `f1595e0c5e`, `7a9589adcb`, `39592145d7`,
`c783af41ca`, `d3ea0ee8af`, `06ebdace13`.

## V. Performance

v0.15.1 on this box (2.3 smoke, windowed nsprof flips/s): heavy-f1
8.3-10.1, W2 7.9-8.1, stream-e2 23.9-25.3, F5 19.9-21.1, fence_wait
0.1-0.56 ms/flip: CPU-bound on the vCPU. The det profile put ~47% of the
vCPU in x87 helpers, mostly store-forwarding stalls on `floatx80` copies.

**Headline (windowed, v0.15.1 against the branch):** heavy-f1 11.1 → 38.7 fps (3.5×), W2 8.5 → 40.7 fps (4.8×), v0.15.1 against the final CI release (`18638975a3`), 3 interleaved 60 s pairs each, 3/3, on a quiet box (load 1-3 %). The vCPU is still the limit there (94 % of a core at ~30 fps in a per-thread probe); the Mac M2 Ultra runs both at its 60 fps cap.

Det A/Bs: `det_golden.py`, cross-binary unless noted, interleaved pairs,
instructions and core cycles per frame from ETW counters (the TSC-based
Mcyc/frame rises with hyperthread contention). Det heavy-f1 to frame 1200
unless noted. The x87 rows took det heavy-f1 from 38.8 to 88 (native `-O2`) and 91 (native PGO) on a quiet box
fps; `docs/optimizations.md` (CPU / JIT, x86_64 hosts) has the details.

| Change | Delta | Signs | Load | Verdict |
|---|---|---|---|---|
| `66a991d060` x87 hard helpers on the registers in place | core cycles −26.1 %, insns −6.2 %, det fps 38.8 → 52.8 (+36 %) | 4/4 | PGO builds on CPUs 4-7 | kept, hash identical |
| `acecea21c8` x87 helpers `TCG_CALL_NO_WG`, fused memory operands | core cycles −1.9 %, insns −0.37 % | 4/4 | quiet | kept, hash identical |
| `deb98ee93f` host x87 control word = guest PC/RC, no `pack` | insns −2.35 %, cycles −2.2 % min to min | 4/4 | worktree build on CPUs 4-7 | kept, goldens identical |
| `aae214c27a` hot x87 ops inline on the host x87 unit (`x87_*` TCG ops) | core cycles −23.5 %, insns −15.3 %, det fps 54.4 → 72.4 (+33 %) | 4/4 | worktree build on CPUs 4-7 | kept, goldens identical |
| `873ab92ed4` TBs keyed by the x87 TOP, registers at fixed offsets | insns −8.3 %, core cycles −6.4 %, det fps 69.2 → 74.1 | 4/4 | quiet | kept, goldens identical; refuter 0 of 5.0G |
| `bfdf7572d2` det-mode FP brackets compare and skip (`fnstcw`/`stmxcsr` instead of mingw's fenv) | insns −0.58 %, core cycles −2.0 % median | 4/4, 3/4 | PGO training on CPUs 4-7 | kept, det mode only; goldens identical |
| `67e48b43b2` `fist`/`fsqrt`/`frndint` inline | insns −0.25 %, cycles within noise | 4/4, 2/4 | PGO training on CPUs 4-7 | kept (exact); goldens identical |
| `e8a32939e5` `fcmov` as a select, not a branch | core cycles −3.0 %, insns −0.11 %, det fps +3.3 % | 4/4 | quiet | kept; goldens identical |
| `57cfe4fb51` SSE host path (softfloat vs `XEMU_SSE_HOST=1`, same binary) | insns −8.8 %, cycles −3.4 %, det fps 35.6 → 38.0 (+3.6 %) | 4/4 | quiet, xemu on CPUs 0-3 | kept, default; hash moves |
| its MXCSR flag mirror (without vs with) | insns +2.0 %, cycles −0.3 % median | 0/4, 2/4 | build on CPUs 4-7 | kept (exact flags) |
| `baf5e0c100` no `-fzero-call-used-regs` | insns −5.47 %, cycles −0.9 % median | 4/4, 2/4 | quiet | kept |
| `4e50b583d4` file-win32 coroutine I/O (cold boot 1800, stream-e2 600) | cold boot cycles −6.1 %, insns −1.3 %; stream-e2 cycles −1.2 % | 3/3, 3/3 | functional runs on CPUs 4-7 | kept, hashes unchanged |
| `6d0e98c828` hot/cold TB layout on tcg/i386 | cycles −0.85 % median (mean −1.7 %), insns −0.13 % | 3/4 | build on CPUs 4-7 | kept, same hash |
| native `-O2` vs `-O3` (058431f + static SDL) | cycles −1.4 %, insns −1.1 % | 3/4, 4/4 | functional runs on CPUs 4-7 | kept: `6a58566a54` |
| `6a58566a54` CI release `-O2` vs `-O3` without the stack protector (cross GCC + LTO) | insns −2.31 %, core cycles +0.26 % median | 4/4, 1/4 | PGO build on CPUs 4-7 | kept: matches the macOS/Linux arms; cycles neutral under LTO; 7.3 6/6 |
| native GCC 16 `-O2` vs CI cross GCC + LTO (same source) | CI cycles −2.8 %, insns −6.1 % | 4/4 | functional runs on CPUs 4-7 | LTO pays: bench LTO or CI builds |
| 7.4 GCC PGO, native (`-O2` vs the same source + PGO) | core cycles −3.08 %, insns −3.93 %, det fps 88.3 → 91.3 | 4/4 | quiet | works natively; not in CI (cross GCC) |

Windowed (bench-savestate-ab-win.py, interleaved, 45-60 s arms): the 4.2
A/A floor is +0.06 % (2/2). `f96f00ba9d` (3.13b): heavy-f1
`XEMU_VCPU_QOS=0` vs `1`, 3 pairs: with a busy loop on all 8 logical CPUs
6.8 → 9.3 fps (+36 %, 3/3); idle 20.0 vs 19.8 (−0.8 %, signs mixed).
`e8e260186d` (W1): the whole process 245.9 % → 147.5 % of one core; with
native builds on CPUs 4-7, 16.9 → 29.3 fps (3/3), where a busy-waiting
run fell to 3.9 fps; the quieter warmups tie at 28.9. 4.3/4.4 (v0.15.1, windowed): heavy-f1 10.7 fps on a quiet box; F5 14.4 fps against v0.14's 12.4 (+15.9 %, 2/2). 4.5's switch A/Bs on v0.15.1 at 9-11 fps, two under build load: `XEMU_BQL_SPIN` default −2.0 % (2/2), `XEMU_KI_IDLE_HALT` +6.1 % (1/2), `XEMU_PFIFO_STALL_DOORBELL` −1.3 % (1/2, quiet); no default changed on that evidence.

Goldens (3.7): Windows det is deterministic in every config (r1 = r2;
dsp56300 JIT = interpreter; `XEMU_SSE_HOST=2` = default) and, as
expected, matches no macOS hash (x87 long double vs double, SSE2 vs NEON
kernels). Windows goldens since `cfcc8e68ab` (unchanged through every
later commit): heavy-f1 f300 `37fe3d839c4be757` f1200 `ec2e9596cc9dfa9c`,
stream-e2 f600 `38b50b4c6f35ac02`, cold boot f900 `d6381be67430024d`
f1800 `1daeb1149976062c`. The debug vs release split: worktree builds of one commit settle part of it. GCC's FMA contraction in the x86-64-v3 release moves heavy-f1 and stream-e2: with `-ffp-contract=off`, or an x86-64-v1 build, the release hashes equal each other (heavy-f1 f300 `0d82336084feb76d` f1200 `599aceace7f41529`, stream-e2 `3808ce768eeb4123`), so nothing else in the v3 ISA matters. The debug build differs from both (`654eee348901d236` / `6938665a5124a399` / `48aeac8dabb00a8e`): a debug-only effect (`-O0`, `CONFIG_DEBUG_TCG`, or undefined behaviour `-O0` exposes), not found yet. Cold boot is equal in every build.

## W. Still open

- Hardware not covered: AMD and Intel GPUs (D1-D5, I.9, §Q), Windows
  ARM64 (plan 3.9, I.10), Linux (its lockless soak, D8, I.11).
- x86_64 JIT gaps against aarch64, each a backend port: guest registers
  pinned in host registers (aarch64 measured −3.9 % cycles; x86_64 has
  half the registers, and the all-call-saved layout lost there), shared
  softmmu TLB walks, inline packed-single SSE (exact MXCSR flags need a
  design: host code pollutes the host flags), and the double-based x87
  write-through path (on x86_64 it would trade the long double helpers'
  precision for doubles).
- Needs a person: 2.1 audio, controller, fullscreen, UI scale and
  thumbnails; 3.14 menu loads (synthesized keys don't reach the window
  from a background process); 3.15 live pads under icount. 4.7's fps readout: Debug > Video opens only with a click (the agent's part is in RESULTS.md).
- Findings left open: `xemu-ram-digest` sha256 takes 655 ms for 128 MiB
  without SHA-NI (CNG is no faster; xxh3-128 takes 29-41 ms); the disc
  fingerprint takes 7.3 s at the first save after a disc insert (disk
  I/O for 2.7 GB; computing it at disc load would hide it but read every
  image on every load); 3.10d frames to compare on the Mac; an empty tray
  on the play HDD shows "Please insert an Xbox disc..." instead of a
  dashboard (compare on the Mac). Thread names are empty unless
  `-name ...,debug-threads=on`, as on every host.
- Upstream: `xemu-project/xemu` is 2 commits ahead (`5c6c5501c7` nv2a X1A7
  clear formats, `8fc3208697` AppImage GLib), for the next upstream sync
  on master.
- Operations: Windows Update restarted the PC at 03:08 mid-run; pause
  updates before an unattended run.
