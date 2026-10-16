# Closed roadmap candidates — full write-ups

> **Finished.** These entries left [`roadmap.md`](../roadmap.md) on
> 2026-10-10 because they are closed, except top candidates 1 and 2,
> which followed on 2026-10-15 because they were parked with reopen bars
> (ranged TLB flush: ~0.2% of vCPU vs a 1.5% gate; prewarm: a ≥ 16 ms
> compile that survives a warm rerun). Top candidate 1 landed the same
> day anyway (see its 2026-10-15 note). "Above" in those two means the
> roadmap's 2026-10-11 profile. The roadmap keeps a one-line stub
> under the same number so citations still resolve. Each is also
> recorded in the [ledger](../optimizations.md). Text is moved verbatim;
> figures are as of the date in each heading, and relative links are
> re-pointed from this folder. The ranked item 1 machinery, and the two
> censuses it cites (`XEMU_CCOP_CENSUS`, `XEMU_SUPERBLOCK_SIZE`), were
> removed from the code on 2026-10-10, as were ranked item 2's
> `XEMU_RETC_BITS` knob (the width is fixed at 12) and top candidate 3's
> `XEMU_X87_ELIDE_CLEAN` (the ledger's Failed row corrects its stated
> mechanism); the figures stand as recorded.

## Top candidate 1 — surface CPU-access-callback churn (parked 2026-10-11)

