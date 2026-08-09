# xemu-macos optimization ledger

Every landed optimization and every attempted-and-settled experiment in
this fork, with the receipts that shipped or killed them. This file is
the doc of record for *what has been done*; the open candidates live in
[docs/roadmap.md](roadmap.md), and the concise user-facing manifest is
the top-level [README](../README.md).

Sections: the per-subsystem landed-change manifest (moved here from the
README 2026-07-18), the runtime escape-hatch / diagnostic knob catalog
(each knob exists because of a specific entry here), and the
failed / reverted / settled-negative experiment tables.

## Runtime escape-hatch / diagnostic knobs

All default off / fast-path; set to `1` to enable. Every knob here is
the escape hatch or measurement channel for a specific entry in the
Changes manifest below — the pairing is the fork's bisect discipline.

| Env var | Purpose |
|---|---|
| `XEMU_NV2A_NSPROF` | Wall-time frame profiler, 5 s summaries to stderr. Since 2026-08-04 it also reports three UI-thread buckets — `drawable_acq` (block inside `nextDrawable`), `ui_hud_lock` (main-loop-mutex + BQL acquire and hold around the ImGui HUD build), `ui_frame_dt` (present-to-present interval) — plus the `ui_present` and `mfx_scaler_rebuild` event counters. UI buckets are normalized per *flip*, not per present |
| `XEMU_PFIFO_HEARTBEAT` | 2-second pfifo diagnostic snapshot |
| `XEMU_ZETA_SHAPE_READBACK` | Restore GPU→CPU readback on zeta shape switches |
| `XEMU_TEX_BIND_RECHECK` | Restore per-bind texture dirty checks (vs once per frame) |
| `XEMU_VTX_EXACT` | `0` restores page-granular vertex-conflict finishes (vs byte-exact skip) |
| `XEMU_REPORTS_SYNC` | `1` restores synchronous zpass-report drains (vs flip-deferred + idle-budget fallback) |
| `XEMU_REPORTS_BUDGET_US` | Continuous-idle budget (µs) before the deferred-report safety-valve submit; default `300`, `5000` restores the pre-v0.10.1 value (clamped 0-100000) |
| `XEMU_MMIO_BQL` | `1` restores BQL-locked TCG dispatch for the audited lockless NV2A regions (PFB, USER, APU VP, PGRAPH) — bisect hatch |
| `XEMU_PGRAPH_LOCKLESS` | `0` restores BQL-locked dispatch for the PGRAPH block only (leaves PFB/USER lockless) — per-block bisect hatch for the PGRAPH audit (see `docs/pgraph-lockless-audit.md`) |
| `XEMU_MMIO_PROF` | `1` prints an exit histogram of guest MMIO traffic (region × page, loads/stores) + BQL acquire-wait totals |
| `XEMU_PGRAPH_MMIO_STATS` | `1` prints an exit histogram of PGRAPH per-register MMIO hit-rate (reads/writes, interrupt-reg + RDI category totals, top offsets) — ranks ranges for the PGRAPH lockless audit |
| `XEMU_MAX_QUERIES` | Occlusion-query pool size, default `4096` (`begin_draw` guard submits before exhaustion) |
| `XEMU_SURFACE_CB_STATS` | `1` prints an exit census of NV2A surface CPU-access-callback registrations/unregistrations — each one is an `async_safe_run_on_cpu` **plus** a full `tlb_flush_all_cpus_synced` **plus** an unconditional jump-cache wipe — split by whether the event came from `update_surface_part`'s invalidate-then-create path (the zeta ping-pong) or anywhere else, plus reuse-pool hit/park/evict counters and a live-coverage audit run on the 33 ms surface throttle tick; `2` also aborts on a coverage violation (`nv2a_vk_assert` is stripped in perf builds, so the check carries its own escalation). No behavior change |
| `XEMU_SURFACE_CB_REUSE` | `1` (**default off, dark**) parks the live `MemAccessCallback` handle on unregister into a 4-entry pool keyed on `(vram_addr, size)` and re-attaches it when an identical-key surface registers again, so a shape ping-pong stops paying two full guest TLB flushes per swap. Sound because the registered set stays a *superset* of live surfaces (the callback re-derives hits from `r->surface_ranges` and ignores the registered range); the pool evicts oldest with a real remove and drains fully at `pgraph_vk_surface_flush` / `pgraph_vk_finalize_surfaces`. Unset = byte-identical legacy path. Measured 2026-08-04 and killed (−0.66 fps 3/3; Failed table) — stays dark as the record |
| `XEMU_PFIFO_KICK_STATS` | `1` prints an exit census of `pgraph_write`'s `pfifo_kick` broadcasts (call site × whether a PFIFO wait condition actually transitioned × `FIFO_ACCESS` × whether a kick was already pending) plus the PFIFO thread's idle-park count — the denominator that says how many broadcasts could ever have woken a parked waiter. Counter only: suppression is deliberately not implemented (archaeology 7.1 — a wrongly-skipped kick is a permanent PFIFO hang), and the 2026-08-04 census closed the idea (Settled vectors) |
| `XEMU_INPUT_PIPE` | FIFO path; lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` inject input through normal bindings (works unfocused; test automation) |
| `XEMU_COREAUDIO_FRAMES` | CoreAudio buffer frames, default `1024` (≈21 ms @ 48 kHz) |
| `XEMU_MFX_REAL_DEPTH` | Feed real zeta depth to the temporal scaler (A/B): `1` = live read (one frame late), `2` = flip-time snapshot (temporally correct); unset/`0` = synthetic. Engages only when a zeta matches the scaler input dims |
| `XEMU_GUEST_PROF` | One-run guest profiler: mach-thread sampler resolves vCPU samples to guest TBs vs host symbols; TB lookups classified by exit kind. Measurement-run only (not benchmark-neutral) |
| `XEMU_RAS` | `0` disables the near-return target memo (default on): 4096-entry eip→TB cache probed inline at ret sites (+0.77 fps, 6/6 pairs — see CPU / JIT changes). `-d exec` tracing won't log inline-hit rets — disable when tracing |
| `XEMU_RETC_BITS` | Ret-memo index width, 8..13 (default 12), latched at process start; every hit is fully validated so any width is correctness-safe. 13 measured −0.23 fps 4/4 on F8 (Failed table) — a diagnostic/cache-footprint A/B knob, not a tuning lever |
| `XEMU_TB_PROF` | Prints TB jump-cache totals at exit (lookups, hit%, htable walks, translations, tb_flush count) |
| `XEMU_INV_PROF` | Prints SMC / TB-invalidation-churn counters at exit: invalidations by source (notdirty vs explicit vs single-TB), the false-invalidation share a byte-range overlap check would skip, inv-htable recycle hit-rate (false-sharing vs true SMC), and translate-time FPU/exit census; plus an `(f)` MMIO line — `io_prepare` calls, `cpu_io_recompile` calls, and the recompiles/s rate over the measured window — which is the gate counter for `XEMU_ELIDE_CANDOIO` (read it from a `XEMU_ELIDE_CANDOIO=0` run, since the elision drives the counter to zero by construction; the legacy `io_prepare` count is 2x-inflated by recompile re-execution) |
| `XEMU_TB_RANGE_INV` | `1` re-applies upstream's per-TB byte-range overlap filter inside the Xbox whole-page code-write invalidation (default off = invalidate every TB on a written code page). Correctness-safe (invalidates a correct subset — a TB whose bytes weren't written can't have changed); A/B knob for the SMC-false-sharing cure (`XEMU_INV_PROF` measured 100% false-invalidation, 0 true SMC) |
| `XEMU_CCOP_CENSUS` | `1` prints a runtime-weighted census of cc-flag liveness across TB boundaries at exit (predecessor tail class × successor head class). Forces every TB transition through the exec loop (no goto_tb / jump-cache / ret-memo) so pairs are exact — much slower, same executed instruction stream; sizing tool for the superblock roadmap item, never a perf mode |
| `XEMU_INV_TIMING` | `1` adds a `cntvct_el0`-based split of the `notdirty_write` body (invalidation+scan+recycle vs preamble+tail) to the `XEMU_INV_PROF` dump — sized the sub-page dirty-tracking arms. Timings are order-of-magnitude; counts are load-immune |
| `XEMU_ELIDE_CANDOIO` | **Default on (elide) since 2026-08-04**; `0` restores the per-TB `can_do_io` bookkeeping stores. With icount and replay off nothing observes the flag, so the two stores are dropped and the guest's mid-TB MMIO accesses stop paying `io_prepare`'s `cpu_io_recompile` round trip (`tcg_tb_lookup` g_tree walk + state restore + siglongjmp + `CF_MEMI_ONLY` re-translate + re-execute). Auto-forced to `0` under icount or `replay_mode != REPLAY_MODE_NONE` — the only observers — and latched at first translation, before the first TB. Gate counter: the `XEMU_INV_PROF` `(f)` line |
| `XEMU_DIRTY_FAST` | **Default on since 2026-08-04**; `0` restores the legacy dirty-bitmap shapes in the store slow path on the same binary — the `physical_memory_is_clean()` re-read at the tail of `notdirty_write` (now answered locally by the invalidation's "page still holds code" return) and the five separate RCU-guarded `find_next_bit` scans behind `physical_memory_is_clean()` (now one guard, one idx/offset computation, five `test_bit`s via `physical_memory_page_dirty_bits()` — which also serves the TLB-refill dirty check at `cputlb.c:1128`). Behavior-equivalent both ways; carries no individual fps claim (see the CPU / JIT bullet) |
| `XEMU_SUBPAGE_DIRTY` | Sub-page code dirty tracking — **default ON** since the 2026-07-11 A/B (`0` restores whole-page invalidation wholesale): a per-`PageDesc` 64-block bitmap lets a guest data store that misses every code sub-block skip whole-page invalidation — an O(1) bitmap test replaces the qht/jmp-unlink/recycle round-trip, leaving the page write-protected so the store re-traps cheaply. Interleaved A/B: **+4.33±1.53 fps F8 (+14.3%, 4/4 pairs), +11.63±0.52 fps F5 (+24.6%, 3/3)**; invalidations 290,595 → 11,180 (25.9×). The higher-ceiling fast-path form that removes the trap itself needs host-`tcg/aarch64` store codegen (held — see [roadmap.md](roadmap.md)) |
| `XEMU_SUBPAGE_REFUTE` | `1` runs the sub-page skip decision against a ground-truth live-TB byte-overlap scan (both pages of spanning TBs) and counts violations (design falsified if > 0); changes no behavior unless `XEMU_SUBPAGE_DIRTY` is also set. Soak: **0 violations over 28.6M filter-skips across 33 loadvm cycles** (~12 min) |
| `XEMU_XPAGE_PROF` | Cross-page-direct exit census: runtime taken-rate counter split by target region (kernel-identity vs low); also armed by `XEMU_INV_PROF`. Dark; atexit summary |
| `XEMU_XPAGE_REFUTE` | `1` = cross-page-chaining refuter: routes every cross-page-direct exit through the safe lookup path plus a page-table-walk check that the target's live phys is unchanged since last seen (loud + counted on violation). Slow; falsification only |
| `XEMU_XPAGE_CHAIN` | Cross-page direct chaining — **default ON (broad)** since 2026-07-12 (`0` restores upstream same-page-only chaining; `1` = kernel-identity window only). Sound by construction: INVLPG (per-page) backstop plus CR3 + generic `tlb_flush_by_mmuidx` backstops covering every full-flush class (0 refuter violations in 1.71B checks across the promotion soak). A/B: +1.30±0.29 fps F8 pre-subpage (7/7 pairs); +0.77±0.55 on top of sub-page tracking (3/3) |
| `XEMU_XPAGE_UNLINK` | **Default ON (v0.11.1)**: the full-flush backstop severs only the registered cross-page chains (synchronous `tb_jmp_unlink` walk; translations survive, chains relink lazily). `=0` restores the v0.11 queued whole-`tb_flush` backstop — which caused the new-area/death-reload lag (139 tb_flushes in 20 s of first-visit walking on F8, fps trough 14.6 vs 38.6 baseline; 0 flushes and no trough with the unlink). Registry overflow falls back to the full flush automatically |
| `XEMU_SSE_HOST` (alias `XEMU_SSE_NEON`) | NEON fast path for single-precision SSE arithmetic; default on for aarch64 with `perf.hard_fpu` (+1.90 fps — see CPU / JIT changes). `0` restores softfloat; `=2` runs both paths and aborts on divergence. x86_64 stays opt-in/dark: run `=2` clean on real silicon first |
| `XEMU_X87_ELIDE_FT0` | **Default on since 2026-08-04**; `0` restores the unconditional store. Skips the write-back of the FT0 scratch at an inline-FPU register-cache flush when no reader of that value is still ahead in the same guest instruction (producer sets pending, consumer clears it). A/B: **+1.56 fps, 3/3 pairs positive** on F8 |
| `XEMU_X87_DEFER_FIP` | **Default on since 2026-08-04**; `0` restores per-instruction stores. Defers the x87 exception-pointer stores (FIP/FCS/FDP/FDS) from one per instruction to one per contiguous x87 run, landing them at the flush points that already precede every consumer (`do_fstenv` via FNSTENV/FNSAVE, every helper call, `tb_stop`), and hoists the CS-selector load out of the per-instruction path (CS cannot change mid-TB). Measured cut ratio 8.81x against a 1.5x bar, mean run length 3.64. A/B: **+7.46 fps, 3/3 pairs positive** |
| `XEMU_X87_ELIDE_CLEAN` | `0` (**dark**). `1` skips the write-back of an ST(i) cache slot that was only read. Default-off on purpose: it is not a pure elision — the cache holds the double-precision projection of an 80-bit `env->fpregs` entry, so today's unconditional write-back also *truncates* that entry, and eliding it leaves the original `floatx80` in place. Invisible to the inline path, observable through FSTPT/FSAVE/FXSAVE and the softfloat transcendental helpers. Refuter reads 3.49e9 checks / 0 violations with the truncation signal absent; promotion still needs an fps + artifact A/B (roadmap) |
| `XEMU_X87_CENSUS` | `1` arms the x87/SSE Gate-0 census (audit §1.4) and its atexit dump: register-cache write-backs split clean-ST(i)/dirty-ST(i)/dead-FT0/live-FT0, per-instruction FIP/FDP store pairs vs deferred updates emitted, x87 runs + insns (mean run length), and SSE scalar compares split `ss`/`sd`. Emits counter RMWs into generated code — measurement mode only, never for an fps run |
| `XEMU_X87_REFUTE` | `1` runs the adversarial shadow-checker for the three x87 mechanisms above: keeps every legacy store, computes the elision/deferral decision anyway, and counts every runtime point where the elided state could have been observed (stale `env->ft0` fed to a reader; deferred FIP/FCS/FDP/FDS ≠ what the legacy stores left at a consumer or helper boundary; a clean slot whose memory moved under the cache). Implies the census dump. Promotion gate is zero violations. **Do not combine with `XEMU_SUBPAGE_FAST_REFUTE` *and* `XEMU_DSP_JIT_DIFF` in one run** — the 3-way combination deadlocks the guest during boot (Failed table) |
| `XEMU_SUPERBLOCK` | `=N` follows up to N branch seams per TB at translate time (superblock formation: unconditional same-page forward jmps + forward-conditional fallthroughs with out-of-line taken stubs). **Default 0 (off/dark)** — see the superblock entry in CPU / JIT changes and docs/roadmap.md for the measured policy verdicts |
| `XEMU_SUPERBLOCK_SIZE` | `1` arms the runtime taken-exit tail-kind census (uncond/call/jcc-taken/jcc-fall/rep/toomany + the M1-capturable subset), atexit dump; sizes superblock policies with merge off. Measurement-only |
| `XEMU_SUBPAGE_FAST` | Sub-page arm (b): the aarch64 store slow-path stub completes a store inline when the slow path is a provable no-op (mismatch exactly `TLB_NOTDIRTY`, non-code 64 B sub-block, all NOCODE dirty clients already set). **DEFAULT-ON since 2026-07-18** (`=0` reverts; forced off with `XEMU_SUBPAGE_DIRTY=0`, which stops bitmap maintenance): quiet-machine receipts measured ~2M inline completions/s on the F8 heavy scene — the eligible population is dominated by renderer-watched pages, ~20x the code-page trap count the +0.3-0.7 fps prediction was sized on — for **+12% draws/s throughput (6/6 interleaved pairs, two independent 3-pair batches)**; fps stays ~flat because Azurik's effect-load feedback re-saturates frame time at ~815 vs ~727 draws/flip. Refuter: ~19.5M validated decisions, 0 violations (static + loadvm-cycling + first-visit-streaming soaks). Cold-stub only; the tag-match fast path is byte-identical. Atexit dump: seen/skips/demote-reason histogram, but only under `XEMU_SUBPAGE_FAST_STATS=1` (counters default-OFF since 2026-08-04) |
| `XEMU_REGION` | `=N` region/diamond former (forward jcc ≤16 B arms; taken edge becomes an intra-TB label bound at the join, backed by the recorded-label liveness elision in tcg.c). **Default 0 — the campaign closed 2026-07-18 with both windows measured negative** (see the Failed table); the machinery + `XEMU_REGION_CHECK` double-pass conformance harness stay as the record |
| `XEMU_REGION_CHECK` | `1` runs `liveness_pass_1` twice per TB (conservative then relaxed, pointer-keyed diff) and aborts on any non-conforming difference — with zero `region_join` labels the runs are bit-identical (the Class-5 containment proof, held over full boot+game runs) |
| `XEMU_SUBPAGE_FAST_REFUTE` | `1` routes every would-skip decision to a C validator (independent `probe_access` re-derivation, live-TB overlap scan, NOCODE dirty ground truth) that counts violations and performs the real store — falsification mode, not perf. Forces `XEMU_SUBPAGE_FAST_STATS=1` so soaks keep their population histogram |
| `XEMU_SUBPAGE_FAST_STATS` | `1` re-enables the arm-(b) prefilter's six diagnostic counter RMWs (`xemu_sf_seen`, `xemu_sf_skips`, and the four `xemu_sf_demote` reasons) that the aarch64 store slow-path stub emits inline. **Default OFF since 2026-08-04**: the counters are baked into generated code at translate time and the stub completes ~2M stores/s, so production now emits ~39 fewer instructions per store stub (12 of them on the inline-skip path itself, the four demote landing pads collapsing into direct branches) and the atexit dump prints `subpage-fast counters disabled (set XEMU_SUBPAGE_FAST_STATS=1)` in place of the histogram — so a zeroed histogram can't be mistaken for a dead mechanism. Latched once at first stub emission, before the first TB (a mid-run flip would leave TBs disagreeing). Diagnostics only: the decision logic and the refuter oracle are identical either way, which is what makes this knob its own A/B lever. Measured cost of the counters: **−0.32% draws/s / −0.57 fps, 3/3 pairs unanimous** |
| `XEMU_MFX_INTERP_ZERO_MOTION` | Old zero-motion interpolator binding (A/B) |
| `XEMU_PUSH_PRESENT` | **Default ON since 2026-07-12** (Metal backend): publish each flip's present schedule into a ring at flip so the UI reads it with no cross-thread round trip (removes the pull-model handshake wait; adds a `frame_seq` skip-when-unchanged dedup). Carries frame interpolation's paced sub-flip steps too (interp on and off); ignored on the GL backend. Measured quiet 2026-07-11 under 2x interp (F8): pull blocks the UI thread 499-607 ms per 5 s (1.4-1.6k waits, 5 ms tails) → **0 with the ring**, flips identical. Set `=0` to restore the legacy pull handshake wholesale |
| `XEMU_PUSH_DEBT` | Consumer catch-up bound for the push-present ring (default: interp mode + 2 steps; `0` = strict FIFO, the pre-fix behavior). When the guest's step rate beats the display (e.g. 40 fps × 2x interp = 80 steps/s on a 60 Hz panel), strict FIFO backlogs and force-drops unread steps — measured ~26 spliced steps/s, the 2026-07-12 "frames double playing" judder; with the bound the consumer jumps to the newest unconsumed real frame instead (60 Hz sim receipt: unread drops 2,253/2,152 per min → **0**, refuter 0 mismatches / 0 resurrections over the policy) |
| `XEMU_UI_FRAME_CAP_NS` | Test-only: floor the UI present period in ns (`16666666` ≈ a 60 Hz consumer). Unfocused/occluded bench windows present unpaced (~200 Hz observed), so ring-pacing behavior is invisible headless without it. Zero cost unset |
| `XEMU_PUSH_PRESENT_REFUTE` | Debug: peek the ring (non-consuming) and cross-check the consumed-step stream vs the pull path — monotonic `frame_seq`, no skipped-then-resurrected step, and identical texture/event/dims where comparable (interp steps compare event object + dims; the pixel-equivalent interp outputs live in distinct allocations). Prints `peeked`/`compared`/`mismatches`/`resurrections` every ~5 s. Re-adds the pull round trip — measure perf with it off |
| `XEMU_MFX_RESIZE_QUANTIZE` | **Default on since 2026-08-04** (`=0` restores the legacy continuous resize). Snaps the MetalFX output width to a 64-px ladder and holds a new shape for ~200 ms before rebuilding the scaler and its 3-texture ring, so a window drag stops rebuilding at the event rate (each rebuild drains MetalFX in-flight work and blocks the PFIFO thread). Height is re-derived from the quantized width (aspect preserved) and an explicit floor keeps the output strictly above `disp->width`, so quantization can never silently drop out of the upscale path. A guest mode change rebuilds immediately, no cooldown. Diagnostic: the `mfx_scaler_rebuild` counter under `XEMU_NV2A_NSPROF=1` |
| `XEMU_PRESENT_DRAWABLE_FIRST` | `1` opts in (**default off**). Acquires the `CAMetalLayer` drawable *before* consuming the push-present ring, so the step to present is chosen after the `nextDrawable` block rather than before it. Only the acquire moves — command buffer → shared-event wait → render pass stay together and in order (architecture Invariant 8); a step dropped by the `frame_seq` dedup after acquiring releases its drawable through `end_frame`. Latency-only and 0 fps by construction; measure at the default `XEMU_PUSH_DEBT` or it measures nothing. Metal backend only |
| `XEMU_UI_LOCK_STATS` | `1` prints the once-per-second UI main-loop-mutex + BQL occupancy line (`[[ vblank @NHz avg - bql Nns/iter, N% time avg ]]`) in a normal build; previously it needed a `DEBUG_XEMU_C=1` build (which still forces it on). One predictable branch per lock when off. Compiled in on macOS and in `DEBUG_XEMU_C` builds only — Windows/Linux release paths preprocess away to today's code |
| `XEMU_DSP_JIT` | `0` disables the fork DSP JIT inside the interpreter engine (kill-switch; *enabling* is config-only — `audio.dsp_jit.enabled`) |
| `XEMU_DSP_JIT_STATS` / `XEMU_DSP_JIT_DIFF=N` | DSP JIT counters / bit-exact validation |
| `XEMU_DSP_JIT_NO_THROTTLE` | Disable the DSP JIT retranslation-churn auto-throttle |
| `XEMU_DSP_JIT_DIFF_SYNC` / `_DIFF_MAX` / `_DUMP` / `_PIN_AUDIT` / `_SENTINEL` / `_FORCE` | JIT bring-up harnesses (see `docs/dsp-jit-design.md`) |
| `XEMU_APU_PROF` | Per-second APU-thread utilization to stderr |

### Build-time and harness knobs added 2026-08-04

Not runtime env vars — these are read by `build.sh` at configure time or
by the bench harness, and are catalogued here because each is the escape
hatch for a Changes bullet below. The authoritative catalog of every
build knob is `xemu-config-and-flags` §4; the README's `### Build knobs`
table carries the user-facing subset.

