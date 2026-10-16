# xemu-macos optimization ledger

Every landed optimization and every attempted-and-settled experiment in
this fork, with the receipts that shipped or killed them. This file is
the doc of record for *what has been done*; the open candidates live in
[docs/roadmap.md](roadmap.md), and the concise user-facing manifest is
the top-level [README](../README.md).

Sections: the per-subsystem landed-change manifest (moved here from the
README 2026-07-18), the runtime diagnostic-knob and normal-mode-switch
catalog (each knob exists because of a specific entry here), and the
failed / reverted / settled-negative experiment tables. An
"archaeology N.M" citation is section N.M of the retired
failure-archaeology skill:
`git show ad569d78c5:.claude/skills/xemu-failure-archaeology/SKILL.md`.

## Runtime diagnostic knobs and normal-mode switches

Default-off knobs turn on with `1`; a `0` knob turns a default-on
behavior off. Each knob is a diagnostic, harness interface, A/B lever,
opt-in for an unvalidated host or tuning knob tied to an entry in the
Changes manifest below, and every one has a `getenv` on this branch.
Since 2026-10-15 (owner decision) a shipped change keeps a behavior
switch only when it is a normal-mode change the deterministic goldens
can't verify: `XEMU_VCPU_QOS`, `XEMU_PFIFO_PUT_LOCK`,
`XEMU_PFIFO_STALL_DOORBELL`, `XEMU_GUEST_MO` and `XEMU_KI_IDLE_HALT`,
the first five rows. The other `=0` row, `XEMU_SPIRV_CACHE`, is a
cold-compile A/B bypass. Knobs that exist
only in the retired wave-1 work are recorded in
[`Finished/windows-wave1-review-2026-09-08.md`](Finished/windows-wave1-review-2026-09-08.md)
and tag `archive/windows-wave1-wip`.

| Env var | Purpose |
|---|---|
| `XEMU_VCPU_QOS` | macOS normal mode, **default on since 2026-10-15** (`94a722acd8`): the MTTCG vCPU thread runs at `QOS_CLASS_USER_INTERACTIVE`; `0` leaves it at the default class (less P-core time and power, slower under load). Deterministic mode's RR thread always requests it. Threads + runtime, P-core QoS |
| `XEMU_PFIFO_PUT_LOCK` | Normal mode: `1` makes the DMA_PUT doorbell and USER reads take `pfifo.lock` again (lockless doorbell, 2026-10-15, `3391b92357`). Deterministic mode always locks, because `kick_seq` feeds `pfifo_icount_sync`. Threads + runtime |
| `XEMU_PFIFO_STALL_DOORBELL` | Normal mode, **default on since 2026-10-16**: a DMA_PUT doorbell inside a flip stall (FLIP_STALL waiting, READ_3D == WRITE_3D) publishes PUT but doesn't wake the PFIFO thread, which could only stall again; `0` wakes it on every doorbell as before. Deterministic mode never takes the lockless doorbell. Threads + runtime |
| `XEMU_GUEST_MO` | `1` keeps every guest memory-order barrier in both modes. By default normal mode drops the load-load barriers (`60fc127871`) and icount drops all of them (`30316da269`) until a network backend attaches: `xemu_net_enable` calls `xemu_guest_mo_keep_all`, which restores them and queues a safe-work tb flush. Normal-mode stores and explicit fences keep their barriers. Replaces `XEMU_ICOUNT_MO`, which is gone. CPU / JIT |
| `XEMU_KI_IDLE_HALT` | Normal mode, **default on since 2026-10-16**: the vCPU halts in the Xbox kernel's idle loop after spinning 50 µs per idle span (`helper_xemu_ki_idle`); `0` keeps the spin, which burns the vCPU's core until the next interrupt. Deterministic mode always halts there at once. Threads + runtime |
| `XEMU_NV2A_NSPROF` | Wall-time frame profiler, 5 s summaries to stderr. Since 2026-08-04 it also reports three UI-thread buckets — `drawable_acq` (block inside `nextDrawable`), `ui_hud_lock` (main-loop-mutex + BQL acquire and hold around the ImGui HUD build), `ui_frame_dt` (present-to-present interval) — plus the `ui_present` and `mfx_scaler_rebuild` event counters. UI buckets are normalized per *flip*, not per present |
| `XEMU_PFIFO_HEARTBEAT` | 2-second pfifo diagnostic snapshot |
| `XEMU_MMIO_PROF` | `1` prints an exit histogram of guest MMIO traffic (region × page, loads/stores) + BQL acquire-wait totals |
| `XEMU_MAX_QUERIES` | Occlusion-query pool size, default `4096` (the `begin_pre_draw` guard submits before exhaustion) |
| `XEMU_SPIRV_CACHE` | `=0` bypasses the on-disk SPIR-V cache entirely (no loads, no stores) — cold-compile A/B without deleting the cache dir. Landed on main 2026-09-08 with the rest of the cache hardening; note that blob validation is *not* gated by the hatch: every entry read from disk is checked (size ≥ 5 words and a multiple of 4, SPIR-V magic, an instruction-stream walk whose word counts land exactly on the last word, and a final `OpFunctionEnd`) and then parsed by SPIRV-Reflect before it can reach `vkCreateShaderModule`; a blob that fails any of those is deleted and recompiled. Measured 2026-09-08 over all 2,416,107 word-aligned truncations of the dev Mac's 394 warm entries: the in-tree rules reject 99.85%, SPIRV-Reflect returns an error for the remaining 0.15% (cuts landing on a function boundary), so none reached `vkCreateShaderModule` and none aborted the launch |
| `XEMU_INPUT_PIPE` | Test-input channel: lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` are OR'd into the keyboard-controller state, so injected keys flow through the user's normal bindings and work with the window unfocused (test automation). **POSIX:** the value is a FIFO path, opened `O_NONBLOCK` — unchanged. **Windows (transport landed 2026-09-08):** a value that starts `\\` is used as the pipe name verbatim (`\\.\pipe\<name>`), anything else contributes only its last `/`, `\` or `:` component, so one `/tmp/xemu.fifo` value works on both hosts; `\r` is a line delimiter too, so CRLF writers (PowerShell `Add-Content`, `cmd echo`) parse. Client: `scripts/win/inject_input.py` (POSIX: `scripts/inject_input.sh`) |
| `XEMU_MFX_REAL_DEPTH` | Feed real zeta depth to the temporal scaler (A/B): `2` = flip-time snapshot (temporally correct; any other non-empty value also selects it); unset/`0` = synthetic. Engages only when a zeta matches the scaler input dims |
| `XEMU_GUEST_PROF` | Apple Silicon only (other hosts print a one-time "ignored" line). One-run guest profiler: mach-thread sampler resolves vCPU samples to guest TBs vs host symbols. Measurement-run only (not benchmark-neutral). The per-exit-kind TB-lookup census was removed 2026-10-15 (revert the removal commit to restore it) |
| `XEMU_TB_PROF` | Prints TB jump-cache totals at exit (lookups, hit%, way-1 hits, htable walks, translations, tb_flush count), and under icount the step cuts, steps and step-cache hits |
| `XEMU_INV_PROF` | Prints SMC / TB-invalidation-churn counters at exit: invalidations by source (notdirty vs explicit vs single-TB), the false-invalidation share a byte-range overlap check would skip, inv-htable recycle hit-rate (false-sharing vs true SMC), and translate-time exit census; plus an `(f)` MMIO line — `io_prepare` calls, `cpu_io_recompile` calls, the recompiles/s rate over the measured window, and the icount io-retaddr cache's misses — which sized the `can_do_io` elision (normal-mode runs read ~0 by construction since the elision is unconditional there, so only icount or replay runs populate it; the `io_prepare` count is 2x-inflated by recompile re-execution) |
| `XEMU_INV_TIMING` | `1` adds a cycle-counter split of the `notdirty_write` body (invalidation+scan+recycle vs preamble+tail) to the `XEMU_INV_PROF` dump — sized the sub-page dirty-tracking arms. The tick source is per host (`accel/tcg/xemu-inv-prof.h`): `cntvct_el0` on aarch64 (Apple Silicon), `CLOCK_UPTIME_RAW` on Apple non-aarch64, `lfence` + `rdtsc` with a one-time ~10 ms ns/tick calibration against `g_get_monotonic_time()` on non-Apple x86 (MSYS2 Windows, x86_64 Linux — **added 2026-09-08**; the dump printed all zeros there before), and no source at all on any other host (still zeros). Timings are order-of-magnitude; counts are load-immune |
| `XEMU_SUBPAGE_REFUTE` | `1` runs the sub-page skip decision against a ground-truth live-TB byte-overlap scan (both pages of spanning TBs) and counts violations (design falsified if > 0) while the skip stays live. Soak: **0 violations over 28.6M filter-skips across 33 loadvm cycles** (~12 min) |
| `XEMU_XPAGE_PROF` | Cross-page-direct exit census: runtime taken-rate counter split by target region (kernel-identity vs low), the backstop's unlink walks (events, dests, cost), the jumps those walks reset split by `tb_add_jump`'s predicate (same-page, an emitter's jump onto its own page, cross-page), the link patches per second and the link flush queue (stale-slot exits and the patches drained there, at `cpu_exec` returns and in full queues); also armed by `XEMU_INV_PROF`. Dark; atexit summary |
| `XEMU_XPAGE_REFUTE` | `1` = cross-page-chaining refuter: routes every cross-page-direct exit through the safe lookup path plus a page-table-walk check that the target's live phys is unchanged since last seen (loud + counted on violation). Slow; falsification only |
| `XEMU_SSE_HOST` | Host fast path for single-precision SSE arithmetic. Always on for aarch64 with `perf.hard_fpu` (+1.90 fps — see CPU / JIT changes). x86_64 hosts: `=1` opts in, dark until `=2` runs clean on real silicon. `=2`, on any host, runs both paths with strict brackets and aborts on divergence; it also keeps packed add/sub/mul/div in the helpers instead of the inline vector ops. The `XEMU_SSE_NEON` alias was removed on 2026-10-15 and is ignored on every host, so an `XEMU_SSE_NEON=2` run is not a differential receipt |
| `XEMU_X87_CENSUS` | `1` arms the x87/SSE Gate-0 census (audit §1.4) and its atexit dump: per-instruction FIP/FDP store pairs vs deferred updates emitted, x87 runs + insns (mean run length), and SSE scalar compares split `ss`/`sd`. Emits counter RMWs into generated code — measurement mode only, never for an fps run |
| `XEMU_X87_REFUTE` | `1` runs the adversarial shadow-checker for the x87 exception-pointer deferral: keeps every legacy store, computes the deferral anyway, and counts every runtime point where the deferred state could have been observed (deferred FIP/FCS/FDP/FDS ≠ what the legacy stores left at a consumer or helper boundary). Implies the census dump. Promotion gate is zero violations. It also runs the write-through x87 path for real with every pending or stale register image poisoned (`wt-*`, `wt-stack-*`), and checks TOP-keyed translation (`top-key-*`) and MMX entry (`mmx-*`); the gate is every `*violations=` field at 0. Its checks emit ~1 KB of host code per guest insn, so a clean run covers TBs up to ~260 guest bytes (production TBs reach ~800). **Do not combine with `XEMU_SUBPAGE_FAST_REFUTE` *and* `XEMU_DSP_JIT_DIFF` in one run** — the 3-way combination deadlocks the guest during boot (Failed table) |
| `XEMU_SUBPAGE_FAST` | Sub-page arm (b): the aarch64 store slow-path stub completes a store inline when the slow path is a provable no-op (mismatch exactly `TLB_NOTDIRTY`, non-code 64 B sub-block, all NOCODE dirty clients already set). **On since 2026-07-18, and always on in Apple builds** (the `=0` off switch was removed on 2026-10-15; on non-Apple aarch64, where it is unvalidated, `=1` opts in): quiet-machine receipts measured ~2M inline completions/s on the F8 heavy scene — the eligible population is dominated by renderer-watched pages, ~20x the code-page trap count the +0.3-0.7 fps prediction was sized on — for **+12% draws/s throughput (6/6 interleaved pairs, two independent 3-pair batches)**; fps stays ~flat because Azurik's effect-load feedback re-saturates frame time at ~815 vs ~727 draws/flip. Refuter: ~19.5M validated decisions, 0 violations (static + loadvm-cycling + first-visit-streaming soaks). **Until 2026-10-15 the stub read the wrong bitmaps**: it indexed the dirty bitmaps and its code-block mirror from ram_addr 0, but the PC machine's unused `pc.ram` backend occupies ram_addr 0-128 MiB and `xbox.ram` starts at 0x8000000. It saw `pc.ram`'s always-set bits and an empty code map, so it completed every pure-NOTDIRTY store inline, including stores into translated code (no TB invalidation) and onto renderer-watched pages (no NV2A/VGA mark), and the refuter made the same offset mistake. The receipts above came from that build. Fixed by rebasing both to `xbox.ram`'s offset: heavy-f1 offscreen, 40 s, now skips 78% of 3.3M stub entries, demoting 87 code-block stores and 709k dirty-clean stores (it was 100% of 13.0M), and the refuter reports 0 violations on 2.46M decisions. Cold-stub only; the tag-match fast path is byte-identical. Atexit dump: seen/skips/demote-reason histogram, but only under `XEMU_SUBPAGE_FAST_STATS=1` (counters default-OFF since 2026-08-04). The prefilter is emitted only by the aarch64 TCG backend, so on any other host (x86_64 Windows/Linux) this knob and its refuter are inert — since 2026-09-08 an explicit `=1` says so once on stderr from the `XEMU_SUBPAGE_REFUTE` latch in `accel/tcg/tb-maint.c` |
| `XEMU_SUBPAGE_FAST_REFUTE` | `1` routes every would-skip decision to a C validator (independent `probe_access` re-derivation, live-TB overlap scan, NOCODE dirty ground truth) that counts violations and performs the real store — falsification mode, not perf. Forces `XEMU_SUBPAGE_FAST_STATS=1` so soaks keep their population histogram |
| `XEMU_SUBPAGE_FAST_STATS` | `1` re-enables the arm-(b) prefilter's six diagnostic counter RMWs (`xemu_sf_seen`, `xemu_sf_skips`, and the four `xemu_sf_demote` reasons) that the aarch64 store slow-path stub emits inline. **Default OFF since 2026-08-04**: the counters are baked into generated code at translate time and the stub completes ~2M stores/s, so production now emits ~39 fewer instructions per store stub (12 of them on the inline-skip path itself, the four demote landing pads collapsing into direct branches) and the atexit dump prints `subpage-fast counters disabled (set XEMU_SUBPAGE_FAST_STATS=1)` in place of the histogram — so a zeroed histogram can't be mistaken for a dead mechanism. Latched once at first stub emission, before the first TB (a mid-run flip would leave TBs disagreeing). Diagnostics only: the decision logic and the refuter oracle are identical either way, which is what makes this knob its own A/B lever. Measured cost of the counters: **−0.32% draws/s / −0.57 fps, 3/3 pairs unanimous** |
| `XEMU_TLB_WCLEAN_REFUTE` | `1` re-runs the full TLB scan after every `tlb_reset_dirty` bitmap pass, arms whatever the writable-clean bitmaps missed (so the run stays correct) and prints `xemu: tlb-wclean refute checks= entries= misses= unset= (calls= arming=)` at exit: `misses` counts eligible entries in the range left unarmed, `unset` eligible entries anywhere missing a bit. The gate is `misses=0 unset=0`. Diagnostic only (a full scan per dirty clear); never for an fps run |
| `XEMU_UI_FRAME_CAP_NS` | Test-only: floor the UI present period in ns (`16666666` ≈ a 60 Hz consumer). Unfocused/occluded bench windows present unpaced (~200 Hz observed), so ring-pacing behavior is invisible headless without it. Zero cost unset |
| `XEMU_PUSH_PRESENT_REFUTE` | Debug: peek the ring (non-consuming) and cross-check the consumed-step stream vs the pull path — monotonic `frame_seq`, no skipped-then-resurrected step, and identical texture/event/dims where comparable (interp steps compare event object + dims; the pixel-equivalent interp outputs live in distinct allocations). Prints `peeked`/`compared`/`mismatches`/`resurrections` every ~5 s. Re-adds the pull round trip — measure perf with it off |
| `XEMU_UI_LOCK_STATS` | `1` prints the once-per-second UI main-loop-mutex + BQL occupancy line (`[[ vblank @NHz avg - bql Nns/iter, N% time avg ]]`) in a normal build; previously it needed a `DEBUG_XEMU_C=1` build (which still forces it on). One predictable branch per lock when off. Compiled in on macOS and in `DEBUG_XEMU_C` builds only — Windows/Linux release paths preprocess away to today's code |
| `XEMU_DSP_JIT_STATS` / `XEMU_DSP_JIT_DIFF=N` | DSP JIT counters / bit-exact validation |
| `XEMU_DSP_JIT_DIFF_SYNC` / `_DIFF_MAX` / `_PIN_AUDIT` | JIT bring-up harnesses (see `docs/Finished/dsp-jit-design.md`) |
| `XEMU_APU_PROF` | Per-second APU-thread utilization to stderr (deterministic mode measures it only when this is set) |

### Build-time and harness knobs

Not runtime env vars — these are read by `build.sh` at configure time or
by the bench harness, and are catalogued here because each belongs to a
Changes bullet below. The README's `### Build knobs` table
carries the user-facing subset; `build.sh` itself is the full list of
build knobs.

| Env var | Purpose |
|---|---|
| `XEMU_WIN_O3` | **Added 2026-09-08. Default off.** `=1` opts the *native* MSYS2/MinGW release into `-Doptimization=3 -Dstack_protector=disabled` (`build.sh:634-644`), the pair the Darwin/Linux arm already uses. Unset, both Windows arms keep meson's project default `optimization=2` (`meson.build:3`) and the probed `-fstack-protector-strong` — exactly what every CI Windows leg and every previous native build shipped. **The polarity is deliberate**: the 2026-09-08 review names this flip as the differential behind the wave-1 exit-139 on the Windows box (it reaches every translation unit of both renderers with every runtime hatch unset), and no CI leg builds it. `-Dstack_protector=disabled` is not cosmetic on mingw: `meson.build:512-536` probes by compiling *and linking* a test program with `-fstack-protector-strong`, which a stock MSYS2 MINGW64 toolchain passes, so the option really does strip the canary from the whole tree. Ignored (with a message) on `win64-cross`, which builds the release artifacts |
| `XEMU_PGO_STALE_FATAL` | `0` (warn only). With `XEMU_PGO=use`, `1` turns a PGO CFG-hash mismatch on a hot-path function name (`cpu_exec*` / `helper_*` / `tlb_*` / `tcg_*` / `pgraph_*`) into a build failure instead of a loud warning (`build.sh:303-306`). Deliberately NOT enabled in CI: flip it on once the gate is proven quiet on a freshly-retrained tree |
| `XEMU_BENCH_ISOLATE_CACHES` | `0`: shader/pipeline caches live in the bench cache every bench shares (`XEMU_BASE_PATH` = `$XEMU_BENCH_CACHE`, since 2026-10-15), warm across benches and never the owner's pref dir. `1` — equivalently `--isolate-caches` — plants the scratch config as a portable-mode marker inside the cloned bundle's `Contents/Resources`, moving xemu's data base path (`spirv_cache_v*/`, `pipeline_cache.bin`) into the disposable work dir, so caches start cold at batch start, warm during the warmup run, and are shared identically by both arms; a binary older than `XEMU_BASE_PATH` gets this automatically (`scripts/bench-savestate-ab.sh:108,215`). Recorded as `cache_isolation` in `meta.json` / `receipt.json` |
| `XEMU_BENCH_CACHE` | Bench cache folder that every macOS bench and probe launcher passes as `XEMU_BASE_PATH` (default `~/Library/Caches/xemu-fork-bench`; `scripts/xemu_lease.py`, `scripts/xemu-lease.zsh`). Cold the first time, then shared by every bench. The Windows bench (`bench-savestate-ab-win.py`) passes it too, default `%LOCALAPPDATA%\xemu-fork-bench` |
| `XEMU_TEST_CONFIG` | Unset = `../fixtures/xemu.toml` beside the checkout. The source config every macOS harness clones its HDD, EEPROM and scratch config from (`bench-det.py`, `bench-savestate-ab.sh`, `bench-abx.sh`, `boot-smoke-corpus.sh`, `probe-run.sh`, `soak-cycles.sh`, `movement-probe.sh`; `bench-det.py` read a fixture folder from `XEMU_FIXTURES` until 2026-10-16); `--config-src` still wins where a script takes it. There is no fallback to the personal `xemu.toml`, so personal play cannot delete a test savestate. Added 2026-10-10. The Windows bench's config template is `--config-template`, else this, else `%APPDATA%`'s `xemu.toml` (only ever copied) |


## Changes

> **2026-08-04 optimization wave.** The bullets dated 2026-08-04 below
> implement [`fork-optimization-audit-2026-08.md`](Finished/fork-optimization-audit-2026-08.md)
> (that page holds each candidate's evidence, prediction and kill
> threshold; this one holds what happened). Headline receipt for the
> wave as a whole, interleaved cross-binary A/B on the F8 heavy anchor:
> **38.12 ± 0.59 → ~48.1 fps (+10.0 fps mean, +26% fps, +20.4%
> draws/s), 5 clean pairs, unanimous** (deltas +10.64/+9.86/+8.36/
> +10.93/+10.21; a sixth pair — pair 2 of the batch — was excluded
> because its E-run hit the known `ohci`/`flatview` crash class below,
> and the receipt records the exclusion and its reason). The mean of the
> five deltas is exactly the +10.0 headline. The prewave anchor of 38.1
> lands inside the 36-39 band the
> audit was written against, so the fixture did not drift — the wave
> moved the anchor. Per-mechanism receipts are on the individual
> bullets; note they are sub-additive against +10.0, which is ordinary
> frame-time composition.
>
> **Known issue carried by this wave's release notes (fixed
> 2026-10-11):** a rare post-`loadvm` SIGSEGV in the `ohci`/`flatview`
> path, 2 sightings across ~40 wave launches against 0 across the
> prewave stability control (4 launches, 21 loadvms, 31.5 min dwell).
> It was recorded here as upstream-class QEMU and unattributed. It was
> neither a QEMU bug nor a wave mechanism: it was a use-after-free in
> xemu's NV2A memory-access callback list, which the main loop walks on
> every OHCI DMA read. The fix and its evidence are under "Threads +
> runtime"; the misdiagnosis history is archaeology 7.6.

### Windows native port (2026-09-07)

First run of the fork on real Windows hardware (Windows 10, i7-4770K,
NVIDIA TITAN Xp 561.09, MSYS2 MINGW64 gcc 16). Full recipe, traps,
results and the open work list: `docs/Campaigns/windows-port-2026-09.md`; the
ranked static audit (60 confirmed / 19 plausible / 10 refuted findings,
22-batch plan): `docs/Campaigns/windows-audit-2026-09-summary.md` +
`docs/Campaigns/windows-audit-2026-09.json`; the review that corrected this
section: `docs/Finished/windows-wave1-review-2026-09-08.md`. macOS behaviour is
unchanged by every item below unless stated — written before any of it
had been built on macOS, and confirmed on 2026-09-08 (arm64 release
build clean, `meson test --suite xbox` 6/6, CI 21/21 incl. both
win64-cross, in-game F5 smoke with 0 artifacts and 0 asserts;
`docs/Campaigns/windows-port-2026-09.md` §8).

- **Upstream merge `2e0aad3e18`** (xemu master `429c9972eb`): DXGI /
  WGL_NV_DX_interop presenter, PTIMER alarm IRQs (nv2a vmstate v4),
  controllerdb from config path, glslang 16.5.0, and upstream flipping
  `audio.use_dsp_jit` to **false** (`fc13b78060`, "for first release").
  Only `ui/xemu.c` conflicted; DXGI hooks sit inside the fork's
  non-Metal GL branch.
- **DSP engine default restored to the fork's (2026-09-08).** Taking
  upstream's `use_dsp_jit` flip moved *every* platform's stock config
  onto the plain C interpreter, macOS included — the merge body and
  three docs claimed "non-Apple hosts only", which was false: with
  `audio.dsp_jit.enabled` also defaulting `false`, nothing selected a
  JIT engine anywhere. The fork owns its defaults, so
  `config_spec.yml` is back to `use_dsp_jit: default: true` and
  pre-merge behaviour holds on every host: a stock config runs
  upstream's dsp56300 engine, and `[audio.dsp_jit] enabled = true`
  selects the fork's inline ARM64 JIT instead. Precedence in
  `dsp_want_external_jit_engine()` (`hw/xbox/mcpx/apu/dsp/dsp.c:120`)
  is unchanged. Owner decision (a) of the 2026-09-08 review; the dev
  box never showed the regression because its `xemu.toml` sets
  `[audio.dsp_jit] enabled = true`.
- **Native MSYS2 configure fixes (`ac28e5a5bd`).** Python ≥ 3.13's
  `ntpath.isabs` rejects QEMU's `/qemu` prefix → `build.sh` passes
  `--prefix=$(cygpath -m /qemu)`; MSYS tar reads `C:/x` as host:path →
  `--force-local` for the dsp56300 prebuilt on Windows. Both
  Windows-branch only.
- **DXGI presenter actually activates on NVIDIA (`2182baec46`).** The
  interop FBO was completeness-checked before the WGL_NV_DX_interop
  object was locked → `GL_FRAMEBUFFER_UNSUPPORTED`, silent fallback to
  `SDL_GL_SwapWindow`. Lock around the check, 1:1 lock/unlock per frame.
  Now "mode=flip, tearing=supported"; output verified upright. Windows
  file only; candidate for upstream.
- **First real-ICD result:** the Vulkan renderer renders Azurik
  correctly on native NVIDIA (the 2026-08-09 native-driver fixes hold);
  intro ≈ 30 flips/s, title area 21–25 flips/s (below the 30 fps cap →
  host headroom to chase). Khronos validation layer on the menu scene
  reports only two VUIDs: `VUID-VkDeviceCreateInfo-enabledLayerCount-12384`
  (device-level layer list, legacy) and
  `VUID-VkShaderModuleCreateInfo-pCode-08740` (SPIR-V declares
  `DemoteToHelperInvocation` without `shaderDemoteToHelperInvocation`
  enabled) — both open, see the handoff doc. Under the layer xemu hung at
  exit once (process stuck terminating in the driver); not reproduced
  without the layer.
- **Wave 1 of the audit plan — landed on main one batch at a time from
  2026-09-08, except item (3).** A contents item below is on main only
  where it says so. Item (3) is the PVIDEO/display fix `285779ce83`,
  which the 2026-09-08 review held. The `windows-wave1-wip` branch was
  retired on 2026-10-10; its tip, including `285779ce83` and every other
  wave-1 commit cited in these docs, is kept as tag
  `archive/windows-wave1-wip`.
  The combined wave-1 binary segfaulted ~10 s into Azurik on
  Windows; the crash is INTERMITTENT and NOT isolated: a same-day
  single-run bisect wrongly blamed the vp.c pitch clamp; the same binary
  later failed 5/5 (both renderers, profiler on/off) while the
  main-branch binary passed as a control. The 2026-09-08 review's
  verdict is that the differential is the **build flags**, not a
  source hunk: `fb42bce22e` flipped the native MSYS2 release arm from
  `-O2` to `-O3 -Dstack_protector=disabled` (the re-cut that landed on
  main 2026-09-08 keeps that flip **opt-in** behind `XEMU_WIN_O3=1`, so
  main's native default is still `-O2` with the probed stack
  protector), every crashing binary was
  produced by an incremental `ninja` rebuild that keeps those
  configure-time flags, and the only passing binary (the control) was
  configured by main's `build.sh` at `-O2`; the runtime hatches that
  were toggled change no codegen. Five of six independent crash
  hunters converged on it and none was refuted — but this is *verified
  by review 2026-09-08, not yet confirmed on the box*, and it names a
  class (latent UB exposed by the vectoriser), not a line. The
  apu/xid commit is **not** the leading suspect: it was exonerated on
  four independent grounds. The full protocol for the next Windows
  session — freeze the control, WER LocalDumps, full-reconfigure `-O2`
  vs `-O3` with ≥5 runs per variant — is in
  `docs/Finished/windows-wave1-review-2026-09-08.md` §3 and
  `docs/Campaigns/windows-port-2026-09.md` §9. Contents: (1) **landed on main
  2026-09-08** (with the transport half of (8) — see the bullet below):
  `XEMU_WIN32_DXGI` hatch (removed 2026-10-16) + the two non-Apple
  compile warnings in `ui/`
  (present header included unconditionally, `g_framebuffer_rect_shader`
  gated). (2) SPIR-V disk
  cache — **landed on main 2026-09-08** (`818fe27828` plus the review's
  fixes): `qemu_fopen` (UTF-16 paths on Windows), atomic temp+rename
  writes, a `-dbg` variant tag when `debug_shaders` is on, and blob
  validation with regenerate-on-miss instead of an abort. The branch did
  not actually deliver that last property — it leaned on SPIRV-Reflect
  to reject a truncated blob, but the parser's bounds check is
  `assert(InRange(...))` in `ReadU32()`/`ReadStr()` and the subproject
  compiles with asserts live, so a word-aligned truncated `.spv` could
  abort every launch and was never deleted. What landed makes
  `spirv_blob_is_valid()` self-sufficient before the parser sees the
  blob: it walks the instruction stream *and* requires the last
  instruction to be `OpFunctionEnd`. The walk alone was not enough
  either — measured, about a fifth of word-aligned truncations land on
  an instruction boundary and walk clean, and SPIRV-Reflect then crashed
  on some of them and accepted others straight through to the driver;
  the trailer rule cuts that residue to function-boundary cuts, for
  which the parser reliably returns an error. It also sweeps `.tmp`
  files orphaned by a crash at cache open.
  (3) PVIDEO/display: the staging reclaim that could call
  `pgraph_vk_finish()` while the display aux command buffer was open is
  hoisted ahead of the aux CB begin; overlay uploads no longer skip on a
  register-only key; the display early-out also honours
  resolution/PVIDEO changes on every host (the commit body calls that
  hunk non-Apple, but `display_skip_strict()` is called unconditionally,
  macOS included). (4) APU/XID — **both halves landed on main 2026-09-08** (the
  XID index guard in `hw/xbox/xid.c` / `hw/xbox/xid-gamepad.c` as its
  own commit — see Input below). Guest voice handles
  are bounds-checked before indexing host `filters[]` / HRTF / SSL
  tables (was an OOB read then write); the voice-list walk stops on an
  out-of-range `NEXT_VOICE_HANDLE` instead of asserting, and reports it
  once per run on stderr rather than through a release-stripped
  `DPRINTF`; an out-of-range handle is now dropped outright instead of
  falling through to the guest-RAM path, because that path's
  `address_space_*` fallback can take the BQL from the APU/voice-worker
  threads while `d->lock` is held — the inverse of `gp_write`'s
  BQL-then-`d->lock` order, i.e. a hang (for an in-range handle with a
  guest-bogus `VPVADDR` the fallback stays reachable, as it already is
  on main). APU direct RAM stores mark dirty again **off Apple**; Apple
  builds keep the unmarked store (the per-store cost was never
  measured; a build-time choice since 2026-10-15). The runtime
  `use_dsp_jit` engine switch is serialised under the BQL and the APU
  frame thread registers with RCU. The `int16` pitch-LUT clamp the
  original commit added was **not** taken: the branch removed it again
  in `3495cd28c0`, and `40f0a01ad3` retracted the single-run bisect
  that had blamed it, so nothing is known about it either way — the
  original wrap stands.
  (5) **Landed on main 2026-09-08** — see the Vulkan
  renderer and CPU / JIT sections below: `nv2a_vk_bounds_check` is a
  real check in every build (was `__builtin_unreachable()` in release)
  — an ungated behaviour change on **every** platform, macOS included,
  and so an exception to this section's "macOS behaviour is unchanged
  unless stated"; `volkLoadDevice()` on non-Apple;
  `VK_KHR_external_semaphore_win32` optional; LRU eviction in-use filter
  restored (it matches upstream's `lru_try_evict_one` exactly, and is a
  no-op while the fork's split-list invariant holds — the 2026-09-08
  review exonerated it as the Windows crash cause on that ground);
  `CPUJumpCache` stride static-asserted. (6)
  Diagnostics on Windows/GL — **landed on main 2026-09-08**
  (`eb971f860d`; full record under Testing & tooling): `nsprof` ticks
  under the OpenGL renderer too, `xemu_inv_ticks()` has an x86 `rdtsc`
  arm, `stderr` is flushed after heartbeat/nsprof prints on Windows,
  `NV2A_STRIP_PROFILE_COUNTERS` tracks the debug build, and
  `XEMU_SUBPAGE_FAST=1` warns once on a host with no emitter. The
  macOS arm64 release path is unchanged. (7) build.sh — **landed on main
  2026-09-08, re-cut** (see Build + packaging): GCC PGO arm
  (`-fprofile-generate`/`-fprofile-use`, `.gcda` discovery) with the
  `cygpath -m` profile-dir fix the branch version lacked, and
  `XEMU_HARDENING` reachable on all arms (knob removed 2026-10-10).
  The `-O3` hunk was **inverted** before landing — the branch made
  `-Doptimization=3
  -Dstack_protector=disabled` the native-Windows default, main makes it
  opt-in behind `XEMU_WIN_O3=1`, because that flip is the review's
  crash differential. The tar `--force-local` rekey landed separately in
  `eeb992dcd2`. (8) Windows
  benchmark harness `scripts/bench-savestate-ab-win.py` + `scripts/win/`
  (TCP monitor, PowerShell capture, receipts) — **landed on main
  `add066354f`** as tooling-only, and main has since moved ahead of the
  branch there (`c77d8a4b05` hardening, `4d0cec2790` boot smoke) — and the
  `XEMU_INPUT_PIPE` Windows named-pipe transport in `ui/xemu-input.c`,
  **landed on main 2026-09-08** with (1). The harness itself has still
  never driven a full run on the Windows box.

