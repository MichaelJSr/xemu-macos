# Closed roadmap candidates

Candidates that left [`roadmap.md`](../roadmap.md), under the numbers
the roadmap and other docs cite. Figures are as of each entry's date;
each is also in the [ledger](../optimizations.md). The full original
write-ups are in this file at `103c6b9187`. The code of ranked item 1
(with `XEMU_CCOP_CENSUS` and `XEMU_SUPERBLOCK_SIZE`), ranked item 2's
`XEMU_RETC_BITS`, top candidate 1's `XEMU_SURFACE_CB_REUSE` pool and top
candidate 3's `XEMU_X87_ELIDE_CLEAN` was removed on 2026-10-10.

| Item | Outcome |
|---|---|
| Top 1, surface CPU-access-callback churn | Ranged flush landed 2026-10-15 (`tlb_flush_host_range`, `d1198c3592`): -2.3% host instructions per frame. The reuse pool was killed (-0.66 fps, 3/3) |
| Top 2, async/prewarm pipeline creation | Prewarm shipped 2026-10-24 (`prewarm.c`, up to 4 workers). The parallel compile of first-use bursts was closed by its scaling gate (Metal-cold 1.68x at 8 workers against 3x). An async first-use compile arrived with the Better-xemu4macOS merge (`perf.async_pipeline_compile`, `ccfdac39a5`) |
| Top 3, x87 clean-ST(i) write-back elision | Closed 2026-08-04: -0.23 fps, 3/3 |
| Top 4, reports idle-budget scan below 300 µs | Closed 2026-08-04: 50 µs costs -12.1 fps, 3/3 |
| Ranked 1, trace/superblock/region formation | Campaign closed 2026-07-18: every translate-time policy measured negative |
| Ranked 2, ret-memo remainder | Closed 2026-07-18: a 13-bit index -0.23 fps, 4/4; 12 bits stand |
| Ranked 3, sub-page arm (b) | Default-on 2026-07-18 (`ede35b84f9`): +12% draws/s, 6/6 |
| PAL 50 Hz guest vblank | Shipped opt-in (`[sys] vblank_rate`); deriving it from the guest's video timing stays on the roadmap |

## Profile at the primary anchor (2026-10-11)

One `probe-run.sh` session on `azurik-heavy-f1` (80 s, retrained PGO)
with `XEMU_GUEST_PROF`, `XEMU_INV_PROF`, `XEMU_TB_PROF` and
`XEMU_NV2A_NSPROF`, plus a 20 s `/usr/bin/sample`, on a loaded box. It
predates the 2026-10-15/16 TCG, x87 and threading work.

- **vCPU time:** 32.3% translated guest code, 67.7% host code. x87
  hard-float helpers about 24% together (`fmul` 5.1, `flds` 3.7 + 2.6,
  `fcom` 2.5, `fsts` 2.4); `pthread_jit_write_protect_np` 10.9% (barrier
  skid, not real W^X cost); mutex waits 8.3%, three quarters of them the
  PFIFO doorbell (`user_write`) blocked on `pfifo.lock`. The lockless
  DMA_PUT doorbell (2026-10-15, `3391b92357`) cut the doorbell from 12%
  to 2.2% of vCPU samples.
- **Guest TLB flush and refill:** about 0.2%, under top candidate 1's
  1.5% gate. Item 1 landed anyway: instructions per frame showed what
  samples missed.
- **Ret-memo:** `xemu: ras hits=45901 (0.5%) fills=10089110` over 110 s
  (the hit rate of inline-probe misses, not of the memo); the per-page
  memo flush costs nothing measurable.
- **`da_multi_subrange`:** 0 in all 21 intervals; multi-subrange draws
  don't occur on the heavy anchor.

## Top candidate 1: surface CPU-access-callback churn