| Env var | Purpose |
|---|---|
| `XEMU_HARDENING` | **Default `1` = upstream's `-fzero-call-used-regs=used-gpr` register zeroing is KEPT.** `XEMU_HARDENING=0` appends `-fzero-call-used-regs=skip` to the macOS `sys_cflags` (clang is last-flag-wins over meson's global), which is the experimental skip arm. **Note the inversion**: the knob was built expecting skip to become the default, and the 2026-08-04 A/B killed that promotion (Failed table), so `0` is now the opt-in experiment rather than the shipped shape. macOS only (`build.sh:703-725`, as of 2026-08-04 — that file grew ~175 lines this wave, so re-grep rather than trusting the anchor); either choice is echoed at configure time; `-ftrivial-auto-var-init=zero` is untouched in both cases and Windows/Linux flag assembly is bit-identical. Security note: this is ROP-gadget hardening only — xemu runs a W^X JIT and is not a sandbox boundary |
| `XEMU_PGO_STALE_FATAL` | `0` (warn only). With `XEMU_PGO=use`, `1` turns a PGO CFG-hash mismatch on a hot-path function name (`cpu_exec*` / `helper_*` / `tlb_*` / `tcg_*` / `pgraph_*`) into a build failure instead of a loud warning (`build.sh:227-230`). Deliberately NOT enabled in CI: flip it on once the gate is proven quiet on a freshly-retrained tree |
| `XEMU_PGO_RETRAIN_REF` | Unset = auto-derived. The commit the PGO staleness *age* is measured from; auto-derivation order is the last commit touching `pgo/`, else the commit that was HEAD at the newest `.profdata`/`.profraw` mtime (`build.sh:190-191`). Degrades to `unknown`/`n/a` with no error on a source tarball with no `.git` |
| `XEMU_BENCH_ISOLATE_CACHES` | `0` (warm-shared, the pre-2026-08-04 behavior). `1` — equivalently `--isolate-caches` — plants the scratch config as a portable-mode marker inside the cloned bundle's `Contents/Resources`, moving xemu's data base path (`spirv_cache_v*/`, `pipeline_cache.bin`) into the disposable work dir, so caches start cold at batch start, warm during the warmup run, and are shared identically by both arms (`scripts/bench-savestate-ab.sh:101,213`). Recorded as `cache_isolation` in `meta.json` / `receipt.json`. Default unchanged so existing baselines stay comparable |