- **Windows test-input transport, plus a DXGI present hatch
  (2026-09-08).** `XEMU_INPUT_PIPE` was a compiled-out stub on `_WIN32`
  (POSIX FIFOs and `O_NONBLOCK` reads do not exist there), so no harness
  that drives the guest — movement probe, boot smoke, live-route
  benchmark — had a Windows transport, even though its client
  `scripts/win/inject_input.py` already shipped. The same line protocol
  now rides a Win32 named pipe polled from the same call site
  (`xemu_input_update_sdl_kbd_controller_state()`): a value starting `\\`
  names the pipe verbatim, any other value contributes only its last path
  component (so one `/tmp/xemu.fifo` value works on both hosts), and `\r`
  is a delimiter too so CRLF writers parse. The single `PIPE_NOWAIT`
  instance is re-armed with `ConnectNamedPipe` in the *same* poll as its
  `DisconnectNamedPipe`, because a disconnected instance is not listening
  and a one-shot writer that arrives before the next poll would otherwise
  be turned away with `ERROR_PIPE_BUSY`. Unset = nothing is created and
  nothing is polled.
  Landed alongside it: `XEMU_WIN32_DXGI=0` restored `SDL_GL_SwapWindow`
  presentation so a DXGI-specific regression stayed bisectable (removed
  2026-10-16 under the no-hatch rule; upstream's own fallback to
  `SDL_GL_SwapWindow` when the presenter can't start remains), and the
  two non-Apple compile warnings are gone — `ui/xemu-present.h` is included unconditionally in
  `ui/xemu.c`, and the Apple-only `g_framebuffer_rect_shader` is now
  declared inside `#ifdef __APPLE__` like its two uses. Windows-only
  behaviour change: the POSIX branch of `test_input_poll()` is
  byte-identical, the hatch lives inside `#ifdef _WIN32`, and both
  warning fixes are declaration placement only.

### Rendering (correctness fixes)

- **Windows / native-Vulkan black screen fixed (2026-08-09).** On a
  native Vulkan driver (Windows/Linux ICD), the renderer booted to a
  black screen with working audio while stock upstream rendered — a
  fork regression that MoltenVK's leniency hid. Root cause: the set-0
  uniform-buffer descriptors were written as one coalesced
  `descriptorCount=2` `VkWriteDescriptorSet` spanning binding 0
  (`VSH_UBO`, VERTEX stage) and binding 1 (`PSH_UBO`, FRAGMENT stage),
  which violates the Vulkan consecutive-binding rule that spanned
  bindings share `stageFlags`
  (`VUID-VkWriteDescriptorSet-descriptorCount-10776` /
  `-dstArrayElement-00321`). MoltenVK tolerates it; a native driver may
  leave the fragment UBO unwritten, so every fragment resolves to black
  on every draw. Fixed by writing the two UBOs as two
  `descriptorCount=1` writes (upstream's form; result-identical on
  MoltenVK). Runtime-confirmed with the Khronos validation layer (the
  coalesced write trips both VUIDs, the split write is clean). Secondary
  fix: the display descriptor "skip if unchanged" cache keyed on a raw
  `SurfaceBinding*` never cleared on eviction, so after heap-address
  reuse a destroyed `VkImageView` stayed bound (black on native,
  ARC-retained/tolerated on MoltenVK) — the skip is now gated to Apple
  only. No escape hatch: these remove spec violations rather than add an
  optimization, and macOS behavior is unchanged. There is no
  interleaved A/B because this is a correctness fix, not a perf change.

- **`shaderTessellationAndGeometryPointSize` no longer required on
  non-Apple (2026-08-09).** It only governs writing `gl_PointSize` from
  geometry/tessellation shaders (Apple already runs without it). It is
  still enabled when the driver reports it available — no fidelity
  change on capable GPUs — but its absence no longer hard-aborts device
  init on weak or paravirtual Windows Vulkan ICDs. The geometry shader now
  omits the `gl_PointSize` passthrough (and thus the `GeometryPointSize`
  SPIR-V capability) when the device lacks the feature, so GS pipeline
  creation doesn't fail its capability check afterward. GL and
  feature-capable Vulkan devices are unchanged.

- **Dynamic-state reuse across command buffers fixed on native drivers
  (2026-08-09).** `begin_draw`'s dedup of line width / depth bias / blend
  constants could skip `vkCmdSet*` against a value cached in a *previous*
  command buffer, leaving that CB-scoped dynamic state undefined
  (VUID-vkCmdDraw-None-07833/07834/07835) — z-fighting / wrong blend on
  native drivers; MoltenVK retains dynamic state across CBs and hid it.
  The conditional caches are now poisoned at command-buffer begin.

- **Texture-upload `bufferOffset` aligned to texel block size
  (2026-08-09).** The staging bump allocator advanced by unpadded decoded
  sizes, leaving `VkBufferImageCopy.bufferOffset` misaligned
  (VUID-VkBufferImageCopy-bufferOffset-00193) → texture corruption on
  native drivers (MoltenVK accepts any byte offset). Offsets are rounded
  up to 16.

### CPU / numerics (host-architecture correctness)

- **Inline x87 FPU host-correctness fixes (2026-08-09).** The default-on
  inline hard FPU, validated only on aarch64 hosts, had two non-aarch64
  defects. (1) Windows ARM64 emitted inline FP ops despite its backend
  advertising `TCG_TARGET_HAS_fpu=0`, tripping the codegen assert (debug)
  / UB (release); `g_use_hard_fpu_inline` is now gated on that macro so
  such hosts use the helper-based hard FPU. (2) On x86_64 hosts,
  `gen_flcr` programs MXCSR but not the x87 control word, so `FIST`/`FISTP`
  under round-toward-±∞ and `FRNDINT` under any non-nearest mode silently
  rounded to nearest; those cases now fall back to the softfloat helper
  (nearest/truncate stay inline; aarch64 unchanged). The gate in (1) in
  fact turned the cache off on every host; its fork layers were removed
  on 2026-10-15.

- **Sub-page dirty-tracking default gated to Apple (2026-08-09).** Its
  default was on unconditionally, contradicting the feature's own
  documented "Apple 1, elsewhere 0" and running unvalidated default-on on
  non-Apple aarch64. Gated to `__APPLE__`; `XEMU_SUBPAGE_FAST=1` opts in
  elsewhere. (x86_64 unaffected — the i386 backend emits no stub.)

### CPU / JIT (ARM64)

- **Sub-page code dirty tracking (default on, 2026-07-11).** A
  per-`PageDesc` 64-block bitmap lets a guest data store that misses
  every code sub-block skip the Xbox whole-page TB invalidation — an
  O(1) bitmap test replaces the qht/jmp-unlink/recycle round-trip
  (INV_PROF: 100% of invalidations were data-write false sharing, 0
  true SMC; invalidations collapse 290,595 → 11,180/s on F8).
  Refuter-first landing: 0 violations over 60M+ ground-truth-checked
  skips across 45 reload cycles. Interleaved A/B (quiet, scene-gated):
  **F8 30.35 → 34.68 fps (+4.33±1.53, 4/4 pairs), F5 47.33 → 58.96
  (+11.63±0.52, 3/3 pairs)** — the largest single TCG win since the
  occlusion rework.
- **Cross-page direct block chaining, Xbox-relaxed (default on,
  2026-07-12).** Direct jumps whose target lies on another page now
  patch a real `goto_tb` chain instead of paying the ~20-op inline
  jump-cache probe (the "other" 43% of the `XEMU_GUEST_PROF` exit-kind
  census, removed 2026-10-15). Upstream
  forbids this because mappings can change under a chain; the fork
  makes it sound by construction with three backstops — INVLPG
  invalidates the flushed code page; CR3 writes and every remaining
  full-TLB-flush class (generic `tlb_flush_by_mmuidx` hook) sever the
  registered cross-page chains. Refuter (`XEMU_XPAGE_REFUTE`:
  page-table-walk, closing the stale-chain fetch-bypass hole): 0
  violations over 3.15B+1.71B checks across 20 reload cycles.
  Interleaved A/B: **+1.30±0.29 fps F8 (7/7 pairs) pre-subpage;
  +0.77±0.55 (3/3) on top of sub-page tracking**.
  **Backstop v2 (v0.11.1, the "new-area lag" fix).** v0.11's full-flush
  backstop severed chains with a queued **whole translation-cache
  flush**; guest level streaming turned out to fire full-TLB-flush
  classes continuously (CR3 reloads included, contrary to the
  single-address-space assumption), so first visits to new areas paid
  a retranslation storm — measured **139 tb_flushes in 20 s** of
  first-visit walking on F8 with an fps trough to **14.6 vs a 38.6
  settled baseline**, exactly the reported map-transition/death/fast-
  travel lag (revisits: 0 flushes, no lag). Now every cross-page link
  registers its destination TB at link time — emission-driven: the
  translator flags TBs that emit relaxed cross-page slots, and every
  dest they link registers (a phys-only test would miss virtual-alias
  targets sharing the source's phys page), with a phys-mismatch
  fallback as belt-and-braces — and the backstop synchronously unlinks
  exactly those chains (`tb_jmp_unlink`), keeping every translation;
  chains relink lazily. Registry overflow (128k dests) falls back to
  the v0.11 queued full flush. A stream-e2 walk severs ~300 (det) to
  ~390 (normal mode) dests and resets ~640 to ~840 jumps on average, up
  to ~3k (`XEMU_INV_PROF` plus a temporary jump counter, 2026-10-15).
  Each reset's own 4-byte `sys_icache_invalidate` (DSB, IC IVAU, DSB,
  ISB; ~350 ns on the M2 Ultra) was ~80% of the walk; since 2026-10-15
  the walk queues them (`flush_idcache_defer_begin`) and runs one
  sequence before returning. Measured on the misc lane (22329bd8bc plus
  the MMX commits, before the pinned registers and TLB memo):
  normal-mode stream-e2 (1800 frames, 2 runs per arm, load 22-25): walk
  mean 449 → 98 µs, p99 1.2 → 0.3 ms, max 1.4 → 0.4 ms, walks over 1 ms
  56-95 → 0. Det stream-e2 (3000 frames, 19 pairs, load 14-38): walk
  mean 285 → 60 µs, Mcycles/frame −1.5% paired median (14/19),
  Minsns/frame flat. What is left, ~100 ns per reset in both modes, is
  the list walk and the batched flush. Severing only the cross-page
  jumps would not shrink it: a normal-mode stream-e2 walk resets 5.9%
  same-page jumps and 5.2% from an emitter onto its own page
  (`XEMU_XPAGE_PROF`, Settled vectors).
- **Link patches queue their icache flush (2026-10-16).** Every
  `tb_add_jump` that wrote a B into a reset slot paid its own 4-byte
  `sys_icache_invalidate` (~430 ns): 272k per 600 det stream-e2 frames
  (118 ms, 2.7% of the vCPU; xic interposer) and ~800k per 32 s
  normal-mode stream-e2 walk (340 ms, 100 ms of it in the 2.5 s burst
  ~13 s after the load), because each backstop walk resets ~800 jumps
  that relink at once. A B replacing another B is a permitted
  concurrent modification and the old one (the reset B) exits to the
  dispatcher, so the aarch64 backend queues a link's flush on the
  thread's link queue (`flush_idcache_link`). The queue runs as one
  DSB / IC IVAU / DSB / ISB sequence when 64 are queued, when
  `tb_add_jump` finds the slot it would link already linked to that TB
  (the TB left through a stale line), at every `cpu_exec` return and
  before `tb_flush` reuses the buffer. Resets, LDR-form links and new
  code flush at once. Det stream-e2: 20.6k stale exits per 600 frames,
  each draining ~11 patches. The link count is unchanged (relinks are
  as many as before; only their flushes went). Cross-binary det against
  cced50204c, three sessions pooled (load 9-45; negative is fewer):
  Mcycles/frame stream-e2 median −1.4% (15/21 pairs), cold boot −0.75%
  (9/14), heavy-f1 +0.1% (6/14, few relinks); Minsns flat. Harness
  (`candlen.py`, 4 rounds per arm, load 15-38): fitted fixed cost
  37.2 → 32.9 Mcyc per candidate restore-then-advance (−12%) and 49.8 →
  39.5 per `xemu-run` (−21%); 1-4 frame candidates −2..−5%. Normal mode,
  16 walks per arm (load 14-33): flush calls in the window 801k → 37k
  (translation only), burst frames over 25 ms per walk median 4.5 →
  3.0, over 33 ms 13 → 11 in all (P(fewer) 0.56: no change resolved).
  `XEMU_XPAGE_REFUTE`: 140M checks, 0 violations. A 1000-restore loop
  digests as the base binary does.
- **Inline hard FPU (retired).** The inline x87 cache (native AArch64
  FP with values held in D-regs across instructions; inline `fucomi`,
  `frndint`, the FPU constant loads, 16-bit `FIST` and `FISTTP`; "~30%
  faster than the helper-call path") has not run since `3eb1b2de10`:
  its `TCG_TARGET_HAS_fpu` gate is private to tcg/. Its fork layers were
  removed on 2026-10-15; upstream's skeleton stays behind
  `g_use_hard_fpu_inline = 0`. Still live on the helper path, in both
  modes: `fnstsw`, the register moves, `fchs`/`fabs`, `fld1`/`fldz` and
  `ffree`/`ffreep` as TCG code (`613670d8eb`), and `fdecstp`/`fincstp`.
  The exact write-through path (`4d2466d106`, +5.6% det heavy-f1)
  replaced the cache for the hot loads, stores and arithmetic on
  aarch64: `floatx80`↔`double` inline through the backend's
  `ld80f`/`st80f`. Each result's `st80f` waits for the next flush
  (helper call, BB end, TB end), so an overwritten slot converts once;
  a mid-TB exit lands the pending ones from shadow doubles in
  `restore_state_to_opc` (2026-10-15, −5% det Minsns/frame), which
  only the slow path of the guest access an exit leaves through
  stores (`st_f64_slow`; 2026-10-15, −1.2% Minsns/frame, −2.8%
  Mcycles/frame).
  `fucom*`/`fcomi*` compare the cached doubles (`fucom*` only when both
  images are `st80f` of their doubles), `fcmov` selects instead of
  branching, and the argument-only `cc_compute_all`/`cc_compute_c`
  calls skip the flush, so the `fld; fld; fucomi; fcmov` min/max idiom
  keeps its stores deferred (2026-10-15, −4.8% det Minsns/frame, −4.9%
  Mcycles/frame). Push, pop, `ffree`, `fdecstp` and `fincstp` only move
  a translate-time `fpstt` offset and note tag bytes; `env->fpstt` and
  `env->fptags` land at the next flush, and a mid-TB exit applies the
  insn's starting offset and tags from insn_start word 2 (2026-10-15,
  det heavy-f1 −3.2% Minsns/frame, −5.7% Mcycles/frame paired median,
  on the x87 lane: 22329bd8bc, before the pinned registers and TLB memo).
  TBs are also keyed on the TOP (`env->fpstt` in `cs_base` bits 32-34),
  so those blocks address ST(i) and the tags as fixed env offsets and
  chained exits pass their TOP as a constant (2026-10-15, det heavy-f1
  −2.1% Minsns/frame on the x87 lane and −2.45% on top of the pinned
  registers and TLB memo, Mcycles/frame neutral on both: code run at
  several TOPs is translated once per TOP). Results also cross flushes
  as doubles: a flush stores the double and marks the register's image
  stale (`env->xemu_x87_dstale`), the next block reads the double, and
  C code converts a stale register when it touches it (2026-10-15, det
  heavy-f1 −2.5% Minsns/frame, −2.9% Mcycles/frame paired median, x87
  lane). `fsqrt`,
  `frndint` and `fist`/`fistp` run inline as well (FSQRT; FRINTX, then
  FCVTZS for `fist`, under the live FPCR as the helpers' `sqrt`, `rint`
  and `llrint` do), and an `fldcw` that keeps RC and precision control's
  bit 9 stores `fpuc` and the softfloat precision itself and chains on to
  the next insn instead of calling the helper and taking the probe; the
  helper and the probe remain only for words that change those
  (2026-10-15, det heavy-f1 −1.2% Minsns/frame, −4.2% Mcycles/frame
  paired median, x87 lane). The compares and `fcmov` got shorter
  (2026-10-16): `com` reads the FCMP result with three CSELs on MI, EQ
  and VS instead of MRS NZCV and ten ALU ops, and can place ZF/PF/CF
  straight as the x87 C3/C2/C0 (a shift operand, 8 for fpus), so
  `fcom*`/`fucom*` merge them into fpus without the AND and LSL;
  `fcmov` is the new `movcond_f64` op on the prepared condition (after
  `fucomi`: TST of CC_SRC, then FCSEL; other hosts expand it through
  GPRs) instead of SETCOND plus four FMOVs and a CSEL. A memory `fcomp`
  went from 20 host insns to 12 after the load, `fucomi` from 12 to 6
  plus the EFLAGS merge, `fcmov` from 7-8 to 2-3. Census counts
  (`XEMU_X87_CENSUS`) unchanged. Cross-binary det A/B against the
  ret-memo commit, 4 pairs each at load 26-29: heavy-f1 Minsns/frame
  −1.46% (4/4), stream-e2 −1.69% (4/4); Mcycles/frame within that
  run's load noise (+2.9% and −1.9%, 3/4 and 2/4).
- **FPCR caching + rounding-correct FIST (retired with the cache,
  2026-10-15).** `MSR FPCR` was re-emitted only when guest RC bits
  changed (`cached_fpuc_rc`), and `FIST`/`FISTP` honored the control
  word via fused `cvt_{rn,rm,rp}_i{32,64}_f{32,64}` TCG ops (single
  host `FCVT{N,M,P,Z}S` on AArch64). Those ops, `rint`, `rint_cvt` and
  their backend lowerings were removed too; `rint_f64` (FRINTX) is back
  for the write-through `frndint` and `fist`.
- **Signaling vs quiet compares (retired with the cache, 2026-10-15).**
  `coms_f32`/`coms_f64` (FCMPE / COMISS) for FCOMI; `com_f32`/`com_f64`
  for FUCOMI. The `coms` ops are removed; `com` (FCMP, upstream's COMISD
  on x86_64) stays for the write-through `fcom`, `fucom*` and `fcomi*`
  (the `__hard` helpers raise no exception flags either way).
- **Control-word resync.** Every path that writes `env->fpuc`
  (`fldcw`, `fldenv`, `frstor`, `fxrstor`, `fninit`, xsave helpers,
  `cpu_post_load`, gdbstub, SEV, reset) routes through
  `cpu_set_fpuc` so `HF_FPU_RC` / `HF_FPU_PC` in `hflags` are
  rebuilt — the write-through x87 path reads `HF_FPU_PC` from
  `tb->flags` and the inline SSE arithmetic `HF_FPU_RC`. The one
  exception, the write-through `fldcw` fast path (`gen_fldcw_wt`), runs
  only for a word that keeps RC and PC bit 9, so no `hflags` bit
  changes, and stores what `update_fp_status` would change then
  (`fpuc` and the softfloat precision).
  `update_fp_status` sets the calling thread's FPCR, and reset, the
  gdbstub and `cpu_post_load` call it from the main thread, so since
  2026-10-15 `x86_cpu_exec_enter` re-applies the x87 RC to the vCPU
  thread's FPCR at every `cpu_exec` entry (one compare; before, only a
  load under icount resynced it, from `cpu_post_load`). Likewise every
  MXCSR write reaches `update_mxcsr_status`, which keeps `HF_SSE_DFL`
  while the inline SSE path is on; reset now goes through
  `cpu_set_mxcsr`.
- **Misc.** `fnstcw` is one load; `insertion_sort_syncs` replaces
  `qsort` for N ≤ 16. (Retired with the cache on 2026-10-15: `gen_flcr`
  materializing rc-bits from `tb->flags`. The `flcr` op and its 5-insn
  `RBIT` lowering stay in the aarch64 backend for upstream's `gen_flcr`
  skeleton; nothing has emitted the op since 3eb1b2de10.)
- **3-MOVK constant materialization.** `tcg_out_movi` now emits
  MOVZ/MOVN + up to 3 MOVK for 3- and 4-chunk constants instead of
  dumping them into the literal pool (LDR + D-cache pressure).
  48-bit host pointers — materialized constantly in TB prologues —
  stay in the instruction stream on Apple cores.
- **Sticky host-FPU bracket: the SSE fast path goes live on Apple
  Silicon (+1.9 fps).** The NEON SSE path had measured parity — the
  per-op serializing FPCR restore was worth exactly the SIMD win.
  The bracket is now sticky (host stays in guest-SSE mode between
  helpers; `=2` differential keeps strict brackets and re-confirmed
  bit-exactness). Measured **+1.90 fps, 3/3 pairs positive**;
  default on for aarch64 with `perf.hard_fpu`. (x87 hard-FPU helpers save/restore around
  their own ops; softfloat/TCG carry no host FP-mode dependence.)
  Follow-up fix (historical: the inline x87 path it protected never
  ran after `3eb1b2de10`, and `cached_fpuc_rc` was removed on
  2026-10-15): the *inline* x87 path skipped its `MSR FPCR` whenever
  the `cached_fpuc_rc` cache matched the TB's rounding bits, so the
  bracket invalidated that cache when it rewrote FPCR.
- **Inline SSE packed-single arithmetic (2026-10-15).** Legacy-SSE
  `addps`/`subps`/`mulps`/`divps` compile to one AArch64
  `FADD`/`FSUB`/`FMUL`/`FDIV .4S` (new generic `f{add,sub,mul,div}_vec`
  TCG ops behind `TCG_TARGET_HAS_fvec`: aarch64 only, off on
  Windows/ARM64 with the other FP ops) instead of a ~53-instruction
  helper call. Only in TBs whose flags carry `HF_SSE_DFL` (hflags bit 2:
  MXCSR rounds to nearest, FTZ and DAZ clear) and x87 RC nearest: there
  the host FPCR already holds what the NEON helper's bracket would set,
  so results are the helper's bit for bit. LDMXCSR, FXRSTOR and XRSTOR
  end their TB. Azurik's population is one 4x4 matrix multiply (16
  `mulps` + 12 `addps` per call, ~50k ops per heavy-f1 frame). Det
  heavy-f1, 34 order-balanced pairs at load 40-68: Minsns/frame −2.67%
  (34/34), Mcycles/frame −0.9% (22/34, 95% CI −2.3..+0.5%); a 28-pair
  recheck at load 12-41: −2.61% (28/28) and +0.2% (13/28, CI
  −1.2..+1.6%), so cycle-neutral. Stream-e2 walk, 8 pairs: Minsns/frame
  −2.1% (8/8), Mcycles/frame −2.5% (6/8, 95% CI −5.5..+0.7%).
- **Guest registers pinned in host registers (2026-10-15).** TB code
  keeps EAX..EDI in x8-x15 and EIP, CC_DST, CC_SRC and CC_OP in x24-x27
  instead of in `CPUX86State`. The JIT study counted ~5.9M env loads and
  stores of these per heavy-f1 frame (GPRs 3.2M, CC_* 1.5M, EIP 1.2M),
  most of its ~6.5M guest-state syncs (segment bases are the rest).
  TCG core: the target calls `tcg_pin_env_global()` from the new
  `TCGCPUOps.pin_globals` hook, before the prologue is generated; the
  global then stays resident in its register, and an op whose result
  only feeds the next op's mov into a pinned global computes straight
  into the home. The aarch64 prologue loads the homes and the epilogue
  stores them. qemu_ld/st slow paths (after the sub-page prefilter) BL
  two shared routines behind the epilogue, one storing every home before
  the helper and one reloading x8-x15 after it; inlined, those 11 insns
  per stub grew TB code 14.7%, now 0.3%. Helper calls save and reload
  the GPR homes (call-clobbered); they store EIP/CC_* only if the helper
  may read globals or fault, and reload them only if it may write them.
  `TCG_CALL_NO_RW_PINNED` exempts the x87 and SSE compute helpers that
  touch none of it and cannot fault (not fcomi/fucomi, which write
  CC_SRC, nor fwait); it saves under 0.1%. TB lookups read EIP and keep
  the sync. Layouts that lost in the prototype (det heavy-f1, 4 pairs):
  all eight GPRs in call-saved x20-x27 cost +4.6% Mcycles/frame, since
  the allocator's call-preserved registers are gone and temps live
  across calls spill; without the coalescing it was +5.1%. The FP temps
  (d8-d15) and vector SSE ops allocate exactly as before; 18 of 15,797
  heavy-f1 TBs gain a spill (x87 FDP addresses across a load). A
  `--debug` build (CONFIG_DEBUG_TCG) poisons the env homes (0xdeadbeef)
  whenever TB code owns them and aborts if a helper without a reload
  changed one: wrongly marking `fucomi` aborts on its CC_SRC write,
  wrongly marking `read_eflags` breaks the f300 golden, and the real
  build passes all goldens. Cross-binary det A/B against its parent
  (measured on the pin lane, based on 94a722acd8), load 7-29, median
  per-pair Mcycles/frame: heavy-f1 −3.9% (11/12 pairs; mean −3.7%
  without the two pairs a load spike moved by more than 10%, −5.3%
  with them), Minsns/frame −4.0% (12/12); stream-e2
  −3.9% (21/28; mean −3.5%, 20/24, without the four spike pairs, −1.5%
  with them), Minsns/frame −4.0% (28/28).
- **Softmmu TLB walks shared within straight-line TB code
  (2026-10-15).** The JIT study put the qemu_ld/st fast-path TLB walk at
  24% of the det vCPU: 2.4M walks per heavy-f1 frame, ~9 host insns
  with two dependent loads each. An access to the page that an earlier
  access of the same kind and mmu_idx walked now reuses that walk. The
  walk leaves ntag = -page in x4 (loads) or x6 (stores), plus the
  addend in x5 (loads) or the TLB entry's address in x7 (stores). A
  sharing load checks `zext(addr) + ntag <= PAGE_SIZE - size` (3
  insns), then loads from addend + addr. A sharing store also re-reads
  `addr_write` and the addend from the entry and requires `addr_write
  == page` (6 insns, one `ccmn`). That covers nv2a's dirty tracking,
  which re-arms TLB_NOTDIRTY from its own thread (`tlb_reset_dirty`):
  a store that missed the re-arm would leave the page clean. A miss
  walks the TLB out of line and refills the slot. Labels, helper calls,
  TB exits and the TB start end every slot at translation time. Slow
  paths set ntag = 1 << 32, which no address matches. Accesses with an
  alignment requirement never share.
  The translation-time plan (`tcg_tlbm_plan`) tracks each address as
  (root value, constant offset) through mov/add/sub. An access shares
  when it has its slot's root and lies within half a page of the fill
  (on the same page, for constant addresses). A slot is filled only
  when a later access shares the fill and needs it sooner than the
  slot's current page. 42% of heavy-f1 accesses share, and 0.4% of the
  checks miss; sharing whenever a slot was live missed 34%. Reserving
  x4-x7 costs the allocator almost nothing (+0.06% Minsns,
  pressure-only build): qemu_ld/st are `TCG_OPF_CALL_CLOBBER`, so no
  value lives in x0-x7 across them anyway. Reserving the call-saved
  x20-x23 instead cost +2.1% in spills. A `--debug` build walks the TLB
  behind every shared access and aborts if it disagrees. For stores it
  ignores TLB_NOTDIRTY in the live entry, which another thread may set
  after the share check (the fast path's usual compare-then-store
  race), and checks instead that the share check read the bare page;
  dropping the store's `addr_write` compare aborts within the f300 run.
  Cross-binary det A/B against its parent (on the pin lane, based on
  94a722acd8): heavy-f1 Mcycles/frame median −3.9% (mean −4.4%, CI
  −6.6..−2.2, 15/18 pairs, load 14-34), Minsns/frame −5.25% (18/18);
  stream-e2 median −4.1% (mean −4.5%, CI
  −7.3..−1.8, 10/12, load 20-28), Minsns −4.1% (12/12). Stores that keep
  the addend and skip the re-read measured −1.6% Minsns and −2.9%
  Mcycles (6/6) against this form (−6.7% Minsns against its parent), but
  they widen the re-arm race from one store to the rest of the segment,
  so they are not shipped.
  **2026-10-16:** accesses with an alignment requirement share too, and
  the 16-byte ones (qemu_ld2/st2) take part in the plan; until then
  every `movaps`/`mulps` m128 operand walked the TLB (64.5K walks per
  heavy-f1 frame, 57.7K on stream-e2). The share check rotates the page
  offset right by the alignment bits (EXTR) and compares against the
  bound shifted the same way, so a misaligned address becomes huge and
  takes the slow path, whose re-walk checks the alignment and leaves the
  #GP to the helper (tested: `movaps xmm1, [eax+1]` sharing the walk of
  `movaps xmm0, [eax]` faults at the second insn). A 16-byte SSE load
  now goes through a new `qemu_ld_v128` op (one LDR Q into a vector
  temp, then one STR Q to env) instead of an LDP and two 8-byte stores
  that the consumer's 16-byte reload could not forward from; the
  optimizer's env-copy tracking hands the temp straight to the inline
  `mulps`/`addps`, `movaps xmm, m128` loads straight into the register,
  and vector env copies are dropped at guest accesses (vector temps
  never live in call-saved registers, so carrying one across meant a
  spill and a reload). The matrix multiply's `movaps` is 6 host insns
  (add, ror, cmp, b.hi, ldr q, str q) where it was 14. Cross-binary det
  A/B against the x87-compare commit: heavy-f1 6 pairs at load 15-18,
  Minsns/frame −0.59% (6/6), Mcycles/frame −2.0% (4/6; a 4-pair run
  read −0.63% and −3.6%, 4/4); stream-e2 4 pairs, −0.34% (4/4) and
  +0.5% (1/4). The two halves alone: shared aligned walks −0.22%
  Minsns (4/4), with the LDR Q path −0.39..−0.46%.
  **2026-10-16, deterministic mode:** stores keep the addend after all,
  under icount only (`tlbm_st_addend`, latched in `tcg_target_init`), so
  a sharing store is the load's 3 insns. An audit of every caller of
  `physical_memory_dirty_bits_cleared` (the only path to the one
  cross-thread TLB writer, `tlb_reset_dirty`) found no re-arm that can
  land while the vCPU runs TB code under icount: the renderer's texture
  and vertex dirty clears run on the PFIFO thread only while the vCPU
  waits in `pfifo_icount_sync` (an MMIO slow path, or before the slice),
  the VGA scan runs on the vCPU (the vblank is a virtual timer, and the
  main loop runs no virtual timers under icount), checkpoints and loads
  stop the VM, and migration's bitmap sync waits for the vCPU to leave
  the TB (`memory_global_after_dirty_log_sync`) before it reads the
  pages it cleared. One same-thread path did break the old addend-only
  form: the sub-page prefilter's inline store returns to the TB after a
  walk that met TLB_NOTDIRTY, which left the slot holding that page, so
  the segment's later stores skipped the trap and its per-sub-block code
  test (without the fix the `--debug` check aborts within the f300 run,
  as it did when the compare was dropped above, likely for this reason).
  The prefilter now ends the slot. The `--debug` check is strict for these
  stores (the live `addr_write` must be the bare page; TLB_NOTDIRTY is
  allowed only while a migration runs). Normal mode keeps the 6-insn
  form, so the det goldens no longer cover it. Cross-binary det A/B
  against 9a8cb87de9, 26 pairs per anchor in two sessions at load
  33-85 (Mcycles swung 5-10% per pair): Minsns/frame −2.48% on
  heavy-f1 1200 frames and −2.07% on stream-e2 600 frames (26/26
  each); Mcycles/frame stream-e2 median −2.7% (mean −3.4%, 20/26),
  heavy-f1 median −3.2% but mean −1.2% (15/26; the second session's
  20 pairs alone: median −0.2%, 10/20).
- **MMX entry inline, once per TB (2026-10-15).** `helper_enter_mmx`
  (fpstt = 0, every tag valid) ran before every MMX insn. Marked
  `TCG_CALL_NO_RW_PINNED`, the call still made TCG store and reload the
  GPR homes (x8-x15 are call-clobbered) and sync every other global
  around each one; before the pinned registers it was a call without
  flags, which wrote back and reloaded all the guest registers: 6.6-18.6%
  of the det vCPU in cold boot, from Azurik's boot-video YUV-to-RGB loop
  (~50 MMX insns per 8 pixels). Now the TB's first MMX insn emits the
  x87 flush the call implied, `fpstt = 0` and one 8-byte tag store;
  later ones emit nothing until an x87 escape, EMMS/FEMMS, FXRSTOR or
  XRSTOR clears `mmx_entered`. Since the x87 registers stay doubles
  between blocks (x87 bullet above), that first entry also loads the
  eight stale bytes (`env->xemu_x87_dstale`, one 8-byte load) and calls
  `helper_enter_mmx_sync` only when one is set. Det cold boot (on the
  MMX lane, before the pinned registers; 1800 frames, 6 pairs, load
  19-24): Minsns/frame −10.4% (6/6), Mcycles/frame
  −17.8% (6/6). heavy-f1, stream-e2 and the movie anchor run no MMX.
  The MMX `punpck{l,h}{bw,wd,dq}` and `pmulhw` then became i64 TCG ops
  (shift/deposit; sign-extract, multiply, deposit), which leaves the
  loop helper-free: Minsns/frame −2.6% more (12/12), Mcycles/frame
  flat (7/12). `movd mm, r/m32` (112K a frame in the loop) then became
  one 8-byte store of the zero-extended value instead of a zero store
  and a 4-byte store, which the loop's next 64-bit read of the register
  could not forward from (2026-10-16): det cold boot 900 frames, 8
  pairs at load 16-20, Minsns/frame −0.27% (8/8), Mcycles/frame −2.9%
  (7/8).
- **TB-dispatch inline probes (+1.5 fps family, 2026-07-05).** The
  first guest-side profile (`XEMU_GUEST_PROF`: mach-thread sampler +
  per-exit-kind TB-lookup census; the census was removed 2026-10-15,
  revert the removal commit to restore it) showed diffuse guest time
  (top TB 1.8% — no hot-loop candidates) but near-returns at 46% of 15.8M/s
  TB lookups. Two shipped mechanisms: the near-return target memo — a
  4096-entry eip→TB cache probed inline at ret sites (default on;
  **+0.77 fps, 6/6 pairs**; helper hits collapse 362M→13k; under
  `-d exec,nochain` every exit takes the helper, so exec logs stay
  complete) — and the same inline-probe technique applied to the
  real jump cache at indirect/cross-page exits (emitter in
  `accel/tcg/xemu-inline-jc.c`, the sole owner of the cache layout;
  +0.70 fps with the enabled arm carrying ~+8% draw throughput). The
  APU VP doorbell block also joined the audited lockless-MMIO set.
  Two honest kills en route are in the failed-experiments table
  (JIT write-protect caching; the per-depth return-address ring).
  **2026-09-08:** that emitter forms the entry address with a hardcoded
  `shli 4` and compares a 64-bit `pc`, neither derived from
  `offsetof`/`sizeof`, so both layout facts are now pinned by
  `QEMU_BUILD_BUG_ON` at the top of the emitter
  (`accel/tcg/xemu-inline-jc.c:50-51`): a future `CPUJumpCache` field
  addition or a narrower `vaddr` becomes a compile error instead of a
  probe that silently reads the neighbouring entry and dispatches to the
  wrong TB. No codegen change — both assertions hold today on this
  tree.
  **2026-10-10:** the memo now drops a page's entries whenever the jump
  cache does (INVLPG and ranged TLB flushes, including the previous page
  for TBs that span into it). Like the jump cache it is keyed by virtual
  eip with no physical check, but until then only full flushes cleared
  it, so after the guest remapped a code page and ran INVLPG a memo hit
  could still return the TB translated from the old page (no failure
  was ever reported). A 4096-bit map of the virtual pages that filled an
  entry keeps the per-page flush O(1) unless the page may be in the
  memo; per-page flushes are rare on this guest (11k INVLPGs over a
  whole PGO training run). Generated code is unchanged.
  **2026-10-16:** the inline probe no longer reads the TB. Each entry
  now holds the TB's code pointer (`tc_ptr`, in the old pad slot), set
  only for TBs with the base cflags, and only TBs with the base cflags
  emit the probe. The live `tb->cflags` load that caught `CF_INVALID` is
  gone: every i386 TB is `CF_PCREL`, whose invalidation clears the whole
  memo, and the fill re-reads the cflags behind a barrier in case
  another thread invalidates the TB between lookup and fill. eip and
  flags compare as one 64-bit word, and the index is `eip ^ eip >> 14`
  (LSR, EOR, UBFIZ) instead of a multiply: 15 host insns from the
  return address to the `br`, with three loads, where it was 24 with
  four dependent ones. That hash also misses less than the multiply on
  Azurik: helper fills per run 945k → 825k on heavy-f1 1200 frames,
  764k → 719k on stream-e2 600 (shifts 8-16 measured; 14 was best on
  both). A `--debug` build checks the TB behind every inline hit.
  Cross-binary det A/B against cced50204c, 4 pairs each at load 19-30:
  heavy-f1 Minsns/frame −1.07% (4/4), stream-e2 −1.39% (4/4);
  Mcycles/frame within the load noise of that run (see the commit).
