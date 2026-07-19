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
| `XEMU_NV2A_NSPROF` | Wall-time frame profiler, 5 s summaries to stderr |
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
| `XEMU_INPUT_PIPE` | FIFO path; lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` inject input through normal bindings (works unfocused; test automation) |
| `XEMU_COREAUDIO_FRAMES` | CoreAudio buffer frames, default `1024` (≈21 ms @ 48 kHz) |
| `XEMU_MFX_REAL_DEPTH` | Feed real zeta depth to the temporal scaler (A/B): `1` = live read (one frame late), `2` = flip-time snapshot (temporally correct); unset/`0` = synthetic. Engages only when a zeta matches the scaler input dims |
| `XEMU_GUEST_PROF` | One-run guest profiler: mach-thread sampler resolves vCPU samples to guest TBs vs host symbols; TB lookups classified by exit kind. Measurement-run only (not benchmark-neutral) |
| `XEMU_RAS` | `0` disables the near-return target memo (default on): 4096-entry eip→TB cache probed inline at ret sites (+0.77 fps, 6/6 pairs — see CPU / JIT changes). `-d exec` tracing won't log inline-hit rets — disable when tracing |
| `XEMU_RETC_BITS` | Ret-memo index width, 8..13 (default 12), latched at process start; every hit is fully validated so any width is correctness-safe. 13 measured −0.23 fps 4/4 on F8 (Failed table) — a diagnostic/cache-footprint A/B knob, not a tuning lever |
| `XEMU_TB_PROF` | Prints TB jump-cache totals at exit (lookups, hit%, htable walks, translations, tb_flush count) |
| `XEMU_INV_PROF` | Prints SMC / TB-invalidation-churn counters at exit: invalidations by source (notdirty vs explicit vs single-TB), the false-invalidation share a byte-range overlap check would skip, inv-htable recycle hit-rate (false-sharing vs true SMC), and translate-time FPU/exit census |
| `XEMU_TB_RANGE_INV` | `1` re-applies upstream's per-TB byte-range overlap filter inside the Xbox whole-page code-write invalidation (default off = invalidate every TB on a written code page). Correctness-safe (invalidates a correct subset — a TB whose bytes weren't written can't have changed); A/B knob for the SMC-false-sharing cure (`XEMU_INV_PROF` measured 100% false-invalidation, 0 true SMC) |
| `XEMU_CCOP_CENSUS` | `1` prints a runtime-weighted census of cc-flag liveness across TB boundaries at exit (predecessor tail class × successor head class). Forces every TB transition through the exec loop (no goto_tb / jump-cache / ret-memo) so pairs are exact — much slower, same executed instruction stream; sizing tool for the superblock roadmap item, never a perf mode |
| `XEMU_INV_TIMING` | `1` adds a `cntvct_el0`-based split of the `notdirty_write` body (invalidation+scan+recycle vs preamble+tail) to the `XEMU_INV_PROF` dump — sized the sub-page dirty-tracking arms. Timings are order-of-magnitude; counts are load-immune |
| `XEMU_SUBPAGE_DIRTY` | Sub-page code dirty tracking — **default ON** since the 2026-07-11 A/B (`0` restores whole-page invalidation wholesale): a per-`PageDesc` 64-block bitmap lets a guest data store that misses every code sub-block skip whole-page invalidation — an O(1) bitmap test replaces the qht/jmp-unlink/recycle round-trip, leaving the page write-protected so the store re-traps cheaply. Interleaved A/B: **+4.33±1.53 fps F8 (+14.3%, 4/4 pairs), +11.63±0.52 fps F5 (+24.6%, 3/3)**; invalidations 290,595 → 11,180 (25.9×). The higher-ceiling fast-path form that removes the trap itself needs host-`tcg/aarch64` store codegen (held — see [roadmap.md](roadmap.md)) |
| `XEMU_SUBPAGE_REFUTE` | `1` runs the sub-page skip decision against a ground-truth live-TB byte-overlap scan (both pages of spanning TBs) and counts violations (design falsified if > 0); changes no behavior unless `XEMU_SUBPAGE_DIRTY` is also set. Soak: **0 violations over 28.6M filter-skips across 33 loadvm cycles** (~12 min) |
| `XEMU_XPAGE_PROF` | Cross-page-direct exit census: runtime taken-rate counter split by target region (kernel-identity vs low); also armed by `XEMU_INV_PROF`. Dark; atexit summary |
| `XEMU_XPAGE_REFUTE` | `1` = cross-page-chaining refuter: routes every cross-page-direct exit through the safe lookup path plus a page-table-walk check that the target's live phys is unchanged since last seen (loud + counted on violation). Slow; falsification only |
| `XEMU_XPAGE_CHAIN` | Cross-page direct chaining — **default ON (broad)** since 2026-07-12 (`0` restores upstream same-page-only chaining; `1` = kernel-identity window only). Sound by construction: INVLPG (per-page) backstop plus CR3 + generic `tlb_flush_by_mmuidx` backstops covering every full-flush class (0 refuter violations in 1.71B checks across the promotion soak). A/B: +1.30±0.29 fps F8 pre-subpage (7/7 pairs); +0.77±0.55 on top of sub-page tracking (3/3) |
| `XEMU_XPAGE_UNLINK` | **Default ON (v0.11.1)**: the full-flush backstop severs only the registered cross-page chains (synchronous `tb_jmp_unlink` walk; translations survive, chains relink lazily). `=0` restores the v0.11 queued whole-`tb_flush` backstop — which caused the new-area/death-reload lag (139 tb_flushes in 20 s of first-visit walking on F8, fps trough 14.6 vs 38.6 baseline; 0 flushes and no trough with the unlink). Registry overflow falls back to the full flush automatically |
| `XEMU_SSE_HOST` (alias `XEMU_SSE_NEON`) | NEON fast path for single-precision SSE arithmetic; default on for aarch64 with `perf.hard_fpu` (+1.90 fps — see CPU / JIT changes). `0` restores softfloat; `=2` runs both paths and aborts on divergence. x86_64 stays opt-in/dark: run `=2` clean on real silicon first |
| `XEMU_SUPERBLOCK` | `=N` follows up to N branch seams per TB at translate time (superblock formation: unconditional same-page forward jmps + forward-conditional fallthroughs with out-of-line taken stubs). **Default 0 (off/dark)** — see the superblock entry in CPU / JIT changes and docs/roadmap.md for the measured policy verdicts |
| `XEMU_SUPERBLOCK_SIZE` | `1` arms the runtime taken-exit tail-kind census (uncond/call/jcc-taken/jcc-fall/rep/toomany + the M1-capturable subset), atexit dump; sizes superblock policies with merge off. Measurement-only |
| `XEMU_SUBPAGE_FAST` | Sub-page arm (b): the aarch64 store slow-path stub completes a store inline when the slow path is a provable no-op (mismatch exactly `TLB_NOTDIRTY`, non-code 64 B sub-block, all NOCODE dirty clients already set). **DEFAULT-ON since 2026-07-18** (`=0` reverts; forced off with `XEMU_SUBPAGE_DIRTY=0`, which stops bitmap maintenance): quiet-machine receipts measured ~2M inline completions/s on the F8 heavy scene — the eligible population is dominated by renderer-watched pages, ~20x the code-page trap count the +0.3-0.7 fps prediction was sized on — for **+12% draws/s throughput (6/6 interleaved pairs, two independent 3-pair batches)**; fps stays ~flat because Azurik's effect-load feedback re-saturates frame time at ~815 vs ~727 draws/flip. Refuter: ~19.5M validated decisions, 0 violations (static + loadvm-cycling + first-visit-streaming soaks). Cold-stub only; the tag-match fast path is byte-identical. Atexit dump: seen/skips/demote-reason histogram |
| `XEMU_REGION` | `=N` region/diamond former (forward jcc ≤16 B arms; taken edge becomes an intra-TB label bound at the join, backed by the recorded-label liveness elision in tcg.c). **Default 0 — the campaign closed 2026-07-18 with both windows measured negative** (see the Failed table); the machinery + `XEMU_REGION_CHECK` double-pass conformance harness stay as the record |
| `XEMU_REGION_CHECK` | `1` runs `liveness_pass_1` twice per TB (conservative then relaxed, pointer-keyed diff) and aborts on any non-conforming difference — with zero `region_join` labels the runs are bit-identical (the Class-5 containment proof, held over full boot+game runs) |
| `XEMU_SUBPAGE_FAST_REFUTE` | `1` routes every would-skip decision to a C validator (independent `probe_access` re-derivation, live-TB overlap scan, NOCODE dirty ground truth) that counts violations and performs the real store — falsification mode, not perf |
| `XEMU_MFX_INTERP_ZERO_MOTION` | Old zero-motion interpolator binding (A/B) |
| `XEMU_PUSH_PRESENT` | **Default ON since 2026-07-12** (Metal backend): publish each flip's present schedule into a ring at flip so the UI reads it with no cross-thread round trip (removes the pull-model handshake wait; adds a `frame_seq` skip-when-unchanged dedup). Carries frame interpolation's paced sub-flip steps too (interp on and off); ignored on the GL backend. Measured quiet 2026-07-11 under 2x interp (F8): pull blocks the UI thread 499-607 ms per 5 s (1.4-1.6k waits, 5 ms tails) → **0 with the ring**, flips identical. Set `=0` to restore the legacy pull handshake wholesale |
| `XEMU_PUSH_DEBT` | Consumer catch-up bound for the push-present ring (default: interp mode + 2 steps; `0` = strict FIFO, the pre-fix behavior). When the guest's step rate beats the display (e.g. 40 fps × 2x interp = 80 steps/s on a 60 Hz panel), strict FIFO backlogs and force-drops unread steps — measured ~26 spliced steps/s, the 2026-07-12 "frames double playing" judder; with the bound the consumer jumps to the newest unconsumed real frame instead (60 Hz sim receipt: unread drops 2,253/2,152 per min → **0**, refuter 0 mismatches / 0 resurrections over the policy) |
| `XEMU_UI_FRAME_CAP_NS` | Test-only: floor the UI present period in ns (`16666666` ≈ a 60 Hz consumer). Unfocused/occluded bench windows present unpaced (~200 Hz observed), so ring-pacing behavior is invisible headless without it. Zero cost unset |
| `XEMU_PUSH_PRESENT_REFUTE` | Debug: peek the ring (non-consuming) and cross-check the consumed-step stream vs the pull path — monotonic `frame_seq`, no skipped-then-resurrected step, and identical texture/event/dims where comparable (interp steps compare event object + dims; the pixel-equivalent interp outputs live in distinct allocations). Prints `peeked`/`compared`/`mismatches`/`resurrections` every ~5 s. Re-adds the pull round trip — measure perf with it off |
| `XEMU_DSP_JIT` | `0` disables the fork DSP JIT inside the interpreter engine (kill-switch; *enabling* is config-only — `audio.dsp_jit.enabled`) |
| `XEMU_DSP_JIT_STATS` / `XEMU_DSP_JIT_DIFF=N` | DSP JIT counters / bit-exact validation |
| `XEMU_DSP_JIT_NO_THROTTLE` | Disable the DSP JIT retranslation-churn auto-throttle |
| `XEMU_DSP_JIT_DIFF_SYNC` / `_DIFF_MAX` / `_DUMP` / `_PIN_AUDIT` / `_SENTINEL` / `_FORCE` | JIT bring-up harnesses (see `docs/dsp-jit-design.md`) |
| `XEMU_APU_PROF` | Per-second APU-thread utilization to stderr |


## Changes

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

### MoltenVK runtime config

Set in `main()` before MoltenVK loads (launch-path independent;
explicit env overrides win) and mirrored in `Info.plist`
`LSEnvironment`. The two homes must stay in sync.

| Key | Value | Purpose |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor binding overhead |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | `0` | Deferred encoding. Must stay 0: with whole-frame CBs, prefill corrupted streamed textures, enabled the AGX visibility-buffer crash, and measured slower (46.3 vs 48.5 fps) |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async submits |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Ignore transient GPU errors |


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
| DSP JIT parmove+ALU fusion | Closed for the current title corpus 2026-07-18: the eligibility proxies (`alu_ccr_nz_skipped` / `alu_ccr_eu_skipped`) read **0** on both F8 and the Azurik audio mix, and the APU thread runs ~18% utilized off every critical path — ~0 fps ceiling regardless of the fold's local win. Reopen only with an audio-bound title, and then first add a write-d=0 / non-aliasing eligibility counter (design-doc row 8) before implementing |