## Changes

> **2026-08-04 optimization wave.** The bullets dated 2026-08-04 below
> implement [`fork-optimization-audit-2026-08.md`](fork-optimization-audit-2026-08.md)
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
> **Known issue carried by this wave's release notes:** a rare
> post-`loadvm` SIGSEGV in the `ohci`/`flatview` path, upstream-class
> (QEMU GitLab #545 family), 2 sightings across ~40 wave launches
> against 0 across the prewave stability control (4 launches, 21
> loadvms, 31.5 min dwell). That is **not** statistically decisive and
> the crash is not attributed to any wave mechanism; it is recorded so a
> future reader recognizes the signature rather than re-debugging it.

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
  (nearest/truncate stay inline; aarch64 unchanged).

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
  occlusion rework. `XEMU_SUBPAGE_DIRTY=0` restores whole-page
  invalidation wholesale.
- **Cross-page direct block chaining, Xbox-relaxed (default on,
  2026-07-12).** Direct jumps whose target lies on another page now
  patch a real `goto_tb` chain instead of paying the ~20-op inline
  jump-cache probe (the "other" 43% of the exit census). Upstream
  forbids this because mappings can change under a chain; the fork
  makes it sound by construction with three backstops — INVLPG
  invalidates the flushed code page; CR3 writes and every remaining
  full-TLB-flush class (generic `tlb_flush_by_mmuidx` hook) sever the
  registered cross-page chains. Refuter (`XEMU_XPAGE_REFUTE`:
  page-table-walk, closing the stale-chain fetch-bypass hole): 0
  violations over 3.15B+1.71B checks across 20 reload cycles.
  Interleaved A/B: **+1.30±0.29 fps F8 (7/7 pairs) pre-subpage;
  +0.77±0.55 (3/3) on top of sub-page tracking**. `XEMU_XPAGE_CHAIN=0`
  restores upstream same-page-only chaining.
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
  chains relink lazily. Registry overflow (128k dests) or
  `XEMU_XPAGE_UNLINK=0` falls back to the v0.11 queued full flush.
- **Inline hard FPU.** x87 ops emit native AArch64 FP
  (`FADD`/`FMUL`/`FDIV`/`FSQRT`); `floatx80`↔`double` inlined
  (~15 vs ~40 host insns); values stay in D-regs across TBs.
  Inlined: `fucomi`, `fnstsw`, `frndint`, FPU constant loads,
  `fdecstp`/`fincstp`, `ffree`/`ffreep`, 16-bit `FIST`, and all
  `FISTTP` widths. (FISTTP inline is AArch64-only: `tcg_gen_cvt_f*_i*`
  lowers to `FCVTZS` on AArch64 but to MXCSR-rounded `CVTSS2SI` on
  x86-64, so non-AArch64 hosts fall back to the floatx80 helper.)
  ~30% faster than the helper-call path, 4–8× faster than softfloat.
- **FPCR caching + rounding-correct FIST.** `MSR FPCR` re-emitted only
  when guest RC bits change. `FIST`/`FISTP` honor the control word via
  fused `cvt_{rn,rm,rp}_i{32,64}_f{32,64}` TCG ops (single host
  `FCVT{N,M,P,Z}S` on AArch64).
- **Signaling vs quiet compares.** `coms_f32`/`coms_f64` (FCMPE /
  COMISS) for FCOMI; `com_f32`/`com_f64` for FUCOMI. Matches x87 NaN
  semantics.
- **Control-word resync.** Every path that writes `env->fpuc`
  (`fldcw`, `fldenv`, `frstor`, `fxrstor`, `fninit`, xsave helpers,
  `cpu_post_load`, gdbstub, SEV, reset) routes through
  `cpu_set_fpuc` so `HF_FPU_RC` / `HF_FPU_PC` in `hflags` are
  rebuilt — otherwise fused FIST would round with the stale mode.
- **Misc.** `gen_flcr` materializes rc-bits from `tb->flags`;
  `fnstcw` is one load; `insertion_sort_syncs` replaces `qsort`
  for N ≤ 16; `flcr` is 5 insns via `RBIT`.
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
  default on for aarch64 with `perf.hard_fpu`, `XEMU_SSE_HOST=0`
  restores softfloat. (x87 hard-FPU helpers save/restore around
  their own ops; softfloat/TCG carry no host FP-mode dependence.)
  Follow-up fix: the *inline* x87 path skips its `MSR FPCR` whenever
  the `cached_fpuc_rc` cache matches the TB's rounding bits, so the
  bracket now invalidates that cache when it rewrites FPCR —
  previously inline x87 could silently run under the sticky SSE
  FZ/RMode state (denormal flushing in x87 doubles). Zero cost in
  steady state (fires only on actual mode transitions); validated by
  a clean 90 s `XEMU_SSE_HOST=2` differential and fps parity.
- **TB-dispatch inline probes (+1.5 fps family, 2026-07-05).** The
  first guest-side profile (`XEMU_GUEST_PROF`: mach-thread sampler +
  per-exit-kind TB-lookup census) showed diffuse guest time (top TB
  1.8% — no hot-loop candidates) but near-returns at 46% of 15.8M/s
  TB lookups. Two shipped mechanisms: the near-return target memo — a
  4096-entry eip→TB cache probed inline at ret sites (`XEMU_RAS`,
  default on; **+0.77 fps, 6/6 pairs**; helper hits collapse
  362M→13k) — and the same inline-probe technique applied to the
  real jump cache at indirect/cross-page exits (emitter in
  `accel/tcg/xemu-inline-jc.c`, the sole owner of the cache layout;
  +0.70 fps with the enabled arm carrying ~+8% draw throughput). The
  APU VP doorbell block also joined the audited lockless-MMIO set.
  Two honest kills en route are in the failed-experiments table
  (JIT write-protect caching; the per-depth return-address ring).
- **`can_do_io` bookkeeping elided when icount and replay are off
  (default on, 2026-08-04).** Upstream emits `set_can_do_io(false)` /
  `(true)` into every multi-insn TB body unconditionally, and
  `io_prepare` turns any mid-TB MMIO access into a `cpu_io_recompile`
  round trip — `tcg_tb_lookup`'s g_tree walk, state restore, siglongjmp,
  a `CF_MEMI_ONLY` re-translate, then re-execute — which is a stable
  top-25 vCPU symbol on the heavy scene. Nothing observes the flag with
  icount and replay off (watchpoint.c's use is nested inside
  `replay_running_debug()`), so both stores now sit behind a
  translate-time latch that force-disables itself under icount or
  `replay_mode != REPLAY_MODE_NONE`; `mem_io_pc` assignment is
  unchanged, so the APIC TPR-access unwind keeps its semantics. Gate
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
  those lockless regions. `XEMU_ELIDE_CANDOIO=0` restores the stores.
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
  and none should be quoted. `XEMU_DIRTY_FAST=0` restores both legacy
  shapes on the same binary.
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
  mid-TB. Adversarial refuter (`XEMU_X87_REFUTE`, legacy stores kept and
  the elision decision run as a shadow): **FT0 violations 0; 2.06e10
  exception-pointer checks, violations 0.** Interleaved A/B on F8, each
  3/3 pairs positive: FT0 **+1.56 fps**, FIP **+7.46 fps** (draws/s
  +8.8%), both together **+7.68 fps** — sub-additive against the sum of
  the singles, which is ordinary frame-time composition, not a
  contradiction. `XEMU_X87_ELIDE_FT0=0` / `XEMU_X87_DEFER_FIP=0` are the
  hatches; the third mechanism, clean-ST(i) write-back elision, ships
  dark (`XEMU_X87_ELIDE_CLEAN`) because it is a numeric behavior change,
  not a pure elision — see the roadmap.
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
- **Deferred-report idle budget cut 5 ms → 300 µs
  (`XEMU_REPORTS_BUDGET_US`).** Guests that consume a zpass report
  mid-frame spin-wait on it with an idle FIFO, and the safety-valve
  submit only fired after 5 ms of continuous idle — every such poll
  cost the guest's critical path up to the full budget. A too-small
  budget merely costs an extra small submit per idle episode
  (bounded by report count), so 300 µs is safe. Measured on a 770
  draws/flip scene: 24.04 ± 0.43 → 25.36 ± 0.22 fps (**+5.5%**, all
  pairs positive); artifact soak clean. `=5000` restores the old
  budget; `XEMU_REPORTS_SYNC=1` remains the full legacy hatch.
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
  honored; `XEMU_ZETA_SHAPE_READBACK=1` restores full fidelity.
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
  `XEMU_VTX_EXACT=0` restores page-granular conflicts.
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
  (surfaces don't take this path). `XEMU_TEX_BIND_RECHECK=1`
  restores per-bind checks.
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
  `XEMU_REPORTS_SYNC=1` restores legacy synchronous drains.
- **Query-pool drain at slot reclaim.** Occlusion queries are
  partitioned per flight slot; each submission's queries + pending
  guest reports are handed to the slot at submit and drained when
  the slot fence is reaped — `pgraph_vk_finish` no longer blocks on
  `vkGetQueryPoolResults(WAIT_BIT)` for the work it *just*
  submitted (which defeated the 2-slot pipeline on every finish in
  zpass-report titles). Guest-facing paths (`STALLED`,
  `REPORTS_FULL`, `FLUSH`) still drain synchronously so report
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
- **Fence-wait diagnostics.** 5 s timeout wrapper on single-time CBs
  aborts with the named call site instead of hanging silently on
  MoltenVK internal-mutex deadlocks.
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
- **Surface CPU-access-callback churn is now measurable — and it is the
  guest's TLB-flush source (instrumentation + dark mechanism,
  2026-08-04).** Every NV2A surface create/destroy calls
  `mem_access_callback_insert`/`remove`, and each of those is an
  `async_safe_run_on_cpu` (an exclusive-execution vCPU stop) **plus** a
  full all-mmuidx `tlb_flush_all_cpus_synced` **plus** an unconditional
  `tcg_flush_jmp_cache`. Renderer-initiated TLB flushing had never
  appeared in any profile and nobody had read the counters, so two
  auditors disagreed by two orders of magnitude about the rate.
  `XEMU_SURFACE_CB_STATS=1` settles it: **199.8 callback events/s =
  4.16 per flip** in the steady F8 scene, attributed to
  `update_surface_part`'s invalidate-then-create path (the zeta
  ping-pong), against **~190 full-mmuidx flush calls/s** measured
  independently — a **0.95:1 ratio, i.e. the surface callbacks are
  essentially *the* guest TLB-flush source in-scene**, each one also
  wiping the jump cache. A dark reuse pool ships alongside the census
  (`XEMU_SURFACE_CB_REUSE=1`, 4 entries keyed on `(vram_addr, size)`):
  it parks the live registration on unregister and re-attaches it when
  an identical-key surface reappears. **Measured 2026-08-04 (same day,
  idle-gated A/B) and killed: −0.66 fps 3/3 negative** — see the
  Failed table; it stays in-tree dark as the record. The live shape is
  the `// FIXME: flush only applicable pages` ranged invalidation in
  `system/physmem.c` / `cputlb.c` (roadmap, needs-theory). Measurement gotcha
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
  owner promotion; `XEMU_PUSH_PRESENT=0` restores the legacy pull
  handshake wholesale; Metal backend). The published slot is a ring
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
  policy (0 mismatches / 0 resurrections). `XEMU_PUSH_DEBT=0`
  restores strict FIFO. The numbers below are the original
  interpolation-off landing.
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
  `nil` enables MetalFX internal motion estimation;
  `XEMU_MFX_INTERP_ZERO_MOTION` restores the old binding);
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
  is rejected or no zeta matches the scaler's input dims. `1` exports
  every zeta and hands the dims-matched one to the scaler live — but
  by present time the single guest zeta already holds the *next*
  in-progress frame's depth (one frame ahead). `2` instead blits the
  display-matching zeta into a dedicated exportable snapshot at
  FLIP_STALL, recorded into that frame's command buffer so it rides
  the same submission (one depth copy per flip, ~2 µs PFIFO record
  time), giving depth that is temporally correct for the presented
  frame. Both ship dark for A/B. Engagement is title-dependent: a
  guest that renders 3D at 2× the scanout width (AA super-width, e.g.
  Azurik) exposes no zeta matching the scaler input, so both modes
  fall back to synthetic there.
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
  bound it is. `XEMU_MFX_RESIZE_QUANTIZE=0` restores the continuous
  resize.
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
- **Drawable-first present ordering (opt-in, dark, 2026-08-04).**
  `XEMU_PRESENT_DRAWABLE_FIRST=1` acquires the `CAMetalLayer` drawable
  *before* consuming the push-present ring, so the step to present is
  chosen after the `nextDrawable` block rather than before it — the
  gain, if any, is that a step selected post-acquire is fresher by
  exactly the acquire's block time. Only the acquire moves: command
  buffer → shared-event wait → render pass stay together and in order
  (architecture Invariant 8), and a step dropped by the `frame_seq`
  dedup after acquiring releases its drawable cleanly through
  `end_frame` (pool churn accepted). It stays **default-off as
  designed**, with the characterization that motivated building it:
  `drawable_acq` blocks **13.8-16.5 ms per acquire**, comfortably past
  the 4 ms bar that says the mechanism has something to hide behind —
  but the fps delta measured **+0.50 under the noise line**, so it has
  no claim and does not get promoted on a mechanism argument alone.
  Latency-only by construction; A/B it at the default `XEMU_PUSH_DEBT`
  or it measures nothing.

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
  autovectorizer and dominated 3D-voice CPU time. (Hand-NEON was
  tried and reverted; removing the modulo lets `-O3` autovectorize
  profitably instead.)
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
- **Accelerate / vDSP.** `vDSP_vsma` for 8-bin × 32-sample mix
  accumulation, `vDSP_vadd` for `float_accumulate`; `float_to_24b`
  uses `vcvtnq_s32_f32` in bulk on ARM64. Single-precision SVF/LPF.
  `SRC_LINEAR` resampler matches Xbox hardware.