- **icount step TBs cached by pc (det mode, 2026-10-16).** Every
  one-instruction step of a cut block (`xemu_istep`) looked its TB up
  in the qht: `get_page_addr_code`, xxhash, the seqlock walk and
  `tb_lookup_cmp`, ~130 cycles, 1100-3900 times per frame (cold boot
  3852, heavy-f1 1106). `xemu_step_lookup` puts a direct-mapped cache
  in front of it: 8192 `{pc, tb, stamp}` entries indexed by the low pc
  bits in `CPUJumpCache`, a hit passing the jump cache's own checks
  (`CF_INVALID` makes an invalidated TB miss). It drops pages exactly
  when the jump cache does, without a walk: `tb_jmp_cache_clear_page`
  stamps the page's bucket (256 buckets) with a new clock value and
  `tcg_flush_jmp_cache` stamps all of them; an entry counts only if it
  was filled at or after its bucket's stamp. `CF_PCREL` TB
  invalidation clears the jump cache but not this one. Off-RAM
  one-shot TBs are never cached. Step hits (`XEMU_TB_PROF`
  `step_hit`): cold boot 94.2%, heavy-f1 81.4%, stream-e2 53.3%. Cold
  boot 1800 frames, 8 pairs at load 11-15: Mcycles/frame −1.68% paired
  median (6/8), Minsns/frame −1.42% (8/8); heavy-f1 1200 frames, 8
  pairs at load 13-29: −0.21% (4/8) and −0.23% (8/8). Cold-boot cycles
  swing ±10% run to run with IDE wait polling, and an early 4-pair
  heavy-f1 batch at load 50-80 read +5.2%; the instruction counts are
  the steady receipt. Step TBs still stay out of the jump cache and
  unchained.
- **Two-way jump cache (2026-10-16).** The 4096-entry, 64 KiB jump
  cache is now 2048 two-way sets: way 0 is
  `tb_jmp_cache_hash_func(pc) & ~1`, way 1 the next entry, both inside
  the page's 64-entry range, so page and full clears are unchanged.
  `tb_lookup` probes way 0, then way 1 (a hit swaps it into way 0); a
  fill moves way 0's entry to way 1. The inline probe
  (`xemu-inline-jc.c`) still reads way 0 only, with the same
  instructions as before (one mask bit differs), so a way-1 entry
  costs a helper call instead of a qht walk. The target was recurring
  conflicts, not capacity: cold boot alternates the two fldcw RC
  variants of one pc (flags `0x604000b4` / `0x204000b4`) in one slot
  ~2700 times a frame. `XEMU_TB_PROF` per frame against the step-cache
  build: htable walks 3711 → 287 on cold boot and 450 → 224 on
  heavy-f1. Det A/B against the step-cache build at load 8-11:
  heavy-f1 1200 frames, 4 pairs, Mcycles/frame −0.80% paired median
  (3/4), Minsns/frame −0.06% (3/4); stream-e2 600 frames, 3 pairs,
  −1.68% (3/3) and −0.07% (2/3); cold boot 1800 frames, 4 pairs,
  Minsns −2.12% (4/4; cycles are bimodal there). A second, inline
  way-1 check (rebuilt from eip and env so that no TCG temp crossed
  its label) also took cold boot's C lookups from 4510 to 1256 a frame,
  but its extra branches and temps cost translation: +0.18-0.21%
  Minsns/frame on heavy-f1 and stream-e2 against the step-cache build
  (0/11 pairs better; heavy-f1 Mcycles +0.5-0.9%), and no measurable
  cold-boot gain over the way-0-only probe. Unlike the
  16-bit enlargement (failed table) the way-0 hit path and the cache
  footprint did not grow.
- **`can_do_io` bookkeeping elided unless replay is on (default on,
  2026-08-04; under icount too since 2026-10-15).** Upstream emits
  `set_can_do_io(false)` / `(true)` into every multi-insn TB body
  unconditionally, and
  `io_prepare` turns any mid-TB MMIO access into a `cpu_io_recompile`
  round trip — `tcg_tb_lookup`'s g_tree walk, state restore, siglongjmp,
  a `CF_MEMI_ONLY` re-translate, then re-execute — which is a stable
  top-25 vCPU symbol on the heavy scene. Nothing observed the flag with
  icount and replay off then (watchpoint.c's use was nested inside
  `replay_running_debug()`), so both stores went behind a
  translate-time latch that force-disabled itself under icount or
  `replay_mode != REPLAY_MODE_NONE` (only under replay since
  2026-10-15, below); `mem_io_pc` assignment is unchanged, so the APIC
  TPR-access unwind keeps its semantics. Since the gdb watchpoint fix
  (`32b1ddf856`), `cpu_watchpoint_hit_reruns` reads the flag in every
  mode; outside replay it is always true, so an access at ra 0 counts
  as a debug-interrupt delivery. The xemu-watch access log asks
  `xemu_io_ok` instead (below). Gate
  (`XEMU_INV_PROF` `(f)` line, read from a knob-off run): **242k
  recompiles/s against a 40k/s build bar (5.2-6.1x)**, and
  `XEMU_GUEST_PROF` shows the symbol plus its callee cluster
  (`g_tree_find_node` / `tb_tc_cmp` / `cpu_unwind_data_from_tb`, ≥ 1.22%
  each) cleanly vanish with the elision on. Interleaved cross-binary
  A/B on F8: **41.59 → 48.12 fps (+6.53, 3/3 pairs positive), draws/s
  +6.1%** — 4x above the prediction's top end (+1.6), and consistent
  with the mechanism (242k/s × ~550 ns round trip ≈ 13% of the vCPU
  thread). Independent of the lockless-MMIO campaign: ~91% of guest
  MMIO already bypasses the BQL and the recompiles fire on exactly
  those lockless regions.
  **2026-10-15: elided under icount too** (only replay keeps the
  stores). The two byte stores were 1.6-1.9% of a deterministic run's
  host instructions. `io_prepare` now asks `xemu_io_ok`, which under
  icount calls `xemu_io_ra_in_last_insn` instead of reading the flag
  (the xemu-watch log asks it too, to skip the first pass of an access
  that `cpu_io_recompile` reruns). That finds the TB from the access's
  return address and calls the access last-insn exactly when
  `cpu_unwind_data_from_tb` reports one insn left, which is when the
  flag was true. A 256-entry cache
  keyed by `tb_flush_count` keeps the `tcg_tb_lookup` walk off the
  ~1,800 MMIO accesses per heavy-f1 frame. `XEMU_INV_PROF` reads
  1,076,129 `io_prepare` calls and 193 recompiles in both binaries. The
  goldens stay bit-exact. Cross-binary det A/B against 94a722acd8 (load
  9-21; new against base, negative is fewer): Minsns/frame −1.6% on
  heavy-f1 (16/16 pairs) and −1.7..−1.8% on stream-e2 (16/16).
  Mcycles/frame is within noise: heavy-f1 +0.9% (3/6 pairs faster) and
  +0.3% (6/10, median −0.9%), stream-e2 −3.7% (6/8) and +3.3% (5/8,
  median −2.7%; a load spike). A same-binary toggle gave −3.7% (5/6) on
  heavy-f1 and −0.4% (2/6) on stream-e2. Duplicating the stores instead
  cost +4.5% (0/6). With the flag left true, icount's "Bad icount read"
  guard no longer fires. **2026-10-16: the cache is 1024 sets of 4
  ways** (a set per cache line, newest first, indexed by
  `(ra >> 2) ^ (ra >> 14)`). The direct-mapped 256 entries aliased every
  KiB of code: ~200 live access sites replaced 4-24 entries per frame,
  and each miss (region lock, `g_tree_lookup`,
  `cpu_unwind_data_from_tb`) was 0.27-0.34% of the det vCPU. Now
  ~1,000 misses per 1200 heavy-f1 or stream-e2 frames, ~all
  compulsory (`XEMU_INV_PROF`'s `(f)` line counts them); `io_prepare`
  and recompile counts are unchanged. Cross-binary det, two or three
  sessions pooled (load 9-45; negative is fewer): Mcycles/frame
  stream-e2 −2.9% (15/21 pairs), heavy-f1 −0.9% (8/14), cold boot
  +0.7% (6/14, noise); Minsns/frame −0.13% (14/21) and −0.16% (13/14).
  The translator asks `xemu_io_pc_is_set` for every insn under icount
  (the pcs `cpu_io_recompile` once saw end their retranslated TB), and
  its `g_hash_table_contains` was 0.64% of the det cold boot's vCPU.
  Since 2026-10-16 a 32768-bit filter keyed by a multiplicative pc
  hash answers first: a cold boot notes 1,262 pcs (3.8% of the bits;
  the `(f)` line prints both), and a bit is never cleared, like the
  set. Cold boot, two sessions pooled (load 16-45): Mcycles/frame
  −1.1% (9/14 pairs), Minsns −0.23% (12/14); stream-e2 and heavy-f1
  neutral (12 more pairs each: +0.3% and +1.2% median, 6/12 and 4/12,
  means +0.5% and +0.2% with se 1.1 and 0.9).
- **`notdirty_write` dirty-check fast path (default on, deliberately
  no fps claim, 2026-08-04).** The store slow path's tail re-read the
  dirty bitmap to decide whether to clear `TLB_NOTDIRTY` after the same
  call had already answered the question: the XBOX invalidation now
  returns "page still holds code" (i.e. `tlb_unprotect_code` did not
  run), and after `set_dirty_range(..., DIRTY_CLIENTS_NOCODE)` the four
  non-CODE clients are dirty by construction, so `is_clean ==
  !(CODE dirty)` is known locally on 100% of calls. The remaining
  single-page queries — that CODE check and `physical_memory_is_clean()`,
  which the TLB refill also calls — read all five clients under one RCU
  guard with one idx/offset computation and `test_bit`, replacing five
  nested guards and five `find_next_bit` loops.
  **The honest part: this ships with no individual fps claim.** The
  audit sized it on a profile where `notdirty_write`'s trap share was
  75.1%; by the time it was built, the 2026-07-18 arm-(b) store-prefilter
  promotion had inhaled that population. Re-measured in-scene on
  2026-08-04: `notdirty_write` runs **~2.6k calls/s at a 2.2% trap
  share — a 34-57x collapse of the benefit base**, putting the ceiling
  at ≤ 0.06% of the vCPU thread, below what the ±0.02 fps protocol can
  resolve. The code stays because it is behavior-equivalent,
  soak-validated and simpler than what it replaces, and it is covered by
  the wave's headline cross-binary A/B — but no individual A/B was run
  and none should be quoted.
- **x87 translator constant-factor pack (default on, 2026-08-04).**
  The F8 profile is flat, so the x87 work was gated on a census first
  (`XEMU_X87_CENSUS`), which found the guest executing **127.3M x87
  insns/s — ~7x the audit's assumed ceiling** — and 21.01M/s of
  combined elidable write-backs (2.6x the gate bar), FT0 alone at
  13.97M/s with a 90% elidable share. Two mechanisms shipped. (1) The
  inline-FPU register-cache flush no longer writes back the FT0 scratch
  when the value's reader has already consumed it: in hard-fpu-inline
  mode every FT0 read is preceded by an FT0 write in the same guest
  instruction, no helper reachable in that mode reads `env->ft0`, and
  `ft0` is not in the vmstate — the one live window (a `cc_compute_all`
  call between the write and an inline FCOMI/FUCOMI read) is tracked
  precisely and still stores. (2) The per-instruction FIP/FCS/FDP/FDS
  exception-pointer stores now land once per contiguous x87 run (cut
  ratio **8.81x** against a 1.5x bar, mean run length 3.64), at the
  flush points that already precede every consumer — `do_fstenv` via
  FNSTENV/FNSAVE, every helper call, and `tb_stop` — and the CS-selector
  load is hoisted out of the per-instruction path since CS cannot change
  mid-TB. Since 2026-10-15 this also runs under icount: a pending
  FIP/FCS lands at mid-TB exits from insn_start word 2, FDP stays per
  instruction and FDS is stored on segment change (det heavy-f1 +3.5%
  frames/Gcycle, −4.0% host instructions). Write-through blocks then
  deferred FDP as well: the slow path of every guest access stores the
  pending value, so a mid-TB exit still finds it in env (2026-10-15, on
  the x87 lane det heavy-f1 −0.77% Minsns/frame, −1.2% Mcycles/frame
  paired median over 24 pairs; on top of the pinned registers and TLB
  memo −0.96% Minsns/frame, cycles unresolved). FDS then rode with it
  (2026-10-16): the flush stores the selector of the last x87 memory
  insn's segment with the FDP, and a mid-TB exit lands it from insn_start
  word 2 (bits 43-45: segment + 1), where until then every basic block's
  first x87 memory insn per segment stored it (296K selector loads and
  `strh` per heavy-f1 frame). A directed test (fld fs:[0], then an NV2A
  MMIO read in the same block, which `cpu_io_recompile` re-runs, then
  FNSTENV) stores FS's selector, and the refuter's restore check runs
  on it. Det A/B against the movd commit: heavy-f1 6 pairs at load
  20-25, Minsns/frame −0.69% (6/6), Mcycles/frame −2.0% (3/6);
  stream-e2 4 pairs, −0.59% (4/4) and −1.9% (3/4). Adversarial refuter
  (`XEMU_X87_REFUTE`, legacy stores kept and the elision decision run
  as a shadow): **FT0 violations 0; 2.06e10 exception-pointer checks,
  violations 0.** Interleaved A/B on F8, each 3/3 pairs positive: FT0 **+1.56 fps**, FIP **+7.46 fps** (draws/s
  +8.8%), both together **+7.68 fps** — sub-additive against the sum of
  the singles, which is ordinary frame-time composition, not a
  contradiction. The FT0 half lived in the inline cache's flush, so it
  was dead from v0.13.2 (`3eb1b2de10`) and was removed with the cache
  on 2026-10-15; the third mechanism, clean-ST(i) write-back elision
  (`XEMU_X87_ELIDE_CLEAN`), was built dark because it is a numeric
  behavior change, not a pure elision; it measured −0.23 fps (3/3) the
  same day and its code was removed on 2026-10-10 — see Failed /
  reverted experiments.
- **arm-(b) prefilter counters off the hot path (default off,
  2026-08-04).** The aarch64 store slow-path stub emitted six diagnostic
  counter read-modify-writes unconditionally into generated code, and
  that stub completes ~2M stores/s; the counters are read only by an
  atexit dump. They are now emitted only under
  `XEMU_SUBPAGE_FAST_STATS=1`, so production stubs carry none of them
  and the four demote landing pads collapse into direct branches to the
  helper path — **~39 fewer instructions / ~156 B per store stub, 12 of
  them on the ~2M/s inline-skip path itself**. Zero behavior change: the
  decision logic and the refuter oracle are untouched, so
  `XEMU_SUBPAGE_FAST_STATS=1` reproduces the previous emission exactly
  and is itself the A/B lever. Measured cost of carrying the counters
  (3/3 pairs unanimous, the pre-registered claim confirmed at its low
  end): **−0.32% draws/s, −0.57 fps.**
- **Devirtualized TB-state extraction (default, no knob, 2026-08-04).**
  The single-target fork paid a double-indirect `TCGCPUOps::get_tb_cpu_state`
  call at every TB lookup (its 25-insn callee shows as its own symbol at
  0.24-0.31% in both profiles); the four hot sites —
  `helper_lookup_tb_ptr`, the exported `xemu_lookup_tb`, the exec loop,
  and the ret-memo helper — now call `x86_get_tb_cpu_state` directly.
  Semantics are identical, so there is no escape hatch; the binding is
  guarded instead by a compile-time `__builtin_types_compatible_p`
  assert on the ops-table slot (catches signature drift) plus a
  once-per-realize identity `assert()` on the vtable slot in
  `tcg_exec_realizefn` (catches someone repointing it). The cold
  `cpu_exec_step_atomic` and translate-all lookups stay virtual. No
  individual fps claim: the pre-registered expectation was +0.07 to
  +0.11 fps, which needs more pairs than the campaign could spend to
  resolve against ±0.02 fps — it rides in the headline A/B.
- **Writable-clean TLB bitmaps for `tlb_reset_dirty` (2026-10-16).**
  Every renderer and VGA dirty clear (texture binds, vertex flushes, the
  vblank VGA scan) re-arms `TLB_NOTDIRTY` through `tlb_reset_dirty`,
  which scanned every main entry of every dirty mmu_idx under
  `tlb_c.lock`: ~16-24k entries per call since the TLB floor, ~300 calls
  per heavy-f1 flip. That was 59-60% of the PFIFO thread, and the vCPU
  spun on the lock in `notdirty_write` and `tlb_set_page_full` (3.7-7%
  of its cycles). Each `CPUTLBDesc` now keeps a whole-table bitmap and 64
  bucket bitmaps keyed by host page (each with a summary bit per nonzero
  word), supersets of the entries the walk could arm. A range of up to 32
  pages walks its pages' buckets (98.8% of calls; 54% are one page),
  longer ones the whole-table bitmap. Fills, victim swaps and
  `tlb_set_dirty` set bits, walks drop stale ones, and the victim swap
  now swaps the full entries under the lock too, so a walk never pairs an
  entry with the other one's slow flags. A single bitmap only got the walk
  to 16.6-18.6% of PFIFO (2074 eligible entries visited per call), 16
  buckets without summaries to 10.4%. Windowed heavy-f1, xctrace PMI,
  load 12-21: `tlb_reset_dirty` 59-60% → 6.3% of PFIFO cycles, PFIFO CPU
  ~12.5 → 6.5 ms/flip, vCPU spin-loop share 3.7-7.1% → 0.02-0.04%; the
  e2 walk 50-52% → 10.3% and 0.9-1.6% → 0.03-0.07%. bench-abx 2x45 s:
  heavy-f1 48.0 → 59.1 fps (+23%, 2/2, load 14-15; the base fell below
  the cap, draws/flip equal); heavy-w2 49.5 → 50.7 (1+/1-, load 12-15)
  and 56.8 → 59.9 (2/2, load 13-37). Det heavy-f1, two n=4 sessions:
  Minsns −0.32%/−0.26% (8/8), Mcycles within noise; goldens unchanged.
  `XEMU_TLB_WCLEAN_REFUTE=1` reported misses=0 unset=0 on windowed
  heavy-f1, the windowed e2 walk, a det cold boot to 1800 and a 6-loadvm
  soak (with the sub-page refuter at 0 violations). Skipping the walk in
  `physical_memory_snapshot_and_clear_dirty` when the VGA snapshot is
  clean was left out: `tlb_set_page_full` tests `physical_memory_is_clean`
  before taking the lock, so an entry can go in writable-clean just after
  a clear, and today's next walk is what re-arms it.
- **No W^X flip per TB entry (Apple, both modes, 2026-10-15).**
  `cpu_tb_exec` called `pthread_jit_write_protect_np(true)` before every
  TB entry: 37 instructions and ~60 cycles even when the thread was
  already in execute mode (a tight-loop microbenchmark). Deterministic
  mode enters TBs ~1.3k times per heavy-f1 frame and ~5k times per
  cold-boot frame (icount steps, loop dispatches; from the instruction
  deltas), so the flip predicts ~1.8% of a ~17 Mcycle cold-boot frame
  but only ~0.4% of a ~22 Mcycle heavy-f1 frame (those frame costs and
  every figure in this bullet are from the misc lane: 22329bd8bc plus
  the MMX commits and the icache batching, before the pinned registers
  and TLB memo). PMI's 1.66% on heavy-f1 and 2.1% on stream-e2
  overstated it: the A/B below finds at most ~0.25% there, so those
  shares were most likely skid onto the flip's ISB, the trap the
  2026-07 caching row describes; only the cold boot's 2.6% roughly
  matches its A/B. The vCPU thread now stays in
  execute mode outside write brackets that close themselves:
  `tb_gen_code` is a write / execute wrapper around `do_tb_gen_code`,
  `tb_add_jump` closes on both exits, and `cpu_exec_longjmp_cleanup`
  flips back for a fault that leaves translation by longjmp (the path
  that broke the 2026-07 TLS cache). The tb-maint paths and the DSP JIT
  already closed theirs (`tb_phys_invalidate` too, which a watchpoint
  hit, gdbstub or guest DR7, reaches from a helper: the gdb watchpoint
  fix bracketed it), so the det APU tick's own flip went too;
  `cpu_exec` keeps one flip per slice as a safety net for work outside
  `cpu_exec` and the thread's first slice. No thread-local state. A
  stress build that longjmps out of every 5th or 7th translation keeps
  every golden; without the cleanup's flip it wedges at the first
  longjmp, the vCPU spinning at a code-buffer address with
  no crash report. Same-binary alternate-chunk A/B (the per-entry flip
  toggled every 20 frames, opposite phases paired chunk by chunk, load
  16-33): cold boot −2.07% Mcycles/frame (se 0.26); heavy-f1 −0.24%
  (se 0.20) and stream-e2 −0.24% (se 0.23), both unresolved;
  Minsns/frame −0.31%, −0.05% and −0.07%. Cross-binary paired medians
  at load 14-33: cold boot −1.1% (11/16), heavy-f1 −0.9% (14/20),
  stream-e2 +0.1% (7/14), all inside the load noise. The gain is a cold
  boot one; heavy-f1 and stream-e2 gain ≤0.25%.
- **Guest memory-order barriers only where a device thread can see the
  order (2026-10-15).** System-mode TCG puts a `dmb ishld` before every
  guest load and a `dmb ish` before every store (about 21 per hot
  x87/SSE block) so I/O threads see the x86 order. Under icount nothing
  reads guest RAM concurrently (APU tick, inline or synced pusher,
  settled IDE DMA and timers all run on the vCPU thread or with it
  parked), so icount drops them all (`30316da269`): det heavy-f1, 1200
  frames, 97.0 → 103.1 fps (3/3, load 9-12), goldens unchanged. Normal
  mode drops only the load-load barriers (`60fc127871`): on the
  single-CPU Xbox the writer-ordered publishers a guest polls with
  loads alone are nvnet RX rings (silent while no network backend is
  attached) and PFIFO data behind a semaphore, where the guest's own
  store between the two loads usually orders them; stores keep theirs
  because OHCI walks guest-linked TD/ED lists from the main loop.
  `xemu_net_enable` (startup included) restores every barrier in both
  modes and flushes translations as vCPU safe work. The normal-mode set
  applied under icount, 5 pairs at load 11-20: +4.1% det frames per
  Gcycle (5/5), −1.4% host instructions (5/5); windowed fps unreadable
  at load 35-42. Explicit fences (`mfence`, locked instructions) are
  untouched. `XEMU_GUEST_MO=1` keeps every barrier (normal-mode switch:
  the goldens can't see device-thread ordering).
- **Hot/cold TB layout (2026-10-16).** A TB's qemu_ld/st slow paths,
  x87 stubs, constant pool, TranslationBlock and search data sat right
  behind its body, about three quarters of its bytes (heavy-f1: 1143 B
  of buffer per TB, 240 B of it body), so the code that runs was spread
  over nearly five times the bytes it needs. On aarch64
  (`TCG_TARGET_COLD_CODE`) each region is now cut into 1 MiB chunks:
  bodies pack back to back in a chunk's first quarter (`tb->tc`), the
  rest goes to its cold part (`tb->xemu_cold`), so every reference
  between a TB's two parts stays within the ±1 MiB of B.cond, CBZ, ADR
  and LDR (literal); a TB that overflows either part starts over in the
  next chunk. Det, cross-binary, `bench-det -n 6`: heavy-f1 −7.7%
  Mcycles/frame (5/6, load 16-33), stream-e2 −2.6% (4/6, load 18-29);
  at load 30-41, stream-e2 −6.0% (5/6) and movie-e2 −10.9% (6/6);
  Minsns/frame −0.06..−0.14%, goldens unchanged (release and debug-TCG).
  Eight and sixteen concurrent det instances (load ~90): −5..−11%
  Mcycles/frame each. Windowed heavy-f1 stays at the 60 fps cap (2
  pairs, load 65-90), where the busy-polling vCPU hides any headroom.
  Cycle-weighted samples on a TB's first insn (the entry `bti`) fell
  from 11.5-12.0% to 7.1-7.9% of the vCPU (CPU Profiler, heavy-f1, two
  runs each); L1I and L1I-TLB demand misses per 1K P-core insns read
  −3% at the median, inside their run-to-run spread.
  Keeping the warm stubs beside the body (heavy-f1 per frame: ~2.5K
  ld80d image-path loads, ~2.6K TLB-memo re-walks, ~1.4K store
  prefilters, against ~1.9K slow-path helper calls) cost 3-7% against
  all-cold in a same-binary trial. `-d out_asm` prints the cold code
  as "tb slow paths" at its own address, `XEMU_GUEST_PROF` attributes
  cold samples to their TB, and Linux perf maps now cover bodies only
  (they used to include the slow paths). A region's tail short of a
  chunk is one more, shorter chunk when it is at least 256 KiB: unused,
  it left `-accel tcg,tb-size=2` stress runs 1 MiB of their 2 (det
  stream-e2, 600 frames: 697.7 Mcycles/frame, 382.3 with the tail
  chunk, 380.8 before the layout; load 24-28).

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces, CPU-side quads /
  line loops / triangle fans / provoking-vertex emulation,
  fragment-shader depth fallback.
- **Descriptor / pipeline.** Split sets (UBOs vs textures) with
  last-bound tracking; coalesced multi-binding writes. Dynamic
  blend / depth bias / viewport / scissor / line width / blend
  constants cached per-CB, skipped on match. Pipeline key excludes
  dynamic state and `NV_PGRAPH_CONTROL_3` (routed via `ShaderState` /
  `PROVOKING_VERTEX`). `check_pipeline_dirty` ignores
  `texture_bindings_changed`; set-1 refresh via
  `pgraph_vk_update_descriptor_sets` handles rebinds.
- **Flight slots (N=2) + single-submit fast path.** CPU records
  slot 1 while GPU runs slot 0; aux CB skipped when staging is empty.
- **Texture upload.** Batched + bump-allocator staging; parallel CPU
  decode pool capped at `MIN(nproc, 4)`. Non-compressed 2/4-bpp
  textures use a compute-shader unswizzler. `VkBufferImageCopy`
  regions stack-allocated (bounded by 6 layers × 16 mips).
- **Dirty tracking.** Per-chunk XXH3 on page-aligned textures
  ≥ 256 KiB (saves 30-60% hash CPU on sparse atlas updates). UBO
  `memcmp` before `memcpy`. Surface expiry throttled to host
  wall-clock (~33 ms).
- **`possibly_dirty` cleared after verified bind.** Upstream leaves
  the mark sticky once a page-sharing neighbor sets it, so every
  later bind re-hashed the full contents forever. A bind that
  verifies the cached hash (or re-uploads) now drops the mark;
  guest writes re-mark via the spatial index, and torn-snapshot
  binds keep it. Texture dirty-check + hash time fell ~2.1x
  (1.3-3.5 → 0.6-1.6 ms/flip in-game).
- **VkPipelineCache persisted across runs.** The cache was only
  saved in `pgraph_destroy()`, which a normal app quit never
  reaches — `pipeline_cache.bin` was never written, so every launch
  recompiled all MSL pipelines (measured 164 ms worst single
  `vkCreateGraphicsPipelines` on a cold cache). The PFIFO thread now
  flushes the cache at flip boundaries (≥ 30 s apart, only when new
  pipelines accumulated; write-then-rename keeps the file
  crash-consistent). Warm-cache worst case measured ≤ 4 ms — a ~40x
  hitch reduction on pipeline-heavy scene transitions.
- **`XEMU_NV2A_NSPROF=1` wall-time profiler.** `nv2a/nsprof.[ch]`:
  release-build-safe ns accumulators around every PFIFO-thread cost
  center (shader/pipeline gen, texture hash/snapshot/upload, fence
  waits, MetalFX drain, surface readbacks, the FLIP_STALL → vblank
  idle gap) plus event counters for finish reasons, draws/flip,
  draw-merge classification (`merge_*`), `vkCmdDraw*` call counts,
  and render-pass-end causes (`rpcause_*`); per-5 s summaries to
  stderr. This is what every optimization above was measured with.
  Observation-only (~3 flag stores per draw when off; fps parity
  46.83 ± 0.24 vs 46.59 ± 0.50).
- **Deferred-report idle budget cut 5 ms → 300 µs.** Guests that consume a zpass report
  mid-frame spin-wait on it with an idle FIFO, and the safety-valve
  submit only fired after 5 ms of continuous idle — every such poll
  cost the guest's critical path up to the full budget. A too-small
  budget merely costs an extra small submit per idle episode
  (bounded by report count), so 300 µs is safe. Measured on a 770
  draws/flip scene: 24.04 ± 0.43 → 25.36 ± 0.22 fps (**+5.5%**, all
  pairs positive); artifact soak clean. Hardcoded since 2026-10-15
  (`REPORTS_IDLE_BUDGET_NS`).
- **Vertex-mirror overwrite waits the just-submitted slot.** The
  `VERTEX_BUFFER_DIRTY` conflict path memcpy'd new guest data over
  the mirror right after finish — safe under upstream's synchronous
  finish, a CPU-vs-GPU race after flight-slot pipelining (the fork's
  finish waits only the *previous* slot). The conflict path now
  waits the submitted slot's fence and clears its upload tracking.
  Validated at fps parity vs v0.9 (24.45 ± 0.58 vs 24.48 ± 1.19),
  zero errors.
- **Invalid-surface destruction gated on submission retirement.**
  With pipelined finish, a quarantined surface image could be
  `vkDestroyImage`d while the just-submitted CB still referenced it
  (invalid usage on every driver; upstream's synchronous finish was
  immune). Evictions now carry the highest submission index that may
  reference them; a fence-derived retirement watermark gates
  destruction, and `pgraph_vk_surface_flush` drains all slots before
  its free-everything prune. Reuse of quarantined images needs no
  gate (ordered by the render pass's `VK_SUBPASS_EXTERNAL`
  dependency — proof comment at
  `get_any_compatible_invalid_surface`). Validated at fps parity
  (46.6-47.2 vs 46.83 ± 0.24), zero errors.
- **Texture/sampler eviction gated on submission retirement.** Third
  member of the same pipelined-finish race family (audit find, no
  observed crash): the LRU pre-evict guard only protected entries
  referenced by the *currently-recording* CB, but the just-submitted
  CB routinely still executes, and the memory-budget trim runs right
  after submit with that clause disabled — an evicted texture or
  sampler could be destroyed while the GPU still referenced it. Both
  caches now refuse eviction until `retired_submit_count` covers the
  entry's last-referencing submission (samplers gained the stamp
  field), with a slot-fence drain if a pool ever fills with pinned
  nodes and a drain in texture finalize so a live renderer switch
  can't leak refused nodes. Validated at fps parity (F5 +0.19 ± 0.14,
  3 interleaved pairs; F8 quiet-machine +0.12 ± 1.46, sign-mixed)
  with a 291-capture artifact soak clean.
- **Zeta shape-switch fast path.** A depth buffer ping-ponging
  between two shapes at one VRAM address every frame cost ~9
  `pgraph_vk_finish` fence cycles per flip (8-19 ms/flip): each
  switch did a full GPU→CPU readback, an eviction finish, and a
  multi-MB seed re-upload. Re-shaped zeta targets are cleared by the
  guest before use and the genuine RAM consumers measured zero, so
  the switch now skips the readback and seed upload and defers the
  eviction without a finish (old image quarantined until the
  recording CB rotates). Pending CPU-requested downloads are still
  honored.
- **Byte-exact vertex-conflict refinement.** Guest dirty bits are
  page-granular, so vertex-stream sync writes arrive page-padded and
  consecutive writes false-share their boundary page with the
  recording command buffer — each false conflict forced a finish
  (submit + rotation + mid-frame fence reclaim). A per-page
  written-span table + span-restricted memcmp proves most conflicts
  byte-identical and skips the finish (conservative: any differing
  byte keeps legacy behavior). Measured via the savestate A/B
  harness (`scripts/bench-savestate-ab.sh`, 3 interleaved pairs,
  baseline reproducibility ±0.02 fps) on a heavy in-game scene
  (408 draws/flip @ ~20 fps): forced finishes 5.65 → 0.37/flip
  (−93%), fence wait −11%, process CPU −7.6%, **fps +5.4%**.
- **Targeted vertex-RAM conflict wait.** Guest writes into VRAM
  pages uploaded by an *in-flight* slot forced a full finish
  (submit + slot rotate + fence) 3-6x per flip on streamed vertex
  data. When the conflict is with an already-submitted slot, only
  that slot's fence is waited (usually already signaled) and its
  upload tracking cleared; the full finish remains only for
  conflicts with the currently recording command buffer. Combined
  with the zeta fast path: medium scenes went 22-26 → 60 flips/s
  (guest vblank cap), heavy scenes ~22 → ~26-30, and per-flip
  fence waits fell from 8-19 ms to 1-3.5 ms.
