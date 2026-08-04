# Lockless MMIO — closure verdict for the remaining BQL blocks — 2026-08-04

The decision **not** to convert the last BQL-holding guest register
blocks (NV2A `PMC` / `PCRTC` / `PTIMER`, APU `main` / `gp` / `ep`) to
BQL-free dispatch, taken on fixture-independent arithmetic instead of a
bench run. This closes the "Reopen if" of failure-archaeology 7.5
("audits for PGRAPH/PMC/APU-VP regions (the remaining 20%) are
written"): PGRAPH and APU-VP were audited and shipped, and this page is
the written verdict on the rest.

Companion to [`pgraph-lockless-audit.md`](pgraph-lockless-audit.md),
which is the *ship* verdict for the one block that cleared the bar. The
two pages share a method — classify each range by what the BQL actually
protected — and reach opposite conclusions for the same reason: the BQL
is load-bearing exactly where device state is shared with another
BQL-holding thread and nothing else serialises it. For PGRAPH that was
two fields, fixable with two atomics. For the blocks below it is the
PCI interrupt line itself.

Verdict: **CLOSED — do not build.** Reopen bar in §E.

## A. The ceiling arithmetic — the entire prize, before any conversion

Every figure here is recorded history from the pre-campaign
`XEMU_MMIO_PROF` run (archaeology 7.5, heavy scene, 70 s), not a new
measurement:

| Quantity | Value | Source |
|---|---|---|
| Guest MMIO ops | 8.2M / 70 s = **~117k ops/s** | `XEMU_MMIO_PROF` histogram, 2026-07-04 |
| Mean BQL acquire cost per op | **~52 ns** | archaeology 7.5's lesson line ("average lock cost (~52 ns) was never the story") |
| ⇒ vCPU time spent waiting for the BQL on MMIO | 117k × 52 ns = 6.1 ms per wall second = **0.61% of one core** | arithmetic |

That 0.61% is the **whole** prize the lockless-MMIO campaign was ever
competing for — the total, across every register block, before a single
region opted in. It is also the number that makes this page
fixture-independent: no scene can make the residual bigger than the
part of 0.61% that has not already been harvested.

What was harvested, from the same 70 s run:

| Block | Ops / 70 s | Share of the 8.2M | Status |
|---|---|---|---|
| NV2A `PFB` (`NV_PFB_WBC` polling) | 4.85M | 59% | lockless since `ee9100e538` (2026-07-04) |
| NV2A `USER` (doorbells) | 1.58M | 19% | lockless since `ee9100e538` |
| APU `vp` (voice-processor doorbell) | ~940k | ~11% | lockless (`apu.c:432-448`) |
| NV2A `PGRAPH` | not isolated in this run | — | lockless since 2026-07-11 (`pgraph-lockless-audit.md`) |
| everything else — `PMC` / `PCRTC` / `PTIMER` / APU `main`,`gp`,`ep` / … | remainder | **< 11%** | still BQL |

PFB + USER + VP alone account for ~89% of the traffic, and PGRAPH — the
hottest of the remainder, 1.6M `NV_PGRAPH_INTR` reads on its own stats
run — came out of what was left. So the un-harvested residual is
**strictly under 11% of 0.61% ≈ 0.07% of one core**, and in truth well
under that once PGRAPH is subtracted.

Converted to the anchor everyone benches on (F8, vCPU-bound; ~36-39 fps
at the time this arithmetic was written, ~48.1 fps after the 2026-08-04
wave — the conversion below is the pre-wave one and §A.1 redoes it at
the new anchor): 0.07% of the vCPU thread is **≈ +0.02 fps**, with the audit's
pre-registered band at **+0.01 to +0.06 fps**. The static-baseline
resolution of the interleaved savestate A/B protocol is ±0.02 fps
(`xemu-validation-and-qa`). The residual is therefore at or below the
measurement floor: **not merely small, but unvalidatable by
construction** — the same kill reason that closed the lock-free
`frame_seq` ring peek (ledger, Settled vectors).

### A.1 The measured residual (2026-08-04) — the arithmetic holds

The number this page owed is in. One clean `XEMU_MMIO_PROF=1` F8 run at
current defaults (all four blocks lockless), quit through the monitor so
the `atexit` dump printed, 100.7 s of wall time:

| Field (`xemu_mmio_prof_dump`, `accel/tcg/cputlb.c:2105-2113`) | Reading |
|---|---|
| `ops` (BQL-taking MMIO only — the lockless path returns before the counter) | **2,178,310** = ~21.6k ops/s |
| `BQL wait total` | **101.47 ms** over 100.7 s = **75.6 ms per 75 s** |
| `avg` | **47 ns/op** |
| `max` | **82.0 µs** |

Three things this settles:

1. **The residual is 0.10% of one core** (101.47 ms / 100.7 s), against
   §A's fixture-independent estimate of < 0.07%. The model is right to
   within ~1.5x — close enough that the estimate was not optimistic, and
   the gap is measurement of a different scene, not a missing prize.
   Converted the same way §A converts: ≈ **+0.05 fps** at the post-wave
   48.1 fps F8 anchor, inside the audit's pre-registered +0.01-0.06 fps
   band and still at the ±0.02 fps static-baseline resolution class.
2. **The 52 ns mean-acquire model survives the harvest.** 47 ns measured
   vs ~52 ns assumed, on a completely different op population three
   campaigns later.
3. **Residual op volume matches the ceiling table.** 21.6k BQL-taking
   ops/s against the pre-campaign 117k ops/s total is ~18% left
   un-harvested — the same order as the "< 11% + PGRAPH's share" row in
   §A, arrived at from the opposite direction.

Against §E's reopen bar (`BQL wait total` ≥ 500 ms / 75 s): 75.6 ms is
**6.6x under**. The verdict stands on a measurement now, not only on
arithmetic.

## B. The mean was never the story — and the tail is already harvested

Archaeology 7.5's honest receipt for the PFB/USER conversion was **fps
parity**, not a speedup: mean +0.54 fps riding one outlier, with the
pre-registered +0.3-1.0 recorded as killed. What did move was
steadiness — per-run fps stdev 1.21 → 0.89, 5/6 pairs — because the
340 µs BQL-acquire spike class stopped landing on those ops. PGRAPH
reproduced the same shape (parity −0.65±1.43 sign-mixed at n=3, spread
0.43 vs 1.00).

The jitter win is a function of *where the ops are*, and the ops are in
the four converted blocks. Guest traffic to `PMC`/`PCRTC`/`PTIMER` is
the interrupt-acknowledge path — a handful of register accesses per
interrupt, against PFB's continuous write-combine-flush polling. There
is no tail left to harvest there either.

## C. The correctness wall — why PMC lockless is not a small change

`memory_region_enable_lockless_io()` removes the BQL from the dispatch
of a region's `.read`/`.write` (`system/memory.c:2632`, honored on the
TCG fast path at the four `if (mr->lockless_io)` sites in
`accel/tcg/cputlb.c` — that file is under active TCG work, so
`grep -n lockless_io` rather than trusting a line number). It removes
it *entirely* — the handler then runs holding no BQL at all. For these
blocks the handler body is:

```
pmc_write   (hw/xbox/nv2a/pmc.c:62-67)
pcrtc_write (hw/xbox/nv2a/pcrtc.c:58-63)
ptimer_write(hw/xbox/nv2a/ptimer.c:74-79)
    d-><unit>.pending_interrupts &= ~val;   /* plain C RMW */
    nv2a_update_irq(d);                     /* inline, same call frame */
```

and `nv2a_update_irq` (`hw/xbox/nv2a/nv2a.c:25-58`) reads every unit's
interrupt pair, recomputes `d->pmc.pending_interrupts` as a plain RMW
aggregate, then drives the shared PCI interrupt line via
`pci_irq_assert`/`pci_irq_deassert` (`include/hw/pci/pci.h:1001/1006`).
Its other callers hold the BQL and no NV2A lock: the VGA
`gfx_update` vblank raise on the iothread (`nv2a.c:200-210`, which
itself does a plain `|=` on `d->pcrtc.pending_interrupts`), the PFIFO
thread, and the sibling unit writers.

Dropping the dispatch BQL here therefore reproduces the
`pgraph-lockless-audit.md` §C data race one level up, and adds a worse
one:

1. **Lost interrupt state.** The vCPU's read-modify-write clear of
   `pending_interrupts` races the iothread's vblank `|=` and the
   aggregate recomputation inside `nv2a_update_irq`. A lost bit here is
   a guest ISR that never fires or never clears — the lost-wakeup class
   archaeology 7.1 names, one layer up (the guest's interrupt instead of
   the PFIFO condvar). It presents as a freeze, not a visual glitch.
2. **An unserialised PCI IRQ-line change.** `pci_irq_assert` from a
   thread holding no BQL is not merely a C11 data race on device state;
   it mutates interrupt-controller state the rest of QEMU accesses
   under the BQL. This is the part that has no atomics-only fix.