- **Atomic consistency.** `d->regs[]` writers, `voice_locked[]`,
  `pause_requested`, `NV_PAPU_FEMEMADDR` load, and `VPVADDR` /
  `FENADDR` reads in lock-free VP paths all use `qatomic_*` so the
  VP frame thread, workers, and lock-free MMIO dispatcher agree
  under weak ordering. `fe_method`'s register reads (FECV / FEAV /
  list tops / VPSGEADDR / VPSSLADDR / FETFORCE1) follow the same
  contract.
- **CoreAudio.** `os_unfair_lock` with trylock. Underruns now
  partial-fill (drain what's pending, zero only the tail) instead
  of zeroing the whole IOProc buffer on any shortfall.
- **DSP execution engines (post upstream merge).** Upstream added a
  DSPOps engine abstraction with two engines: the C interpreter
  (`dsp/interp/`) and a JIT wrapping the external `dsp56300` Rust
  subproject (`audio.use_dsp_jit`, default on). This fork's inline
  ARM64 JIT lives *inside* the interpreter engine
  (`interp/dsp56k_jit_arm64.c`, symbols `dsp56k_jit_*`); when
  supported and enabled it takes precedence and `use_dsp_jit` is
  ignored. Non-Apple hosts get upstream's dsp56300 engine by
  default.
- **Full inline DSP JIT (Apple Silicon).** ARM64
  basic-block JIT for both MCPX DSP56300 cores (GP + EP). Enable
  via `[audio.dsp_jit] enabled = true` (engine selection is
  config-only; `XEMU_DSP_JIT=0` is a runtime kill-switch within the
  selected engine).
- **Per-core retranslation auto-throttle.** The EP core overlays its
  P-space every pass (~1 retranslation per 10 block executions), so
  translating it costs more than interpreting it; a sustained-churn
  window (>1/16) permanently hands that core back to the
  interpreter. APU-thread utilization: JIT-both-cores 25%,
  interpreter 22%, JIT with EP throttled **20%** (upstream dsp56300
  engine 19%). GP never trips. `XEMU_DSP_JIT_NO_THROTTLE=1` for A/B.
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
  [docs/dsp-jit-design.md](docs/dsp-jit-design.md).
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

### Input

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

- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE`.
- **BQL discipline.** `surface_access_callback` drops BQL before
  blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`; kick-check +
  release-wait under `d->pfifo.lock` guarantees no missed wake-up.
  `halt` via `qatomic_read` matches the writer contract.
- **BQL-free MMIO dispatch for the hottest guest register blocks.**
  `XEMU_MMIO_PROF` measured 78% of 8.2M guest MMIO ops/70 s hitting
  just PFB (`NV_PFB_WBC` polling) and USER doorbells, each paying
  the unconditional BQL in TCG's MMIO helpers — upstream's
  `lockless_io` flag is now honored on the TCG path too, and the
  audited PFB / USER / APU-VP regions opt in. Locked crossings drop
  ~80%. Measured honestly at **fps parity** (not claimed as a
  speedup) with a consistent steadiness win: per-run fps stdev
  1.21 → 0.89, 5/6 pairs — the 340 µs BQL-spike class no longer
  hits these ops. `XEMU_MMIO_BQL=1` restores locked dispatch.
- **PGRAPH joins the BQL-free MMIO set.** Interleaved A/B (F8, 3
  pairs, quiet): fps parity as expected for this class (legacy-BQL
  minus lockless = -0.65±1.43, sign-mixed), with the lockless arm's
  run-to-run spread tighter (0.43 vs 1.00 fps) — directionally the
  same steadiness win the PFB/USER set measured, not formally powered
  at n=3. The PGRAPH block was left out of
  the original lockless set because its handlers genuinely interleave
  with the PFIFO thread. The audit (`docs/pgraph-lockless-audit.md`)
  shows `pg->lock`/`pfifo.lock` — not the BQL — arbitrate every shared
  access except one: the interrupt pair
  (`pending_interrupts`/`enabled_interrupts`), which the BQL serialised
  against the lock-free `nv2a_update_irq` reader and the PFIFO thread's
  raise. Those two fields are now `qatomic` (fixing a pre-existing
  unserialised raise-vs-read pair in the process), and PGRAPH dispatch
  drops the BQL. `XEMU_PGRAPH_MMIO_STATS=1` measured the guest ISR
  *on* this path: `NV_PGRAPH_INTR` is the hottest PGRAPH register
  (1.6M reads on the F5 stats run; count ratios — the load-immune
  figures — put ~11 PGRAPH MMIO ops on every PGRAPH interrupt, 3 INTR
  reads per ack). Soak: 8 F5⇄F8 loadvm cycles over a 12-min window,
  zero hangs/asserts/artifacts; xbox suite green. `XEMU_PGRAPH_LOCKLESS=0`
  restores BQL dispatch for PGRAPH only; `XEMU_MMIO_BQL=1` still
  restores it for all blocks.
- **PFIFO kick census (counter only, 2026-08-04).**
  `XEMU_PFIFO_KICK_STATS=1` buckets every `pgraph_write` `pfifo_kick`
  broadcast by call site (INTR / INCREMENT / FIFO_ACCESS_ON /
  FIFO_ACCESS_OFF) × whether a PFIFO wait condition actually
  transitioned × post-write `FIFO_ACCESS` × whether a kick was already
  pending, and separately counts the PFIFO thread's idle parks — the
  wakeup denominator that says how many broadcasts could ever have woken
  a parked waiter. **No kick is ever suppressed, deliberately**: a
  wrongly-skipped kick is a permanent PFIFO hang (archaeology 7.1), and
  the file comment records that reasoning so a future reader does not
  read the omission as an oversight. The census then closed the
  suppression idea outright — see Settled vectors. Two owned-file
  proxies bracket the truth about waiters (the `dup` bucket = kick
  already pending = provably redundant; the pfifo-side park census =
  parks per kick); the exact per-kick "a waiter existed" bit would need
  a field in `nv2a_int.h`'s anonymous pfifo struct.

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
  `.pc` files; `build.sh` strips all `LC_RPATH` before
  `dylibbundler` and adds the single correct one.
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
[moltenvk-optimization-experiments.md](moltenvk-optimization-experiments.md).


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
  screenshot clean) soaks. `XEMU_SUBPAGE_FAST=0` reverts.