- **Once-per-frame texture verification.** A binding verified (or
  uploaded) earlier in the same guest frame skips the per-bind
  dirty-bitmap scan + hash on re-binds unless re-marked
  possibly_dirty (~85% of the ~1100 binds/flip take the skip; the
  residual is genuine animated-content rehashing). A CPU write
  landing between two binds of the same texture within one frame
  takes effect one frame later; render-to-texture is unaffected
  (surfaces don't take this path).
- **No hidden GL contexts under the Metal backend.** The 4 startup
  gloffscreen contexts (1 Vulkan-renderer + 3 GL-renderer) are
  skipped when presenting via Metal; live renderer switches away
  from Vulkan are deferred to the next launch (the settings UI
  already prompts for a restart), matching the window's fixed
  SDL_WINDOW_METAL type.
- **Texture VRAM spatial index.** Active `TextureBinding`s bucketed
  by 128 KiB VRAM ranges; `pgraph_vk_mark_textures_possibly_dirty`
  scans only touched buckets instead of all 65536 LRU bins. Lazy
  init on first dirty signal; seeded from active LRU; maintained
  via insert/remove on cache miss / eviction. Whole-VRAM signals
  (renderer flush) take a one-pass LRU walk instead of visiting
  every bucket (spanning textures appear once, not per-bucket).
- **MoltenVK configuration is launch-path independent (pink-tile
  fix).** The `MVK_CONFIG_*` tuning previously lived only in
  `Info.plist` `LSEnvironment`, which macOS applies to Finder
  launches exclusively — terminal launches ran library defaults, so
  automated testing could never see Finder-only bugs. The canonical
  values are now set in `main()` before MoltenVK loads (explicit
  env still overrides). Along the way,
  `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` moved from `2` to `0`:
  with whole-frame command buffers, immediate prefill encoding
  corrupted streamed textures (screen-wide magenta blocks, ~5% of
  frames in streaming-heavy areas), enabled an AGX visibility-buffer
  crash, and measured *slower* (46.3 vs 48.5 fps — encoding work
  landed on the PFIFO thread).
- **Maintained MoltenVK build** (`scripts/build-moltenvk.sh`):
  pinned upstream commit, `-O3`, per-machine `-mcpu`, arm64-only,
  installed to `/usr/local/lib` where `build.sh` prefers it and
  logs the bundled version + UUID (provenance). Validated at fps
  parity with 65 newer upstream fixes and half the binary size.
- **Crash fixes in the new query path**: bulk query-pool resets +
  in-pass rotation could exhaust the per-slot query partition
  mid-frame (release builds strip the old assert; MoltenVK then
  wrote through a nil visibility offset) — a capacity guard now
  submits before exhaustion and the pool grew 1024→4096. A
  per-command-buffer primer query keeps MoltenVK's visibility
  buffer attached regardless of which pass sees the frame's first
  zpass draw.
- **Texture snapshot copy-until-clean.** Guest writes landing
  mid-copy previously shipped one torn (Morton-tiled magenta)
  frame and repaired on the next bind; the copy now retries until
  no write tears it (bounded, self-consistent rehash).
- **Snapshot robustness.** Failed snapshot loads (controller USB
  topology drift — a pad asleep or re-enumerated since the save)
  no longer leave the VM stopped ("frozen"); the game resumes and
  the error is surfaced. Emulated pads with no host binding report
  neutral input instead of aborting, which also enables
  topology-matched snapshot loading in automation.
- **In-pass occlusion queries + deferred zpass reports (2.4x in
  report-heavy scenes).** Two coupled changes, measured together on
  a heavy in-game savestate (3 interleaved pairs): 15.78 →
  **38.57 ± 0.80 fps** (+144%), render passes **376 → 14 per
  flip**, fence waits 2.2 ms/flip, 0 stalled finishes. (1)
  Per-query `vkCmdResetQueryPool` (illegal inside a render pass)
  tore the pass down on every query rotation — ~one full tile
  load/store per draw on Apple GPUs; the slot's query partition is
  now bulk-reset at CB begin and queries begin/end *inside* the
  pass. (2) The FIFO-idle STALLED path did a full submit + GPU sync
  per pending report (23+/flip); engines consume last frame's
  counts, so reports now ride the next natural submission and
  deliver at slot reclaim, with a continuous-idle fallback submit
  for guests that truly spin-wait (see the idle-budget bullet).
- **Query-pool drain at slot reclaim.** Occlusion queries are
  partitioned per flight slot; each submission's queries + pending
  guest reports are handed to the slot at submit and drained when
  the slot fence is reaped — `pgraph_vk_finish` no longer blocks on
  `vkGetQueryPoolResults(WAIT_BIT)` for the work it *just*
  submitted (which defeated the 2-slot pipeline on every finish in
  zpass-report titles). Guest-facing paths (`STALLED`,
  `REPORTS_FULL`, `FLUSH`, and since 2026-10-10 the query-capacity
  guard's `QUERIES_FULL`) still drain synchronously so report
  polling can't deadlock and savestate/renderer-switch stay
  consistent.
- **LRU free-list + true-LRU eviction.** `lru.h` keeps free nodes on
  a dedicated list (O(1) allocation) and `global` holds only in-use
  nodes, so eviction starts at the true LRU entry instead of
  scanning a mixed list from the tail — O(cache size) per eviction
  before, relevant for the texture cache and the 50k-entry shader
  module cache.
- **Texture-upload CPU trims.** Post-snapshot full re-hash skipped
  when the dirty re-check proves no guest write landed during the
  snapshot memcpy (the snapshot is then bit-identical to what the
  incremental chunk hash covered — also keeps the stored hash
  comparable with the chunked composition); `TextureLayout` (~5 KiB)
  reuses a single scratch instead of g_malloc0/g_free per upload.
- **Tight barriers.** Precise stage masks replace
  `ALL_COMMANDS_BIT`; exact byte ranges replace `VK_WHOLE_SIZE`;
  coherent-memory flushes skipped via per-buffer `is_coherent`.
- **Renderer-switch hardening.** AB-BA-safe lock ordering on
  GL↔VK toggle; atomic `flush_pending`; `pgraph.lock` dropped across
  `framebuffer_released` wait.
- **Monotonic sync clock.** `pgraph_vk_sync` uses
  `QEMU_CLOCK_REALTIME` (CLOCK_MONOTONIC — suspend / NTP slew don't
  stall the 8 ms gate; see "Monotonic present-path clocks" below).
- **Fence-wait diagnostics.** The fence-wait wrapper warns at 5 s and
  aborts at 60 s with the named call site instead of hanging silently on
  MoltenVK internal-mutex deadlocks (5 s alone killed a startup upload
  at host load 212).
- **Narrowed GPU syncs.** `pgraph_vk_upload_surface_data`'s full
  sync only fires when the target is the active color/zeta binding
  or a CB is recording; otherwise MoltenVK's single-queue order +
  aux-CB fence wait suffice. `invalidate_overlapping_surfaces`
  collects into a 64-entry stack array in one pass (heap fallback
  for >64).
- **Narrowed remapped-attribute staging.**
  `pgraph_vk_bind_vertex_attributes` shifts
  `vertex_attribute_offsets[i]` by `min_element * stride`;
  `remap_unaligned_attributes` copies only
  `[min_element..max_element]`. Draws rebase via `firstVertex -
  min_element` / `vertexOffset = -min_element`.
- **Draw-path dedup caches** (memcmp against last CB-scoped value,
  skip on match, reset on CB begin):
  - `vkCmdBindVertexBuffers` (buffers + offsets)
  - `vkCmdBindIndexBuffer` (buffer + offset + type)
  - `vkCmdPushConstants` (inline uniform attrs, keyed on layout)
  - Vertex layout snapshot (attribute + binding descriptions;
    replaces two `fast_hash` passes per draw)
  - Scaled `surface_binding_dim`
- **`surface_ranges` insert / remove.** `lower_bound` insert
  (O(log n)) and cached `surface_range_slot` remove (O(1)) replace
  linear scans. One-shot `fprintf` on invariant break for release
  observability.
- **Shader-uniform pull gated on `shader_uniform_inputs_dirty`.**
  Set by `pgraph_reg_w`, every `ltctxa/b/c1/vsh_constants` writer,
  every `vertex_attributes[].inline_value` writer, GL texture
  rebinds, renderer switch. `bind_shaders` skips
  `update_shader_uniforms` when clean.
  `pgraph_update_inline_value` memcmp's before write.
- **Correctness.** Frame-interpolation state (saved prev/cur
  IOSurface pair + remaining count) is dropped on display resize so
  deferred interpolation can't present stale-resolution frames.
  Screenshots/thumbnails query the framebuffer through
  `GL_TEXTURE_RECTANGLE` on the IOSurface path (previously
  `GL_TEXTURE_2D` returned zero dimensions → broken captures).
  Partial-channel clear updates cached scissor +
  blend constants so subsequent same-CB draws on the pipeline
  fast-out path don't inherit the overrides. PVIDEO non-coherent
  flush uses `row_bytes * in_height` (exact CPU-written range)
  instead of `yuv_size` (short for odd `in_width`). GPU-unswizzle
  upload emits WAR fences on `BUFFER_COMPUTE_DST` /
  `BUFFER_COMPUTE_SRC` between iterations and once up-front.
  `create_texture` snapshots guest VRAM once into a grow-only
  scratch on `PGRAPHVkState`, hashes + uploads from the snapshot,
  and re-checks the dirty bitmap afterwards; closes the CPU-CPU
  tear race between guest writes and the renderer memcpy that
  otherwise showed as single-frame Morton-tiled magenta flashes
  on 2/4-bpp textures under heavy contention. GPU-unswizzle levels
  point `TextureLevel.decoded_data` directly into the snapshot,
  dropping the per-level intermediate copy.
- **Surface CPU-access-callback churn was measured — and it was the
  guest's TLB-flush source (instrumentation + dark mechanism,
  2026-08-04; both since removed).** Every NV2A surface create/destroy calls
  `mem_access_callback_insert`/`remove`, and each of those was an
  `async_safe_run_on_cpu` (an exclusive-execution vCPU stop) **plus** a
  full all-mmuidx `tlb_flush_all_cpus_synced` **plus** an unconditional
  `tcg_flush_jmp_cache`. Renderer-initiated TLB flushing had never
  appeared in any profile and nobody had read the counters, so two
  auditors disagreed by two orders of magnitude about the rate.
  The census (`XEMU_SURFACE_CB_STATS=1`, removed 2026-10-15; the ranged
  refill does not move `info jit`'s flush counters, so revert the
  removal commit to recount) settled it: **199.8 callback events/s =
  4.16 per flip** in the steady F8 scene, attributed to
  `update_surface_part`'s invalidate-then-create path (the zeta
  ping-pong), against **~190 full-mmuidx flush calls/s** measured
  independently — a **0.95:1 ratio, i.e. the surface callbacks are
  essentially *the* guest TLB-flush source in-scene**, each one also
  wiping the jump cache. A dark reuse pool shipped alongside the census
  (`XEMU_SURFACE_CB_REUSE=1`, 4 entries keyed on `(vram_addr, size)`):
  it parked the live registration on unregister and re-attached it when
  an identical-key surface reappeared. **Measured 2026-08-04 (same day,
  idle-gated A/B) and killed: −0.66 fps 3/3 negative** — see the
  Failed table; it stayed in-tree dark as the record until its code was
  removed on 2026-10-10 (implementation in `804bc5d87e`). The live shape was
  the `// FIXME: flush only applicable pages` ranged invalidation in
  `system/physmem.c` / `cputlb.c`, landed 2026-10-15 as
  `tlb_flush_host_range` (roadmap item 1). Measurement gotcha
  recorded with them: HMP `info jit`'s **`TLB full flushes` counter is
  structurally unreachable on this guest** — it only increments when
  `to_clean == ALL_MMUIDX_BITS`, and the Xbox target has
  `NB_MMU_MODES=22` against plain i386's 8, so the flush arrives as a
  partial. Use the partial + elided counters, not the "full" line.
  (`hw/xbox/nv2a/pgraph/gl/surface.c` carries the identical churn and
  was left untouched — the GL renderer is not the shipping path, so this
  verdict is Vulkan-only until someone mirrors it.)
- **SPIR-V disk cache keyed on the shader compiler (2026-08-04).** The
  cache directory was keyed on the xemu version alone, so a glslang bump
  served the *previous* compiler's blobs out of any warm cache — which
  makes a compiler-change A/B invalid by construction, not merely
  inaccurate. The directory is now
  `spirv_cache_v<x.y.z>-glslang<major.minor.patch>` (the version macros
  come from glslang's generated `build_info.h`, already on the include
  path). Costs a one-time cold recompile on first launch after the
  change (~671 shaders, lazy, 2-10 ms each); no prune is bundled.
  Residual gap, recorded in the code: a glslang *revision* bump that
  keeps the same version number is still not distinguished — carrying
  the wrap revision needs a build-system define, and Linux distro
  packaging goes through `dpkg-buildpackage` with no `build.sh`, so the
  same tree would key two ways on two platforms.
- **That key now asks the linked glslang, not a header (2026-09-08).**
  Reading the version from `<glslang/build_info.h>` keys the cache on
  whichever header wins the include path, which is not necessarily the
  one the linked library was built from: `build/compile_commands.json`
  puts `-I/opt/homebrew/include` ahead of the subproject's
  `__CMake_build/include`, so a Homebrew glslang names the directory
  while the binary links the subproject's. Observed on the dev Mac as a
  `spirv_cache_v0.13.2-glslang16.5.0` directory produced by a build
  whose glslang checkout was still 16.2.0 — i.e. the exact stale-blob
  failure the key exists to prevent, wearing the right name. The key
  now comes from `glslang_get_version()` on the linked library
  (`glslang_c_interface.h:249`, backed by `glslang::GetVersion()`),
  formatted `glslang<major>.<minor>.<patch><flavor>` — byte-identical
  to the old macro expansion for the same numbers, so every warm cache
  survives. Hosts where no `build_info.h` was reachable used to key on
  `glslang-unknown` and pay one cold regeneration now. CI/release
  builds have no Homebrew glslang and were never affected; the
  same-version-different-revision gap above is unchanged. Related:
  `build.sh` now warns when a subproject checkout has drifted from its
  `.wrap` (Build + packaging), which is how the 16.2.0-vs-16.5.0 skew
  went unnoticed.
- **The SPIR-V cache stopped trusting what it reads (2026-09-08).**
  Cherry-picked from `818fe27828` on `windows-wave1-wip`, written for a
  native-Windows profile path, with the 2026-09-08 review's fixes folded
  in; it changes behaviour on every platform. Four parts. (a) The cache
  opens its files with `qemu_fopen()` — `_wfopen()` on a UTF-16 path on
  Windows, an inline `fopen()` everywhere else — so a non-ASCII
  `%APPDATA%` stops silently missing the cache forever (the path comes
  from `SDL_GetPrefPath` as UTF-8 and Windows `fopen()` reads it in the
  process ANSI code page). (b) Stores go to `<hash>.spv.<pid>.tmp` and
  are `g_rename`d over the entry (the pipeline cache's discipline), so a
  kill mid-write or two instances compiling the same shader cannot leave
  a torn `.spv` behind; `.tmp` files orphaned by a crash are swept once
  per process, when the cache directory is first resolved. (c) Every
  blob read from disk is validated — size ≥ 5 words and a multiple of 4
  (`VUID-VkShaderModuleCreateInfo-codeSize-08735`), the SPIR-V magic,
  an instruction-stream walk over `<word-count:16><opcode:16>` headers
  that rejects a zero count or one that runs past the last word, and a
  last instruction of `OpFunctionEnd` — and only a blob that survives
  that is handed to SPIRV-Reflect and then to `vkCreateShaderModule`;
  any rejection deletes the file and recompiles instead of aborting the
  process. (d) `debug_shaders` adds a `-dbg` suffix to the cache
  directory, so a debug run stops poisoning later normal runs with
  unoptimized SPIR-V and toggling the option stops no-oping against a
  warm cache. Parts of (c) are the review's fix, in two rounds. The
  branch's claim that reflect-parsing a cached blob turns a torn file
  into a regeneration was false, because SPIRV-Reflect's bounds check is
  `assert(InRange(...))` in `ReadU32()`/`ReadStr()`
  (`subprojects/SPIRV-Reflect/spirv_reflect.c:367`, `:416`) and it is
  compiled with asserts live — nothing in `build.sh` or `meson.build`
  sets meson's `b_ndebug`, and the only `-DNDEBUG` among the 3017
  entries of `build/compile_commands.json` is tomlplusplus's — so a
  truncated `.spv` could abort inside the parser on every launch and was
  never deleted. The first round added the walk; measuring the walk then
  refuted *it* as a complete answer, because a truncation landing on an
  instruction boundary walks clean: over 34,774 word-aligned truncations
  of five warm entries, 20.8% survived the walk, and SPIRV-Reflect
  crashed on 15% of those and returned SUCCESS (i.e. handed a torn
  module to the driver) on 23%. Hence the trailer rule, which costs
  nothing and is sound from the spec: SPIR-V's logical layout (§2.4)
  puts function definitions last and any module with an entry point has
  at least one function, so a complete module always ends in
  `OpFunctionEnd`. The reflect parse stays, as the second gate for a
  blob that is structurally well-formed but wrong. Hatch:
  `XEMU_SPIRV_CACHE=0` (bypass); validation is unconditional by design — a cache the
  process cannot trust is worse than no cache. No base-tag bump, so
  existing valid entries stay warm on every platform; only a `-dbg` run
  starts cold. Not a perf change: the added work is one linear pass over
  a blob that is read once per shader per process. Measured 2026-09-08:
  zero false rejects on 448 real modules (the dev Mac's 394 warm
  `v0.13.2-glslang16.5.0` entries, 46 older `v0.13.1` ones, and
  `glslangValidator -g`/`-gV`/`-gVS` output for the `debug_shaders`
  option set), all of which end in `OpFunctionEnd`; and of all 2,416,107
  word-aligned truncations of those 394 entries the two rules reject
  99.85%, with SPIRV-Reflect returning an error — never SUCCESS, never
  an abort — for all 3,719 function-boundary cuts that survive, so every
  one is deleted and recompiled. The orchestrator's landing check is to
  force-truncate a cached `.spv` and confirm the entry is deleted and
  regenerated instead of aborting the launch.
- **glslang's built-in symbol tables warm at renderer init
  (2026-10-16).** glslang builds them on the first compile for each
  (version, profile, SPIR-V, source), every stage at once, under a
  global lock: 57-73 ms, which every process paid on the PFIFO thread at
  its first SPIR-V cache miss (PMI: a 166 ms e2-walk frame with the vCPU
  88% idle, and inside the transition's load frame). Every Vulkan shader
  is `#version 450` with the same compile inputs, so
  `pgraph_vk_init_glsl_compiler` compiles and drops a throwaway
  `#version 450` shader on a short-lived thread, joined before
  `glslang_finalize_process`. A miss during those ~70 ms waits on
  glslang's lock for the rest, which is what a cold cache's first
  compile at renderer init does (~10 ms after the start: no change
  there). Boot-primed SPIR-V cache, stream-e2 walk, 2 runs per arm
  (load 20-30): first in-game compile 64.2/68.6 → 2.5/2.8 ms, load
  interval `shader_gen` max 64.8/69.3 → 7.9/6.8 ms, load frame
  336/332 → 265/249 ms; renderer init within noise (180-196 ms both).
  The 67 SPIR-V blobs of a cold run match byte for byte. No switch: the
  output is identical.
- **Guest-driven bounds checks are real in release, on every platform
  (2026-09-08).** `nv2a_vk_bounds_check()` used to compile to
  `if (!(x)) __builtin_unreachable();` whenever `NV2A_VK_PERF_BUILD` was
  1 — which it always was (the macro was replaced by `NV2A_VK_ASSERTS` on
  2026-10-10). That is
  not a stripped check but a licence for the optimizer to assume the
  bound, so a guest-controlled violation (a surface size out of
  `NV_PGRAPH` registers exceeding the staging buffer, a register field
  wider than the table it indexes) became a silent OOB read or a write
  into host-visible mapped memory instead of a diagnosable failure. It
  is now one definition in every build (`debug.h:106-113`): a
  `G_UNLIKELY` never-taken branch plus an `abort()` whose message names
  the failing expression *and* its `__FILE__:__LINE__`. 28 call sites
  are affected — 8 in `texture.c`, 9 in `draw.c`, 11 in `surface.c`
  (`grep -n nv2a_vk_bounds_check hw/xbox/nv2a/pgraph/vk/*.c`) — none on
  the per-draw hot path, and the cost is exactly what upstream's plain
  `assert()` costs at the same sites, because QEMU never builds with
  `NDEBUG` (`include/qemu/osdep.h:311-312` `#error`s on it). This is an
  **ungated behaviour change on macOS too**: the 2026-09-08 review
  counts six table-index sites whose register field is wider than the
  mapped table, where a title that previously read a neighbouring
  `.rodata` word now terminates loudly instead. Upstream parity was
  chosen over clamp-and-warn (owner-visible beats silent), and the
  landing evidence is the multi-title corpus soak
  (`scripts/boot-smoke-corpus.sh` over Azurik, BF2MC, Conker, KOTOR,
  Vexx, NevolutionX). There is deliberately **no env hatch** — a
  guest-driven OOB must not be switchable at runtime; the restore path
  is compile-time, by editing that single macro, which reads no build
  switch (`NV2A_VK_ASSERTS` governs only `nv2a_vk_assert`).
- **volk device-level dispatch on non-Apple (2026-09-08).** With
  `volkLoadInstance()` alone every device entry point is the Vulkan
  loader's trampoline, which re-derives the ICD dispatch table from the
  handle on each call; `volkLoadDevice()` after device creation
  (`hw/xbox/nv2a/pgraph/vk/instance.c:708`) replaces those globals
  with `vkGetDeviceProcAddr()` results that enter the ICD directly.
  Sound because the renderer creates exactly one `VkDevice` (a second
  would need `volkLoadDeviceTable()`); the only extension entry points
  the tree calls through volk are `vkGetMemoryWin32HandleKHR` /
  `vkGetMemoryFdKHR`, whose extensions are required, `VK_EXT_debug_utils`
  stays on volk's instance loader, `vkExportMetalObjectsEXT` is resolved
  separately, and VMA builds its own table with `volkLoadDeviceTable()`
  regardless. Hygiene, not a measured win. Its `XEMU_VK_VOLK_DEVICE=0`
  hatch was removed on 2026-10-16. **Apple is untouched** — the whole
  block is `#ifndef __APPLE__`, MoltenVK is the ICD and there is no
  trampoline to bypass.
- **`VK_KHR_external_semaphore_win32` is optional, not required
  (2026-09-08).** The Windows GL-interop present path exports only
  memory (`vkGetMemoryWin32HandleKHR` + `glImportMemoryWin32HandleEXT`)
  and synchronises on the Vulkan side with fences; nothing in the tree
  imports or exports a semaphore handle, so a device lacking the
  extension must not be rejected. It moved out of
  `required_device_extensions[]` and into
  `add_optional_device_extension_names()` (`instance.c:383-392`), which
  keeps device creation byte-identical on every ICD that does offer it.
  Windows only.
- **LRU eviction skips out-of-use nodes again (2026-09-08).**
  `lru_try_evict_one()` regained the `lru_is_node_in_use()` predicate
  (`include/qemu/lru.h:156`), which is now semantically identical to
  upstream xemu's `include/qemu/lru.h`. Upstream needs it because its
  single `global` list mixes free and in-use nodes; in this fork's
  split-list layout `global` holds in-use nodes only, so it is a no-op
  while that invariant holds — which is why the 2026-09-08 review
  exonerated it as the Windows exit-139 cause. `lru_lookup()` also
  asserts the node it takes came off `free` (`lru.h:227`).

- **Query-capacity guard moved ahead of per-draw setup (2026-10-10).**
  The guard that submits before a flight slot's occlusion-query
  partition runs out ran in `begin_draw`, after `begin_pre_draw` had
  built the draw's framebuffer and descriptor sets. Its finish destroyed
  the slot's framebuffers and moved to the next slot, so
  `begin_render_pass` then read `framebuffers[framebuffer_index - 1]`
  with the index at 0 (the slot's `VkSemaphore` handle, passed to
  MoltenVK as a `VkFramebuffer`) and `bind_descriptor_sets` bound
  `descriptor_sets[base - 1]`; an inline draw's vertex and index data
  had also already gone into the previous slot's staging range. The
  guard now runs first thing in `begin_pre_draw`, so the code after it
  builds all of that for the new slot. It finishes with its own reason,
  `QUERIES_FULL` (counter `NV2A_PROF_FINISH_QUERIES_FULL`, nsprof
  `finish_queries_full`), which drains like `REPORTS_FULL`, so a
  forced-capacity run (`XEMU_MAX_QUERIES=64`) shows which of the two
  guards fired. The 2026-07 validation count covered both and most
  likely never reached this path, which would almost certainly have
  crashed. Found by the 2026-09 Windows audit (confirmed finding 1, plan
  batch 3; the batch's other items are still open). With the default
  4096-query pool the guard did not fire in normal play.

- **`nv2a_vk_assert()` is live in debug builds (2026-10-10).** Since
  `9834214c58` (2026-04) the header hardcoded `NV2A_VK_PERF_BUILD 1`,
  which compiled the renderer's ~80 internal invariant checks out of
  every build, `--debug` included; the choice was never measured, and
  the comments calling the asserts "debug-only" were false. The macro is
  now `NV2A_VK_ASSERTS`, which follows `XEMU_DEBUG_BUILD` the way
  `NV2A_STRIP_PROFILE_COUNTERS` already does: `./build.sh --debug` gets
  real `assert()`s, release builds compile them out, and
  `-DNV2A_VK_ASSERTS=1`/`=0` overrides either way. The stripped form is
  `((void)sizeof(!(x)))`, so a stale assert expression still fails to
  build. Release machine code is unchanged (the assembly of `draw.c`,
  `surface.c` and `texture.c` compiled with the release flags is
  byte-identical before and after). Before enabling them, an audit of
  every call site fixed the three that would have fired on correct
  behaviour or were undefined-behaviour hints (`bf002d5fa8`) and the
  query-capacity guard bug (`a825c1ff07`). About 62 of the sites match
  live `assert()`s in upstream xemu, including some that check
  guest-driven values, so a debug build now aborts where upstream's
  release build does; the published `-dbg` bundles become
  abort-on-invariant builds.

- **Surface uploads stage in the current flight slot (2026-10-10).**
  `pgraph_vk_upload_surface_data` copied the surface into
  `BUFFER_STAGING_SRC` at offset 0, which is flight slot 0's partition,
  whatever slot was current. `pgraph_vk_finish` does not wait for the
  command buffer it submits, so slot 0's texture copies could still be
  reading that range on the GPU while the CPU `memcpy` overwrote it: a
  host-write race Metal's hazard tracking cannot see (garbled textures,
  most likely right after a finish). It now stages at the top of the
  current slot's partition, which the finish that selected the slot
  already waited out: texture uploads grow upward from the slot base and
  are never pending here, and PVIDEO stages at the base for a compositor
  command buffer that is not fence-waited. Every staging offset (copy
  regions, barriers, flush) follows, and the bounds check is against the
  slot, not the whole buffer. Found by the
  2026-09 Windows audit (batch 4, item 1).

- **Texture VRAM-end checks are live in release (2026-10-10).** Three
  guest-driven bounds in `vk/texture.c` sat in `nv2a_vk_assert`, which
  release builds compile out. For the two dirty checks
  (`check_texture_dirty` and the fork's large-texture chunked check),
  QEMU's core already caught part of it: `physical_memory_test_and_clear_dirty`
  asserts the range stays inside its RAM block, live in release because
  QEMU refuses `NDEBUG`, so a texture straddling VRAM's end already
  aborted there. What nothing stopped was a texture lying wholly past
  VRAM, and the out-of-bounds host reads of `vram_ptr + offset` by the
  texture hash, snapshot copy and decoder that follow these checks. The
  upload's staging check is the only bound on a decoded texture larger
  than a 256 MiB slot. All three are now `nv2a_vk_bounds_check` (31 sites
  in all), live in every build. Upstream keeps `check_texture_dirty`'s
  bound and a staging-size bound as live `assert()`s; the chunked check
  is fork-only. `check_texture_dirty`'s off-by-one is also fixed: its end
  is exclusive, so a texture whose last page is VRAM's last page is
  legal (`<=`; upstream still has `<`). A malformed texture now aborts
  loudly instead of reading past VRAM, the same choice as the 2026-09-08
  bounds-check change. `pgraph_vk_mark_textures_possibly_dirty` keeps
  its debug-only assert: it only compares ranges.

### MetalFX + presentation

- **Metal-native presentation backend** (macOS). New
  `display.window.presentation_backend = auto | opengl | metal`;
  `auto` selects Metal when the Vulkan renderer is configured. The
  main window becomes `SDL_WINDOW_METAL` + `CAMetalLayer`
  (`ui/xemu-metal.m`): the NV2A present frame is handed off as an
  IOSurface/`MTLTexture` (`nv2a_get_present_frame`, new
  `get_present_frame` renderer op) and composited with an MSL gamma
  blit; ImGui renders through `imgui_impl_metal`; the entire
  GL blit hop + `glFlush` + CGL rebind disappears from the present
  path. The complete UI (mask decals, SDF logo, render-to-texture
  panels, thumbnails, screenshots via GPU readback) has a Metal twin
  in `ui/xui/metal-helpers.mm`; the GL path remains fully
  selectable (`presentation_backend = 'opengl'`) and is the only
  path for the OpenGL NV2A renderer (switching renderer away from
  Vulkan under Metal prompts for a restart).
- **Push-model present handoff** (**default ON since 2026-07-12**,
  owner promotion; Metal backend; the pull handshake remains the
  fallback for GL, headless and before the first publish). The published slot is a ring
  that also carries frame interpolation's paced sub-flip steps (see
  the schedule-publish record later in this entry). Measured quiet
  2026-07-11 under 2x interpolation (F8, 90 s arms): the pull path
  blocks the UI thread **499-607 ms per 5 s interval** (1.4-1.6k
  `present_wait` events, 3.3-3.9 ms/flip, 5 ms tails) — with the ring
  the counter never fires (**0 ms**), flips identical (29.8-31.3 vs
  29.4-31.1/s). Ring refuter: 149,846 steps peeked / 60,104 compared /
  0 mismatches / 0 resurrections (5.5 min at 2x; 4x also clean).
  **Pacing fix (same day):** the promotion surfaced a strict-FIFO
  consume flaw once the v0.11.1 speedups pushed step rates past the
  display rate (40 fps × 2x = 80 steps/s vs 60 Hz): the ring
  backlogged and force-dropped unread steps (~26/s measured under a
  60 Hz-simulated consumer), splicing `midpoint→midpoint` sequences —
  seen as doubled/juddering frames. The consumer now bounds its step
  debt (interp mode + 2) and catches up to the newest unconsumed
  real frame: unread drops **2,253/2,152 per minute → 0** (2 pairs),
  policy inert when the display keeps up, refuter clean over the
  policy (0 mismatches / 0 resurrections; `78c4573dfb`). The numbers
  below are the original interpolation-off landing.
  The pull-model handoff above (`nv2a_get_present_frame`) costs a
  guaranteed cross-thread round trip per UI frame: the UI kicks the PFIFO
  thread and blocks on `qemu_event_wait` until it answers, and that
  handshake is also what *publishes* the frame — so the UI cannot skip an
  unchanged frame without first paying for the round trip. Under the flag
  the PFIFO thread composites and publishes the complete present tuple
  (texture/IOSurface + retain, shared event + value, `frame_seq`, dims)
  into a mutex-guarded ring at flip (`pgraph_vk_flip_stall`); the UI reads
  it with **no kick and no wait** and skips re-compositing when `frame_seq`
  is unchanged (the dedup the pull model structurally couldn't do). This
  is a latency/jitter change, **not** an fps change — guest flip/vblank
  timing is untouched. Measured on F5, quiet machine, interleaved 3-pair
  A/B (35 intervals/arm): the new `present_wait` nsprof counter drops
  from **81.2 ms/interval (sd 40, max 157 ms) across ~599 UI-thread
  blocks per interval to 0.000 across every push interval** (a one-time
  startup burst before the first publish, then the pull fallback never
  fires); **flips/s identical** (50.0 ± 6.6 vs 51.4 ± 5.6, the scene's
  own 38-60 bimodal range). The refuter (`XEMU_PUSH_PRESENT_REFUTE=1`)
  cross-checked **48,151** equal-`frame_seq` pushed-vs-pull reads over
  65 s with **0 mismatches** (plus 47,151/0 in the loaded-machine run).
  Startup / post-resize / GL fall back to the pull path unchanged.
- **Async MetalFX via `MTLSharedEvent`** (Metal backend only). The
  three `waitUntilCompleted` stalls (spatial/temporal/interp,
  1-5 ms/frame on the PFIFO thread) are replaced by a monotonic
  shared-event signal; the UI present pass encodes a GPU-side wait
  on the frame's value before sampling. The GL backend keeps the
  synchronous waits (GL cannot wait on `MTLSharedEvent`).
- **Async `render_display`** (Metal backend only). The compositor
  pass no longer blocks the PFIFO thread in
  `vkWaitForFences(aux_fence)` — the last synchronous GPU wait on
  the present path. A timeline `VkSemaphore` (exported as an
  `MTLSharedEvent` via `VK_EXT_metal_objects`, one-time probe with
  sync fallback) is signaled per compositor submit; MetalFX command
  buffers and the UI present pass order GPU-side against it.
- **Dedicated compositor command-buffer ring.** The async compositor
  originally shared the per-slot aux CB + single aux fence, so the
  next aux use (texture/surface uploads, staging sync) reclaimed the
  compositor's fence on the PFIFO thread — measured 1.4-1.9 ms/flip
  in heavy scenes. The compositor now submits through its own 2-deep
  CB ring; an entry is reclaimed only when its slot recurs two sync
  intervals later (measured: 1.4-1.9 ms/flip → ~0.2 µs/flip), and
  upload paths never wait on compositor work at all.
- **IOSurface-free present chain** (Metal backend only). When
  MoltenVK can export the `MTLTexture` backing the compositor
  `VkImage` (probed at init), no IOSurface is created at all: MetalFX
  consumes the exported texture directly and the UI receives it
  through the present-frame handoff. This removes the macOS 26
  IOSurface constraint from the *input* side too, so **MetalFX
  engages at `surface_scale = 4`** (2560x1920 input, previously
  skipped). IOSurface remains the automatic fallback and the GL
  path.
- **Frame interpolation quality.** The interpolator is driven
  properly: presentation queue shows interpolated frames *before*
  the real frame they lead to, paced at `frame_period / mode`
  (forward-backward motion fixed); a zero-filled motion texture is
  no longer bound ("nothing moved" made moving objects ghost-blend —
  `nil` enables MetalFX internal motion estimation);
  `fieldOfView`/`aspectRatio` are set and synthetic luminance depth
  is bound at interpolation resolution; in temporal mode the
  interpolator is **linked to the temporal scaler**
  (`MTLFXFrameInterpolatableScaler`) and inherits its real
  motion/depth/history; 4x re-presents the cached midpoint instead
  of re-encoding the same pair (which violated the interpolator's
  history contract); a hitch guard skips interpolation and resets
  history on frame-gap spikes instead of blending across content
  jumps.
- **1920px output cap lifted** (Metal backend only). MetalFX outputs
  rotate through rings of 3 private `MTLTexture`s instead of one
  shared IOSurface — no IOSurface means the macOS 26 BGRA
  `bytesPerRow` bug doesn't apply, so upscaling targets the
  CAMetalLayer's pixel size (aspect-fit), e.g. 1280×960 → 2880×2160
  on a 4K panel instead of 1920×1440. The ring also makes frame
  interpolation receive genuinely distinct prev/cur frames (the
  single shared output surface aliased them).
- **Spatial + temporal upscaling** (`MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`). Temporal uses 8-frame Halton(2,3) jitter
  + optional compute-kernel synthetic depth. Shared `MTLDevice` +
  `MTLCommandQueue` refcounted with MRC discipline.
- **Frame interpolation** (`MTLFXFrameInterpolator`, macOS 26+):
  2× = 60 fps, 4× = 120 fps from 30 fps; deferred generation on
  idle syncs.
- **Teardown safety.** Per-subsystem in-flight counter drains before
  releasing. IOSurface cache keyed on `IOSurfaceGetID`; output width
  capped at 1920 under the GL backend (macOS 26 BGRA `bytesPerRow`
  bug).
- **`MTLFXFrameInterpolator.deltaTime` in real seconds**
  (`QEMU_CLOCK_REALTIME` between input `CFRetain`s, clamped
  `[1/240, 1/10]` s) instead of a unitless ratio. Reduces ghosting
  on fast pans.
- **Exclusive fullscreen picks the highest refresh** among the
  native-resolution modes (120 Hz instead of a 60 Hz first entry);
  Metal vsync maps to `CAMetalLayer.displaySyncEnabled`.
- **GPU-paced interpolation steps.** Published frames carry a
  sequence number + intended hold duration; the UI presents new
  content with `presentDrawable:afterMinimumDuration:` and skips
  re-presenting unchanged frames during gameplay — exact
  `frame_period / mode` step timing instead of quantizing to the UI
  loop (±8 ms at 120 Hz). Menus keep full-rate rendering.
- **Monotonic present-path clocks.** Sync gate, interpolation
  timestamps, step pacing and surface expiry moved from
  `QEMU_CLOCK_HOST` (gettimeofday — reflects NTP changes) to
  `QEMU_CLOCK_REALTIME` (CLOCK_MONOTONIC); an earlier pass had the
  two semantics inverted. The guest-vblank IRQ timer was already a
  dedicated fixed-60 Hz monotonic-deadline thread, fully decoupled
  from the present path.
- **Opt-in real depth for temporal** (`XEMU_MFX_REAL_DEPTH`):
  feeds the temporal scaler the NV2A zeta (standard-Z) in place of
  synthetic luminance depth, with automatic fallback when the format
  is rejected or no zeta matches the scaler's input dims. `2` blits
  the display-matching zeta into a dedicated exportable snapshot at
  FLIP_STALL, recorded into that frame's command buffer so it rides
  the same submission (one depth copy per flip, ~2 µs PFIFO record
  time), giving depth that is temporally correct for the presented
  frame; it ships dark for A/B. Mode `1` (exporting every zeta and
  reading the live one, a frame late by present time) was removed on
  2026-10-15. Engagement is title-dependent: a guest that renders 3D
  at 2× the scanout width (AA super-width, e.g. Azurik) exposes no
  zeta matching the scaler input, so it falls back to synthetic
  there.
- **MetalFX resize quantization (default on, 2026-08-04).** The Metal
  backend's MetalFX output size tracked the live layer size with only
  even-pixel rounding, so dragging a window rebuilt the spatial/temporal
  scaler and its 3-texture ring at up to the event rate — and every
  rebuild is preceded by a `metalfx_drain_inflight()` that blocks the
  PFIFO thread. The output width now snaps to a 64-px ladder and a new
  shape must hold for ~200 ms before it is adopted; the present blit
  aspect-fits the residual mismatch, and a guest mode change still
  rebuilds immediately with no cooldown. An explicit floor keeps the
  quantized width above the guest surface width, so quantization can
  never silently drop out of the upscale path. Measured on a scripted
  drag: **90 → 7 scaler rebuilds (a 92% cut)**, read from the
  `mfx_scaler_rebuild` nsprof counter. Honest caveat on that receipt:
  `osascript` drives the window at ~2.7 events/s against a hand drag's
  ~120 Hz, so the legacy arm's 90 rebuilds is a **floor**, not a typical
  case — the real-world cut is larger, and the number is quoted as the
  bound it is.
- **UI-thread present instrumentation (2026-08-04).**
  `XEMU_NV2A_NSPROF=1` now time-attributes the three UI-thread costs the
  push-present default left unmeasured — `drawable_acq`, `ui_hud_lock`,
  `ui_frame_dt` — plus `ui_present` and `mfx_scaler_rebuild` event
  counts. Zero cost with the profiler off, and no bespoke build is
  needed to answer the HUD-lock or drawable-acquire questions. Companion
  knob: the main-loop-mutex + BQL occupancy line that lived behind
  `DEBUG_XEMU_C` is now runtime-enableable with `XEMU_UI_LOCK_STATS=1`
  (one predictable branch per lock when off; Windows/Linux release paths
  preprocess away unchanged). Both exist because the ImGui-under-lock
  rework needed a number before anyone built it — it got one, and the
  number closed it (Settled vectors).

### MCPX APU

- **Voice processing.** 128-byte voice struct memcpy'd to stack;
  `voice_get/set_mask` hits the local copy. Paused voices skip the
  memcpy entirely (peek PAUSED / STEREO from guest RAM first).
  LUTs replace transcendentals: attenuation (4096), pitch (65536),
  LPF cutoff (65536), envelope decay / release (1024 each, linear
  interp, branchless clamp).
- **Batched voice register reads.** VBIN / FMT / VOLA / VOLB / VOLC:
  one word-load per register + constant-mask bit extracts; replaces
  a ~14-call `voice_get_mask` fanout per active voice per frame.
  3D voices skip VBIN V0..V3 (overwritten from `hrtf_submix[]`).
  Same batching in `voice_get_samples` (runs inside the resampler
  callback: 9 CFG_FMT fields from one word-load) and
  `get_voice_bin_src_dst` (10 reads → 2 word-loads per queued
  voice). The paused-voice peek loads FMT/STATE directly off the
  precomputed `voice_addr` instead of two `voice_get_mask` round
  trips.
- **Mono LPF skip.** Mono voices carry identical data in both
  channels and both use the FCA register; the DLS2 SVF now filters
  ch 0 only and mirrors the result, halving SVF cost on the common
  mono-voice case (ch0==ch1 invariant kept for HRTF / monitor).
- **HRTF FIR linearization.** `hrtf_filter_process` linearizes the
  circular history once per frame (two-span memcpy + input append)
  so the 31-tap convolution runs over contiguous memory with no
  per-tap modulo — the `% HRTF_BUFLEN` indexing defeated the
  autovectorizer and dominated 3D-voice CPU time. Frames with settled
  parameters take a hand-NEON pass over outputs; frames whose
  parameters still move first run the smoothing into a per-output
  coefficient table, then take one too (an older gather-over-taps NEON
  lost to the autovectorizer). Other hosts keep the C forms.
- **SSE2 mix kernels (non-Apple x86_64).** The vDSP mixbin
  accumulate, `float_accumulate`, and `float_to_24b_bulk` now have
  SSE2 equivalents so Windows / Linux x86_64 builds get the same
  vectorized audio mix path (float-domain clamp before
  `cvtps2dq` matches NEON/lrint saturation semantics).
- **SVF coefficient cache.** `setup_svf` short-circuits when
  `(fc, q, filter-type)` is unchanged — skips `sqrtf` + stores on
  the common voice-per-channel-per-frame path.
- **Mono libsamplerate.** 1-channel `src_callback_new` for mono
  voices (was always 2-channel with duplicated output); format
  changes trigger `src_delete` + recreate. Halves the resampler's
  inner loop for the common mono case.
- **ADPCM block reuse.** Each voice keeps the ADPCM block it decoded
  last, bytes and samples (`MCPXAPUVoiceFilter.adpcm`, host state).
  `voice_get_samples` still reads the block from guest RAM on every call
  and decodes only when the bytes, size or channel count differ: the
  resampler pulls 32 samples at a time and a block holds 64, so each
  block was decoded twice (half the decodes hit on heavy-f1 and
  stream-e2). The decoder keeps each channel's predictor and step index
  in locals (one loop for mono, one for stereo) instead of arrays
  indexed by channel. A malformed or short block is decoded over zeros,
  so its unwritten samples still play silence as the zeroed stack
  buffer gave, not the voice's previous block. Det host instructions per
  frame -0.14% on heavy-f1 and -0.42% on stream-e2 (6/6 pairs each),
  -2.2% inside the APU tick.
- **Resampler loops per channel count.** `linear_src_callback_read`
  takes the channel count as a constant, and `voice_src_read` builds it
  once for mono and once for stereo, out of line (inlining both copies
  into `voice_worker_run` cost more instructions than it saved). Same
  operations in the same order, fused where they were. Det host
  instructions per frame -0.27% heavy-f1 and -0.16% stream-e2 on top of
  the ADPCM reuse (6/6 pairs each), -2.0% inside the APU tick.
- **No utilization clock in deterministic mode.** `se_frame` read
  `QEMU_CLOCK_REALTIME` twice per frame for the utilization the debug
  UI and `XEMU_APU_PROF` show, which means nothing while frames run in
  bursts on the vCPU thread. Deterministic mode skips it unless
  `XEMU_APU_PROF` is set: ~4k fewer instructions per APU tick.
- **Accelerate / vDSP.** An inline fused loop (Apple Silicon; `vDSP_vsma`
  on Intel) for 8-bin × 32-sample mix accumulation, skipping zero-gain
  sends, `vDSP_vadd` for `float_accumulate`; `float_to_24b`
  uses `vcvtnq_s32_f32` in bulk on ARM64. Single-precision SVF/LPF.
  `SRC_LINEAR` resampler matches Xbox hardware.
- **Atomic consistency.** `d->regs[]` writers, `voice_locked[]`,
  `pause_requested`, `NV_PAPU_FEMEMADDR` load, and `VPVADDR` /
  `FENADDR` reads in lock-free VP paths all use `qatomic_*` so the
  VP frame thread and the lock-free MMIO dispatcher agree
  under weak ordering. `fe_method`'s register reads (FECV / FEAV /
  list tops / VPSGEADDR / VPSSLADDR / FETFORCE1) follow the same
  contract.
- **DSP execution engines (post upstream merge).** Upstream added a
  DSPOps engine abstraction with two engines: the C interpreter
  (`dsp/interp/`) and a JIT wrapping the external `dsp56300` Rust
  subproject (`audio.use_dsp_jit`, **default on** — fork-owned;
  upstream flipped it off in `fc13b78060` and the fork restored it
  2026-09-08, see the Windows native port section). This fork's
  inline ARM64 JIT lives *inside* the interpreter engine
  (`interp/dsp56k_jit_arm64.c`, symbols `dsp56k_jit_*`); when
  supported and enabled it takes precedence and `use_dsp_jit` is
  ignored. Since 2026-10-16 `audio.dsp_jit.enabled` defaults to true,
  so a stock config runs the fork JIT on AArch64 POSIX hosts (Apple
  Silicon, Linux arm64) and upstream's dsp56300 engine elsewhere
  (x86_64, Windows).
- **Full inline DSP JIT (Apple Silicon).** ARM64
  basic-block JIT for both DSP56300 cores, GP and EP. On by default
  since 2026-10-16; `[audio.dsp_jit] enabled = false` (or the Settings
  toggle) turns it off, the only switch.
- **Per-core retranslation auto-throttle.** A sustained-churn window
  (>1/16 of block executions retranslating) permanently hands that
  core back to the interpreter; a safety net, which neither core
  trips. The EP's old churn (~1 retranslation per 10 block
  executions, blamed on P-space overlays) was block bookkeeping: one
  owner block per PC, so a mid-block entry evicted the covering block
  and the two rebuilt each other. From `d287c6cb23` until blocks
  could overlap (2026-10-15), the EP ran on the interpreter.
- **JIT coverage + mechanics.** 100% ALU and 99.99% CF inlined
  (only `emu_undefined` and a few side-effect-register `bit_manip`
  variants stay on BLR); `pm_read_accu24` inlined for the scaling=0
  case universal on Xbox. A/B/X/Y/SR pinned in callee-saved ARM64
  regs. Static block chaining (with retro-chaining: untranslated
  targets get a patchable chain site, patched when the target is
  later translated) keeps hot loops inside JIT code; all chain
  repatching sits inside the JIT-write window (W^X), and
  invalidation-time chain *unpatching* now also flushes each
  repatched site's icache line (the write window covers permission,
  not coherency — the stale branch could otherwise keep executing
  from I-cache). Lazy-flag elimination elides ~20% of SR updates. Harnesses:
  `XEMU_DSP_JIT_DIFF=N` (off-thread bit-exact validation, bounded
  per unique translation), `XEMU_DSP_JIT_STATS=1` (counters). See
  [docs/Finished/dsp-jit-design.md](Finished/dsp-jit-design.md).