**Surface CPU-access-callback churn — pool shape MEASURED AND
KILLED 2026-08-04 (same day); ranged flush is the only live shape,
needs-theory.** The mechanism receipt stands: **199.8 callback
events/s = 4.16 per flip** against **~190 full-mmuidx TLB flush
calls/s** (0.95:1) — NV2A surface register/unregister is essentially
*the* in-scene source of guest TLB flushes, each an
`async_safe_run_on_cpu` vCPU stop + full flush + unconditional
jump-cache wipe. But the fps half died on measurement: the
`XEMU_SURFACE_CB_REUSE=1` pool ran the full gauntlet (loadvm cycles
clean, coverage audit 23,003/0, movement probe `tb_flush` flat 1)
and then read **−0.66 fps, 3/3 unanimous negative, draws/s −1.9%**
on idle-gated pairs against the pre-registered ≥ +0.25 bar. The
likely mechanism for the miss: post-candoio-elision the vCPU's
flush/refill cost was already off the critical path (the audit
priced §1.3 against the PRE-wave profile), while the pool's
park/reattach bookkeeping runs on the render thread every shape
swap. The pool stayed in-tree dark as the record until its code was
removed on 2026-10-10 (Failed row in the ledger; implementation in
`804bc5d87e`; a REUSE=1 arm means reverting the removal commit).
The only live shape is
(b) the `// FIXME: flush only applicable pages` ranged invalidation
in `system/physmem.c` / `accel/tcg/cputlb.c` (template:
`tlb_reset_dirty_range_locked`) — it spares the jump cache and
costs no per-swap bookkeeping, but needs a deferred/queued flush
API. Status: needs-theory; reopen only with a predicted number
derived from a POST-wave profile, not the audit's.
**2026-10-11:** the post-wave profile exists (above): guest TLB
flush + refill is ~0.2% of vCPU samples on `azurik-heavy-f1`, so the
ranged flush has nothing to win; stays parked.
**2026-10-15:** the ranged flush landed anyway (`tlb_flush_host_range`,
`d1198c3592`): −2.3% host instructions per frame on deterministic
offscreen heavy-f1; see the ledger.
Measurement gotcha, recorded so nobody re-derives it: HMP `info jit`'s
**`TLB full flushes` line is structurally unreachable on this guest**
(it counts only `to_clean == ALL_MMUIDX_BITS`, and the Xbox target
has `NB_MMU_MODES=22` vs plain i386's 8) — use the partial + elided
counters.

## Top candidate 2 — async/prewarm pipeline creation (parked 2026-10-11)

**Async/prewarm pipeline creation — arch 1.12 REOPENED 2026-08-04.**
The original kill measured steady state only. The audit's zero-code
gate (movement probe, forward first-visit phase vs the backward
control, gating on `max=` and event count rather than a per-flip
average) **fired**: forward-phase `pipeline_gen` runs **468.7
µs/flip with a single worst compile of 45.09 ms** — about two dropped
frames at the current anchor, inside the first-visit streaming
workload class the steady-state kill never sampled. That graduates
Experiment B option (a), prewarm, from
[`moltenvk-optimization-experiments.md`](../Future/moltenvk-optimization-experiments.md)
into a ranked item **with a baseline number attached**. First steps:
re-run the probe with cold and warm `pipeline_cache.bin` to separate
"compile" from "cache miss", then price prewarm against that 45 ms
tail. Bar to attempt: the hitch must survive a warm-cache run — a
warm-cache-only hitch is a cache-coverage problem, not a
pipeline-creation one.
**2026-10-11: answered — cache coverage, not prewarm.** The
`azurik-stream-e2` walk (the first-visit streaming walk-in) never
reproduced a 45 ms-class compile: worst single compile in the forward
phase 0.8 ms pipeline / 2.3 ms shader with warm caches, 4.3 / 6.8 ms
cold (`movement-probe.sh --isolate-caches`), and its ~20 fps dip is
the same warm and cold, so it is scene cost. The one hitch of that
class came on `azurik-transition-f1`'s walk back into F1: a single
**52.7 ms** pipeline compile on the first-ever run of that route,
with the shared caches otherwise warm; the rerun, after that run had
saved its pipelines, peaked at 0.95 ms for the same 66-pipeline
burst. The hitch is a first-visit miss in the persistent pipeline
cache, gone once warm, so by this item's own bar it is a
cache-coverage problem and prewarm is not the fix. Parked; reopen
only on a forward-phase compile ≥ 16 ms that survives a warm rerun.

## Top candidate 3 — x87 clean-ST(i) write-back elision (closed 2026-08-04)

**x87 clean-ST(i) write-back elision (`XEMU_X87_ELIDE_CLEAN`) —
MEASURED AND CLOSED 2026-08-04.** The fps A/B it was awaiting ran on
idle-gated pairs: **−0.23 fps, 3/3 negative** against the ≥ +0.20
promotion bar. The refuter had already come back stronger than
predicted (3.49e9 checks, 0 violations, truncation signal absent),
but safe-and-worthless is still worthless: the elidable flush
population is too thin after the FT0 + FIP/FDP harvests to pay for
the dirty tracking. Stays dark permanently as the record; do not
reopen without a census showing the clean-ST class regrew past
8M/s on some other title.

## Top candidate 4 — reports idle-budget scan below 300 µs (closed 2026-08-04)

**Reports idle-budget scan below 300 µs — RAN AND CLOSED
2026-08-04.** The deferred pinned-value A/B ran with the `abx.sh`
pinned-env form (300 vs 50 µs, 3 idle-gated pairs): **50 µs costs
−12.1 fps, unanimous, draws/s −21.4%** — the lower threshold makes
*more* idle episodes qualify for an early submit and lands squarely
on the arch-1.1 submit-storm side of the cliff, exactly the failure
mode the scan's guard predicted. The modest +0.1-0.2 hope from the
20.7%-conversion calibration was swamped by the pass-count blowup.
**300 µs is validated as sitting correctly above the cliff; the
scan is closed permanently.** The knob stays for triage. Residual
method note: `bench-savestate-ab.sh` still cannot express
pinned-value A/Bs (B arm hardcoded to 0); promoting `abx.sh` into
`scripts/` remains the cheap enabler for any future pinned-env
experiment.

## Ranked item 1 — trace/superblock/region formation in TCG (campaign closed 2026-07-18)

**Trace/superblock/region formation in TCG — CAMPAIGN CLOSED
2026-07-18.** Five translate-time policies were designed, built,
and measured in one arc — M1 uncond concatenation
(instrument-killed at 3.7% capture), M2 all-conditionals (−1.6%
throughput 3/3), M2 forward-only (−2.3% 3/3), region/diamond
formation at 64 B (−4.5% 3/3, 72% drain-demotes) and at 16 B (the
single pre-registered mechanism iteration: −0.69 fps 3/3, demotes
still 64%). The enabling TCG work — recorded-label liveness elision
with a bit-identical-when-unflagged proof harness — is sound and
stays landed dark (`XEMU_REGION*`), alongside the seam machinery
and censuses, as a complete reproducible record. **Final verdict:
the censused 39-44% dead-flag mass is not harvestable by
translate-time policy on this workload; real-60 on F8 was not
reached (floor ~37.5-38.5), and the honest displayed-60/120 path
remains MetalFX interpolation.** Reopen only with a fundamentally
different attack (e.g. persistent profile-guided region selection,
or hot-path IR caching across TBs) — not another window/policy
variant. (Original re-scope notes below for history.)

*(historical)* Re-scoped 2026-07-18 after
the seam-following campaign measured negative. The quantified
basis stands: `XEMU_CCOP_CENSUS` puts ~39-44% of block boundaries
carrying a dead flag materialization, and the 2026-07-18 tail-kind
census (`XEMU_SUPERBLOCK_SIZE=1`, runtime-weighted, F8) splits the
taken goto_tb exits: jcc_taken 50.4%, jcc_fall 27.3%, call 12.2%,
uncond jmp 5.2% (M1-capturable same-page-forward subset: 3.7%),
rep 2.7%. The seam-following mechanism is in-tree and dark
(`XEMU_SUPERBLOCK=N`): M1 concatenates through unconditional
same-page forward jumps; M2 continues through conditional
fallthroughs with the taken edge in an out-of-line lookup stub.
Campaign verdicts (ledger, Failed table): M1-alone
instrument-killed (3.7% capture < the pre-registered 10% bar);
M1+M2 all-conditionals **−1.6% draw throughput** (feedback-amplified
to −8.4% fps, 3/3 pairs); forward-conditionals-only **−2.3%
throughput** (3/3) — the loop-protection hypothesis refuted.
**The corrected theory:** TCG must spill dirty cc/eip globals
before ANY branch whose taken edge exits the TB (the successor
re-derives state from env), so conditional seams can never elide
the flag materialization regardless of stub layout — the censused
39-44% is harvestable only where BOTH successor edges are inline.
The real design is therefore **region/diamond formation**
(translate both sides of short forward conditionals to their join,
eliminating the exits themselves) with code-duplication control —
a genuinely weeks-scale compiler build, matching this item's
original "transformative or bust" framing. The landed machinery
(merge gates, stub emission, tail-kind census) is its foundation;
before any default-on promotion the movement-phase probe and a
loadvm soak are REQUIRED (TB-lifetime change class).

## Ranked item 2 — ret-memo remainder (closed 2026-07-18)

**Ret-memo remainder — CLOSED 2026-07-18.** The 12→13-bit
experiment ran as a one-binary A/B (the index width is now a
runtime-latched knob, `XEMU_RETC_BITS`, default 12): **13-bit
measured −0.23 fps, 4/4 pairs negative, scene identity clean** —
the 128→256 KiB footprint cost is real and the 12-bit hash already
captures the recurring ret set (same shape as the jump-cache
12→16-bit kill). 12 bits stands; the knob stays for re-tests on
other titles. Ledger has the Failed row.

## Ranked item 3 — sub-page arm (b) (promoted default-on 2026-07-18)

**Sub-page arm (b) — PROMOTED DEFAULT-ON 2026-07-18** (the recipe
below was executed same-day on a quiet machine and passed every
gate). Result: **+12% draws/s throughput on F8, 6/6 interleaved
pairs across two independent 3-pair batches** (raw fps ~flat —
Azurik's effect-load feedback re-saturates frame time at ~815 vs
~727 draws/flip; batch 2 alone was +0.43 fps 3/3); mechanism
quantified at ~2M inline store completions/s, a population ~20x
the code-page trap count the original prediction was sized on.
Refuter total ~19.5M decisions / 0 violations including
loadvm-cycling and movement-probe streaming soaks (`tb_flush`
flat). `XEMU_SUBPAGE_FAST=0` reverts. Full entry: ledger.