- **Region/diamond formation + recorded-label liveness elision (dark,
  2026-07-18).** The final campaign against the censused 39-44%
  dead-flag mass: `TCGLabel.region_join` labels get their live-in
  recorded during the backward liveness walk (forward branches see the
  label first), and globals provably dead into the join — EXACTLY
  `TS_DEAD`, since `TS_DEAD|TS_MEM` at an exit-bound tail still owes
  its store — keep `TS_DEAD` without the `TS_MEM` forcing at both the
  `brcond` and the join; the writing op then takes the `DEAD_ARG` path
  and the dead store never emits. Liveness-only (no allocator edits;
  `liveness_pass_2` is inert for i386), bit-identical with zero flagged
  labels (proven over full boot+game runs by the `XEMU_REGION_CHECK`
  double-pass harness). The i386 former internalizes forward jcc arms:
  the taken label binds at the join with cc reconciled via the
  `do_gen_rep` label-merge idiom and `pc_save` via the −1 sentinel;
  unreached joins demote to exit stubs with checkpointed state.
  **Both measured windows lost** (Failed table: −4.5% then −0.69 fps,
  drain-demotes dominating) — everything stays dark as the campaign's
  reproducible record.
- **Superblock seam-following machinery (dark, 2026-07-18).**
  Translate-time TB merging behind `XEMU_SUPERBLOCK=N` (default 0):
  unconditional same-page forward jmps concatenate (M1 — emit nothing,
  keep translating at the target; gates: `jmp_opt`, no icount, no
  data16 wrap, target on `pc_first`'s page, forward-only for
  `tb->size` contiguity), and conditional fallthroughs continue inline
  with the taken edge in an out-of-line lookup stub (M2 —
  forward-target conditionals only; stubs never consume goto_tb slots
  or xpage registry entries). Plus a runtime taken-exit tail-kind
  census (`XEMU_SUPERBLOCK_SIZE=1`). Ships dark: every measured policy
  regressed on F8 (see the Failed table) and the corrected theory says
  the censused dead-flag win needs region/diamond formation — the
  machinery and census are that campaign's foundation
  (`docs/roadmap.md` item 1).

