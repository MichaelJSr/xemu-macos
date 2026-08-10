# PGRAPH MMIO lockless audit — 2026-07-11

Static + concurrency audit of the NV2A **PGRAPH** register block
(`0x400000`, size `0x2000`) to decide whether its guest MMIO dispatch can
drop the BQL the way PFB/USER already did (`ee9100e538`, archaeology 7.5).
Performed on `macos-optimizations`. This is README roadmap item 3 and the
explicit "Reopen if" of archaeology 7.5 ("PGRAPH races the PFIFO thread …
none are trivially safe like PFB/USER").

Verdict vocabulary, per register range:

- **SAFE-LOCKLESS** — correctness under BQL-free dispatch already follows
  from a *non-BQL* lock the handler holds (`pg->lock` and/or
  `pfifo.lock`) or from an existing atomic access. The BQL added nothing.
- **NEEDS-FINE-LOCK** — safe only after a named, targeted change. Here the
  only such case is the interrupt-status/enable pair, whose sole
  BQL-provided guarantee (serialisation against the lock-free
  `nv2a_update_irq` reader *and* the PFIFO thread's raise) is restored by
  making two fields atomic. After that change these ranges are
  SAFE-LOCKLESS.
- **UNSAFE** — cannot be made BQL-free without unacceptable risk or scope.
  **No PGRAPH range lands here** once the two interrupt fields are atomic.

Honest scope: this delivers a *statically-audited, atomics-hardened,
soak-validated* BQL-free PGRAPH dispatch. It makes **no** perf or jitter
claim — the orchestrator measures. The hit-rate counter (§F) is provided
so the ranking of ranges by guest hit-rate can be confirmed empirically
rather than asserted.

## A. What `lockless_io` guarantees, and what it does not

`memory_region_enable_lockless_io(mr)` sets `mr->lockless_io`; the fork's
cputlb fast path (`accel/tcg/cputlb.c:2111/2145/2674/2707`) then calls the
region's `.read`/`.write` **without** `BQL_LOCK_GUARD()`. It changes
*exactly one thing*: the BQL is no longer implicitly held across the
handler. It does **not**:

- remove any lock the handler itself takes (`pg->lock`, `pfifo.lock`);
- provide or remove ordering versus the PFIFO thread — that was never the
  BQL's job here (the PFIFO thread does not hold the BQL while pulling
  methods; it arbitrates against MMIO purely through `pg->lock`);
- change TCG single-vCPU serialisation — the xbox machine runs exactly one
  vCPU thread, so there is never a second concurrent guest MMIO access.

The PFB/USER precedent argued safety two ways (archaeology 7.5): PFB
handlers touch only constants / plain `regs[]` and raise no IRQs, and the
PFIFO thread's tile-config reads *were never BQL-protected anyway*; USER
handlers run entirely under `pfifo.lock` with the Invariant-10 kick
discipline, so the BQL was redundant. **The through-line: the BQL is
load-bearing only where device state is shared with another *BQL-holding*
thread and nothing else serialises it.** For PGRAPH that set turns out to
be exactly two fields (§C).

## B. The PGRAPH MMIO surface

File:line references in §B-§E are **pre-change coordinates** (tree at
`2d304b0958`, before this audit's edits landed); the shipped tree is
offset by the §F counter block but structurally identical.

`pgraph_read` (`pgraph.c:44`) takes `pg->lock` only. `pgraph_write`
(`pgraph.c:85`) takes `pfifo.lock` **then** `pg->lock` (the tree-wide
`pfifo.lock -> pgraph.lock` order; comment at `pgraph.c:3362`). Every
guest-reachable case:

| Range (offset) | Op | Touches | Kick? |
|---|---|---|---|
| `NV_PGRAPH_INTR` (0x100) | r/w | `pg->pending_interrupts`; clears `waiting_for_nop`/`waiting_for_context_switch` (already `qatomic`) | write: yes |
| `NV_PGRAPH_INTR_EN` (0x140) | r/w | `pg->enabled_interrupts` | no |
| `NV_PGRAPH_CTX_USER` (0x148) | via CTX_TRIGGER | `regs_[]` | no |
| `NV_PGRAPH_SURFACE` (0x710) | default | `regs_[]` (flip READ_3D/WRITE_3D/MODULO_3D handshake) | no |
| `NV_PGRAPH_INCREMENT` (0x71C) | w | `SET_MASK(SURFACE, READ_3D)` in `regs_[]` | yes |
| `NV_PGRAPH_FIFO` (0x720) | w (default) | `regs_[NV_PGRAPH_FIFO]` (FIFO_ACCESS) | yes (events switch) |
| `NV_PGRAPH_RDI_INDEX` (0x750) | default | `regs_[]` (select+address pair) | no |
| `NV_PGRAPH_RDI_DATA` (0x754) | r/w | `pg->vsh_constants[]` + auto-increments RDI_INDEX.address in `regs_[]` | no |
| `NV_PGRAPH_CHANNEL_CTX_TRIGGER` (0x788) | w | reads `d->ramin_ptr` (guest RAM), writes `CTX_USER` in `regs_[]` | no |
| everything else | r/w | plain `regs_[]` load/store via `pgraph_reg_r/w` | no |

All kicks are `pfifo_kick(d)`, which requires `pfifo.lock` held across the
`fifo_kick=true` + `qemu_cond_broadcast` pair (Invariant-10, `pfifo.c:100`).
`pgraph_write` holds `pfifo.lock` for its whole body, so **every PGRAPH
kick keeps its discipline with or without the BQL.** This audit adds no
kick site (still 20 tree-wide).

## C. Concurrency map — what the BQL actually protected

`regs_[0x2000]` and the RDI `vsh_constants` are shared between the vCPU
MMIO handlers and the **PFIFO thread**, which reads/writes them under
`pg->lock` while running `pgraph_method`/`pgraph_context_switch`
(`pfifo.c:207-218`, `:246-254`). The BQL never mediated that — `pg->lock`
does, unchanged by `lockless_io`. The main/UI present path
(`pgraph_vk_get_present_frame`, `renderer.c:312`) reads surface state under
`pfifo.lock` + a `qemu_event` handshake executed *on the PFIFO thread*, not
under a BQL-only read of `regs_`. Reset/savevm/loadvm run under
`nv2a_lock_fifo` (`pfifo.lock` + `pgraph.lock`), and renderer-switch runs
via `run_on_cpu` (vCPU parked). None of these rely on the BQL to exclude
the MMIO handlers.

The **one** exception is the PGRAPH interrupt pair
(`pending_interrupts`, `enabled_interrupts`):

- `nv2a_update_irq` (`nv2a.c:25`) reads *both* and then drives the PCI IRQ
  line (`pci_irq_assert/deassert`, which itself needs the BQL). Its callers
  — VGA `gfx_update` (`nv2a.c:205`, iothread), `pmc`/`ptimer`/`pcrtc`
  writes, and the PFIFO thread — hold the **BQL but not `pg->lock`**.
- The vCPU clears/sets them in `pgraph_write` (`&= ~val` at `:97`,
  `= val` at `:108`), today under `pg->lock` **and** the dispatch BQL.
- The PFIFO thread *raises* them: `|= CONTEXT_SWITCH` at `pgraph.c:204`
  runs **under the BQL after dropping `pg->lock`**; `|= ERROR` at `:963`
  runs under `pg->lock`.

So today the BQL is doing two real jobs on these two fields: (1) it
serialises the vCPU's read-modify-write clear against the PFIFO thread's
`|= CONTEXT_SWITCH` RMW (which is *not* under `pg->lock`), and (2) it
serialises those writers against `nv2a_update_irq`'s lock-free read. Drop
the dispatch BQL without any other change and both become C11 data races
(lost interrupt-status update; UB read in `update_irq`).

The fix is not a new lock — it is to make those fields **atomic**, which is
strictly better than the status quo (the PFIFO-raise-vs-`update_irq`-read
pair is *already* an un-serialised access today, since the raise at `:963`
holds only `pg->lock` while `update_irq` holds only the BQL). Atomic RMW
(`qatomic_and_fetch`/`qatomic_or`) + atomic loads (`qatomic_read`) make
every access well-defined and lose no updates, matching the idiom already
used for `NV_PGRAPH_FIFO` (`can_fifo_access` reads it via `qatomic_read`,
`pfifo.c:116`) and the `waiting_for_*` flags. Note neither `pgraph_write`
case calls `nv2a_update_irq`, so the guest's ack/enable already only
affects the IRQ line at the *next* `update_irq` (vblank or PFIFO activity)
— that timeliness is identical with or without the BQL, so no interrupt is
lost or delayed by this change.

## D. Per-range classification

| Range | Verdict | One-line argument |
|---|---|---|
| default `regs_[]` r/w | **SAFE-LOCKLESS** | Shared only with the PFIFO thread, arbitrated by `pg->lock`; the BQL never protected `regs_` (PFB precedent). |
| `NV_PGRAPH_SURFACE` (flip handshake) | **SAFE-LOCKLESS** | `regs_` under `pg->lock`; the flip READ_3D/WRITE_3D poll runs on the PFIFO thread under `pg->lock` (`pfifo_stall_for_flip`, `:147`). |
| `NV_PGRAPH_INCREMENT` | **SAFE-LOCKLESS** | `SET_MASK(SURFACE,READ_3D)` under `pg->lock`; `pfifo_kick` under `pfifo.lock`. No BQL role. |
| `NV_PGRAPH_FIFO` write | **SAFE-LOCKLESS** | Store under `pg->lock`; the PFIFO thread already reads it via `qatomic_read` (`can_fifo_access`); kick under `pfifo.lock`. |
| `NV_PGRAPH_RDI_INDEX` | **SAFE-LOCKLESS** | Plain `regs_` under `pg->lock`. |
| `NV_PGRAPH_RDI_DATA` r/w | **SAFE-LOCKLESS** | `vsh_constants` + auto-increment of the index all under `pg->lock`; the PFIFO thread never touches RDI; single vCPU ⇒ the index/data stateful pair cannot be interleaved by a second guest access. |
| `NV_PGRAPH_CHANNEL_CTX_TRIGGER` | **SAFE-LOCKLESS** | `d->ramin_ptr` read is a plain guest-RAM load (no memory transaction, no BQL); `CTX_USER` write via `pgraph_reg_w` under `pg->lock`. |
| `NV_PGRAPH_INTR` r/w | **NEEDS-FINE-LOCK → SAFE-LOCKLESS** | The BQL serialised the vCPU RMW clear vs the PFIFO `|= CONTEXT_SWITCH` RMW and vs the lock-free `update_irq` read; restored by making `pending_interrupts` atomic (`qatomic_and_fetch`/`qatomic_or`/`qatomic_read`). |
| `NV_PGRAPH_INTR_EN` r/w | **NEEDS-FINE-LOCK → SAFE-LOCKLESS** | Same, for `enabled_interrupts` (`qatomic_set`/`qatomic_read`). |

**No range is UNSAFE.** The whole PGRAPH block can dispatch BQL-free once
the two interrupt fields are atomic — because the block flag is per-region
(all-or-nothing), that atomic change is the *precondition*, not an
optional extra.

## E. The enabling change (interrupt fields → atomic)

Converted to atomic access (all on the cold interrupt path — a handful of
ops per frame):

- `pgraph.c` `pgraph_read`: `NV_PGRAPH_INTR`/`_EN` → `qatomic_read`.
- `pgraph.c` `pgraph_write`: clear → `qatomic_and_fetch(&pending, ~val)`
  (return value drives the `waiting_for_*` clears); enable →
  `qatomic_set(&enabled, val)`.
- `pgraph.c:204` (PFIFO raise, CONTEXT_SWITCH) and `:963` (PFIFO raise,
  ERROR) → `qatomic_or`; `:952` assert → `qatomic_read`.
- `nv2a.c` `nv2a_update_irq`: the PGRAPH read → `qatomic_read` of both
  fields.

`pmc`/`ptimer`/`pcrtc` interrupt aggregation and `d->pmc.pending_interrupts`
are untouched (still BQL-only, correct — those handlers are not lockless).
GL and Vulkan renderers untouched. Windows/Linux compile the identical C
(no `__APPLE__`, no platform gate) and get the same UB-removal benefit.

## F. What ships + escape hatch + hit-rate counter

**Lockless enablement** (`nv2a.c` `nv2a_realize`): `NV_PGRAPH` joins the
`lockless_io` set alongside `NV_PFB`/`NV_USER`. Gating:

- `XEMU_MMIO_BQL=1` — master switch, restores locked dispatch for *all
  three* blocks (unchanged meaning).
- `XEMU_PGRAPH_LOCKLESS=0` — PGRAPH-specific hatch: keeps PFB/USER lockless
  but restores BQL dispatch for PGRAPH only (bisection knob for this
  campaign).

**Hit-rate counter** (`pgraph.c`, `XEMU_PGRAPH_MMIO_STATS=1`, default off,
zero-cost when unset, single-vCPU writer — same discipline as the
`XEMU_MMIO_PROF` counters): per-register `reads[]`/`writes[]` histogram
over the `0x2000` block, `atexit` dump of totals + the semantic-category
split (interrupt regs vs RDI vs everything-else) + the top offsets by
count.

**Measured** (F5 savestate, 150 s in-scene + boot, lockless dispatch
active): 4.26M reads + 1.60M writes total. Ranking:

| Offset | Register | Reads | Writes |
|---|---|---|---|
| 0x100 | INTR | 1,596,799 | 532,267 |
| 0x720 | FIFO | 4 | 1,064,539 |
| 0x14c | (unnamed; ISR set) | 532,264 | 2 |
| 0x108 | NSOURCE | 532,265 | 0 |
| 0x704 | TRAPPED_ADDR | 532,265 | 0 |
| 0x708 | TRAPPED_DATA_LOW | 532,264 | 0 |
| 0x1a88 | (unnamed; ISR set) | 532,264 | 0 |
| 0x71c | INCREMENT | 6,009 | 6,009 |
| 0x750/0x754 | RDI pair | 0 | 228 |

The audit's *prediction* that the interrupt registers would be cold is
**killed by this data**: `NV_PGRAPH_INTR` is the single hottest PGRAPH
register (37% of all reads), and the guest ISR's read constellation
(INTR + NSOURCE + TRAPPED_* + 0x14c + 0x1a88, all equal at ~532K = one
set per interrupt) plus the FIFO-access toggle pair (1.06M writes, each
holding `pfifo.lock` across its kick) dominate traffic. This changes
nothing for correctness — the §E atomics are frequency-independent, and
the ISR's plain-reg reads are `pg->lock`-safe default-case loads — but it
means the interrupt path is squarely ON the hot path: (a) the atomic
conversion is load-bearing, not belt-and-braces; (b) the BQL crossings
eliminated here fire once per ISR MMIO op, the plausible mechanism for
any pacing effect the orchestrator may (or may not) measure. RDI traffic
is negligible (228 ops) — the stateful index/data pair is a correctness
footnote, not a perf surface.

Measurement context: the stats run executed on a machine shared with
several sibling agent instances, so **only counts and count ratios are
quoted** (532K interrupt acks; ~11 PGRAPH MMIO ops per interrupt — the
ISR constellation of 3 INTR reads + 5 status reads + 1 ack + 2 FIFO
toggles; 3 INTR reads per ack). Wall-clock rates from this run are
contaminated by host load and are deliberately not claimed; the
orchestrator's quiet-window A/B owns every timing figure.

## G. Validation

All guest runs executed while up to ~5 sibling agent instances shared the
machine (an accepted condition for this campaign): evidence below is
**functional counts only** — no timing figure from these runs is quoted
or claimable, and a fence-timeout-class abort would have been treated as
an environment artifact (note + one rerun) per the campaign's ground
rules. None occurred.

- **Build**: `./build.sh` clean; zero compiler warnings in the touched
  files (`pgraph.c`, `nv2a.c`, `pfifo.c`).
- **Unit suite**: `ninja test-xbox` + `meson test --suite xbox` — 6/6 OK
  (DSP interpreter/JIT corpus + differential arms, swizzle round-trip).
- **Stats run** (lockless active, `XEMU_PGRAPH_MMIO_STATS=1`): boot →
  F5 `loadvm` → 150 s in-scene → clean quit, exit 0, zero
  asserts/errors; produced the §F histogram.
- **Soak** (lockless active, `XEMU_NV2A_NSPROF=1` +
  `XEMU_PFIFO_HEARTBEAT=1`): 12-minute window, 8 alternating `loadvm`
  cycles between the F5 (bright beach scene) and F8 (dark in-game combat
  scene) snapshots — savestate load is the adversarial case (it runs the
  full halt/flush handshake against the now-lockless dispatch). All 8
  loads succeeded; clean monitor quit; exit 0. Zero asserts, zero vk
  errors, zero fence-or-die aborts. PFIFO heartbeat healthy start to
  finish (final beat `iters=5.7M halt=0 flush=0 sync=0`, `waiting_flip`/
  `waiting_nop` toggling normally — the flip/nop handshakes the audit
  called out as the risk surface). The window showed correct gameplay
  throughout: fully-rendered scenes, animated water/fire effects, HUD
  gauges, no stuck frames (window captures below double as liveness
  proof).
- **Visual artifact oracle**: 196 window captures at ~2 s spacing across
  the soak, scored with `score_frames.py` — **zero rendering
  artifacts**. 195/196 CLEAN; the single flag (`cap_0122`, two tiny
  morton-square clusters, 32×32 + 24×16 px on a 3840×2224 frame) was
  eyeballed against its neighbor frames and is a **false positive on
  legitimate content**: the pink-white core of an animated fire effect
  in the F8 combat scene (organic gradient, moves frame-to-frame;
  context crops kept in the session scratchpad).
- **Escape hatch, both directions**: `XEMU_PGRAPH_LOCKLESS=0` (PGRAPH
  back on BQL, PFB/USER still lockless) — boot → F5 `loadvm` → 90 s →
  clean quit, exit 0, zero asserts; per-register count *profile* is
  shape-identical to the lockless run (3.0 INTR reads per ack, RDI = 228
  writes — RDI is a fixed scene-setup cost, not steady-state traffic).
  `XEMU_MMIO_BQL=1` (all three blocks locked) — boot → F8 `loadvm` →
  60 s → clean quit, exit 0, zero asserts, same ratio profile. Both
  hatch paths therefore dispatch correctly and reach the same handler
  behavior; the only difference is who holds the BQL.

## H. Re-verification

```sh
grep -n "NV_PFB\]\|NV_USER\]\|NV_PGRAPH\]" hw/xbox/nv2a/nv2a.c            # lockless set: 3 blocks
grep -n "XEMU_PGRAPH_LOCKLESS\|XEMU_MMIO_BQL" hw/xbox/nv2a/nv2a.c          # hatches
grep -n "qatomic" hw/xbox/nv2a/pgraph/pgraph.c | grep -i "interrupt"       # atomic INTR fields
grep -n "qatomic_read(&d->pgraph.pending_interrupts)" hw/xbox/nv2a/nv2a.c  # update_irq reader
grep -c "pfifo_kick(" hw/xbox/nv2a/*.c hw/xbox/nv2a/pgraph/*.c hw/xbox/nv2a/pgraph/vk/*.c hw/xbox/nv2a/pgraph/gl/*.c  # 20 calls, unchanged
grep -n "XEMU_PGRAPH_MMIO_STATS" hw/xbox/nv2a/pgraph/pgraph.c             # dark counter
```

## I. Open risks

1. **Interrupt-line timeliness** — unchanged by design: neither
   `pgraph_write` interrupt case ever called `nv2a_update_irq`, so the
   guest ack/enable has *always* taken effect only at the next
   `update_irq`. The atomic change preserves this exactly; it is not a new
   deferral. Confirm no interrupt-storm/stall in soak (§G).
2. **The `pmc` aggregation race is pre-existing and out of scope** — the
   PFIFO-raise-vs-`update_irq`-read access on `pending_interrupts` was
   already un-serialised before this audit; this change makes it *atomic*
   (a strict improvement) but does not add a lock around the whole IRQ
   path. A full IRQ-path lock review is a separate item.
3. **Windows/Linux get the same lockless dispatch** — upstream already
   honors `lockless_io` in `prepare_mmio_access` (the address-space path
   WHPX uses), so PGRAPH joins PFB/USER as BQL-free there too, exactly as
   those blocks already ship today. The safety argument is
   platform-neutral (`pg->lock`/`pfifo.lock`/`qatomic` are QEMU
   primitives; every `nv2a_update_irq` caller holds the BQL on all
   platforms). Equivalence-argued in windows-gating-audit terms;
   **needs-real-HW** soak on a native Windows box, same as PFB/USER.
4. **No perf/jitter claim here** — the orchestrator owns the savestate A/B
   and the hit-rate ranking; this document asserts correctness only.