Note that PGRAPH did **not** hit wall 2: neither `pgraph_write`
interrupt case ever calls `nv2a_update_irq` (audit §C), which is
precisely why converting two fields to `qatomic` was sufficient there.
PMC/PCRTC/PTIMER call it inline, unconditionally, on every interrupt
write. The APU `main`/`gp`/`ep` regions were left BQL-locked for the
identical reason (`apu.c:437-441`: "direct `update_irq` calls").

## D. The correct future shape, if the residual ever justifies it

Not "make PMC lockless". The only sound shape is the one the APU
already demonstrates in-tree:

1. **Atomic state.** Convert `pending_interrupts`/`enabled_interrupts`
   for PMC/PCRTC/PTIMER *and* the `d->pmc` aggregate to `qatomic`
   accessors — the `pgraph-lockless-audit.md` §E move, applied one
   level up. This is a strict improvement even without any lockless
   dispatch, since the iothread vblank raise at `nv2a.c:206` and the
   BQL-held readers are already an unserialised pair today.
2. **Explicit BQL around the IRQ drive.** Wrap `nv2a_update_irq` in an
   explicit `bql_lock()`/`bql_unlock()` inside the handler — exactly
   what the APU thread already does before its own `update_irq`
   (`hw/xbox/mcpx/apu/apu.c:295-305`).

The arithmetic consequence is the reason this stays closed: every
*write* to these blocks calls `update_irq`, so step 2 re-pays the lock
on the ops being converted. Only the *reads* — the guest ISR's
`NV_PMC_INTR_0` / `NV_PMC_BOOT_0` polls — become genuinely lock-free.
The mechanism that is safe covers only a fraction of a residual that is
already below the measurement floor.

## E. Reopen bar

One `XEMU_MMIO_PROF=1` run on the F8 savestate, current defaults (all
four converted blocks lockless). **Run 2026-08-04 — §A.1 has the
reading: 75.6 ms per 75 s, 6.6x under the bar.** Reopen **only if** a
future dump line reads **`BQL wait total` ≥ 500 ms over a ≥ 75 s run**
— i.e. ≥ 0.67% of a core still spent waiting for the BQL on guest MMIO
after the harvest.
That is an order of magnitude above §A's arithmetic, so clearing it
would mean the model in §A is wrong, not that the residual grew. Below
that threshold there is nothing to win and §C's hazards stand.

The dump is an `atexit` print, so the run must quit cleanly (monitor
`quit`, per `xemu-run-and-operate`); the line to record is the one
beginning `xemu: MMIO prof:`.

Two things this page does **not** close, both tracked elsewhere:

- **The real-hardware soak of the already-shipped lockless blocks**
  (Windows/Linux take the same BQL-free dispatch —
  `pgraph-lockless-audit.md` §I.3). That is `docs/roadmap.md` item 5,
  not a perf item.
- **The full IRQ-path lock review** (`pgraph-lockless-audit.md` §I.2).
  A correctness item on its own merits, and the thing that would have
  to land *first* if the residual in §A were ever worth chasing.

## F. Re-verification

```sh
cd /Users/michaelsrouji/Documents/Xemu/tools/xemu-macos
# the lockless set: PFB / USER / PGRAPH (NV2A) + vp (APU) — and nothing else
grep -n 'memory_region_enable_lockless_io' hw/xbox/nv2a/nv2a.c hw/xbox/mcpx/apu/apu.c
# the wall: every un-converted block's write handler calls update_irq inline
grep -n 'nv2a_update_irq' hw/xbox/nv2a/pmc.c hw/xbox/nv2a/pcrtc.c hw/xbox/nv2a/ptimer.c
grep -n 'pci_irq_assert' hw/xbox/nv2a/nv2a.c hw/xbox/mcpx/apu/apu.c
# the precedent for §D step 2 (atomic state + explicit BQL around the drive)
grep -n -B2 -A2 'bql_lock();' hw/xbox/mcpx/apu/apu.c
# the profiler's dump format quoted in §A.1
grep -n 'MMIO prof' accel/tcg/cputlb.c
```

Line anchors verified 2026-08-04 at `f4bb867d3c`; they drift — re-grep
before citing. Method and thresholds: `.claude/skills/xemu-testing`,
`.claude/skills/xemu-validation-and-qa`. History:
`.claude/skills/xemu-failure-archaeology` 7.5. Candidate provenance:
`docs/fork-optimization-audit-2026-08.md` §4.