### Testing & tooling (2026-07-18)

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

## Failed / reverted experiments

Lessons worth preserving so they aren't re-attempted.

| Attempt | Reason |
|---|---|
| Surface-callback reuse pool default-on (`XEMU_SURFACE_CB_REUSE`, 2026-08-04) | The mechanism receipt was real — the zeta ping-pong's ~200 register/unregister events/s are 0.95:1 with ALL in-scene guest TLB flushes, each with a jump-cache wipe — and the pool survived every correctness gate (loadvm cycles clean, coverage audit 23,003/0, movement probe `tb_flush` flat). The fps A/B then read **−0.66 fps, 3/3 unanimous negative, draws/s −1.9%** (idle-gated pairs, bar was ≥ +0.25): post-candoio-elision the vCPU's flush/refill cost was already off the critical path — the audit's +0.35..+1.5 band was priced against the PRE-wave profile — while the pool's park/reattach bookkeeping bills the render thread on every shape swap. Kept dark in-tree as the record. The live shape is the ranged-flush FIXME in `physmem.c`/`cputlb.c` (roadmap, needs-theory with a post-wave predicted number) |
| x87 clean-ST(i) write-back elision default-on (`XEMU_X87_ELIDE_CLEAN`, 2026-08-04) | Refuter said safe (3.49e9 checks, 0 violations, truncation signal absent); the idle-gated A/B said worthless: **−0.23 fps, 3/3 negative** vs the ≥ +0.20 bar. After the FT0 + FIP/FDP harvests the elidable flush population is too thin to pay for the per-slot dirty tracking. Stays dark; reopen bar = clean-ST census ≥ 8M/s on some other title |
| Reports idle-budget 300 → 50 µs (`XEMU_REPORTS_BUDGET_US`, 2026-08-04) | The deferred pinned-value scan ran: **−12.1 fps unanimous, draws/s −21.4%** — 50 µs qualifies far more idle episodes for early submits and lands on the arch-1.1 submit-storm side of the cliff, exactly as the scan's guard predicted. 300 µs validated as correctly above the cliff; scan closed permanently, knob stays for triage |
| Ret-memo 13-bit index (`XEMU_RETC_BITS=13`, 2026-07-18) | Doubling the eip→TB memo (4096→8192 entries, 128→256 KiB) predicted +0.3-0.5 fps from fewer hash collisions; measured **−0.23 fps, 4/4 pairs negative** (scene identity clean, arm (b)-on shipping context). Same mechanism as the jump-cache 12→16-bit kill: the recurring ret set already fits at 12 bits, so the larger table pays cache footprint on every probe to avoid collisions that were mostly one-shot. The width stayed runtime-latched (`XEMU_RETC_BITS`, default 12, validated-hit design makes any width safe) so the experiment reruns as a one-binary A/B on other titles |
| TB byte-range invalidation filter (`XEMU_TB_RANGE_INV`, 2026-07-11) | `XEMU_INV_PROF` measured 100% of TB invalidations as data-write false sharing (0 true SMC, 100% recycle-hit) — so re-applying upstream's overlap filter looked free. Counter A/B: invalidations 338k → 675 (works) but notdirty traps **25.2×** (198k → 4.98M) + 46M range scans — whole-page invalidation is what empties the page and fires `tlb_unprotect_code`, making subsequent data writes free; base-xemu removed the check (`6ea11938b2e`) deliberately. Instrument-killed pre-fps-A/B; knob ships dark. Real lever: sub-page dirty tracking (design parked pending correctness review) |
| L2 victim jump-cache (2026-07-05) | 64k-entry victim tier probed on L1 miss looked capacity-shaped (410k TBs vs 4096 entries) — but live hit rate measured **11.2%**: residual misses are one-shot/cold pcs, not a recurring set. Ceiling ~0.1 fps; instrument-killed without an fps A/B. Measure the miss stream's *shape* (recurrence), not its volume, before building any cache tier |
| JIT write-protect flip caching (2026-07-05) | Sampling attributed 9.7% of the vCPU thread to per-TB-entry `pthread_jit_write_protect_np`; a thread-local skip of redundant flips measured **parity** (6 pairs, −0.10 mean). The 9.7% was `thread_suspend`-sampling skid onto barrier instructions — discount barrier-heavy symbols in suspend-based profiles |
| Per-depth return-address ring (2026-07-05) | Classic shadow stack: eip prediction paired at 99.6%, but per-depth slots are shared by every same-depth call site, so TB fills ran 2.6× hits; a 512-deep ring changed nothing (a key problem, not a depth problem). Superseded same day by the eip-keyed ret-target memo (`XEMU_RAS`): the ret target is already in `env->eip` at dispatch, so depth-shaped state adds nothing a target-keyed cache doesn't |
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
| HRTF hand-NEON | Gather-then-FMA on a circular buffer defeated OoO overlap; `-O3 -mcpu=native` autovectorizes better |
| BQL event batching | Breaks QEMU cooperative scheduling |
| TLS-cached `pthread_jit_write_protect_np` | Per-thread cache drifts from kernel W^X on nested `tb_gen_code` / setjmp paths → SIGSEGV/SIGILL after ~1 min |
| Hoist `can_fifo_access` out of pfifo pusher word loop | `pfifo_run_puller` drops `pfifo.lock` when taking `pgraph.lock`, so "lock held throughout" is false → `ERROR_CALL` on boot |
| DSP JIT round-4 `cur_inst` preset skip | Regresses on startup with `op=0x001000` — some inlined path reads `cur_inst` at runtime that the static classifier doesn't see. Harness (`XEMU_DSP_JIT_SENTINEL` / `_FORCE`) kept in-tree |
| DSP JIT round-4 EPI_NO_PC | 9.75% mismatch per pcskip sentinel — calc_ea mode 6 lengthens parmoves and REP/DO loops rewind pc; both legitimately diverge |
| Vblank cadence aligned to host display refresh | Host `vblank_interval_ns` drives `process_vblank` → `graphic_hw_update` → `nv2a_vga_gfx_update` → `NV_PCRTC_INTR_0_VBLANK`. Retuning retimes the *guest* IRQ (titles gate sim on vblank count). Needs NV2A-internal timer (fixed 60 / 50 Hz) decoupled from host present cadence |
| AArch64 `fsin` / `fcos` libm-helper inline | Swapping `gen_helper_fsin` / `_fcos` (floatx80 round-trip) for a thin `helper_sin_fast_f{32,64}` / `cos_fast_*` (f-bits → `memcpy` → libm → bits) made audio crackly on at least one title. Plausible causes: (a) `gen_flush_fp` was also the de-facto flush for *other* live x87 temps; (b) `TCG_CALL_NO_RWG_SE` let TCG reorder across boundaries the floatx80 helper implicitly fenced |
| Voice-list `SE2FE_IDLE_VOICE` re-notify skip | No guest-write hook on `NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` transitions → can't reliably clear a "notified this activation" flag. Risk of missing an FE-observed idle transition outweighs the per-frame save |
| Cross-TB FPCR elision via `cached_fpuc_rc` as TCG global | TCG globals are memory-backed and reloaded per TB — a global doesn't eliminate the per-TB `ld16u` in `gen_flcr`, it just renames it. Eliminating the load needs either TB-chain metadata (RC equality across chain edges) or relaxing `gen_flush_fp`'s `flcr_set` reset — the same implicit-fence class that made the fsin/fcos inline regress. No win available at acceptable risk |


