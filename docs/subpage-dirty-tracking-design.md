<!--
Provenance: written 2026-07-11 by the SMC/TB-invalidation-churn campaign
(archaeology 1.22) as its gate-3 deliverable, after XEMU_INV_PROF measured
100% of TB invalidations as data-write false sharing (0 true SMC, 100%
recycle-hit) and the range-filter cure was counter-killed (25.2x notdirty-
trap explosion). STATUS: PARKED — ranked #1 open fps lever, wrong-code-hang
failure class, requires refuter-first treatment per xemu-research-methodology
before any implementation. Counters to re-run first: XEMU_INV_PROF=1.
-->

# Sub-page dirty-tracking — design sketch (PARKED, do NOT implement without orchestrator sign-off)

Gate bullet 3 deliverable. The INV_PROF data is notdirty-dominant (100% of
invalidations are guest data writes to code pages) with 100% false-sharing and 0%
true SMC on both F5 and F8. The range-check cure (XEMU_TB_RANGE_INV) removes the
invalidate/recycle round-trip but leaves the page write-protected, so it may raise
notdirty-trap frequency (the tlb_unprotect_code tradeoff). Sub-page dirty tracking
is the higher-ceiling / lower-margin-risk alternative: stop the write from trapping
at all when it lands outside the code sub-block.

## The mechanism today (page-granular)
- A physical page holding any TB is marked "contains code": its TLB entry is
  write-protected (dirty bitmap DIRTY_MEMORY_CODE cleared). Any guest store to that
  page faults into notdirty_write (cputlb.c:1340) -> physical_memory_test_and_clear_dirty
  (a full-TLB walk, ~2.0% of the vCPU thread) + tb_invalidate_phys_range_fast.
- Granularity is TARGET_PAGE (4 KiB). A write anywhere on the page traps, even if
  the code lives in a disjoint 64-byte span of that page. That is exactly the
  false-sharing INV_PROF measured (100%).

## The sketch
Track "contains code" at a sub-page block granularity (e.g. 64 or 128 B blocks,
one bit per block, a small per-PageDesc bitmap alongside the existing per-page
code state). A guest store consults the sub-block bit:
- store to a NON-code sub-block  -> take the fast (dirty) store path; NEVER enter
  notdirty_write; no test_and_clear, no range scan, no invalidation.
- store to a code sub-block       -> current notdirty path (unchanged: which TBs
  get invalidated once triggered is unchanged; only the TRIGGER rate drops).
Set a block's code bit when a TB is linked whose [tb_page_addr0 .. +size) touches
it (tb_link_page / tb_record); clear when the page's last covering TB is removed.

Ceiling: eliminates the trap + test_and_clear + scan for the 100% of writes that
INV_PROF shows do not overlap code — strictly better than the range check, which
only removes the invalidation half and keeps (or worsens) the trap half. Estimated
same several-% of the vCPU thread the roadmap bounds, without the trap-frequency
downside.

## Race-window analysis (archaeology 1.9 class: test_and_clear on shared state)
The existing page-granular dirty state already has a known cleared-before-consumed
hazard class (README "Dirty-clear TLB-walk coalescing", the one shipped bug in that
family). Adding a second, finer bitmap widens the surface:
1. TLB arming vs bitmap update. The write-protect decision is cached in the TLB
   comparator (TLB_NOTDIRTY bit, set at tlb_set_page). A sub-block bitmap consulted
   AFTER the TLB says "notdirty" must agree with what armed the TLB, or a store to
   a non-code block that the TLB still has marked notdirty will trap anyway (benign:
   just no speedup) OR — the dangerous direction — a store the bitmap thinks is
   non-code but that actually overlaps freshly-linked code skips invalidation
   (wrong-code hang, the worst class). The bitmap must therefore be updated (code
   bit SET) strictly BEFORE the TB is executable, i.e. before tb_link_page publishes
   it into the page list — with the same page-lock discipline do_tb_phys_invalidate
   uses, and a release/acquire so the TLB-arming CPU sees the set bit.
2. Clear on last-TB-removal. When the last TB covering a sub-block is invalidated,
   the code bit clears. If it clears while a concurrent store is mid-flight on
   another path, the store could take the fast path against a page whose TLB is
   still armed notdirty -> divergent (store lost from the dirty log). Single vCPU
   thread makes true concurrency here nil, but the async tb_flush / RCU reclaim and
   the PFIFO-thread dirty consumers (vertex/texture dirty tracking read the SAME
   DIRTY_MEMORY bitmaps) are cross-thread — the sub-block bitmap must not alias the
   bitmaps those consumers read, or it changes their semantics.
3. Interaction with tlb_unprotect_code. Today unprotect keys on "page has no TBs".
   With sub-block tracking, unprotect must key on "no code sub-blocks set" — and the
   partial-page case (some blocks code, some not) must keep the page protected while
   still fast-pathing the non-code stores, which requires the per-store bitmap
   consult to be on the store fast path itself (a TLB-comparator-adjacent check),
   not only in the trap handler. That is the expensive part to get right and cheap.

## Verdict
Higher ceiling, no trap-frequency downside, but touches the store fast path and the
shared dirty-bitmap machinery that already shipped one cleared-before-consumed bug
(1.9 / README dirty-clear entry) and that the PFIFO-thread vertex/texture trackers
also read. Needs a dedicated correctness review + an adversarial refuter (run
sub-block-filtered and full-page in parallel, assert identical invalidation sets and
identical dirty-consumer output over a level-transition/death-reload soak) BEFORE
any A/B. Do not implement without orchestrator sign-off.