- **`dsp_core_t` reordered into the ARM64 imm12 window (2026-08-04).**
  The JIT-hot scalars — `loop_rep`, `pc_on_rep`, the `interrupt_*`
  block, `num_inst`, `cur_inst_len`, `cur_inst`, and the two JIT flags —
  sat *behind* 122 KB of `xram` / `yram` / `pram` / `pram_opcache` /
  `mixbuffer` / `periph`, at offsets **78,736-80,049**, so every
  reference from generated code needed a two-instruction `ADD` + access
  sequence instead of a single `LDR`/`STR`. Moving them ahead of `xram`
  puts all of them **below offset 450** — inside the immediate window
  for every access width (4095 B for `LDRB`/`STRB`, 8190 B for
  `LDRH`/`STRH`, 16380 B for word) — and the `emit_*_any` emitters pick
  the one-instruction form with no emitter change, removing 8 synthesis
  instructions from the unconditional per-op epilogue. Mandatory coupled
  edit, made in the same change: `dsp_state_diff()`'s offset range table
  is cut from field adjacency, and a stale table would have produced an
  inverted `periph` range and a silently false-green DIFF validator.
  Guard against regression: width-scaled `QEMU_BUILD_BUG_ON`s in
  `dsp56k_jit_arm64.c` now fail the build if any hot field escapes its
  window again. Receipt against the pre-registered −10 to −14% band:
  `XEMU_DSP_JIT_STATS` **`code_buf_used` −12.79%** versus a
  directly-comparable historical run (same tag, same duration; GP core
  only — the EP core always auto-throttles and its `code_buf` is 0).
  `blocks_translated` moved −4.87% (1313 → 1249), accepted as
  cross-binary session variance because the correctness oracle is clean:
  **`XEMU_DSP_JIT_DIFF=1` failures = 0 on both cores** (GP checked=566,
  EP=31) and 6/6 on the xbox unit suite. **No fps change is claimed or
  expected** — the APU thread runs ~20% utilized and off the frame
  critical path; the win is generated-code footprint and i-cache
  locality. No `XEMU_*` escape hatch, deliberately: the mechanism is a
  compile-time struct layout, so a runtime toggle would mean carrying
  two struct layouts and two sets of emitted offsets — strictly more
  risk than the change it would guard. The hatch is the single-commit
  revert, with `XEMU_DSP_JIT_DIFF=1` as the oracle. Deliberately NOT
  folded in, as its own pre-registerable candidate: swapping `yram`
  ahead of `xram` (yram's base at 16836 is the one array offset still
  outside the 16380 word window, costing an extra `ADD` on every emitted
  Y-space access) — shipping it here would have contaminated the
  pre-registered `code_buf_used` band.
- **One exit-word test per DSP op (2026-10-16).** The JIT's per-op
  epilogue ran ~27 host instructions of bookkeeping (31 inside a DO
  loop), 53% of DSP JIT cycles on det heavy-f1. `interrupt_state`,
  `interrupt_counter`, `interrupt_pipeline_count`, `is_idle` and
  `jit_exit_block_request` now fill one aligned 8-byte word
  (`jit_exit_word`) tested with `LDR X` + `CBNZ`; a non-zero word runs
  the old checks in the old order, out of line after the block's exit,
  with the REP helper and the loop edge. The emitters record what an
  op's body may change (stores to `pc`, `cur_inst_len`, `instr_cycle`,
  `loop_rep`; any BLR outside a whitelist of memory/peripheral,
  `pm_read_accu24`, `rnd56` and `calc_ea`'s slow call). An op that
  can't move pc stores it from a constant and drops the main-line PC
  check (88% of executed ops; the REP, loop-edge and interrupt paths
  keep it), compares LA against a constant, and adds a constant to
  `num_inst` when `instr_cycle` can't change (82%); `loop_rep` is tested
  only on a block's first op and around ops that may set it (13.5%). A
  typical in-loop op's epilogue is 11 instructions. Every check fires
  at the same op as before. Det, RAM equal in every arm: heavy-f1 1200,
  18 pairs in 2 sessions (load 12-47), instructions −1.26% per frame
  (18/18), cycles paired median −0.95% (11/18, noisy); stream-e2 600,
  8 pairs, instructions −1.18% (8/8), cycles within noise. xctrace PMI
  on det heavy-f1: DSP JIT 3.93% → 3.09% of the vCPU, bookkeeping
  2.09% → 1.26%. `XEMU_DSP_JIT_DIFF=1` failures=0 on heavy-f1,
  stream-e2, cold boot and a normal-mode run; translations unchanged
  (GP 44 / EP 86). Not done: LA-specialised blocks (the LA load and
  compare are 0.27% of the vCPU after this).
- **DSP DMA copy loops keep the scratch pointer in a register
  (2026-10-16).** QEMU builds with `-fno-strict-aliasing`, so every
  `uint16_t`/`uint32_t` store through the static `scratch_buf` could
  alias it and `dsp_dma_run` reloaded it per element. The loops now use
  a local copy and are specialised on `item_size` instead of switching
  per element; masking, shifts and the `mem_write` path for P-space
  targets (which invalidates JIT blocks) are unchanged. Det, RAM equal:
  instructions −0.15% per frame on heavy-f1 (8/8), −0.10% on stream-e2
  (7/8), about −2% on a cold boot (DSP program loads, 3/3); cycles
  within noise.
- **Guest voice handles bounds-checked; APU RAM dirty marking behind a
  per-platform default (2026-09-08).** Cherry-picked from `971292ed48`
  on `windows-wave1-wip` (APU half; the XID half lands separately).
  Guest-controlled voice handles reached host arrays unchecked:
  `voice_get_mask` / `voice_get_word` / `voice_set_mask` indexed
  `d->vp.filters[]` (256 entries) with a raw handle, and the HRTF / SSL
  setters read `NV_PAPU_FECV` — any 32-bit value the guest wrote — into
  a signed `int`, so `>= 0x80000000` passed the `<` bound and indexed
  negatively. Handles are now range-checked; an out-of-range one is
  dropped (reads 0, writes discarded) and reported once per run on
  stderr, deliberately *not* by falling through to the guest-RAM path,
  whose `address_space_*` fallback would take the BQL from the APU /
  voice-worker threads under `d->lock` — the inverse of `gp_write`'s
  BQL-then-`d->lock` order. The voice-list walk stops on an
  out-of-range `NEXT_VOICE_HANDLE` (once-per-run stderr report) instead
  of running into the assert in `voice_process`, i.e. a
  guest-triggerable abort in a release build. Alongside: the APU frame
  thread calls `rcu_register_thread()` (it enters read-side sections
  through `scatter_gather_rw` and the `ram_st*` helpers), and the
  runtime `audio.use_dsp_jit` engine switch takes the BQL across
  `dsp_set_engine()` so it cannot free a backend under the lock-free
  `gp_read` / `ep_read`. That switch is reachable: the Settings and
  debug UIs bind the toggle directly (`ui/xui/main-menu.cc:936`,
  `ui/xui/debug.cc:210`) and `se_frame` polls it every APU frame. It is
  inert only when the fork's own inline JIT is on
  (`audio.dsp_jit.enabled`), which makes `dsp_set_engine()`
  short-circuit — the dev box's config, which is why the hazard went
  unnoticed. Restored with it: the fork's direct-`ram_ptr` voice
  stores mark the written range dirty again instead of silently
  skipping `invalidate_and_set_dirty()`. That is the one hunk a
  correctly-behaving guest can observe, so it is gated: default
  **off on Apple** (unchanged from what ships today; the per-store cost
  — an RCU read lock, i.e. two real `smp_mb()`s without
  `CONFIG_MEMBARRIER`, plus up to four contended bitmap ORs, from every
  voice worker on every voice-register store — has never been measured
  here) and **on elsewhere**, in the style of the sub-page dirty gate
  `0aeb1998c3`; a build-time choice since 2026-10-15. **Not taken:**
  the same commit's `int16` saturation of the modulated pitch-LUT
  index. The branch removed it again in `3495cd28c0` on a single-run
  Windows bisect, and `40f0a01ad3` retracted that bisect as
  non-evidence, so nothing is known about the clamp either way; the
  original wrapping index stands until there is a real receipt. No fps
  claim: this batch is a correctness fix. On Apple the store path is
  unchanged and every in-range handle behaves exactly as before; what
  is new on all platforms is the range test in the three accessors, the
  frame thread's RCU registration, and the locking around the engine
  switch.
- **DSP JIT dead code removed (2026-10-10).** Ten functions that never
  had a caller: seven ARM64 emitter primitives (`emit_ldr_x_imm`,
  `emit_asr_w_imm`, `emit_tbz_w`, `emit_orr_x_reg`, `emit_adds_x_reg`,
  `emit_sxtb_w`, `emit_br`; added unused in `1288fae90f`, `4dfbae90d7`,
  `5e40a982ef`), the `dsp56k_jit_sentinel_enabled`/`_force_enabled`
  accessors (the knobs were read through their statics, so
  `XEMU_DSP_JIT_SENTINEL`/`XEMU_DSP_JIT_FORCE` kept working; both
  removed 2026-10-15, the harness is in `1f444fa272`/`4668947cc9`)
  and the `dsp56k_jit_helper_update_rn` shim. No generated-code
  change.
- **Voices run on the APU frame thread (2026-10-16).** Four voice
  worker threads summed a frame's voices in partitions while the frame
  thread waited for them holding `d->lock`, which `vp_write` takes on
  the vCPU. A frame's voices are 10-15 µs of work; each worker woke 1500
  times a second for ~0.5% user time and 70-90% sys, and the frame
  thread's broadcast-and-wait took 23-27 µs a frame. `voice_work_dispatch`
  now runs the partitions in order on the frame thread and sums them in
  that order, as deterministic mode already did, so the floats are the
  same. `audio.vp.num_workers` still sets the partition count; its
  default is 4 in both modes (normal mode took `MIN(cores, 4)`), so a
  realtime frame mixes exactly as a deterministic one. Windowed, muted,
  30 s windows, ABBA, load 31-74: process CPU heavy-f1 184.1/182.3% →
  170.3/175.0% (−10.5 points), W2 168.7/172.7% → 157.4/161.5% (−11.2);
  the APU's threads 15.1% → 4.5% of a core (the frame thread 6.8% →
  4.5%, its sys 2.9% → 0.7%); `XEMU_APU_PROF` 9-10% → 3.5-4.1% at 1500
  frames/s. xwx on heavy-f1 (load 18-21): the vCPU's `d->lock` waits in
  `vp_write` 1.12% → 0.52% of wall (mean 39 → 17 µs, max 951 → 321 µs),
  the frame thread's broadcasts 45k → 154 per 30 s. Normal-mode
  `XEMU_AUDIO_WAV` captures show no gaps. Deterministic goldens unchanged.
- **Fewer fixed costs per APU frame (2026-10-16).** Three costs ran
  every frame whether or not anything used them. The debug snapshot
  (an 18.5 KB copy of `g_dbg` and a 256-voice reset, 1500 times a
  second) now runs only while the Audio debug window reads it (each
  `mcpx_apu_get_debug_info` call re-arms a 3000-frame countdown) or a
  voice is isolated (`voice_should_mute` reads it); `voice_work_dispatch`
  reads the realtime clock only for that window too. Each voice
  partition records the mixbins it wrote (`bins_written`), so a frame
  clears and sums only those, straight into the frame's zeroed mixbins.
  Exact: an unwritten bin is +0, and adding +0 changes nothing for a
  mixbin, which starts at +0 and only accumulates, so is never -0 (the
  silent-send skip relies on the same). Det, RAM and WAV equal:
  instructions per frame −0.70% heavy-f1 (12/12 pairs), −0.82% cold
  boot, −0.76% movie-e2 (6/6). Cycles, both binaries run at once so
  load swings cancel (load 18-19): heavy-f1 median −0.57% (8/8 pairs),
  cold boot −0.67% (7/8); sequential sessions −0.67% heavy-f1 (12 pairs)
  and −2.8% cold boot (10 pairs, load 16-26, ±3% per pair).
- **DSP DMA copies RAM spans in bulk (2026-10-16).** `dsp_dma_run`
  tested for a span (plain X/Y RAM) on every word; it now tests once
  per transfer. With one, a 4-byte read is a `memcpy`, a 4-byte write a
  masked loop the compiler vectorises, and the 2-byte and interleaved
  forms index the span directly. P-space and peripheral targets keep
  the per-word backend calls in the old order (P writes invalidate JIT
  blocks). Det, RAM and WAV equal: instructions per frame −0.37%
  heavy-f1 (12/12 pairs), −0.45% cold boot (9/10), −0.50% movie-e2
  (6/6). Cycles, both binaries at once (load 18-20): heavy-f1 median
  −0.33% (5/8), cold boot −0.04% (4/8); sequential sessions −1.2%
  heavy-f1 (10/12 wins), −0.4% cold boot (load 16-26).