Session 2026-07-18 additions — the superblock seam-following campaign
(mechanism kept in-tree, dark, as the foundation for region formation;
full story in `docs/roadmap.md` item 1):

| Attempt | Reason |
|---|---|
| Superblock M1 alone: concatenate through unconditional same-page forward jmps (`XEMU_SUPERBLOCK`) | Instrument-killed pre-A/B: the runtime tail-kind census measured the capturable subset at **3.7% of taken goto_tb exits** (~1.26M/s) against a pre-registered ≥10% build bar |
| Superblock M2, all-conditionals policy (cap 4) | Unanimous 3/3 regression: draw throughput **−1.6%**, feedback-amplified to −8.4% fps (the guest scales per-frame effect load with frame time, so the slower arm also reads ~7% heavier frames — compare throughput, not raw fps). Suspected loop back-edge demotion (hot taken edges → lookup stub) |
| Superblock M2, forward-conditionals-only policy | Refuted the loop-protection hypothesis: still **−2.3% throughput, 3/3 negative** with ~29k seams/run merged (4,922 jmp + 24,064 fallthrough). Root cause understood: TCG must spill dirty cc/eip globals before ANY branch whose taken edge exits the TB, so conditional seams structurally cannot elide the flag materialization the census counted — only designs where BOTH successors stay inline (region/diamond formation) can harvest the censused 39-44% |
| Region/diamond formation, 64 B window (`XEMU_REGION=4`) | The both-edges-inline design, enabled by a recorded-label liveness elision in tcg.c (drop `TS_MEM` for globals provably dead into a forward join; no allocator edits; bit-identical when unflagged — proven over full runs by the `XEMU_REGION_CHECK` double-pass harness after its own index-misalignment bug was fixed). Candidate census passed its ≥15% gate at **21.4%**; the A/B still lost: **−1.34 fps (−4.5%), 3/3**, B arms ±0.30. Mechanism: **72% of opened regions drain-demoted** into the known-negative stub shape (arms hit block-enders before the join) — 11,240 demotes vs 4,290 internalized |
| Region/diamond formation, 16 B window (the single pre-registered mechanism iteration) | Predicted the demote ratio would crater below 25%; measured **64%** (4,918 vs 2,794) and the A/B still **−0.69 fps, 3/3 negative**. Campaign STOPPED permanently per pre-registration. Final verdict on the census's 39-44% dead-flag mass: **not harvestable by any translate-time policy tried** (five policies, all negative, each mechanism identified); the liveness elision itself is sound and stays landed dark with its checker as the record |

Session 2026-08-04 additions — the fork-wide optimization wave
(candidates from `docs/fork-optimization-audit-2026-08.md`; the
mechanisms that survived are in the Changes manifest above):

| Attempt | Reason |
|---|---|
| Drop upstream's `-fzero-call-used-regs=used-gpr` on macOS builds (`XEMU_HARDENING`, 2026-08-04) | The ROP-gadget register zeroing costs real work — the zeroing movs measured **3.4-3.8% of this fork's dynamic compiled-instruction stream**, and the flag also defeats tail-call optimization (`bl`+`ret` where a `b` would do) — in a process that runs a W^X JIT and is not a sandbox boundary. Pre-registered **+0.11 to +0.83 fps (central ~+0.34)**. Measured over 5 interleaved cross-binary pairs: mean **+0.52 fps but signs 3+/2−**, which is worse than sign-mixed-by-one and therefore below the pre-registered evidence bar — killed as pre-registered, not renegotiated. The binary-size receipt (`__text` **8,741,512 B hardened vs 8,330,592 B skip = −411 KB**) is real and was explicitly ruled insufficient to override an fps bar the change did not clear. Default reverted to upstream's `used-gpr` in `build.sh`; the skip arm stays reachable as `XEMU_HARDENING=0`. **Retest ran same day and CLOSED IT PERMANENTLY**: the promised quieter-machine rerun (8 idle-gated pairs, post-upstream-merge tree) read **mean −0.39 fps, signs 3+/5−** — the earlier +0.52 was noise, there is no real fps effect from removing register zeroing on this workload post-PGO. Upstream's hardening stands; the −411 KB `__text` saving buys nothing measurable. Note the knob inversion when reading `build.sh` |
| Running the x87, sub-page-fast and DSP-JIT refuters in one process (`XEMU_X87_REFUTE` + `XEMU_SUBPAGE_FAST_REFUTE` + `XEMU_DSP_JIT_DIFF` together) | Deadlocks the guest during **BOOT, 2/2 attempts** — a test-infrastructure interaction, not a product bug: the arch-4.5 diff-validator-starvation family, three shadow validators contending for the same execution. **Singles and pairs are clean** and remain the supported way to soak. This also retroactively attributes the campaign's run-1 `vp_write` wedge, which had no other explanation. Practical rule: cap refuter soaks at two validators per process |

---

## Settled vectors — measured, not worth it (or premise dead)

Candidates that were investigated and closed with data rather than
attempted-and-reverted. Reopen only when the recorded bar is met with
fresh numbers (`XEMU_NV2A_NSPROF` / `XEMU_GUEST_PROF` / the named
counter).

