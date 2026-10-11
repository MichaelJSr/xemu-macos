# Closed roadmap candidates — full write-ups

> **Finished.** These entries left [`roadmap.md`](../roadmap.md) on
> 2026-10-10 because they are closed; the roadmap keeps a one-line stub
> under the same number so citations still resolve. Each is also
> recorded in the [ledger](../optimizations.md). Text is moved verbatim;
> figures are as of the date in each heading, and relative links are
> re-pointed from this folder. The ranked item 1 machinery, and the two
> censuses it cites (`XEMU_CCOP_CENSUS`, `XEMU_SUPERBLOCK_SIZE`), were
> removed from the code on 2026-10-10, as were ranked item 2's
> `XEMU_RETC_BITS` knob (the width is fixed at 12) and top candidate 3's
> `XEMU_X87_ELIDE_CLEAN` (the ledger's Failed row corrects its stated
> mechanism); the figures stand as recorded.

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