- **DSP JIT chains fall-throughs, calls, DO entries and loop rewinds
  (2026-10-16).** `XEMU_DSP_JIT_STATS` now files each return to the
  dispatcher under a reason and counts chain-site passes. Per 1200
  heavy-f1 frames the GP entered blocks 5.99M times and the EP 1.99M
  (~19 host cycles each). The GP's exits: taken bit-test branches
  (`BRSET`, never chainable) 31%, DO-loop rewinds to a loop start in
  another block 31%, not-taken two-word branches 10% (the length
  classifier calls `Bcc xxxx`, `JCLR` and the like one word, so their
  fall-through never matched), calls 8%, returns 10%; the EP's: rewinds
  to the block's own start 47%. Now a block-ending op ends in chain
  sites for every static successor (`jit_static_succ`): the taken target
  of each branch and call with a static one (bit tests and mode-6 `_ea`
  forms included), the fall-through of the conditional ones (a second
  compare where the first site's pc check fails) and of DO / DOR /
  ENDDO. The op at the LA of the DO loop that ran when its block was
  translated chains a rewind to that loop's start, its own block
  included (LC bounds it). Each site sits behind the exit-word test and
  a pc compare and is registered for unpatching. Entries per 1200
  heavy-f1 frames: GP 1.00M, EP 0.40M (−83%, −80%); what still exits is
  returns and two-word mode-6 `movep`/`move` ops that end a block. Det,
  RAM and WAV equal: instructions per frame −0.77% heavy-f1 (16/16
  pairs), −1.22% cold boot (14/14), −1.11% movie-e2 (6/6). Cycles, both
  binaries run at once so load swings cancel: heavy-f1 median −0.26%
  (mean −0.50%, 6/8, load 18-21), cold boot −0.62% (mean −0.71%, 8/8,
  load 16-19); in sequential sessions cold boot −0.74% and −1.09% (10
  and 14 pairs), heavy-f1 +0.4% and −0.95% (12 and 16 pairs, load
  16-47). `XEMU_DSP_JIT_DIFF=1` failures 0 (chaining is off under it);
  `XEMU_DSP_JIT_PIN_AUDIT=1` silent with chaining on. A chained run
  still stops where `dsp_run`'s budget stops dispatching one block at a
  time: each block's chain entry compares `num_inst` with
  `jit_chain_limit` (four instructions) and leaves before its first op
  once the budget is spent. Without that bound a frame wait polled
  across two blocks (`jset #1,x:$ffffc5,go` / `jmp`, or the `jclr`
  form) never came back from `dsp_run`, so the APU frame thread spun
  holding `d->lock` and the emulator hung, and where a run stopped
  depended on which sites were patched (xbox suite
  `/corpus/budget-poll-*`). The bound costs +0.12% instructions per
  frame (det heavy-f1 and stream-e2, 4/4 pairs each, load 29-57); in
  normal mode a tenth of the dispatcher entries are budget exits. The
  auto-throttle (more than 1/16 of
  dispatcher entries retranslating) counts a chained run once, so the
  same churn now weighs about five times as much; Azurik's cores stay
  far below it.
- **DSP JIT blocks survive DSP resets and loads (2026-10-16).** DSP
  reset, bootstrap, opcode-cache flushes and `sync_from_vm` flushed the
  whole JIT cache, so every harness restore (the APU reset flushes, the
  load's sync again) retranslated all of it: over 41 restores × 16
  frames, 1848 GP and 3612 EP translations against 44 and 86 without
  restores. They now call `dsp56k_jit_revalidate_all`, which marks the
  cache; the next block lookup compares each live block's P dependency
  words (`[pc_start, dep_end)` and its `bake_mask` words, copied at
  translation into `dep_words`) with P memory and evicts only the blocks
  that differ, before any block runs, so chains still join verified
  blocks only. The same loop: 44 / 86 translations, no flushes, 42
  revalidations keeping all 1804 / 3526 blocks, RAM digests equal in
  every candidate and to the old binary. Per-candidate fixed cost
  (`xemu-run checkpoint` at L = 1..64 frames, linear fit, 2 runs each,
  load 23-25): 72.96 → 68.37 Minsn (−6.3%), cycles 44.3 → 40.0 Mcyc
  (−10%, noisy). 500 restores with `XEMU_DSP_JIT_DIFF=1`: failures 0,
  one RAM digest; goldens and det WAVs unchanged. `XEMU_DSP_JIT_DIFF`
  turns chaining off, so it can't see the one way kept blocks could
  change a replay: where a chained run stops. Chained runs stop at
  `dsp_run`'s budget (`jit_chain_limit`), so a restore runs as the
  first pass did whatever the cache holds; without that bound a counted
  loop of two blocks ran to its end in the first `dsp_run` after a
  restore instead of stopping where the first pass did (xbox suite
  `/corpus/budget-replay`).

### Input

- **XID pad `index` is bounded before it reaches `bound_controllers[]`
  (2026-09-08).** The `index` qdev property is a `uint8`
  (`hw/xbox/xid-gamepad.c:249`) but selects a slot in the 4-element
  `bound_controllers[]` (`ui/xemu-input.h:118`), and
  `xemu_input_get_bound()` (`ui/xemu-input.c:742`) does not range-check
  it — so a hand-written `-device usb-xbox-gamepad,index=4` read past
  that array, and the fork's `if (!state)` guard cannot be relied on to
  reject what comes back before `update_output()` writes through it.
  `update_input()`/`update_output()` now go through
  `xid_get_bound_controller()` (`hw/xbox/xid.c:53`), which returns NULL
  for an out-of-range index, and both realize hooks reject one up front
  via `error_setg` (`hw/xbox/xid-gamepad.c:177`) so a mistyped `-device`
  line fails loudly instead of producing a silently dead pad. Neither
  path is reachable from the app: `index` is only ever set by
  `xemu_input_bind()` (`ui/xemu-input.c:830`), whose callers all pass
  0..3, and for 0..3 behaviour is unchanged on every platform.

- **In-app rebinding UI polish** (Input tab → Input Mapping; the
  remap table itself is upstream's). Esc cancels an in-progress
  capture (previously the only exits were pressing an input or
  unplugging the controller), right-click on a binding unbinds it
  (persisted; unbound entries read as never-pressed), capture rows
  say what they're waiting for, and a hint line documents the
  gestures. Controller buttons rebind to other controller buttons,
  stick/trigger axes to other axes, every Xbox control has a
  keyboard binding, and per-stick axis inversion toggles sit under
  the table — all per-controller (keyed by SDL GUID) and saved to
  `input.gamepad_mappings` / `input.keyboard_controller_scancode_map`
  in xemu.toml.

### Threads + runtime

- **Memory-access callback list is RCU-protected (2026-10-11).** Fixes
  the recurring SIGSEGV in `flatview_read_continue` under
  `ohci_frame_boundary` / `ohci_service_ed_list`, most often just after
  a `loadvm` (four crash reports 2026-10-10/11, plus the 2026-08-04
  wave's excluded pair). NV2A surfaces register CPU-access callbacks in
  `cpu->mem_access_callbacks`; every `flatview_read_continue` /
  `flatview_write_continue` walks that list, so device DMA on the main
  loop reads it (OHCI once per 1 ms frame, a dword at a time). Since
  upstream xemu `966115336a` (PR #2320) entries are removed by
  `async_safe_run_on_cpu` work, which runs with the BQL dropped, and
  the old `QTAILQ_REMOVE` + `g_free` could free the node a main-loop
  walker stood on; `surface_access_callback` dropping the BQL to wait
  for PFIFO mid-walk widened the window to milliseconds. The list now
  uses the RCU variants and `g_free_rcu` (`13afd5f244`), and xemu's
  `qemu_main` thread, which runs the main loop but was never an RCU
  reader, registers itself (`4161ab5f24`). Upstream xemu master has the
  same code.
- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE`, and so does the vCPU thread: the RR
  thread in deterministic mode, the MTTCG thread in normal mode since
  2026-10-15 (`94a722acd8`; windowed heavy-f1 37.2 → 58.3 fps, W2
  41.5 → 54.6, 4/4 each at load 13-64; `XEMU_VCPU_QOS=0` leaves it at
  the default class).
  Since 2026-10-16 the APU frame thread does too (its voice workers did
  until the voices moved onto it, see MCPX APU).
  The frame thread holds `d->lock` across each frame (8-frame bursts
  per EP period) and `vp_write` takes it on the vCPU; at the default
  class (PRI 29-31, partly on E-cores) host load preempted it inside the
  lock. Windowed heavy-f1/W2 at load 76-94, xwx interposer, 4/4 runs:
  the vCPU's contended `d->lock` waits fell from 3.3-12.5% of wall
  (mean 0.28-1.34 ms) to 0.83-0.92% (mean 32-55 µs); `XEMU_APU_PROF`
  utilization 34-48% → 9-10% at 1500 frames/s. Windowed fps shows no
  change at moderate load: `bench-abx` heavy-f1 49.2 → 59.7 over 2 pairs
  (load 25-56, but one baseline run dipped to 40.9) and 57.9 → 57.1
  over 3 pairs (1+/2−, load 12-26); W2 60.09 → 60.08 at the cap (load
  12-14). The gain is the lock-wait cut under heavy load, at no CPU
  cost. No switch: a scheduling hint, and det mode runs the APU on the
  vCPU.
- **Normal mode halts the vCPU in the kernel's idle loop
  (2026-10-16).** KiIdleLoop never executes HLT: it spins on `sti;
  nop; nop; cli` and the DPC-list and NextThread checks, and every STI
  ends a TB. The round-4 profile put 35-67% of the windowed vCPU there
  (heavy-f1 35-38%, W2 36%, the movie 67%), with the vCPU at 97-99%
  CPU in every scene. Deterministic mode already halted there; normal
  mode now runs the same byte match and checks in
  `helper_xemu_ki_idle`, spins 50 µs of host time per idle span (calls
  under 5 µs apart), then halts as HLT does and wakes through the
  stock kick. Same binary, `XEMU_KI_IDLE_HALT=0` against the default,
  windowed at the 60 fps cap: bench-abx heavy-f1 60.08 → 60.14 fps
  (2/2 +), process CPU 179 → 153% (load 26-33); W2 58.67 → 60.13 fps
  (2/2 +), 161 → 125% (load 43-64). Over five scenes, two runs each,
  load 13-29: process CPU W2 163-169 → 128-130%, stream-e2 walk
  161-162 → 111-122%, transition 153-154 → 88-107%, movie 133-135 →
  59-66%, and the vCPU thread from 98% to 76-79% (heavy-f1), 62-63%
  (W2), 28-30% (movie). Every heavy-f1 run at the cap: 184% (10 runs)
  → 156% (14 runs). The guest-EIP sampler's spin samples at idle EIPs
  (JIT code and the dispatch loop) fell from 22% of the vCPU's to
  0.7%; 31% now sit in the halt wait. Frames over 20 ms did not rise
  in quiet runs (stream-e2 walk 17 → 12 over four walks; transition 14
  → 9 over the last two pairs, load 14-18). Under heavy GPU contention
  from other processes (fence waits 1.1-2.0 s per 5 s) the transition
  hitched more (122 off against 224 and 351 on): likely a halted vCPU
  waiting for a core while other processes' user-interactive threads
  hold them. Under CPU load alone it did not: a review re-run at load
  22-40 had transition frames over 20 ms 34 → 19 over five pairs (no
  pair worse), process CPU 158 → 95%, and bench-abx heavy-f1 176 →
  150% (2 pairs, load 19-44). A halt wake costs 6-36 µs on average,
  with millisecond tails at load 30+. Rejected budgets and variants: 0 µs saved ~9
  points more CPU on heavy-f1 (6 runs) but halted up to 4700 times/s,
  a wake on every short wait; 200 µs cost ~5 points more than 50;
  halting at once within 20 µs of a halt wake, a 20-50 µs span gap and
  spinning while PFIFO is busy each cost CPU for no frame-time gain.
  Goldens unchanged (release and debug-TCG); x87 refuter clean; xbox
  suite 7/7; PROMPT-7 hooks 97 PASS, gdb tests only the known
  z3/z2/zirq failures; a normal-mode loadvm soak (6 cycles, PFIFO
  heartbeat advancing) and a 9-minute windowed heavy-f1 → transition →
  movie session ran clean. `XEMU_KI_IDLE_HALT=0` keeps the spin
  (normal-mode switch: the goldens never take the budget path).
- **Deterministic mode's FP brackets write FPCR only when it differs.**
  Device work on the vCPU thread (the inline PFIFO pusher, ~360 runs
  per frame on heavy-f1, and the APU tick) ran between `fegetenv`,
  `fesetenv(FE_DFL_ENV)` and `fesetenv(&guest)`: two FPCR and two FPSR
  writes each time. `hw/xbox/host_fp.h` reads FPCR and writes the
  default only when TCG left another x87 rounding mode in it (0 of
  430k pusher runs on heavy-f1 1200), puts it back only if it changed,
  and leaves FPSR alone: nothing on the vCPU thread reads it. Other
  hosts keep `fenv`. A pusher run costs ~75 fewer cycles (thread
  counters around it, both arm orders, 8/8 pairs), ~0.1% of det
  heavy-f1 cycles.
- **BQL discipline.** `surface_access_callback` drops BQL before
  blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`; kick-check +
  release-wait under `d->pfifo.lock` guarantees no missed wake-up.
  `halt` via `qatomic_read` matches the writer contract. Normal mode's
  DMA_PUT doorbell skips the lock (`pfifo_put_doorbell`): a release
  store of PUT plus a doorbell/parked Dekker pair keeps the
  no-missed-wakeup guarantee, and only the first doorbell per park
  takes the lock.
- **Lockless DMA_PUT doorbell in normal mode (2026-10-15,
  `3391b92357`).** On windowed heavy-f1 the vCPU spent 12% of its
  samples in `user_write`, 7.9% of them blocked on `pfifo.lock` while
  the PFIFO thread held it across the reports safety-valve fence wait
  (REPORTS_SUBMIT). The doorbell now stores PUT with release (the
  pusher loads it with acquire) and pairs a doorbell flag with the
  thread's parked flag, full barrier on both sides: either the vCPU
  sees the thread parked and kicks it under the lock as before, or the
  thread sees the doorbell before it parks and runs the pusher again.
  USER reads skip the lock too (DMA_GET polls would otherwise wait out
  the same fence); the pusher publishes GET with release.
  Deterministic mode keeps the locked path (`kick_seq` feeds
  `pfifo_icount_sync`). vCPU samples in the doorbell 12% → 2.2%;
  windowed heavy-f1, interleaved 4×45 s at load 2-16: 35.3 → 42.3 fps
  (+19.7%, 3+/1−). Goldens unchanged; xbox suite 7/7.
  `XEMU_PFIFO_PUT_LOCK=1` restores the lock in normal mode
  (normal-mode switch: the goldens always take the locked path).
- **No doorbell wake inside a flip stall (2026-10-16).** On windowed
  heavy-f1 the PFIFO thread parked ~27k times a second and three of four
  wakes landed inside a flip stall (58% of wall), where a new PUT gives
  it nothing to do: it took `pgraph.lock`, found the flip incomplete
  and parked again, and the vCPU paid ~1.5 µs per broadcast.
  `pfifo_put_doorbell` now returns after publishing PUT and the doorbell
  flag when `waiting_for_flip` is set (acquire, against FLIP_STALL's
  now-release store, so WRITE_3D is current), READ_3D == WRITE_3D and
  the new PUT differs from GET.
  Only the vCPU ends a flip stall: NV_PGRAPH_INCREMENT kicks, and a
  direct NV_PGRAPH_SURFACE write is the vCPU's own store, which the
  doorbell's test reads. The flag stays set, so a thread that has not
  parked yet runs the pusher once more, and the full barrier keeps a
  stale `waiting_for_flip` from meeting a thread that parked without
  seeing it. Reports are unaffected: their delivery returns early while
  GET != PUT, which every skipped doorbell makes true; a re-rung PUT
  equal to GET (a guest polling a report on an idle FIFO) still wakes,
  so FIFO-idle delivery runs as before (heavy-f1 and the cold boot write
  none: 0 of 838k doorbells). Same binary,
  `XEMU_PFIFO_STALL_DOORBELL=0` vs default, windowed at the cap: in-stall
  wakes heavy-f1 4246 → 182/s (−96%), W2 2806 → 314/s (−89%) (xwx
  interposer, load 15-20); PFIFO thread CPU heavy-f1 40.4 → 36.0%, W2
  30.1 → 26.3% (2 pairs each, load 10-59; its system time 7.9 → 3.0%
  and 6.5 → 2.3%); bench-abx 2 pairs × 45 s at load 15-26: heavy-f1
  60.14 → 60.10 fps, process CPU 178.5 → 166.8%; W2 60.11 → 60.11 fps,
  169.9 → 161.4%. Heartbeat soak over 12 loadvms advancing, transition,
  stream-e2 walk and movie runs flip throughout; goldens unchanged.
- **BQL-free MMIO dispatch for the hottest guest register blocks.**
  `XEMU_MMIO_PROF` measured 78% of 8.2M guest MMIO ops/70 s hitting
  just PFB (`NV_PFB_WBC` polling) and USER doorbells, each paying
  the unconditional BQL in TCG's MMIO helpers — upstream's
  `lockless_io` flag is now honored on the TCG path too, and the
  audited PFB / USER / APU-VP regions opt in. Locked crossings drop
  ~80%. Measured honestly at **fps parity** (not claimed as a
  speedup) with a consistent steadiness win: per-run fps stdev
  1.21 → 0.89, 5/6 pairs — the 340 µs BQL-spike class no longer
  hits these ops.
- **PGRAPH joins the BQL-free MMIO set.** Interleaved A/B (F8, 3
  pairs, quiet): fps parity as expected for this class (legacy-BQL
  minus lockless = -0.65±1.43, sign-mixed), with the lockless arm's
  run-to-run spread tighter (0.43 vs 1.00 fps) — directionally the
  same steadiness win the PFB/USER set measured, not formally powered
  at n=3. The PGRAPH block was left out of
  the original lockless set because its handlers genuinely interleave
  with the PFIFO thread. The audit (`docs/Finished/pgraph-lockless-audit.md`)
  shows `pg->lock`/`pfifo.lock` — not the BQL — arbitrate every shared
  access except one: the interrupt pair
  (`pending_interrupts`/`enabled_interrupts`), which the BQL serialised
  against the lock-free `nv2a_update_irq` reader and the PFIFO thread's
  raise. Those two fields are now `qatomic` (fixing a pre-existing
  unserialised raise-vs-read pair in the process), and PGRAPH dispatch
  drops the BQL. A per-register hit counter (`XEMU_PGRAPH_MMIO_STATS=1`,
  removed 2026-10-10) measured the guest ISR *on* this path:
  `NV_PGRAPH_INTR` is the hottest PGRAPH register
  (1.6M reads on the F5 stats run; count ratios — the load-immune
  figures — put ~11 PGRAPH MMIO ops on every PGRAPH interrupt, 3 INTR
  reads per ack). Soak: 8 F5⇄F8 loadvm cycles over a 12-min window,
  zero hangs/asserts/artifacts; xbox suite green. The bisect hatches
  (`XEMU_PGRAPH_LOCKLESS=0` for PGRAPH, `XEMU_MMIO_BQL=1` for the whole
  set) were removed on 2026-10-15; the lockless set is unconditional.
- **PFIFO kick census (counter only, 2026-08-04; removed 2026-10-15).**
  `XEMU_PFIFO_KICK_STATS=1` bucketed every `pgraph_write` `pfifo_kick`
  by call site, wait-condition transition, `FIFO_ACCESS` and
  already-pending, plus the PFIFO thread's idle parks. It closed kick
  suppression (0 of 412,074 suppressible; Settled vectors). No kick is
  ever suppressed: a wrongly-skipped kick is a permanent PFIFO hang.
  The one exception is the DMA_PUT doorbell inside a flip stall (above),
  where the thread can't act on the new PUT and only the vCPU's
  INCREMENT kick or its next doorbell ends the stall.
- **Leaner deterministic inline pusher (2026-10-15).** With the null
  renderer under icount the vCPU runs the pusher itself
  (`pfifo_icount_sync`) holding both locks, and PMI put the pusher's own
  per-method loop at 2.8% of the det heavy-f1 vCPU. That inline run now
  gets its own copy of the loop (`pfifo_run_pusher_impl(d, true)`; the
  threaded copy keeps every check). It keeps DMA_GET in a register with
  a plain store per word and reads DMA_PUT once, so the release/acquire
  pair the lockless doorbell needs is gone. The puller skips the second
  FIFO-access and `waiting_for_nop` checks and the LOW_MARK test the
  pusher has just made, and `pgraph_method` skips the five
  CTX_CACHE → CTX_SWITCH compare-and-copies while the subchannel repeats
  (`pg->ctx_switch_subch`, reset at every run start). Nothing else
  writes those registers mid-run. `pgraph_method_log` returns at once
  when its two trace events are off, which under the nop backend drops
  its static bookkeeping from every method word in both modes. The
  edit cost `pfifo_run_pusher` its PGO counts, which had been all that
  inlined `pgraph_method` there (+0.69% instructions with the call
  back), so `pgraph_method` is `always_inline` under clang (ThinLTO
  honours it across files). On the misc lane (22329bd8bc plus the MMX
  commits, the icache batching and W^X, before the pinned registers and
  TLB memo): det heavy-f1 Minsns/frame −1.64% (44/44), per step −0.36%
  (GET/PUT), −0.27% (stall checks), −0.50% (CTX copies), −0.55% (log);
  stream-e2 −1.65% (4/4); cold boot 0.00%. Cycles:
  same-binary alternate-chunk A/B (old inline path in alternate 20-frame
  chunks, load 16-24) heavy-f1 −1.08% Mcycles/frame (se 0.26, median
  −1.27%), against an old path that ran 0.7% more instructions than the
  real base (no PGO layout), so expect nearer −0.8%. Cross-binary paired
  medians at load 16-26 couldn't resolve it: heavy-f1 −0.12% (22/44, CI
  −2.0..+1.4%), cold boot +0.6% (9/19, CI −2.2..+4.1%).
- **Fused per-command loop in the deterministic inline pusher
  (2026-10-16).** The inline run still went around the whole pusher
  loop for every data word: stall and FIFO-access checks, the DMA_STATE
  decode, the puller's PULL0, engine and object-method tests,
  `pgraph_method`'s context checks and class switch, and five register
  write-backs. A replay of heavy-f1's pushbuffer (6 frames) gives 37.8k
  loop iterations per frame: 12.8k headers and 25.0k data iterations,
  11.2k of them one-word steps of unbatched multi-word commands
  (texture bump-env, combiner registers). `pfifo_inline_command` now
  runs a kelvin command's data words in one loop, entered straight from
  the header decode: PULL0, the engine binding and the class are checked
  once per subchannel per run, and per word only the handler lookup and
  call are left (`pgraph_method`'s kelvin case, factored out with its
  batching and the DRAW_ARRAYS squash), then one test of the three stall
  flags a handler can set. DMA_GET, DMA_STATE, DCOUNT, DATA_SHADOW and
  LOW_MARK are written back at the command's end or stall. Method 0,
  object methods and non-kelvin classes keep the per-word path, as does
  the threaded pusher. Goldens unchanged; offscreen det (threaded
  pusher) heavy-f1 RAM equal; per-frame PFIFO GET/PUT/DMA_STATE and RAM
  equal over 6 heavy-f1 frames (one stalled mid-command); xbox suite
  7/7. Cross-binary det, load 13-58: heavy-f1 Minsns/frame −1.92%
  (paired median, 10/10), Mcycles/frame −2.9% (8/10, range
  −21.8..+6.8%); stream-e2 Minsns −1.64% (4/4), Mcycles −1.1% (2/4,
  unresolved).
- **Headless null mode scans VGA only for a screendump (2026-10-15).**
  Every vblank ran `vga_update_display` for a surface that nothing shows
  under `XEMU_HEADLESS=null`. Its dirty-bitmap clear re-arms
  `TLB_NOTDIRTY` on the framebuffer pages (`tlb_reset_dirty`), so the
  next guest write to each takes the notdirty slow path. On the misc
  lane (22329bd8bc plus the MMX, icache-batching, W^X and pusher
  changes, before the pinned registers, the TLB memo and
  `bd02c7c745`'s writable-clean TLB bitmaps), where every figure in
  this bullet but the last was measured, the clear also walked the
  whole TLB, and the scan was 0.45-0.5% of the det vCPU (PMI). `nv2a_vga_gfx_update` now skips the scan in null mode (the
  VBLANK interrupt and raster reset stay), and `screendump_vga`, the
  surface's only reader there, calls `nv2a_vga_refresh()` under the BQL
  first: a full redraw (`hw_ops->invalidate`, so the size check forces
  it) that clears no dirty bits. Faking a mode change instead
  (`graphic_mode = -1`) would restart the text cursor's blink and flip
  it on every screendump. Screendumps match the old binary byte for
  byte at cold boot f1-f1800 (across the f180-f240 mode switch),
  heavy-f1 f1/f300, stream-e2 f600 and live in normal mode; one taken
  right after a `loadvm`, before any frame runs, now shows the loaded
  state instead of the one before it. Same-binary alternate-chunk A/B
  (old path in alternate 20-frame chunks, load 7-19): heavy-f1 −0.39%
  Mcycles/frame (se 0.09), stream-e2 −0.81% (se 0.29), cold boot
  unresolved (−0.7%, se 2.3); Minsns/frame −0.25%, −0.26%, −0.36%.
  Cross-binary at load 7-21: Minsns/frame −0.28% on heavy-f1 (24/24)
  and stream-e2 (9/10); Mcycles/frame paired medians −1.1% (15/24) and
  +1.2% (4/10), under the noise (se 1.2 and 2.5). With the bitmaps the
  clear visits only writable-clean entries, and the skip saves less:
  re-measured on top of them (2026-10-16, against the same binary
  forced to scan by a temporary switch, load 4-7), Minsns/frame −0.12%
  on heavy-f1 (9/10 pairs), −0.06% on stream-e2 (3/3) and −0.17% on
  cold boot (3/3), Mcycles/frame within noise. Windowed and offscreen
  modes still scan every vblank.
- **PTIMER zero-divisor guard (2026-09-08).** The upstream merge gave
  `NV_PTIMER_ALARM_0` a write handler that schedules a QEMU timer
  (`d73326b621`), and every conversion on that path divides by
  `ptimer.numerator`, `ptimer.denominator` and
  `pramdac.core_clock_freq` — all three of which are zero for part of
  every boot: the two PTIMER registers are never reset and start at
  device zero-init, and `core_clock_freq` is re-zeroed by any NVPLL
  coefficient write with `MDIV == 0` (`pramdac.c:97`). A guest that
  writes `ALARM_0` before programming the clock therefore reached
  `muldiv64`'s divide by zero: a trap where integer division by zero
  traps, and on AArch64 — where the lowered divide yields 0 instead —
  an immediate-fire timer loop, because `diff_ns` came out 0 and
  `timer_mod(now + 0)` re-armed at the current instant forever. One
  predicate (`ptimer_clock_configured()`) now guards all three sites:
  the two conversions return 0, which is exactly what the old code
  already produced in every non-trapping zero case, and
  `schedule_qemu_timer()` deletes the timer rather than arming it —
  correct, because an unconfigured clock leaves `get_reg_time()` frozen
  at `time_offset`, so no alarm can ever legitimately be reached. The
  configured path is byte-identical (three early exits, 32 insertions
  and 0 deletions); no vmstate or struct change, so old snapshots are
  unaffected. Recorded behaviour delta: an `ALARM_0` write issued while
  the clock is unprogrammed is now dropped rather than deferred — no
  worse than pre-merge, where `alarm_time` was stored and nothing
  consumed it, and re-arming on the unconfigured→configured transition
  would arm at `alarm_time == 0` for guests that never wrote `ALARM_0`.
  Reasoned from the code and syntax-checked against the real compile
  command; the fault itself was not reproduced.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`, `-mcpu`
  auto-detected. STBI_NEON + fpng CRC32 for ARM64.
- **PGO shipped for arm64 release builds.** A trained profile
  (`pgo/default.profdata`, Azurik savestate corpus: the four bench
  scenes, ~75 s each) is committed and applied by CI to macOS arm64
  release legs. Measured on the 770 draws/flip heavy savestate scene
  (interleaved cross-binary A/B, 3 pairs, identical source both
  arms): 25.42 ± 0.34 → 27.81 ± 0.62 fps (**+9.4%**, deltas
  +2.71/+1.85/+2.61, scene identity 740-746 draws/flip). x86_64 legs
  stay plain (profiles are arch-specific); a stale profile degrades
  to partial coverage, and `build.sh` fails loudly if the profile
  file disappears. Retrain per the build-knob table.
- macOS 26 build fixes: `download-macos-libs.py` uses
  `os.path.abspath` and repairs stale `prefix=` lines in vendored
  `.pc` files (and, since 2026-10-10, the absolute `libintl.h`
  include in `gi18n.h`, which broke the first build after the checkout
  moved); `build.sh` strips all `LC_RPATH` before `dylibbundler` and
  adds the single correct one.
- **No `-fzero-call-used-regs` on macOS arm64 (2026-10-15).**
  `meson.build` skips upstream's `used-gpr` ROP hardening when the host
  is Darwin aarch64; Linux, Windows and macOS x86_64 keep it. The
  register-zeroing movs before every `ret`, plus the tail calls the
  flag turns into `bl`+`ret`, were 3.2% of the deterministic run's host
  instructions (`helper_shufps_xmm`: 19 → 11 instructions).
  `bench-det.py -n 6 -f 1200` on heavy-f1: host instructions per frame
  −3.15% (12/12), the load-independent figure. The cycle gain grows
  with host load: −1.2% mean (8/8) at load 17-28, lowest-load pair
  −0.5%; −1.7% median (9/12) at load 39-78. Expect about 0.5-1.2% on a
  quiet box. Wall fps stayed within noise (median +0.8%). `__text`
  8,746,604 → 7,977,728 B. Goldens stay bit-identical and the IR PGO
  profile stays valid. It reopens the Failed row, whose fps sign tests
  could not resolve a ~1% change.
- Ad-hoc codesign by default (MAP_JIT works);
  `XEMU_CODESIGN_ENTITLEMENTS=1` opts into hardened runtime +
  `xemu.entitlements`. For notarized distribution use
  `scripts/sign-macos-release.sh`.
- Optional PGO (`XEMU_PGO=generate` → run → `XEMU_PGO=use`), wired
  into the macOS, Linux, and native Windows (MSYS2) branches via one
  shared `setup_pgo` — the consolidation also fixed the MSYS2
  branch's stale-profile trap (it only re-merged `.profraw` files
  when `default.profdata` was absent, so a retrain silently lost to
  the committed profile).
- **Cross-platform parity batch (2026-07-05).** The DSP56K JIT gate
  widened to all POSIX aarch64 hosts, so Linux arm64 compiles the
  fork's biggest CPU win — and since 2026-07-11 CI runs the `xbox`
  suite (interpreter/JIT differential + swizzle) on the
  `ubuntu-22.04-arm` leg: the first automated runtime coverage of
  the DSP JIT on non-Apple aarch64, all three differential arms
  green with the engagement assert (blocks actually JIT-executed).
  (The chain-unpatch-under-SMC icache path is corpus-invisible, so
  an in-game `XEMU_DSP_JIT_DIFF` run there is still owed —
  `docs/windows-gating-audit.md`.)
  build.sh grew Linux PGO wiring, an arch-clean guard (a stale
  `build/` for another arch was silently reused, so `-a x86_64`
  could "succeed" with an arm64 binary), and arch-aware MoltenVK
  resolution (a single-arch system dylib silently disabled the whole
  Vulkan renderer for cross builds; the UI's MetalFX references are
  CONFIG_VULKAN-guarded now). MoltenVK resolution + vendoring now
  live in single functions (`resolve_moltenvk` / `vendor_moltenvk`)
  shared by configure and package time.
- Native Windows release builds default to `-Dx86_version=3`
  (AVX2 / BMI2 / FMA — matches CI release config; override by
  passing your own `-Dx86_version=`).
- Windows cross-compile fixes (found by building win64-cross from
  this fork): the x86_64 hard-FPU `__hard` set was missing
  `sqrt` / `round_to_int` / `to_int32[_round_to_zero]` / `to_int64`
  (implicit-decl compile error in `fpu_helper_hard.c`); the SPIR-V
  shader cache used 2-arg POSIX `mkdir` (→ `g_mkdir_with_parents`);
  the pipeline cache used `rename()` onto an existing file, which
  Windows rejects (→ `g_rename`); the `HAVE_EXTERNAL_MEMORY` display
  path lost its `gl_internal_format` declaration.
- **Shipped MoltenVK converged with the tested one — 1.4.2
  (2026-08-04).** Releases built and shipped stock **1.4.1** while every
  local soak, A/B and gauntlet ran against 1.4.2-rc1: tested-vs-shipped
  driver drift, and a stray system or Homebrew dylib shadowing the
  vendored release used to ship silently. `scripts/build-moltenvk.sh`'s
  `MVK_PIN` moved to the v1.4.2 final tag, `build.sh`'s vendored default
  moved to `1.4.2` (`build.sh:66` — that default is now the **single
  home** for the expected version), and macOS CI awk-parses it back out
  of `build.sh` and fails the job if the packaged bundle's
  `Bundling MoltenVK … (version X)` provenance line disagrees. Pin-bump
  gauntlet against the vendored universal 1.4.2 build: **fps parity at
  +0.13%, sign-mixed, 0 artifact frames.** Provenance divergence to know
  about on this dev machine only: `/usr/local/lib/libMoltenVK.dylib` was
  deliberately not rebuilt mid-campaign (rebuilding would have
  invalidated in-flight A/Bs), so a local `dist/` still resolves the
  custom rc1 dylib through the tier walk while CI vendors 1.4.2. Run
  `scripts/build-moltenvk.sh` to converge the local one.
- **CI pins an `-mcpu` distribution floor (2026-08-04).** `build.sh`'s
  `-mcpu` auto-detect follows the *build host*, so a GitHub
  runner-silicon refresh would have silently raised the shipped ISA
  baseline — a SIGILL-on-launch class with no CI signal at all. The
  macOS release job now sets `XEMU_ARM_CPU=apple-m1`, matching the
  advertised M1 floor. Dev boxes keep auto-detect and now get a loud
  warning whenever the detected `-mcpu` exceeds `apple-m1`. `apple-m1`
  vs `apple-m2` codegen is nil on this tree, so this is a guard, not a
  tuning change. Scope note for review: the variable is set for the
  whole macOS `build` job (both configurations, both arches) rather than
  only the release legs — the `-dbg` bundles are also distributed,
  `build.sh` ignores the variable entirely on the x86_64 leg, and a
  job-level setting cannot drift out of sync with a per-leg conditional.
- **PGO builds report profile staleness (2026-08-04).** A profile that
  no longer matches the source degrades *silently*: clang drops guidance
  per function and the build stays green. `XEMU_PGO=use` builds now
  print a staleness summary after the make step — CFG-hash-mismatch and
  unprofiled-function counts weighted against the hot-function set
  (`cpu_exec*` / `helper_*` / `tlb_*` / `tcg_*` / `pgraph_*`), plus
  commits-since-retrain on `accel/tcg tcg target/i386 hw/xbox`. Hot-path
  mismatches print a loud warning; `XEMU_PGO_STALE_FATAL=1` makes them
  fatal (opt-in, not enabled in CI). Why it isn't a grep: a bare "any
  hash mismatch" count is not a detector, because straight-line edits
  leave the IR-PGO CFG hash unchanged, so small-but-real drift reads as
  zero. **The gate fired on this wave's own tree — 7 hot-path CFG
  mismatches including `cpu_exec_loop` and `pgraph_write`**, i.e. it
  caught exactly the functions this wave edited. Known limitation
  recorded with it: the *age* half is blind to uncommitted trees (it
  measures from a commit), so on a dirty working tree only the
  mismatch half is meaningful. Consequence for the release path:
  **retrain the PGO profile after the wave's commits land and before
  tagging** (owner-waived-A/B precedent: `46b15d19b4`).
- **Bench batches can isolate their shader/pipeline caches
  (2026-08-04).** `scripts/bench-savestate-ab.sh` gained
  `--isolate-caches` / `XEMU_BENCH_ISOLATE_CACHES=1`, which moves xemu's
  data base path (`spirv_cache_v*/`, `pipeline_cache.bin`) into the
  disposable work-dir bundle clone via portable mode — making cache
  warmth a controlled variable like the hdd already is. Caches start
  cold at batch start, warm during the warmup run, and are shared
  identically by both arms. Default is unchanged (warm-shared) so
  existing baselines stay comparable, and the choice is recorded as
  `cache_isolation` in the protocol receipt.
- **build.sh warns when a subproject checkout has drifted from its wrap
  (2026-09-08).** Bumping a `.wrap` does not re-checkout an existing
  `subprojects/<name>`: meson keeps building whatever is on disk, so a
  local build silently stops being the one CI ships. That bit this tree
  in 2026-09 — `glslang.wrap` said 16.5.0, the checkout was still
  16.2.0, and the SPIR-V cache key went with the checkout (see the
  Vulkan renderer entry). Configure now reads each `[wrap-git]` wrap's
  `revision`/`directory` (section-aware, CRLF-tolerant) and compares it
  against the checkout's `HEAD`, printing the exact
  `meson subprojects update --reset <name>` to run. Warning only and
  never fatal: it returns 0 on every path, is invoked as
  `check_wrap_drift || true`, skips missing/non-git subprojects and
  `revision = head`, and short-circuits when `git` is absent (so the
  win64-cross Docker leg and source-archive builds simply skip it).
  Costs ~0.4 s. On this tree it flags `SPIRV-Reflect`, `volk` and
  `VulkanMemoryAllocator`; `imgui`'s meson warning turned out to be the
  `patch_directory` overlay, not a HEAD drift. Verified under bash 3.2,
  `/bin/sh` and `dash` plus a fixture tree covering tag/short-sha/CRLF/
  `directory=` cases; not executed on MSYS2 or Linux.
- **dsp56300 `tar --force-local` keyed on the build machine
  (2026-09-08).** The flag exists because MSYS2/GNU tar parses the
  `C:/...` path meson hands it as `host:path`, but it was gated on
  `host_machine`, so the Linux-hosted win64-cross legs got it too
  (harmless with GNU tar) while a Windows-cross build hosted on macOS
  would have handed it to bsdtar under `check: true` — a hard configure
  error. It is a property of the tar meson invokes, so it is now keyed
  on `build_machine.system()`. Native MSYS2 and Darwin builds are
  unchanged (the two predicates agree there). Matches the wave-1 hunk
  in `fb42bce22e`; that commit's second hunk (`run_command(tar, ...)` →
  `find_program('tar')`) was deliberately left on the branch.
- **GCC PGO works on native Windows; the `-O3` flip is opt-in
  (2026-09-08).** Re-cut of the wave-1 build commit `fb42bce22e`. The
  MSYS2 arm inherited a clang/`llvm-profdata`-only `setup_pgo` but never
  selects clang, so on a stock MINGW64 install (`cc` = GCC)
  `XEMU_PGO=use` handed GCC the committed *clang* `pgo/default.profdata`,
  found no `.gcda`, and built with no profile at all while announcing
  success. `pgo_compiler_family()` now asks the compiler itself
  (`cc -dM -E -`, testing `__clang__` — not piped into `grep -q`, which
  under `set -o pipefail` can SIGPIPE the compiler and misclassify a
  clang toolchain), and the GCC arm uses `-fprofile-generate` +
  `-fprofile-update=atomic` (xemu's counters are incremented from the
  vCPU, PFIFO, audio and vblank threads; GCC's default non-atomic update
  loses them) / `-fprofile-use` + `-fprofile-correction`
  `-fprofile-partial-training` `-Wno-missing-profile`
  `-Wno-coverage-mismatch`, with a `.gcda` presence check that names the
  wrong-family profile instead of silently building unprofiled. Both
  families' "wrong artifacts in the profile dir" cases now say so, and
  the clang-worded staleness parser reports "not implemented" on GCC
  rather than a 0/0 that reads as clean. Native-Windows-specific fix the
  branch version did not have: `cc`/`gcc` there are native Windows
  binaries, so a POSIX `${PWD}` profile dir (`/c/Users/...`) is baked
  into libgcov verbatim and the training run writes its `.gcda` under
  `<drive>:\c\Users\...` while discovery searches the real directory —
  the arm now normalises with `cygpath -m` before `setup_pgo`, the same
  way `--prefix` already did. `XEMU_HARDENING` moved out of the Darwin
  arm to just after the platform `case`, so the register-zeroing A/B is
  finally runnable on Linux and both Windows arms; the block moved
  verbatim and at the default (`1`) it only echoes, so no arm's flags
  change (the knob itself was removed on 2026-10-10; the Failed row has
  the `CFLAGS` replacement). **The branch's `-O2` →
  `-O3 -Dstack_protector=disabled` flip of the native Windows release
  was inverted to opt-in
  (`XEMU_WIN_O3=1`)**: the 2026-09-08 review names it as the differential
  behind the wave-1 exit-139, no CI leg builds it, and the branch comment
  claiming `-Dstack_protector=disabled` is a mingw no-op was wrong —
  `meson.build:512-536` probes by compiling *and* linking with
  `-fstack-protector-strong`, which stock MSYS2 MINGW64 passes, so the
  option really strips the canary tree-wide. Simulated across
  {Darwin, Linux, win64-cross, MINGW, MSYS, CYGWIN} × {unset, 0, 1}: the
  only configuration whose `opts` differ from main is native Windows
  with `XEMU_WIN_O3=1`; `win64-cross` ignores the knob by construction
  (with a message) so the release-artifact legs cannot pick it up. Not
  yet built on Windows — the GCC PGO arm and the `cygpath` fix are
  unexercised until the next session on the box.
- **Upstream merge `bce8323063` (2026-10-10)** (xemu master
  `478b4f4961`, 16 commits past the 2026-09-07 merge base): PTIMER alarm
  scheduling rounds up to whole internal ticks with a 1 ns floor
  (`muldiv64_round_up`, `get_next_alarm_time`) and gains an on-target
  unit test (`tests/xbox/ptimer`, 20 subtests, now in the `xbox` suite);
  glslang 16.5.0 → 16.6.0 (the SPIR-V cache key rolls); SDL3 3.4.16 and
  nlohmann_json 3.12.0 become git wraps (macOS still takes SDL3 from the
  vendored MacPorts bottle via pkg-config); a `make check-unit` CI job
  (`build-tests.yml`, ubuntu-22.04) joins the matrix; AGENTS.md /
  CONTRIBUTING.md guidance. Two conflicts: `ptimer.c`'s
  `schedule_qemu_timer()` keeps upstream's interrupt-enable early-out
  *and* the fork's zero-divisor guard (upstream's reset now seeds
  NUMERATOR = DENOMINATOR = 1, so the guard mostly covers the PLL
  MDIV == 0 case from here on); `tests/xbox/meson.build` keeps the
  fork's `test-xbox` alias and upstream's ptimer test registers with it
  (its executable is `build_by_default: false`, so the `--no-rebuild`
  CI run would otherwise find no binary). DSP namespaces and the
  `config_spec.yml` audio defaults are untouched (re-verified).
  Validated: arm64 release build clean, `meson test --suite xbox` 7/7,
  in-game smoke **deferred** — the box ran a load-70+ ffmpeg batch all
  of 2026-10-10 and the owner held every timed run until it is quiet.
- **`download-macos-libs.py` vendors the newest MacPorts build and
  survives Python 3.14 (2026-10-10).** The picker took the
  lexicographically *last* listing entry, so `SDL3-3.4.8` beat `3.4.18`
  and every clean build — CI's included — vendored the 3.4.8 bottle for
  months; packages are now ranked by numeric version, then port
  revision, then fewest `+variants`. Separately, Python 3.14 made
  tarfile's `data` extraction filter the default, which refuses the
  absolute symlink inside `dbus` (a new dependency of MacPorts'
  `glib2 +dbus+x11`) and aborted the fetch with `AbsoluteLinkError`;
  extraction now asks for the `tar` filter where the interpreter
  supports it (path-traversal checks kept, links allowed — the
  pre-3.14 behaviour). First clean build after: SDL3 3.4.18, glib2
  2.88.3_2, libusb 1.0.30, pcre2 10.48; dbus is extracted but nothing
  in the link graph uses it, so the bundle's dylib set is unchanged.
- **Local MoltenVK pin → main `67b26826` (2026-10-10; self-reports
  1.4.3).** 54 commits past the v1.4.2 tag with no 1.4.3 tag yet. What
  it carries that this fork cares about: query-pool callback lifetime
  (#2839), occlusion queries under immediate prefill (#2846), deferred
  prefill of secondary CBs (#2845), encoder execution dependencies
  (#2819), external-subpass fence ordering (#2823), shader-cache
  dynamic offsets (#2861), `areQueriesDeviceAvailable` (#2794).
  Releases keep vendoring stock 1.4.2 (`XEMU_MOLTENVK_VERSION`,
  CI-asserted), so the tested-vs-shipped driver gap the 2026-08-04
  entry above closed is open again until a 1.4.3 tag ships — converge
  the same way then. Bundled UUID
  `0C9CFBE7-F160-37A1-B1CD-407EA736B5DF`. Validated: arm64 release
  build clean with the provenance line reporting 1.4.3 / `0C9CFBE7…`,
  `meson test --suite xbox` 7/7; the in-game smoke, the artifact soak
  and the interleaved fps A/B half of the pin-bump gauntlet are all
  **deferred** to a quiet box — the host ran a load-70 ffmpeg batch
  throughout 2026-10-10, so no fps number from that session is citable.

### MoltenVK runtime config

Set in `main()` before MoltenVK loads (launch-path independent;
explicit env overrides win) and mirrored in `Info.plist`
`LSEnvironment`. The two homes must stay in sync.

Driver-source review at the v1.4.2 pin (2026-08-04): the per-CB
visibility primer stays (its target mechanism is verbatim in 1.4.2 —
archaeology 2.2 addendum has the line refs), and the changelog's
"channel corruption on color RTs used as transfer sources" fix was
reverted upstream before the tag — the shipped 1.4.2 behaves like
rc1 there (triage lead recorded in archaeology 2.2).

| Key | Value | Purpose |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor binding overhead |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | `0` | Deferred encoding. Must stay 0: with whole-frame CBs, prefill corrupted streamed textures, enabled the AGX visibility-buffer crash, and measured slower (46.3 vs 48.5 fps) |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async submits |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math (ALWAYS; driver default is ON_DEMAND) |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Ignore transient GPU errors |
| `MVK_CONFIG_LOG_LEVEL` | `1` | Errors only; driver default is INFO |

(`MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION` was tried and
reverted — measured inert on macOS 26 / M2 Ultra; see the experiments
doc below.)

Two MoltenVK optimizations that need code changes + validation before
they could land — building the driver with `MVK_USE_METAL_PRIVATE_API`
(unlocks `wideLines`/`logicOp`/`VK_EXT_provoking_vertex`, empirically
confirmed) and async/prewarm pipeline creation — are recorded in
[moltenvk-optimization-experiments.md](Future/moltenvk-optimization-experiments.md).


- **Sub-page arm (b): inline NOTDIRTY store-skip (default-on
  2026-07-18, +12% heavy-scene throughput).** The aarch64 store
  slow-path stub gains a pre-filter that completes a guest store inline
  when the slow path is a PROVABLE no-op: the TLB mismatch is exactly
  `TLB_NOTDIRTY` (a 2-insn XOR discriminator — every hard flag implies
  `TLB_FORCE_SLOW` in the comparator), the store's 64 B sub-block
  carries no translated code (a flat per-RAM-page mirror of the arm-(a)
  bitmap, single-vCPU so plain loads suffice), and every NOCODE dirty
  client is already set (so `set_dirty_range` would change nothing —
  the renderer's vertex/texture dirty views stay bit-identical).
  Anything else demotes to the real helper. Cold-stub only: the
  tag-match fast path emits byte-for-byte as before.
  Promotion receipts (quiet machine, F8): the stub completes **~2M
  eligible stores/s inline** — the population is dominated by
  renderer-watched pages and is ~20x the code-page trap count the
  original +0.3-0.7 fps prediction was sized on — yielding **+11.8% and
  +12.0% draws/s throughput in two independent 3-pair interleaved
  batches (6/6 positive)**; raw fps stays ~flat (batch 2: +0.43 fps,
  3/3) because the guest's effect-load feedback converts the headroom
  into ~815 vs ~727 draws/flip at identical visuals (screenshots
  clean). `XEMU_SUBPAGE_FAST_REFUTE` re-derives every would-skip
  decision from ground truth (independent `probe_access`, live-TB
  overlap scan, NOCODE dirty check) — **~19.5M decisions, 0
  violations** across static, loadvm-cycling, and
  first-visit-streaming (movement probe: `tb_flush` flat, forward
  screenshot clean) soaks.
- **Superblock seam-following and region/diamond formation (2026-07-18;
  code removed 2026-10-10).** Translate-time TB merging (M1 through
  unconditional same-page forward jmps; M2 through conditional
  fallthroughs with the taken edge in an out-of-line lookup stub) and a
  region former that kept both edges of short forward jcc arms inside
  the TB, backed by a recorded-label liveness elision in `tcg/tcg.c`.
  The campaign was sized by a cc-flag liveness census (39-44% of block
  boundaries carry a dead flag materialization; erratum `ce38bb8204`)
  and a taken-exit tail-kind census. Every measured policy regressed
  (Failed table) and the campaign stopped by pre-registration; the
  machinery shipped dark until its removal (Retired diagnostics and dark
  experiments, below).

### Testing & tooling (2026-07-18)

- **Harness runs are muted and carry their own pads (2026-10-11).**
  `scripts/harness-config.py` finishes every harness's scratch config:
  `[audio] volume_limit = 0` (owner request; harness audio played on the
  owner's machine), and for players 1-2 not bound to the keyboard it
  drops the binding, stubs a hub + gamepad with `-device` args and sets
  `auto_bind = false`. The 2026-10-11 `azurik-*` fixture tags carry the
  owner's pad on player 1, so with it switched off every `loadvm` of
  them failed (a whole 20-run batch DEAD); `auto_bind` off keeps a pad
  connecting mid-run from aborting xemu on the occupied port. Bench
  runs whose xemu exits mid-run are now recorded DEAD instead of being
  gated on a truncated log (one such crashed run had printed a VALID
  receipt). `movement-probe.sh` takes `--app` and `--isolate-caches`
  and quits through the monitor so atexit dumps and PGO profiles land.
- **Mid-run screenshot artifact check in the bench fixtures.** Every
  `scripts/bench-savestate-ab.sh` run now captures one window
  screenshot partway through (`scripts/bench-screenshot.py`: Quartz
  window lookup by PID, works unfocused) and scores it for
  white-screen, stuck-uniform-frame, and magenta-tile classes; the
  verdict lands in the run line and batch summary, and any ARTIFACT
  sets exit code 3. Verdicts write to sidecar `shot_<label>.verdict`
  files — appending to the shared log loses the line, because xemu's
  stdout redirect is opened without `O_APPEND` and its next flush
  overwrites appended bytes. Validated against synthetic frames of all
  four classes and live F8 runs.
- **Scene-anchored bench gating.** The receipt gate
  (`scripts/bench-receipt.py`) now anchors each run at the trailing
  contiguous in-band run of nsprof intervals (then drops one post-load
  transient) instead of dropping a fixed count from the log head. Root
  cause: warm launches print boot-menu intervals before `loadvm`
  lands, while a cold first launch boots flip-silent and prints none —
  position-based dropping misclassified every warm run (gate INVALID
  on healthy runs, or menu intervals polluting fps means). Incident
  9.1/9.2 fixtures still fire; `--e-value V` added for N-valued knob
  A/Bs; the pair loop no longer trips BSD `seq 1 0` counting down.
- **Movement-phase probe committed** (`scripts/movement-probe.sh`).
  The first-visit streaming probe (settle 15 s → hold-forward 20 s →
  rest → hold-backward 20 s, per-phase `info jit` TB-flush deltas,
  mid-forward screenshot) — the REQUIRED validation class for TB
  lifetime/invalidation changes, previously only a session-scratch
  script.
- **Bench harness monitor socket moved to `/tmp`.** The A/B harness
  derived its socket path from the per-invocation work dir; any deep
  outdir (session scratchpads) pushed it past macOS's 104-byte AF_UNIX
  cap and every run died as `DEAD (no monitor socket)` before launch.
  Sockets are now `/tmp/xemu-bench.$$.<label>.sock`, removed per run —
  the same rule the movement probe already applied (archaeology 9.9).
- **Ret-memo fill path exports the lookup.** The miss/fill path called
  `helper_lookup_tb_ptr` (which resolves the TB then returns only
  `tc.ptr`) and then paid `tcg_tb_lookup`'s g_tree walk to reverse the
  pointer it already had. `xemu_lookup_tb` (exported from
  `accel/tcg/cpu-exec.c`) returns the TB directly; the fill memoizes
  it with no reversal. Sub-noise fps by design (~1.8% of vCPU samples
  across `g_tree_find_node` + `tb_tc_cmp` expected to leave the
  profile); guarded by the same outer gates, logging semantics
  unchanged.
- **Sub-page consult branch hint corrected.** The notdirty-write
  consult was still marked `unlikely()` from its dark-launch days;
  sub-page tracking has been default-on since 2026-07-11, so the hint
  statically mispredicted the ~90k/s trap path. Cost was sub-noise;
  the hint was simply wrong.
- **Spanning-dest chain-decline counter.** `xemu_inv_span_nochain`
  (INV_PROF dump line (d')) counts the dispatch-loop refusals to
  direct-chain into a TB spanning two guest pages — sizing the xpage
  "page-spanning targets decline to chain" coverage gap before anyone
  builds the risky registry extension for it.
- **The measurement channels report on Windows and under OpenGL
  (2026-09-08).** Three of the four channels the fork's methodology
  depends on were dead or lying outside the macOS Vulkan path.
  `nsprof_flip_tick()` had a single call site, in
  `pgraph_vk_flip_stall()`, so `XEMU_NV2A_NSPROF=1` printed nothing
  under the OpenGL renderer — the Windows Vulkan-init fallback and the
  user-selectable GL renderer had no wall-time channel at all.
  `pgraph_gl_flip_stall()` now ticks it too (`gl/renderer.c:124`), and
  that is one tick per guest flip, not two: exactly one renderer's ops
  table is installed at a time (`pg->renderer`, set in
  `attempt_renderer_init`) and `DEF_METHOD(NV097, FLIP_STALL)`
  dispatches a single `ops.flip_stall(d)` through it. The Vulkan call
  site is deliberately untouched, so macOS per-flip denominators are
  literally unchanged rather than merely equivalent. Three GL sites
  feed two existing buckets — `fence_wait` (the flip `glFinish` and the
  display `glClientWaitSync`) and `present_wait` (the UI thread waiting
  for the PFIFO thread to compose the display surface) — on top of the
  renderer-neutral `flip_idle`; every other PFIFO-thread bucket is
  instrumented at Vulkan-only call sites, and the UI-thread buckets
  live in `ui/xemu.c` / `ui/xemu-metal.m` (Apple presentation only),
  so a sparse GL summary means "not instrumented", not "no cost". `xemu_inv_ticks()` returned a constant
  0 on x86_64, so `XEMU_INV_TIMING` printed an all-zero
  `notdirty_write` split; it has an `rdtsc` arm with a one-time ns/tick
  calibration (knob table above). `NV2A_STRIP_PROFILE_COUNTERS` was
  unconditional, so the ImGui NV2A panel plotted zeros even in
  `--debug` builds; it now tracks `XEMU_DEBUG_BUILD`, and the stripped
  no-op is a typed `static inline` instead of a function-like macro so
  its argument stays type-checked — compiling `vk/draw.c`,
  `vk/surface.c` and `vk/texture.c` with the tree's own release flags
  (`-O3 -flto=thin -mcpu=apple-m2`, `-g0`) gives byte-identical objects
  before and after, which is an optimizer property, not a guarantee.
  On Windows `stderr` is a fully buffered file after `ui/xemu.c`'s
  `freopen`, so the heartbeat and nsprof lines were lost on exactly the
  force-kill they exist to explain: both now flush after each print,
  inside their existing gates. And `XEMU_SUBPAGE_FAST=1` /
  `XEMU_SUBPAGE_FAST_REFUTE=1` say once that a non-aarch64 TCG backend
  has no inline store-prefilter emitter instead of being silently
  inert. Every new hook is gated on `nsprof_enabled()` or a `getenv`
  first and `NV2A_STRIP_PROFILE_COUNTERS` stays 1 in release, so the
  macOS arm64 release path is unchanged. Cherry-picked from
  `eb971f860d` on `windows-wave1-wip`.
- **Windows A/B harness survives the failure it exists to measure
  (2026-09-08).** `scripts/bench-savestate-ab-win.py` sent its `loadvm`
  monitor command unguarded, so an xemu that dies 6-10 s after launch —
  precisely the wave-1 exit-139 being chased — killed the whole batch
  with a traceback instead of recording one DEAD run. It now polls
  `proc.poll()` after the boot wait and catches `OSError`/`socket.timeout`
  around `loadvm`, recording whether the process was still running (hung)
  or already gone with its exit code, sampled before the harness's own
  kill can mask it. Three more sharp edges from the same review: the
  renderer is read from the template, refused unless `VULKAN` (or
  `--allow-renderer`), pinned into every scratch config and recorded in
  the receipt — when that was written `nsprof_flip_tick()` had a single
  call site, `pgraph/vk/renderer.c:241`, so an OpenGL run gated DEAD
  with no explanation. The GL renderer ticks it too since 2026-09-08,
  so a GL run does print intervals; the harness still pins `VULKAN` by
  default because only the Vulkan call sites populate the per-draw /
  upload / shader buckets, so a GL receipt is comparable only against
  another GL receipt. Every DEAD run now names the likely cause
  (empty captured log ⇒ look at `xemu.log` in the exe dir, since `ui/xemu.c:1940-1947`
  freopens the streams there when `AttachConsole` fails; a non-empty log
  with zero `nsprof:` headers ⇒ named with the renderer); and
  `parse_env_set` uses `shlex` with `escape=""` so Windows backslash
  paths survive *and* quoted values with spaces still parse. The
  non-Windows refusal now says which Python is wanted and why an MSYS2
  `/usr/bin/python` cannot work (no `ctypes.WinDLL` sampler, MSYS path
  translation). `scripts/win/capture_screen.ps1` calls
  `SetProcessDPIAware` before its first screen query and prefers
  `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` over
  `GetWindowRect`, with a try/catch fallback to the old path, so crops on
  a scaled display are no longer offset. `--dry-run` covers the new
  paths (21 checks); no Windows execution yet.
- **`xemu-run` restores incrementally (2026-10-16).** Its restore was
  a full one: every RAM page compared against the checkpoint's. It now
  passes `incremental` to `qmp_xemu_checkpoint_restore` (default true;
  `"incremental": false` asks for the full compare, for RAM written by
  another process through `XEMU_RAM_SHM`). The first restore, and the
  first after a loadvm, an incoming migration or a failed restore,
  still compares every page. Cross-binary, two det null instances side
  by side, 5 interleaved reps of 40 candidates: heavy-f1 at load 59-64
  L=1 −22.0% Mcycles per candidate (5/5), −31% Minsns, 21.2 → 16.0 ms;
  stream-e2 at load 32 L=1 −19.0% (5/5), L=16 −6.0% (5/5). 200 random
  candidates (input, traces) gave the same reply, trace and RAM sha256
  as a full restore followed by `xemu-run` without a checkpoint, and an
  incremental `verify` restore after 20 more found `pages-missed` 0.
- **`xemu-ram-digest` takes `"algorithm": "xxh3-128"` and hashes pages
  in parallel (2026-10-16).** The sha256 of 128 MiB is ~55 ms, nearly
  all of it `AccelerateCrypto_SHA256_compress` (CPU SHA-2 at ~2.4 GB/s,
  the serial ceiling), and `pages` added ~60 ms of serial per-page
  sha256. XXH3-128 of all of RAM takes ~4 ms; page hashes (sha256, or
  XXH3-64 with the new algorithm) run as 64 tasks of 512 pages on
  `dispatch_apply_f` (QEMU's `ThreadPool` elsewhere), since the VM is
  paused. The default reply is unchanged and byte-identical to the old
  binary's (whole, pages and `against`); `against` stays serial (memory
  bound: in parallel it took 1.2 instead of 4.7 ms but tripled the CPU
  time). xxHash is compiled inline in the file: the in-tree library is
  built with every `auto` feature disabled (configure
  `--without-default-features`), `XXH_CPU_LITTLE_ENDIAN=0` included, so
  its XXH3 matches no other xxHash (a zero page hashes to
  `d91784870c6ee3f1`, not `93d76fe148c689ba`); `fast_hash` only compares
  its own hashes, so it is left alone. Cross-binary, load 10, median of
  10: sha256 57.6 → 54.9 ms (same algorithm), XXH3-128 4.4 ms (−92% wall
  against sha256), `pages` 125 → 68 ms wall (page part 62 → 5 ms; 0.8 ms
  with XXH3), process cycles for `pages` unchanged (385 → 392 Mcycles).
  XXH3 replies equal across identical det runs, match an independent
  xxHash 0.8.3 build of the dumped RAM, and change in exactly one page
  after a one-byte gdbstub write.
- **gdbstub breakpoints are guest PC hook sites (2026-10-16).** An
  armed `Z0` made every lookup in its page take CF_BP_PAGE (one
  instruction per TB, no goto_tb), turned off the return memo, the
  icount step cut and new cross-page links, and every `Z0`/`z0`
  invalidated the page (~460 TBs per `z0`/`s`/`Z0`/`c` stop). Each
  BP_GDB PC is now a hook site (`xemu_gdb_bp_site_insert`): its TB calls
  a `XEMU_HOOK_PRIO_GDB` hook, first at the site, that raises EXCP_DEBUG
  after `cpu_restore_state` unless the vCPU is single-stepping (`s`, and
  the Qxemu.stepover step), however the TB is entered. The lookup's
  exact-PC check stays; CF_BP_PAGE and the fast-path opt-outs remain
  only for DR7 (BP_CPU) breakpoints (`xemu_cpu_breakpoints`). Only a
  PC's first breakpoint invalidates its page (norecycle); a `z0` leaves
  the hookless site, so re-arming costs nothing. CF_BP_PAGE also kept
  apart TBs translated at another mapping of the page, which i386's
  CF_PCREL lookup otherwise hands to the breakpoint's address, so every
  hook site now also cuts and calls at the RAM addresses its PC has
  mapped to; the call reads the running PC. Det heavy-f1, `TICK`
  (`0x8C230`) run once through its `0xF0000000` alias (the NV2A's view
  of RAM) with a breakpoint on its third insn: the next call at
  `0x8C230` ran the alias's TB past it, and the first stop came 14.1 s
  later; now that call stops. Same-binary pairs at
  load 15-16, heavy-f1 120 frames: a never-hit `Z0` on the hot TICK
  page costs +0.1%/+0.6% Mcycles/frame and +0.1% Minsns (old binary
  +31% and +50%, 22.09 vs 16.81 Mcycles/frame); per stop, `z0`/`s`/`Z0`/
  `c` 14.6 → 3.3 Mcycles with no invalidations beyond the advance's own
  (56,034 → 374 per 120-frame walk), Qxemu.stepover 7.7 → 2.9 Mcycles.
  A 300-hit gdb walk to frame 300 ends on `f300=6a553d8c7727c6c6` both
  ways (stepover and z0/s/Z0), 11.5 → 7.4 Gcycles. gdb alltests: only
  the three known rerun.py FAILs; hooks run_all 95 PASS; goldens exact on
  release and debug-TCG; xbox suite 7/7.
- **The TB check after an incremental restore visits only written pages
  (2026-10-16).** Every load hashed all ~36.5k live TBs against their
  bytes (`xemu_tb_revalidate_after_load`: 42% of the restoring thread in
  the incremental-restore profile, `XXH3_64bits` 31%), though a restore
  copies ~1.7k of 32k pages and drops ~129 TBs. After an incremental
  restore, `xemu_tb_revalidate_pages` hashes the TBs, and re-protects,
  only on the pages `xemu_checkpoint_ram_reval_scope` names: those
  `restore_page` wrote plus every page the migration dirty log saw
  written since the last check (reset and the device load included;
  `Block.reval` survives saves, and counts only while the log ran since
  that check). `XEMU_TB_PEEKS` TBs come from their own list and are
  always dropped; pages a dropped TB leaves off the scope get the
  page-empty tail. loadvm, incoming migrations and full restores keep
  the full walk. Cross-binary against the previous commit, two det null
  instances side by side, heavy-f1, 5 interleaved reps (load 17): an
  incremental restore after 16 frames 17.5 → 12.2 Mcycles (−31%, 5/5),
  5.5 → 3.8 ms; `xemu-run` L=1 −11.9%, L=4 −4.9%, L=16 −3.2% (5/5 each).
  All four commits against the branch point (lat.py, load 16-18): an
  incremental restore after 16 frames 19.9 → 13.5 Mcycles, with nothing
  run 17.2 → 9.2; a 16-frame `xemu-run` 362.5 → 328.3 Mcycles.
  `XEMU_TB_REVAL_REFUTE=1` (the full walk after each scoped check,
  counting what it alone would drop or re-protect) reported 0 extra
  drops, 0 unprotected and 0 untailed pages over 1,110 scoped restores
  each on heavy-f1 and stream-e2 (random `xemu-run` candidates from two
  checkpoints, saves against a base between them; 201 more on
  debug-TCG), and through the hooks (1,126 loads) and loads (260) suites;
  `verify` restores found `pages-missed` 0, and 300 candidates matched a
  full restore's reply, trace and RAM sha256.

### Retired diagnostics and dark experiments (2026-10-10)

Each removal is its own commit; to bring one back for a re-measurement,
revert that commit. The numbers stay where they were recorded. The
committed PGO profile predates these commits, so it is retrained before
the next tag.

- **`XEMU_PGRAPH_MMIO_STATS` removed.** The per-register PGRAPH MMIO hit
  counter (`a477ad9f91`) served the PGRAPH lockless audit, closed
  2026-07-11; its results stand in the PGRAPH bullet under Threads +
  runtime and in `Finished/pgraph-lockless-audit.md` §G. `pgraph_read`
  and `pgraph_write` lose an `unlikely()` latch check each.
- **Trace-formation machinery removed.** `XEMU_SUPERBLOCK`,
  `XEMU_REGION` and `XEMU_REGION_CHECK`, plus the two censuses that
  sized the campaign, `XEMU_CCOP_CENSUS` and `XEMU_SUPERBLOCK_SIZE`. All
  five pre-registered policies lost and the 2026-07-18 STOP was
  permanent. The reason given for keeping the code, a complete
  reproducible record, no longer held: the kept code could only re-run
  the losing A/Bs the STOP forbids, and the region former had a latent
  rollback bug (a join that straddles a page could lose its label). The
  record is the Failed rows, `Finished/roadmap-closed-items.md` ranked
  item 1 and archaeology 6.7-6.8. `tcg/tcg.c` loses the liveness
  relaxation and the double pass; `include/tcg/tcg.h` and
  `accel/tcg/cpu-exec-common.c` match upstream again; `gen_CALL`,
  `gen_JMP` and `gen_jmp_rel`'s goto_tb gate are upstream's; the
  `tb->xemu_ccop` byte is gone. Generated code is unchanged with every
  knob unset. On the host side the CCOP latch was an out-of-line call
  on every TB execution (`cpu_loop_exec_tb`) and in `curr_cflags()`,
  about 1e9 calls in the PGO training run; unmeasured, no fps claim.
  The code lived in `ee1de9f654`, `ce38bb8204`, `5274b3b050`,
  `bbb802c8c8`, `3b17ae08e9` and `d65fc5f83e`; restore it by reverting
  the removal commit, never `bbb802c8c8` or `3b17ae08e9`, which also
  carry live code.
- **`XEMU_TB_RANGE_INV` removed.** The knob put upstream's per-TB
  byte-range overlap check back into the Xbox whole-page code-write
  invalidation (`69636c84a7`, 2026-07-11). It was killed on a
  notdirty-trap proxy (25.2×) before any fps A/B, and Gate-0's timed run
  later showed that proxy was an artifact of the O(N) scan. Sub-page
  dirty tracking superseded it the same day: arm (a) makes the same
  keep-the-TBs decision with an O(1) consult before this loop is
  reached. The record is its Failed row, archaeology 1.22 and
  `Finished/subpage-gate0-prediction.md` §0/§3.
  `tb_invalidate_phys_page_range__locked` is back to the counting-only
  shape, its `!XBOX` side matches upstream again, and the `(b)`/`(b')`
  `XEMU_INV_PROF` counters stay (the `(b')` line drops its `RANGE_INV=`
  field). Default path unchanged: one per-TB latch check and an
  always-false skip are gone. Restore it by reverting the removal
  commit, never `69636c84a7`, which also added the live `(b')` counters.
- **`XEMU_HARDENING` removed.** `build.sh`'s `=0` arm appended
  `-fzero-call-used-regs=skip` (`00a404c774`; reachable on every platform
  since `5dfde3356d`), with a compile-database flag-order checker. The
  skip arm failed two sign tests on macOS on 2026-08-04 (5 pairs 3+/2−,
  then 8 pairs 3+/5−). The record is its Failed row and archaeology 8.7,
  which now say the macOS verdict does not carry to other hosts. The
  whole mechanism is one compiler flag that upstream's `CFLAGS`
  passthrough already reaches on every arm, so a retest is
  `CFLAGS=-fzero-call-used-regs=skip ./build.sh`, checked with the
  one-liner in the Failed row. Default build flags and the binary are
  unchanged; build logs lose the `Hardening: ... kept (default)` line.
  Reopened and shipped for macOS arm64 on 2026-10-15 (Build +
  packaging): `meson.build` no longer adds `used-gpr` there, so the
  `CFLAGS` recipe is a no-op on macOS arm64 and only matters on other
  hosts.
- **`XEMU_RETC_BITS` removed.** The ret-memo index-width knob
  (`4ca101772b`, 2026-07-18) was a one-binary A/B for 12 vs 13 bits,
  killed the same day (13 bits −0.23 fps, 4/4 negative on F8). Sizing
  the table for its 13-bit maximum had doubled the default path's
  `env->xemu_retc` and its per-flush memset (128 → 256 KiB) with no
  measurement; both are back to 128 KiB. The width is the compile-time
  `XEMU_RETC_BITS` (12) in `target/i386/cpu.h`, and generated code is
  identical (the shift immediate is still 20). The record is the Failed
  row "Ret-memo 13-bit index" and the closed roadmap item 2.
- **`XEMU_SURFACE_CB_REUSE` removed.** The 4-entry surface
  CPU-access-callback reuse pool in `pgraph/vk/surface.c` (`804bc5d87e`)
  goes with its `surface_cb_audit` coverage checker (and that checker's
  33 ms expiry-tick call) and the pool drains in
  `pgraph_vk_surface_flush` / `pgraph_vk_finalize_surfaces`; it was
  killed 2026-08-04 at −0.66 fps 3/3 (draws/s −1.9%) against a ≥ +0.25
  bar. `register/unregister_cpu_access_callback` are back to upstream's
  text, with the census hooks moved to their two call sites.
  `XEMU_SURFACE_CB_STATS` stayed as event counts plus the
  `update_surface_part` split, without the pool counters, the audit or
  the `=2` abort level, until its own removal on 2026-10-15 (below).
  Default path unchanged: with the knob unset the pool never ran and the
  drain loops never found an entry. The live question, a ranged TLB
  flush, landed 2026-10-15 as `tlb_flush_host_range`.
- **`XEMU_X87_ELIDE_CLEAN` removed.** The clean-ST(i) write-back elision
  (built in `8da40e6fed`, killed in `7aa746a952`, both 2026-08-04) goes
  with its `clean-wb` refuter check and the per-slot x87 dirty tracking
  that existed only for it (`xemu_fp_dirty`, the mark/clear helpers,
  `XEMU_ST_DIRTY` at 21 `ops_fpu.h` writer sites and the FXCH marking);
  the census loses its ST(i) clean/dirty split. It was a numeric
  behaviour change (it kept 80-bit residue the write-back truncates),
  it put a dirty-mark duty on every future ST(i) writer, its kill was an
  under-powered 3-pair read (−0.23 fps, 3/3), and its reopen bar did not
  hold together. The record is its Failed row, with the corrected
  mechanism. Generated code is unchanged with every knob unset;
  `ELIDE_FT0`, `DEFER_FIP` and the FT0/exception-pointer checks in
  `XEMU_X87_REFUTE` are untouched.
- **`XEMU_PRESENT_DRAWABLE_FIRST` removed.** The opt-in Metal present
  reorder (`ui/xemu.c` + `ui/xemu-metal.m`, built in `5b8aef5717`) took
  the drawable before reading the push-present ring. Its only result
  was an fps A/B (+0.50 under noise) on a latency-only change that fps
  cannot show, and its live regime (the bounded-debt catch-up firing, or
  the pull path) was never measured on presented-frame age. The record,
  with the reopen bar, is its Failed row (the entry moved there from
  Changes). `xemu_metal_acquire_drawable()` is folded back into
  `xemu_metal_begin_frame()`, the `drawable_acq` counter stays, and the
  default path is unchanged (acquire after the ring read, as before).
- **`XEMU_PFIFO_KICK_STATS` removed (2026-10-15).** The `pgraph_write`
  kick census and the PFIFO idle-park counter (`804bc5d87e`) answered
  their one question on 2026-08-04: no kick is suppressible (Settled
  vectors), and never suppressing one is an invariant. `pgraph_write`
  loses a latch check and four note sites, `pfifo_thread` a getenv and
  a counter in the park path; all three `pfifo_guest_kick` calls and
  the park/doorbell logic are unchanged. Nothing else counts parks (the
  `XEMU_PFIFO_HEARTBEAT` `iters` field counts loop passes); revert the
  removal commit to count them again.
- **`XEMU_SURFACE_CB_STATS` removed (2026-10-15).** The surface
  CPU-access-callback census (`804bc5d87e`) sized the reuse pool (killed
  2026-08-04) and then the ranged TLB flush, which landed 2026-10-15 as
  `tlb_flush_host_range`; its receipt (199.8 events/s on F8) stays in
  the Vulkan renderer bullet. The ranged refill does not touch `info
  jit`'s flush counters, so re-counting the events means reverting the
  removal commit. `invalidate_surface_full`, `surface_put` and
  `update_surface_part` lose their latch checks.

## Failed / reverted experiments

Lessons worth preserving so they aren't re-attempted.

| Attempt | Reason |
|---|---|
| Surface-callback reuse pool default-on (`XEMU_SURFACE_CB_REUSE`, 2026-08-04) | The mechanism receipt was real — the zeta ping-pong's ~200 register/unregister events/s are 0.95:1 with ALL in-scene guest TLB flushes, each with a jump-cache wipe that also clears the near-return memo (`tcg_flush_jmp_cache` → `xemu_i386_ras_flush`) — and the pool survived every correctness gate (loadvm cycles clean, coverage audit 23,003/0, movement probe `tb_flush` flat). The fps A/B then read **−0.66 fps, 3/3 unanimous negative, draws/s −1.9%** (idle-gated pairs, bar was ≥ +0.25): post-candoio-elision the vCPU's flush/refill cost was already off the critical path — the audit's +0.35..+1.5 band was priced against the PRE-wave profile — while the pool's park/reattach bookkeeping bills the render thread on every shape swap. That explanation was never measured: the bookkeeping is a 4-entry scan, and the A/B's pair order is not on record. Kept dark in-tree as the record until 2026-10-10, when the code was removed; restore it by reverting the removal commit, never `804bc5d87e`, which also carries `XEMU_PFIFO_KICK_STATS` and the SPIR-V cache key. `XEMU_SURFACE_CB_STATS` stayed, minus the pool counters and audit, until 2026-10-15 (Retired diagnostics). The live shape was the ranged-flush FIXME in `physmem.c`/`cputlb.c`, landed 2026-10-15 as `tlb_flush_host_range` |
| x87 clean-ST(i) write-back elision default-on (`XEMU_X87_ELIDE_CLEAN`, 2026-08-04) | Refuter said safe: it re-read memory for every clean slot at each flush and compared it with the cached double, 3.49e9 checks, 0 violations. That certifies the dirty marks were complete for every ST(i) writer the soak reached. It could not see the 80-bit residue the elision keeps, so the truncation signal was absent by construction. The idle-gated A/B read **−0.23 fps, 3/3 negative** vs the ≥ +0.20 bar. The original explanation here (the elidable flush population too thin to pay for the per-slot dirty tracking) was wrong. The dirty tracking ran at translate time and was identical in both arms, so the omitted store was the only runtime difference. The kill is an under-powered read: 3 pairs (one-sided sign p = 0.125), and the in-tree harness of that day ran B before E in every pair. **Code removed on 2026-10-10.** It changed numerics, it put a dirty-mark duty on every future ST(i) writer (21 sites in `ops_fpu.h` plus FXCH), and its reopen bar (clean-ST census ≥ 8M/s on some other title) did not hold together: the 7.04M/s census reading came from a run whose fps was not recorded, so the bar was never tied to a measured population at the ~48 fps shape the kill ran on. Restore it by reverting the removal commit, never `8da40e6fed`, which also carries the shipped `ELIDE_FT0` and `DEFER_FIP` mechanisms. Reopening needs per-op host-PC attribution tooling (`XEMU_GUEST_PROF` resolves only to whole TBs) plus the artifact A/B on a title that exercises the x87 transcendentals, which promotion always required and never got |
| Reports idle-budget 300 → 50 µs (2026-08-04) | The deferred pinned-value scan ran: **−12.1 fps unanimous, draws/s −21.4%** — 50 µs qualifies far more idle episodes for early submits and lands on the arch-1.1 submit-storm side of the cliff, exactly as the scan's guard predicted. 300 µs validated as correctly above the cliff; scan closed permanently, and the knob was hardcoded at 300 µs on 2026-10-15 |
| Ret-memo 13-bit index (`XEMU_RETC_BITS=13`, 2026-07-18) | Doubling the eip→TB memo (4096→8192 entries, 128→256 KiB) predicted +0.3-0.5 fps from fewer hash collisions; measured **−0.23 fps, 4/4 pairs negative** (scene identity clean, arm (b)-on shipping context). Same mechanism as the jump-cache 12→16-bit kill: the recurring ret set already fits at 12 bits, so the larger table pays cache footprint on every probe to avoid collisions that were mostly one-shot. "Recurring" is bounded by flushes: every full guest TLB flush (~190/s post-wave, nearly all from surface callbacks) wipes the memo through `tcg_flush_jmp_cache`, so what fits is the ret set seen between two full flushes; a per-page flush (INVLPG) drops only that page's entries, from the jump cache and, since 2026-10-10, from the memo. The A/B ran one binary with the table sized for 13 bits, so both arms carried 256 KiB and a 256 KiB memset per flush (the 12-bit arm addressed only the first half); that doubling of the default path was never measured against the 128 KiB build. **Knob code removed 2026-10-10**: the width is the compile-time `XEMU_RETC_BITS` (12, `target/i386/cpu.h`), the table and its per-flush memset are back to 128 KiB, and generated code is unchanged (same shift immediate). A re-test is a two-binary `scripts/bench-abx.sh` interleave, worth running only if the ranged-flush FIXME stops the per-flush wipe and a census shows ≥ 1M collision fills/s. Neither a memo hit nor a jump-cache hit checks the physical page; until 2026-10-10 INVLPG cleared the jump cache per page but never the memo (fixed then: see the near-return memo bullet under CPU / JIT) |
| TB byte-range invalidation filter (`XEMU_TB_RANGE_INV`, 2026-07-11) | `XEMU_INV_PROF` measured 100% of TB invalidations as data-write false sharing (0 true SMC, 100% recycle-hit) — so re-applying upstream's overlap filter looked free. Counter A/B: invalidations 338k → 675 (works) but notdirty traps **25.2×** (198k → 4.98M, per-run totals) + 46M range scans — whole-page invalidation is what empties the page and fires `tlb_unprotect_code`, making subsequent data writes free. Base xemu dropped the check in `703566ce33` with no rationale recorded; `c1e4e87f5f` added the invalidated-TB recycle cache 20 days later, which suggests a performance choice. Killed on that trap proxy before any fps A/B, and the proxy did not hold up: the Gate-0 timed run measured invalidating traps 190k → 1.56M (9.3× per second) and total `notdirty_write` calls only 1.89×, a +0.7% vCPU body cost that came entirely from the O(N) range scan (archaeology 1.23 lesson 1). Never fps-measured; superseded the same day by sub-page dirty tracking, whose arm (a) is this filter with an O(1) bitmap consult in place of the scan (default on since 2026-07-11; arm (b) since 2026-07-18). The knob shipped dark until its code was removed on 2026-10-10; the `(b)`/`(b')` `XEMU_INV_PROF` counters that measured it remain |
| L2 victim jump-cache (2026-07-05) | 64k-entry victim tier probed on L1 miss looked capacity-shaped (410k TBs vs 4096 entries) — but live hit rate measured **11.2%**: residual misses are one-shot/cold pcs, not a recurring set. Ceiling ~0.1 fps; instrument-killed without an fps A/B. Measure the miss stream's *shape* (recurrence), not its volume, before building any cache tier |
| JIT write-protect flip caching (2026-07-05) | Sampling attributed 9.7% of the vCPU thread to per-TB-entry `pthread_jit_write_protect_np`; a thread-local skip of redundant flips measured **parity** (6 pairs, −0.10 mean). The 9.7% was `thread_suspend`-sampling skid onto barrier instructions — discount barrier-heavy symbols in suspend-based profiles. **Reopened and shipped 2026-10-15** (CPU / JIT, No W^X flip per TB entry): the deterministic cold boot enters TBs ~5k times per frame, and self-closing brackets with no cached state removed the flip there for −2.07% Mcycles/frame (misc lane, 22329bd8bc). heavy-f1 and stream-e2 gained ≤0.25% (unresolved) although PMI had put the flip at 1.7-2.1% of their vCPU: PMI skids onto the ISB too, so check a barrier-heavy symbol's share against entries × cost before trusting it |
| Per-depth return-address ring (2026-07-05) | Classic shadow stack: eip prediction paired at 99.6%, but per-depth slots are shared by every same-depth call site, so TB fills ran 2.6× hits; a 512-deep ring changed nothing (a key problem, not a depth problem). Superseded same day by the eip-keyed ret-target memo: the ret target is already in `env->eip` at dispatch, so depth-shaped state adds nothing a target-keyed cache doesn't |
| Vertex copy-on-conflict transient remap (`XEMU_VTX_TRANSIENT`, 2026-07-05) | Replace the ~6/flip conflict finishes with per-slot transient copies + remapped draw bindings. Faithfully implemented; **−2.09 fps, 6/6 pairs negative**: the title rewrites broad vertex ranges every frame, so the 64-entry/2 MiB table overflowed every frame (each overflow an all-slot drain, heavier than the waits it replaced) while the remap search ran 227 times/flip. The targeted submitted-slot wait remains the shipped design |
| TB jump-cache enlargement (12→16 bits, 2026-07-05) | 93.4% hit at 15.8M lookups/s made capacity misses look like free money; measured **−1.10 fps, 4/6 pairs negative** — the 4096-entry (64 KiB) cache is L1-resident, and a 1 MiB cache pays a few ns on each of 14.8M *hits*/s to avoid ~500k walks. Target lookup *volume*, not cache geometry |
| Eager report submit (`XEMU_REPORTS_EAGER=N`, 2026-07-04) | Submit the recording CB when its Nth zpass report is *requested*, front-running the guest's poll stall. Mean −0.08 fps (6 pairs, sign-inconsistent) with a +4% draws/flip composition shift. With the 300 µs idle budget the residual wait is GPU catch-up time — eager submission moves the submit without shrinking the wait |
| Occlusion-rework intermediates: in-pass queries with synchronous drains; submit-on-idle per pending report | Both **regressed** vs the 15.8 fps baseline the rework started from (6.9 fps; ~11.2 fps at ~144 tiny submissions/flip). Query placement × report delivery is a policy *pair* — never evaluate piecewise (the shipped pair: 2.4x, +144%) |
| Per-flight vertex-RAM mirrors | One 128 MiB host mirror per flight slot so in-flight slots read frozen data (cross-slot conflict waits structurally impossible). Measured ~**neutral** (an initial "-30%" read traced to an invalid baseline parked on a menu): the dominant cost was the *recording-CB* boundary-page conflict cascade, which mirrors can't address. Reverted — +128 MiB and delta complexity for no win. The salvage, byte-exact conflict refinement, shipped separately (+5.4% fps) |
| `floatx80` union overlay on ARM64 | Layout incompatible with IEEE 64-bit — segfaults |
| Voice register `__thread` cache | Stale data; Xbox HW mutates voice regs via DMA |
| Async MetalFX under *GL presentation* | GL↔Metal cross-API sync can't be expressed with `SDL_GL_SwapWindow` + vsync alone. Landed later for the Metal presentation backend, where both sides speak `MTLSharedEvent` |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export (CPU readback or `vkExportMetalObjectsEXT`) | CPU: 2 ms/frame + 18 MiB of copies for minimal quality. Metal export deadlocks MoltenVK's internal mutex |
| Direct-VRAM compute unswizzle via `VK_EXT_external_memory_host` | `HOST_WRITE → SHADER_READ` barriers enforce visibility up to *submission*, not GPU execution; on HOST_COHERENT memory CPU can tear reads mid-frame → particle/HUD flicker |
| Host-imported `BUFFER_VERTEX_RAM` | Same torn-read class for vertex data; GPU fetches see two CPU frames mid-primitive on dynamic meshes |
| `pthread_jit_write_with_callback_np` for `tb_phys_invalidate` | Scoped-W semantics force-drop W on callback return, incompatible with QEMU's nested JIT-write paths |
| `VK_KHR_dynamic_rendering` full lowering | Missing `VK_SUBPASS_EXTERNAL → first subpass` dep; back-to-back passes stalled Metal's tile renderer |
| APU voice resampler `rate == 1.0` fast path | Skipped libsamplerate but didn't replicate silence-padding on voice drain → `voice_process` spun on draining voices during scene transitions |
| `HLT` BSOD recovery (IF=1 force at-HLT) | Unblocks `CLI;HLT` deadlocks, but the IRQ-loop pattern is a *symptom* of a stuck thread elsewhere |
| Surface-expiry throttle keyed on `pg->frame_time` | `frame_time` advances only on `NV097_FLIP_INCREMENT_WRITE`; rapid level loads starved the prune → black-screen freeze. Switched to host wall-clock |
| Surface-range binary search on `end` | `end` isn't monotonic in a start-sorted array → skipped valid overlaps |
| Two-level quick texture hash / hash-skip > 64 KiB | 192-byte sampling missed content changes (YUV / FMV); false positives caused GPU re-upload churn |
| Dirty-range VRAM flush (early) | Bitmap cleared before `flush_memory_buffer` read it. Tracked per-flight `[first, last]` range does the safe equivalent |
| Incremental texture hash on misaligned textures | Host-page boundaries straddle chunks → `test_and_clear_dirty` steals another chunk's signal. Gated to page-aligned textures only |
| HRTF hand-NEON (gather over taps) | Gather-then-FMA on a circular buffer defeated OoO overlap; `-O3 -mcpu=native` autovectorizes better. Lanes over outputs later shipped for settled and moving-parameter frames (2026-10-15) |
| BQL event batching | Breaks QEMU cooperative scheduling |
| TLS-cached `pthread_jit_write_protect_np` | Per-thread cache drifts from kernel W^X on nested `tb_gen_code` / setjmp paths → SIGSEGV/SIGILL after ~1 min |
| Hoist `can_fifo_access` out of pfifo pusher word loop | `pfifo_run_puller` drops `pfifo.lock` when taking `pgraph.lock`, so "lock held throughout" is false → `ERROR_CALL` on boot. The deterministic inline run does hold both locks throughout, and since 2026-10-15 its copy of the loop drops the repeated checks (Threads + runtime, Leaner deterministic inline pusher) |
| DSP JIT round-4 `cur_inst` preset skip | Regresses on startup with `op=0x001000` — some inlined path reads `cur_inst` at runtime that the static classifier doesn't see. Harness removed; recover from `1f444fa272`/`4668947cc9` |
| DSP JIT round-4 EPI_NO_PC | 9.75% mismatch per pcskip sentinel — calc_ea mode 6 lengthens parmoves and REP/DO loops rewind pc; both legitimately diverge |
| Vblank cadence aligned to host display refresh | Host `vblank_interval_ns` drives `process_vblank` → `graphic_hw_update` → `nv2a_vga_gfx_update` → `NV_PCRTC_INTR_0_VBLANK`. Retuning retimes the *guest* IRQ (titles gate sim on vblank count). Needs NV2A-internal timer (fixed 60 / 50 Hz) decoupled from host present cadence |
| AArch64 `fsin` / `fcos` libm-helper inline | Swapping `gen_helper_fsin` / `_fcos` (floatx80 round-trip) for a thin `helper_sin_fast_f{32,64}` / `cos_fast_*` (f-bits → `memcpy` → libm → bits) made audio crackly on at least one title. Plausible causes: (a) `gen_flush_fp` was also the de-facto flush for *other* live x87 temps; (b) `TCG_CALL_NO_RWG_SE` let TCG reorder across boundaries the floatx80 helper implicitly fenced |
| Voice-list `SE2FE_IDLE_VOICE` re-notify skip | No guest-write hook on `NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` transitions → can't reliably clear a "notified this activation" flag. Risk of missing an FE-observed idle transition outweighs the per-frame save |
| Cross-TB FPCR elision via `cached_fpuc_rc` as TCG global | TCG globals are memory-backed and reloaded per TB — a global doesn't eliminate the per-TB `ld16u` in `gen_flcr`, it just renames it. Eliminating the load needs either TB-chain metadata (RC equality across chain edges) or relaxing `gen_flush_fp`'s `flcr_set` reset — the same implicit-fence class that made the fsin/fcos inline regress. No win available at acceptable risk (`cached_fpuc_rc` and the cached `gen_flcr` were removed with the inline cache on 2026-10-15) |


Session 2026-07-18 additions — the superblock seam-following campaign
(the mechanism shipped dark and was removed on 2026-10-10; full story in
`docs/Finished/roadmap-closed-items.md`, ranked item 1):

| Attempt | Reason |
|---|---|
| Superblock M1 alone: concatenate through unconditional same-page forward jmps (`XEMU_SUPERBLOCK`) | Instrument-killed pre-A/B: the runtime tail-kind census measured the capturable subset at **3.7% of taken goto_tb exits** (~1.26M/s) against a pre-registered ≥10% build bar |
| Superblock M2, all-conditionals policy (cap 4) | Unanimous 3/3 regression: draw throughput **−1.6%**, feedback-amplified to −8.4% fps (the guest scales per-frame effect load with frame time, so the slower arm also reads ~7% heavier frames — compare throughput, not raw fps). Suspected loop back-edge demotion (hot taken edges → lookup stub) |
| Superblock M2, forward-conditionals-only policy | Refuted the loop-protection hypothesis: still **−2.3% throughput, 3/3 negative** with ~29k seams/run merged (4,922 jmp + 24,064 fallthrough). Root cause understood: TCG must spill dirty cc/eip globals before ANY branch whose taken edge exits the TB, so conditional seams structurally cannot elide the flag materialization the census counted — only designs where BOTH successors stay inline (region/diamond formation) can harvest the censused 39-44% |
| Region/diamond formation, 64 B window (`XEMU_REGION=4`) | The both-edges-inline design, enabled by a recorded-label liveness elision in tcg.c (drop `TS_MEM` for globals provably dead into a forward join; no allocator edits; bit-identical when unflagged — proven over full runs by the `XEMU_REGION_CHECK` double-pass harness after its own index-misalignment bug was fixed). Candidate census passed its ≥15% gate at **21.4%**; the A/B still lost: **−1.34 fps (−4.5%), 3/3**, B arms ±0.30. Mechanism: **72% of opened regions drain-demoted** into the known-negative stub shape (arms hit block-enders before the join) — 11,240 demotes vs 4,290 internalized |
| Region/diamond formation, 16 B window (the single pre-registered mechanism iteration) | Predicted the demote ratio would crater below 25%; measured **64%** (4,918 vs 2,794) and the A/B still **−0.69 fps, 3/3 negative**. Campaign STOPPED permanently per pre-registration. Final verdict on the census's 39-44% dead-flag mass: **not harvestable by any translate-time policy tried** (five policies, all negative, each mechanism identified); the liveness elision itself checked out sound under its double-pass checker; both were removed with the rest of the machinery on 2026-10-10 |

Session 2026-08-04 additions — the fork-wide optimization wave
(candidates from `docs/Finished/fork-optimization-audit-2026-08.md`; the
mechanisms that survived are in the Changes manifest above):

| Attempt | Reason |
|---|---|
| Drop upstream's `-fzero-call-used-regs=used-gpr` on macOS builds (`XEMU_HARDENING`, 2026-08-04; knob removed 2026-10-10) | **Reopened and shipped for macOS arm64 on 2026-10-15** (Build + packaging): the deterministic host counters resolve what these fps sign tests couldn't: host instructions −3.15% per frame, and cycles −1.2% to −1.7% per frame depending on host load. `meson.build` no longer adds `used-gpr` there, so the `CFLAGS` skip recipe below only matters on other hosts. The original record follows. The ROP-gadget register zeroing costs real work — the zeroing movs measured **3.4-3.8% of this fork's dynamic compiled-instruction stream**, and the flag also defeats tail-call optimization (`bl`+`ret` where a `b` would do) — in a process that runs a W^X JIT and is not a sandbox boundary. Pre-registered **+0.11 to +0.83 fps (central ~+0.34)**. Measured over 5 interleaved cross-binary pairs: mean **+0.52 fps but signs 3+/2−**, which is worse than sign-mixed-by-one and therefore below the pre-registered evidence bar — killed as pre-registered, not renegotiated. The binary-size receipt (`__text` **8,741,512 B hardened vs 8,330,592 B skip = −411 KB**) is real and was explicitly ruled insufficient to override an fps bar the change did not clear. Default reverted to upstream's `used-gpr` in `build.sh` (`00a404c774`). **The same-day retest failed the bar again**: the promised quieter-machine rerun (8 idle-gated pairs, post-upstream-merge tree, `7aa746a952`) read **mean −0.39 fps, signs 3+/5−**; pooled with the first run that is −0.04 fps over 13 pairs. Upstream's hardening stands; the −411 KB `__text` saving buys nothing measurable. **What the verdict covers**: macOS on Apple Silicon, closed by two failed sign tests. Neither run is recorded as using a PGO profile trained on its own tree (the first came before that morning's retrain `6ff4467c27`, while the new staleness gate was reporting 7 hot-path mismatches; the merged-tree retrain landed in the retest's commit, after the campaigns ran), draws/s was never recorded, and the local toolchain has since been replaced (Command Line Tools 27 installed 2026-09-10, Xcode 27, Apple clang 21). It does not carry to other hosts: arm (b)'s inline store stub is absent on x86_64 and default-off on non-Apple aarch64 since `0aeb1998c3`, so more stores take the compiled-C slow path the zeroing taxes; the win64-cross arm64 leg is llvm-mingw clang, so the tail-call loss applies there; GCC x86-64 keeps its sibcalls under `used-gpr`, so expect zeroing without the tail-call loss (unmeasured). **The `XEMU_HARDENING=0` arm was removed from `build.sh` on 2026-10-10**: it was one flag, and upstream's `CFLAGS` passthrough already reaches every arm. The skip arm is `CFLAGS=-fzero-call-used-regs=skip ./build.sh`: `CFLAGS` is the tail of `--extra-cflags`, which configure hands to C, C++ and ObjC and meson places after its global `used-gpr`. The recipe has never been run, so check it took effect before trusting a result: from the checkout root, `python3 -c 'import json,sys;P="-fzero-call-used-regs=";w=[t[-1] for t in ([x for x in e.get("command","").replace(chr(34),"").split() if x.startswith(P)] for e in json.load(open("build/compile_commands.json"))) if t];n=w.count(P+"skip");print(n,"of",len(w),"compile lines end on =skip");sys.exit(not w or n<len(w))'` must print N of N with N > 0 (it strips meson's Windows quoting, and exits non-zero if any compile line ends on another value or none carries the flag) |
| Running the x87, sub-page-fast and DSP-JIT refuters in one process (`XEMU_X87_REFUTE` + `XEMU_SUBPAGE_FAST_REFUTE` + `XEMU_DSP_JIT_DIFF` together) | Deadlocks the guest during **BOOT, 2/2 attempts** — a test-infrastructure interaction, not a product bug: the arch-4.5 diff-validator-starvation family, three shadow validators contending for the same execution. **Singles and pairs are clean** and remain the supported way to soak. This also retroactively attributes the campaign's run-1 `vp_write` wedge, which had no other explanation. Practical rule: cap refuter soaks at two validators per process |
| Drawable-first present ordering (`XEMU_PRESENT_DRAWABLE_FIRST`, opt-in, 2026-08-04; code removed 2026-10-10) | Acquire the `CAMetalLayer` drawable *before* reading the push-present ring, so the step to present is chosen after the `nextDrawable` block (only the acquire moved; command buffer → shared-event wait → render pass stayed together and in order, architecture Invariant 8). Built in `5b8aef5717` from the audit's §2.2 and left default-off: `drawable_acq` read **13.8-16.5 ms**, quoted as per acquire against a 4 ms "worth attempting" bar, but nsprof normalizes UI buckets per *flip* and which column that figure came from is unconfirmed; the only A/B was fps, **+0.50 under the noise line**, a proxy a latency-only reorder cannot move, with no receipt of the run's window or regime. Mechanism limits: under push, FIFO consumption returns the same step either way, so the reorder acts only when the bounded-debt catch-up fires (interpolation on and the step rate above the panel's drain rate, e.g. 2x at ~48 fps on a 60 Hz panel, the case the bounded-debt catch-up exists for) or on the pull path (the automatic fallbacks: push inactive, GL backend, before the first publish at startup or after a resize); and because the debt test runs at read time, reading after the block can itself make the catch-up fire. That regime was never measured on presented-frame age (fps cannot move) and the harness cannot reach it: the bench window is unfocused and presents unpaced (no vsync block for the reorder to hide behind), and `XEMU_UI_FRAME_CAP_NS` returns before either ordering. Removed rather than kept dark; restore it by reverting the removal commit, never `5b8aef5717`, which also carries the default-on MetalFX resize quantization, the `drawable_acq`/`ui_hud_lock`/`ui_frame_dt` nsprof buckets and `XEMU_UI_LOCK_STATS`. The `drawable_acq` counter stays. Reopen bar: a publish-to-commit `present_age` nsprof bucket showing **≥ 2 ms mean gain** with `push_policy_skip > 0` in a focused vsynced 60 Hz window |

---

## Settled vectors — measured, not worth it (or premise dead)

Candidates that were investigated and closed with data rather than
attempted-and-reverted. Reopen only when the recorded bar is met with
fresh numbers (`XEMU_NV2A_NSPROF` / `XEMU_GUEST_PROF` / the named
counter).

| Vector | Measured reality / why closed |
|---|---|
| xpage backstop severing only the cross-page jumps (2026-10-16) | The walk resets every jump into a registered dest; same-page jumps (source not an emitter, its first physical page the dest's) need no sever, as the same jumps into unregistered dests show. `XEMU_XPAGE_PROF`'s census on a normal-mode stream-e2 walk (stream-e2, 32 s window, load 40-60): 894-916 walks reset 726-749k jumps, 5.9% same-page, 5.2% from an emitter onto its own page (the emitter flag is per TB, so per-slot bits could keep at most these), 89% onto another physical page. Every registration came from an emitter (`reg-phys-only=0`). The cut keeps at most ~11% of the ~790k link patches a walk run pays, below the 50% bar. Reopen only with a backstop that keeps the cross-page chains whose target page still maps where it did |
| PFIFO spin before parking (2026-10-16) | Normal mode parked PFIFO ~10k times/s on heavy-f1/W2, most parks under 25 µs, each ended by a kernel broadcast from the vCPU and a 5-20 µs wakeup. The trial: before the untimed wait, drop `pfifo.lock` and poll `doorbell`/`fifo_kick`/`exiting` every 250 ns for up to 30 µs with `parked` false. Mechanism held (xwx interposer: parks −92..−95%, vCPU `pthread_cond_broadcast` time −83..−87%), fps did not: `bench-abx`, same binary, spin off vs on, heavy-f1 7 pairs over two sessions 2+/5− (54.25→53.94 at load 10-44; 54.19→54.26 at load 7-20, below the cap, so vCPU-bound), W2 6 pairs 56.76→59.06 (one dipped baseline) and 60.12→60.11 at the cap; process CPU +1-6% of a core. Polling with only an `isb` between reads lost fps outright (the poll steals the flags' line from the vCPU's doorbell barrier). Not committed |
| APU lock hand-off to a waiting vCPU between frames (2026-10-16) | With the frame thread at user-interactive QoS the vCPU still waited on `d->lock` for 0.73-0.91% of wall (load 11-16), two thirds of it in ~450 µs waits for the rest of an 8-frame EP burst. The trial: `vp_write` trylocks and counts itself in `fe_waiters`; after each `se_frame` the thread unlocks and waits until the waiter is through and the guest's method writes pause (10 µs quiet, 100 µs cap). xwx, 4 runs: 0.73→0.72, 0.91→0.61, 0.84→0.55, 0.86→0.45% (mean −30%): the long waits went, but each later write in a run waited for the next frame instead (40-160 µs waits up from ~150 to ~1800 per ring). Not committed: ~0.25% of vCPU wall for a timing change. Splitting `d->lock` across VP/DSP is out (`fe_method` writes outbuf/SGE state the DSP DMA reads) |
| Texture-upload barrier batching | Per-mip `pre/post_compute` pairs are load-bearing on reused `COMPUTE_DST/SRC`; total upload time ≤ 15 ms per 5 s even during streaming-heavy transitions, ~0 steady state — not worth the risk |
| GPU S3TC decode (compute shader) | Total texture-upload CPU incl. S3TC ~0 steady, ≤ 15 ms/5 s streaming — no longer a meaningful cost center |
| `VK_EXT_external_memory_host` revival (snapshot scheme) | The snapshot memcpy it would eliminate totals < 0.5 ms per 5 s in-game — far below the ≥ 1 ms/frame bar; the direct form is a torn-read correctness hole (archaeology 1.6) |
| MetalFX *input* ring (drop `metalfx_drain_inflight`) | Drain costs ~0.1 µs/flip steady (max ~4 ms on rare hitches) — complexity for noise |
| ~~Shader specialization constants~~ | Stale premise: alpha-test / fog-enable are *already* compile-time GLSL variants (`PshState`/`VshState` key the LRU shader cache); only value uniforms are per-draw and must stay uniforms. Useful reframing = variant-count / compile-stutter work |
| `MTLResidencySet` (macOS 15+) | Blocked, twice over: each MetalFX subsystem owns only 2-4 long-lived resources (below the technique's payoff), and the NV2A renderer runs through MoltenVK, which exposes no Metal residency sets to a Vulkan client (`metalfx_upscale.m` Phase-5 note). Re-check only on a MoltenVK capability change |
| Streamed-vertex stall reduction (CPU-side sync) | Attempted twice, both ~neutral on the then-GPU-bound attract fixtures: byte-exact refinement killed 75-85% of `finish_vtx_dirty` yet flips/s never moved; per-flight mirrors made cross-slot waits structurally impossible and totals stayed ~2.5-3.5 s per 5 s — the waits were GPU frame time. The salvage (byte-exact skip) shipped separately at +5.4% fps. 2026-07-18 re-check: current savestate fixtures are CPU-bound with ~14 ms/flip GPU slack, so the *renderer-thread* waits these touched are further off the critical path than ever |
| GPU frame-cost campaign menu (draw merging, pass cuts, `VK_EXT_multi_draw`) | Killed by measurement on the 2026-07 savestate fixtures: zero subrange-coalesce opportunity, 10.6% merge candidates at ~0 per-call PFIFO cost (prefill=0 defers Metal encoding), per-draw CPU sites < 1 ms/flip, ~14 ms/flip GPU slack, and `VK_EXT_multi_draw` was then unimplemented in MoltenVK. It landed upstream as `bc0d5bf7` on 2026-08-09 and is in the local pin, but MoltenVK records one `MVKCmdDraw` per sub-draw (`vulkan.mm:4303-4336`), so the Metal stream is identical to xemu's per-subrange loop and the saving is one direct call per sub-draw; concatenating the subranges into one indexed draw (campaign Mechanism A1) dominates it, so xemu does not request it (checked 2026-10-10). Re-run campaign Phase 0/2 gates before reviving on a future GPU-bound scene |
| Dirty-clear TLB-walk coalescing | Parked 2026-07-05 (~1-1.5 fps ceiling, widens an archaeology-1.9-class race window). 2026-07-18 gate re-check on the post-sub-page profile: renderer-attributable `test_and_clear_dirty` vCPU share is now **< 1%** (`physical_memory_is_clean` 0.47%, the walk itself absent from the top-25 sampler symbols) — the old "~6% echo" premise died with sub-page tracking. Stays parked as a walk-count change. 2026-10-16: the TLB floor had made each walk 32-64x longer (60% of the PFIFO thread, the vCPU spinning on `tlb_c.lock`), and the walks got cheap instead: writable-clean bitmaps (CPU / JIT changes) |
| Voice-register writeback batching | Closed 2026-07-18: unsafe by construction. Guest audio drivers read voice state (position / `PAR_STATE` / play cursors) directly from RAM with no MMIO interception point (`NV1BA0_PIO_GET_VOICE_POSITION` confirms live queries), so deferring `voice_set_mask`'s `ram_stl` to end-of-frame serves stale state mid-frame — archaeology 5.2's unsoundness, mirrored. The no-op-store elision already shipped |
| `qemu_cpu_kick` via `dispatch_semaphore_t` | Closed 2026-07-18: wrong premise. Under MTTCG this fork's vCPU kick is `tcg_kick_vcpu_thread` — two atomic flag stores, no signal; `pthread_kill(SIG_IPI)` (the ~50 µs P99 the vector cited) survives only on cold paths (`qemu_cpu_kick_self`, `pause_all_vcpus`). Nothing on the hot path to replace |
| DSP JIT parmove+ALU fusion | Closed for the current title corpus 2026-07-18: the eligibility proxies (`alu_ccr_nz_skipped` / `alu_ccr_eu_skipped`) read **0** on both F8 and the Azurik audio mix, and the APU thread runs ~18% utilized off every critical path — ~0 fps ceiling regardless of the fold's local win. **Closed a second time, harder, 2026-08-04: the fold is instruction-neutral by construction**, so the local win is 0 too. Static count against the post-Phase-8 emitter every fold would have to change — `emit_pm_read_reg` (`dsp56k_jit_arm64.c`, ~:2029 as of 2026-10-10; that file moves, so re-derive with `grep -n -A85 "static void emit_pm_read_reg"`) — whose three source classes are exhaustive: pinned X/Y = 1 `UBFX` from the pin on either side of the ALU; A/B = the same multi-path limited-read block (fast / saturate / `pm_read_accu24` BLR) either side; anything else = 1 `LDR`. Nothing is staged that a fold could delete — the read writes its callee-saved destination directly. **Do not build the write-d=0 / non-aliasing eligibility counter this row used to propose** (design-doc row 8): a 100%-eligibility reading would authorize a 0-instruction win. Narrowed reopen condition: a non-audio DSP program whose parmove sources are neither pinned nor A/B — and even then, re-derive a *nonzero* static delta before any counter or emitter work. Full arithmetic: §2.5 of the retired `xemu-frontier-and-positioning` skill (`git show ad569d78c5:.claude/skills/xemu-frontier-and-positioning/SKILL.md`) |
| COMISS / UCOMISS inline lowering (audit §1.4c) | Closed **permanently** 2026-08-04 by its own census before a line of the lowering was written. The whole candidate rode on an unmeasured compare rate, so the census counter shipped instead of the mechanism (`XEMU_X87_CENSUS=1` prints `sse scalar compares: ss=… sd=…` from `gen_VCOMI`/`gen_VUCOMI`): in-scene the guest executes **0.0 scalar compares/s** (496 for the entire process lifetime, all of them pre-`loadvm`), and **`sd` = 0** independently confirms the verifier's claim that COMISD is structurally unreachable — the guest is a Pentium III with no SSE2. There is nothing to lower. `decode-new.c.inc` was never touched; the census counter stays as the record and the reopen test. Reopen only if some title's census clears ~1.5M `ss`-compares/s |
| `pfifo_kick` broadcast suppression (audit §1.5) | Closed 2026-08-04: the suppressible population is **empty**. `XEMU_PFIFO_KICK_STATS=1` counted **0 of 412,074 in-scene kicks** with no PFIFO wait-condition transition (7 such kicks exist for the whole process lifetime, all pre-`loadvm`), so a filter that skipped provably-useless broadcasts would skip nothing. That is the good outcome: archaeology 7.1 says a wrongly-skipped kick is a permanent PFIFO hang, and the census bought the answer at zero risk. The counter was removed 2026-10-15 (revert the removal commit to restore it); suppression is not implemented and should not be. Separately noted, not proposed: 62.9% of broadcasts land with a kick already pending — provably redundant, but a *different* class from the one the audit proposed, and it needs its own sizing before anyone touches it |
| ImGui frame build under BQL + main-loop mutex (audit §1.9) | Closed 2026-08-04 exactly as the audit predicted it would be. The HUD build holds both locks, which looked like a vCPU-blocking hazard; the measurement (`XEMU_UI_LOCK_STATS=1`, which shipped precisely to answer this without a bespoke build) puts combined occupancy at **mean 0.386% / median 0.373%**, under the pre-registered 0.5% kill bar. No rework. The knob stays as the standing re-check — this is a "measure first, expect a kill" candidate that got its kill |
| `notdirty_write` dirty-check **fps claim** (audit §1.2) — the claim, not the code | Killed 2026-08-04 while the code shipped. The audit sized the candidate on a profile where `notdirty_write` carried a 75.1% trap share; the 2026-07-18 arm-(b) store-prefilter promotion then inhaled that population, and the in-scene re-measurement reads **~2.6k calls/s at a 2.2% trap share — a 34-57x collapse of the benefit base**, ceiling ≤ 0.06% of the vCPU thread, i.e. under the ±0.02 fps protocol resolution. The code stays (unconditionally since its `XEMU_DIRTY_FAST` hatch was removed on 2026-10-15) as a behavior-equivalent simplification with **no individual fps claim and no individual A/B** (Changes manifest). The transferable lesson is the proxy-collapse one: a candidate sized against a *profile* rather than a *mechanism* silently expires when an earlier landing changes the population, so re-measure the benefit base at build time, not at audit time |
| Lockless MMIO for the remaining BQL blocks (NV2A `PMC`/`PCRTC`/`PTIMER`, APU `main`/`gp`/`ep`) | Closed by written verdict 2026-08-04 — **[`docs/Finished/lockless-mmio-verdict.md`](Finished/lockless-mmio-verdict.md)**, which satisfies archaeology 7.5's reopen condition ("audits for the remaining regions are written") without building anything. Fixture-independent ceiling: the *entire* pre-campaign BQL-MMIO prize was 117k ops/s × ~52 ns = **0.61% of one core**, and PFB/USER (78%) + APU-VP (~11%) + PGRAPH already harvested it — residual **< 0.07% of a core ≈ +0.01-0.06 fps**, at or below the ±0.02 fps static-baseline resolution. Correctness wall on top: these handlers call `nv2a_update_irq` inline, so BQL-free dispatch would drive `pci_irq_assert` with no BQL at all and race the iothread's vblank raise — the `pgraph-lockless-audit.md` §C race one level up, hang class, unfixable by atomics alone. Reopen bar: `XEMU_MMIO_PROF` `BQL wait total` ≥ 500 ms / 75 s — **measured 2026-08-04 at 75.6 ms per 75 s (2,178,310 BQL-taking ops, avg 47 ns, max 82.0 µs), 6.6x under the bar**, so the verdict now rests on a measurement and not only on the arithmetic (verdict page §A.1) |
| Code buffer within ADRP reach of the binary (2026-10-16) | Darwin maps the 1 GiB buffer above the shared cache (0x300000000), more than 4 GiB from QEMU's text, on about half the launches, so helper calls build their target with MOVZ+MOVK×2 before the BLR (ADRP+ADD on the rest). A `vm_region` hole search past the text moved it within 4 GiB on every launch; no hole lies within BL reach (the ICU data and malloc zones sit between), and the 2 GiB below the shared cache sometimes has no 1 GiB hole, so the buffer shrank to the largest one. Only ~44K external helper calls run per heavy-f1 or stream-e2 frame (counted with an instrumented build), so the ceiling is −0.07% instructions; vCPU Minsns/frame read −0.01% (median pair, 6 pairs) and the spread did not narrow. Not committed |
| icount TB-entry store forwarding (`icount_decr.low` stored 16-bit, the next TB loads `.u32`) | Closed 2026-10-15: no measurable stall. A matched 32-bit store (racy, since it can drop a cross-thread kick) measured 0% pooled over 14 heavy-f1 and 10 stream-e2 det pairs. The exact form, loading the two halves separately and ORing in the sign-extended `.high`, cost +1.4..+1.5% Minsns/frame and +1.3..+2.7% Mcycles/frame. On top of the `can_do_io` change it still cost +1.3% Minsns/frame, and its cycles were mixed (−3.2%, 4/6 pairs, then +1.7%, 2/8; new against base, negative is fewer). A microbenchmark shows the partial-forward penalty, about 1 cycle, only when TB bodies are under ~25 host insns. So the sampler's 3.5% of the vCPU on the entry `ldur` is not a forwarding stall; it is probably skid from the TB-entry redirect |