| Vector | Measured reality / why closed |
|---|---|
| Texture-upload barrier batching | Per-mip `pre/post_compute` pairs are load-bearing on reused `COMPUTE_DST/SRC`; total upload time ≤ 15 ms per 5 s even during streaming-heavy transitions, ~0 steady state — not worth the risk |
| GPU S3TC decode (compute shader) | Total texture-upload CPU incl. S3TC ~0 steady, ≤ 15 ms/5 s streaming — no longer a meaningful cost center |
| `VK_EXT_external_memory_host` revival (snapshot scheme) | The snapshot memcpy it would eliminate totals < 0.5 ms per 5 s in-game — far below the ≥ 1 ms/frame bar; the direct form is a torn-read correctness hole (archaeology 1.6) |
| MetalFX *input* ring (drop `metalfx_drain_inflight`) | Drain costs ~0.1 µs/flip steady (max ~4 ms on rare hitches) — complexity for noise |
| ~~Shader specialization constants~~ | Stale premise: alpha-test / fog-enable are *already* compile-time GLSL variants (`PshState`/`VshState` key the LRU shader cache); only value uniforms are per-draw and must stay uniforms. Useful reframing = variant-count / compile-stutter work |
| `MTLResidencySet` (macOS 15+) | Blocked, twice over: each MetalFX subsystem owns only 2-4 long-lived resources (below the technique's payoff), and the NV2A renderer runs through MoltenVK, which exposes no Metal residency sets to a Vulkan client (`metalfx_upscale.m` Phase-5 note). Re-check only on a MoltenVK capability change |
| Streamed-vertex stall reduction (CPU-side sync) | Attempted twice, both ~neutral on the then-GPU-bound attract fixtures: byte-exact refinement killed 75-85% of `finish_vtx_dirty` yet flips/s never moved; per-flight mirrors made cross-slot waits structurally impossible and totals stayed ~2.5-3.5 s per 5 s — the waits were GPU frame time. The salvage (byte-exact skip) shipped separately at +5.4% fps. 2026-07-18 re-check: current savestate fixtures are CPU-bound with ~14 ms/flip GPU slack, so the *renderer-thread* waits these touched are further off the critical path than ever |
| GPU frame-cost campaign menu (draw merging, pass cuts, `VK_EXT_multi_draw`) | Killed by measurement on the 2026-07 savestate fixtures: zero subrange-coalesce opportunity, 10.6% merge candidates at ~0 per-call PFIFO cost (prefill=0 defers Metal encoding), per-draw CPU sites < 1 ms/flip, ~14 ms/flip GPU slack, multi_draw unimplemented in MoltenVK. Re-run campaign Phase 0/2 gates before reviving on a future GPU-bound scene |
| Dirty-clear TLB-walk coalescing | Parked 2026-07-05 (~1-1.5 fps ceiling, widens an archaeology-1.9-class race window). 2026-07-18 gate re-check on the post-sub-page profile: renderer-attributable `test_and_clear_dirty` vCPU share is now **< 1%** (`physical_memory_is_clean` 0.47%, the walk itself absent from the top-25 sampler symbols) — the old "~6% echo" premise died with sub-page tracking. Stays parked |
| Voice-register writeback batching | Closed 2026-07-18: unsafe by construction. Guest audio drivers read voice state (position / `PAR_STATE` / play cursors) directly from RAM with no MMIO interception point (`NV1BA0_PIO_GET_VOICE_POSITION` confirms live queries), so deferring `voice_set_mask`'s `ram_stl` to end-of-frame serves stale state mid-frame — archaeology 5.2's unsoundness, mirrored. The no-op-store elision already shipped |
| `qemu_cpu_kick` via `dispatch_semaphore_t` | Closed 2026-07-18: wrong premise. Under MTTCG this fork's vCPU kick is `tcg_kick_vcpu_thread` — two atomic flag stores, no signal; `pthread_kill(SIG_IPI)` (the ~50 µs P99 the vector cited) survives only on cold paths (`qemu_cpu_kick_self`, `pause_all_vcpus`). Nothing on the hot path to replace |
| DSP JIT parmove+ALU fusion | Closed for the current title corpus 2026-07-18: the eligibility proxies (`alu_ccr_nz_skipped` / `alu_ccr_eu_skipped`) read **0** on both F8 and the Azurik audio mix, and the APU thread runs ~18% utilized off every critical path — ~0 fps ceiling regardless of the fold's local win. **Closed a second time, harder, 2026-08-04: the fold is instruction-neutral by construction**, so the local win is 0 too. Static count against the post-Phase-8 emitter every fold would have to change — `emit_pm_read_reg` (`dsp56k_jit_arm64.c`, ~:2095 as of 2026-08-04; that file moves, so re-derive with `grep -n -A85 "static void emit_pm_read_reg"`) — whose three source classes are exhaustive: pinned X/Y = 1 `UBFX` from the pin on either side of the ALU; A/B = the same multi-path limited-read block (fast / saturate / `pm_read_accu24` BLR) either side; anything else = 1 `LDR`. Nothing is staged that a fold could delete — the read writes its callee-saved destination directly. **Do not build the write-d=0 / non-aliasing eligibility counter this row used to propose** (design-doc row 8): a 100%-eligibility reading would authorize a 0-instruction win. Narrowed reopen condition: a non-audio DSP program whose parmove sources are neither pinned nor A/B — and even then, re-derive a *nonzero* static delta before any counter or emitter work. Full arithmetic: `xemu-frontier-and-positioning` §2.5 |
| COMISS / UCOMISS inline lowering (audit §1.4c) | Closed **permanently** 2026-08-04 by its own census before a line of the lowering was written. The whole candidate rode on an unmeasured compare rate, so the census counter shipped instead of the mechanism (`XEMU_X87_CENSUS=1` prints `sse scalar compares: ss=… sd=…` from `gen_VCOMI`/`gen_VUCOMI`): in-scene the guest executes **0.0 scalar compares/s** (496 for the entire process lifetime, all of them pre-`loadvm`), and **`sd` = 0** independently confirms the verifier's claim that COMISD is structurally unreachable — the guest is a Pentium III with no SSE2. There is nothing to lower. `decode-new.c.inc` was never touched; the census counter stays as the record and the reopen test. Reopen only if some title's census clears ~1.5M `ss`-compares/s |
| `pfifo_kick` broadcast suppression (audit §1.5) | Closed 2026-08-04: the suppressible population is **empty**. `XEMU_PFIFO_KICK_STATS=1` counted **0 of 412,074 in-scene kicks** with no PFIFO wait-condition transition (7 such kicks exist for the whole process lifetime, all pre-`loadvm`), so a filter that skipped provably-useless broadcasts would skip nothing. That is the good outcome: archaeology 7.1 says a wrongly-skipped kick is a permanent PFIFO hang, and the census bought the answer at zero risk. Counter stays; suppression is not implemented and should not be. Separately noted, not proposed: 62.9% of broadcasts land with a kick already pending — provably redundant, but a *different* class from the one the audit proposed, and it needs its own sizing before anyone touches it |
| ImGui frame build under BQL + main-loop mutex (audit §1.9) | Closed 2026-08-04 exactly as the audit predicted it would be. The HUD build holds both locks, which looked like a vCPU-blocking hazard; the measurement (`XEMU_UI_LOCK_STATS=1`, which shipped precisely to answer this without a bespoke build) puts combined occupancy at **mean 0.386% / median 0.373%**, under the pre-registered 0.5% kill bar. No rework. The knob stays as the standing re-check — this is a "measure first, expect a kill" candidate that got its kill |
| `notdirty_write` dirty-check **fps claim** (audit §1.2) — the claim, not the code | Killed 2026-08-04 while the code shipped. The audit sized the candidate on a profile where `notdirty_write` carried a 75.1% trap share; the 2026-07-18 arm-(b) store-prefilter promotion then inhaled that population, and the in-scene re-measurement reads **~2.6k calls/s at a 2.2% trap share — a 34-57x collapse of the benefit base**, ceiling ≤ 0.06% of the vCPU thread, i.e. under the ±0.02 fps protocol resolution. `XEMU_DIRTY_FAST=1` stays default-on as a behavior-equivalent simplification with **no individual fps claim and no individual A/B** (Changes manifest). The transferable lesson is the proxy-collapse one: a candidate sized against a *profile* rather than a *mechanism* silently expires when an earlier landing changes the population, so re-measure the benefit base at build time, not at audit time |
| Lockless MMIO for the remaining BQL blocks (NV2A `PMC`/`PCRTC`/`PTIMER`, APU `main`/`gp`/`ep`) | Closed by written verdict 2026-08-04 — **[`docs/lockless-mmio-verdict.md`](lockless-mmio-verdict.md)**, which satisfies archaeology 7.5's reopen condition ("audits for the remaining regions are written") without building anything. Fixture-independent ceiling: the *entire* pre-campaign BQL-MMIO prize was 117k ops/s × ~52 ns = **0.61% of one core**, and PFB/USER (78%) + APU-VP (~11%) + PGRAPH already harvested it — residual **< 0.07% of a core ≈ +0.01-0.06 fps**, at or below the ±0.02 fps static-baseline resolution. Correctness wall on top: these handlers call `nv2a_update_irq` inline, so BQL-free dispatch would drive `pci_irq_assert` with no BQL at all and race the iothread's vblank raise — the `pgraph-lockless-audit.md` §C race one level up, hang class, unfixable by atomics alone. Reopen bar: `XEMU_MMIO_PROF` `BQL wait total` ≥ 500 ms / 75 s — **measured 2026-08-04 at 75.6 ms per 75 s (2,178,310 BQL-taking ops, avg 47 ns, max 82.0 µs), 6.6x under the bar**, so the verdict now rests on a measurement and not only on the arithmetic (verdict page §A.1) |