NV2A surface register/unregister was the in-scene source of guest TLB
flushes: 199.8 callback events/s (4.16 per flip) against about 190
full-mmuidx flush calls/s, each a vCPU stop, a full flush and a
jump-cache wipe. The `XEMU_SURFACE_CB_REUSE=1` pool (`804bc5d87e`) read
-0.66 fps, 3/3, draws/s -1.9%, against a +0.25 bar (2026-08-04); its
bookkeeping ran on the render thread every shape swap. Its code was
removed on 2026-10-10, so a REUSE=1 arm means reverting the removal
commit. The ranged invalidation (template:
`tlb_reset_dirty_range_locked`) landed on 2026-10-15 although the
profile above priced it at about 0.2%.
Gotcha: HMP `info jit`'s `TLB full flushes` line can't fire on this
guest (it counts only `to_clean == ALL_MMUIDX_BITS`, and the Xbox target
has `NB_MMU_MODES=22`); use the partial and elided counters.

## Top candidate 2: async/prewarm pipeline creation

The 2026-08-04 movement probe saw a 45.09 ms worst compile (468.7
µs/flip) in the first-visit phase. On 2026-10-11 the `azurik-stream-e2`
walk never reproduced it (worst pipeline / shader compile 4.3 / 6.8
ms cold); the one hitch of
that class, 52.7 ms on `azurik-transition-f1`'s first-ever walk back
into F1, fell to 0.95 ms on the rerun: a persistent-cache coverage miss,
so it was parked. Azurik's fire-armor cast (an effect's first use with
cold driver caches, as after an update: 1.9-2.3 s of compiling)
reopened it, and prewarm shipped on 2026-10-24.

## Top candidate 3: x87 clean-ST(i) write-back elision

-0.23 fps, 3/3, against a +0.20 bar, although the refuter ran 3.49e9
checks with 0 violations: after the FT0 and FIP/FDP harvests the
elidable flushes are too few to pay for the dirty tracking. The
ledger's Failed row corrects the stated mechanism. Reopen only on a
census showing the clean-ST class past 8M/s on another title.

## Top candidate 4: reports idle-budget scan below 300 µs

300 against 50 µs, 3 pinned-env pairs: 50 µs costs -12.1 fps and -21.4%
draws/s, because more idle episodes qualify for an early submit (the
arch-1.1 submit storm). 300 µs sits correctly above the cliff; the knob
stays for triage.

## Ranked item 1: trace/superblock/region formation in TCG

Five translate-time policies, all measured on F8: M1 unconditional
concatenation (3.7% capture against a 10% bar), M2 all conditionals
(-1.6% throughput, 3/3), M2 forward-only (-2.3%, 3/3), region/diamond
formation at 64 B (-4.5%, 3/3; 72% drain-demotes) and at 16 B (-0.69
fps, 3/3; 64% demotes). TCG must spill dirty cc/eip globals before any
branch whose taken edge leaves the TB, so the censused 39-44% of block
boundaries with a dead flag materialization is harvestable only where
both successor edges are inline. Real 60 on F8 was not reached (floor
about 37.5-38.5 fps). Reopen only with a different attack (persistent
profile-guided region selection, IR caching across TBs), not another
window or policy variant.

## Ranked item 2: ret-memo remainder

A 13-bit index (128 to 256 KiB) measured -0.23 fps, 4/4, with clean scene
identity: the 12-bit hash already captures the recurring ret set, the
same shape as the jump-cache 12-to-16-bit kill.

## Ranked item 3: sub-page arm (b)

Promoted default-on 2026-07-18 (`ede35b84f9`): +12% draws/s on F8, 6/6
interleaved pairs over two batches (raw fps about flat: Azurik's effect
load re-saturates frame time). About 2M inline store completions/s, 20x
the code-page trap count the prediction was sized on. Refuters: about
19.5M decisions, 0 violations, including loadvm-cycling and streaming
soaks. `XEMU_SUBPAGE_FAST=0` reverts.
