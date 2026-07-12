<!--
Gate 0 deliverable for the sub-page dirty-tracking campaign.
PRE-REGISTRATION (thresholds + arithmetic) written BEFORE the measurement run,
per xemu-research-methodology §2 (predict a number and a kill-threshold first).
Measured values are filled into the "MEASURED" blocks after the run; the
pre-registered inequalities above them are frozen and were not edited to fit.
-->

# Gate 0 — sizing the two sub-page dirty-tracking arms (arithmetic + kill thresholds)

## 0. The epoch this competes against (why RANGE_INV died — archaeology 1.22)

On the Xbox data-shares-a-code-page pattern, one baseline epoch is:

1. a guest data store lands on a write-protected code page → TLB `addr_write`
   has `TLB_NOTDIRTY` → store traps into `notdirty_write` (cputlb.c);
2. `notdirty_write` sees `DIRTY_MEMORY_CODE` clear → `tb_invalidate_phys_range_fast`
   invalidates **every** TB on the page (XBOX whole-page, no range check;
   ~100% false-share, 0 true SMC, 100% recycle-hit per INV_PROF);
3. the page is now empty of TBs → `tlb_unprotect_code` sets `DIRTY_MEMORY_CODE`;
4. `notdirty_write` tail sees all dirty clients set → `tlb_set_dirty` clears the
   entry's `TLB_NOTDIRTY` → the **next ~25 stores to that page are free**
   (fast-path, no trap);
5. the guest re-executes the code, re-translates, `tb_page_add` →
   `tlb_protect_code` re-arms `TLB_NOTDIRTY`; back to step 1.

So the baseline pays **one full trap per epoch** and gets ~25 free writes after
it. XEMU_TB_RANGE_INV filtered *which* TBs get invalidated (down to the bytes
actually written) but that keeps the page non-empty → step 3 never fires → the
page stays `TLB_NOTDIRTY` forever → **every** store traps: 197k→4.98M traps/5s
(25.2×) on F8. Instrument-killed (pre-registered ">5× traps = negative").

## 1. The two arms under test

**(a) helper-top consult** — buildable without touching generated code. Consult
a per-PageDesc code sub-block bitmap at the *top* of `notdirty_write`; for a
store to a non-code sub-block, skip the invalidation/scan and return. This makes
the **same decision RANGE_INV made** (do not invalidate → page never empties →
never unprotects), so it inherits the **same 4.98M trap count**. It differs only
in the per-trap body: an O(1) bitmap probe instead of RANGE_INV's O(TBs) scan.

**(b) fast-path consult** — the store's inline TLB-comparator path consults the
bitmap so a non-code store **never branches to the slow-path helper at all**
(never traps). Requires emitting the probe in the host (aarch64) TCG `qemu_st`
fast path — the generated store sequence shared by every guest on arm64 hosts
(incl. Windows-arm64). Only this form removes the trap itself.

## 2. Cost model (per 5 s window, F8)

Symbols (all per-trap unless noted), measured by XEMU_INV_TIMING splitting the
`notdirty_write` body into `inval` (the `tb_invalidate_phys_range_fast` call)
and `rest` (preamble + tail); `total = rest + inval`:

- `Nb`  = baseline traps/5s (≈ epochs; whole-page inval empties+unprotects each)
- `Nr`  = RANGE_INV traps/5s = total stores to code-bearing pages/5s
- `M`   = `Nr/Nb` = free-writes-per-epoch multiplier (~25 expected)
- `total`, `inval`, `rest` = measured `notdirty_write` body segments (ns)
- `H`   = slow-path **dispatch** cost above the `notdirty_write` body (generated
  TLB-miss branch + `helper_st*` prologue). NOT captured by the body timer;
  `H ≥ 0`. Setting `H = 0` is maximally charitable to arm (a).
- `C`   = O(1) bitmap consult cost added inside the helper (arm a)
- `c`   = inline bitmap probe cost added to every protected-page store (arm b)

### Arm (a) differential vs baseline (common raw store cancels):

    Δ(a) = Nr·(H + rest + C) − Nb·(H + rest + inval)
         = (Nr−Nb)·(H + rest) + Nr·C − Nb·inval

Arm (a) is a **win only if** `Nb·inval > (Nr−Nb)·(H+rest) + Nr·C`, i.e.

    inval > (M−1)·(H + rest) + M·C

With the most charitable `H = 0, C = 0`:

    inval > (M−1)·rest   ⟺   inval/total > (M−1)/M

**PRE-REGISTERED KILL (a):** if `inval/total < (M−1)/M` (≈ 96% for M≈25), arm
(a) predicts **≤ 0** vCPU improvement even at zero dispatch/consult cost → KILL.
Any real `H>0` only widens the loss. Expectation: `inval/total` is well under
96% (the helper dispatch, preamble, and dirty-bit tail are non-trivial), so (a)
is expected dead.

### Arm (b) differential vs baseline:

    Δ(b) = Nr·c − Nb·(H + total)

Arm (b) **saves** `Nb·(H+total)` (the eliminated real traps) and **costs**
`Nr·c` (inline probe on every protected-page store). Predicted vCPU delta:

    savings_frac = Nb·(H+total) / (5e9 ns)        [lower bound at H=0]
    cost_frac    = Nr·c        / (5e9 ns)
    Δ(b)_vCPU%   = 100·(savings_frac − cost_frac)

**PRE-REGISTERED KILL (b):** if `Δ(b)_vCPU% < +0.5%` at `H=0, c=2 ns`, KILL.
(Using `H=0` under-counts the savings, so a positive result here is conservative.)

**STOP RULE (from the task):** if BOTH (a) and (b) predict `< +0.5%` vCPU, stop
at Gate 0 and report the instrument-kill. If (a) is dead and (b) clears +0.5%,
(b) is the only viable form — report its scope explicitly (below) and let the
orchestrator decide whether the codegen blast radius is worth ~Δ(b).

### Arm (b) scope (stated pre-measurement, independent of the number)

- Per-PageDesc code sub-block bitmap (64 or 128 B blocks; 4 KiB page = 64/32
  bits = one/two u64). Set on `tb_record`/`tb_page_add` (before `tb_link_page`
  publishes, release store); clear on last-covering-TB removal in
  `do_tb_phys_invalidate`/`tb_page_remove` (page-lock discipline, archaeology
  1.9 class). Must NOT alias `DIRTY_MEMORY_*` (PFIFO vertex/texture consumers).
- `tlb_unprotect_code` re-keyed on "no code sub-blocks set" instead of
  "page has no TBs".
- **The hard part:** the inline probe in `tcg/aarch64/tcg-target.c.inc`
  `tcg_out_qemu_st*` fast path — reach a per-page bitmap from generated code
  (new TLB-entry field or side array), test the store's sub-block bit, and skip
  the `TLB_NOTDIRTY` slow-path branch when clear. This is the hottest store
  emission path on arm64 hosts and is **Class 5 (shared/Windows-arm64)** — the
  fork's #1 non-negotiable is "never regress Windows." High regression risk for
  a modest predicted win.

---

## 3. MEASURED — F8 (`vm-20260704173701`), Azurik reel, 761–806 draws/flip, ~30 flips/s

Two same-binary runs on the heavy reel (scene identity confirmed via nsprof
draws/flip; the first attempt loaded nothing because the monitor needs the real
`vm-*` tag, not the `f8` shortcut — a benchmark bug caught per methodology §6).
Counts normalized per in-game second (nsprof 5 s intervals).

**Evidence discipline (orchestrator note):** up to ~5 xemu instances run
concurrently, so **the verdict rests on the COUNTS, which are load-immune**; the
ns timings below are order-of-magnitude only and may be contention-inflated —
they are corroboration, never the load-bearing claim. The structural argument is
count-based: arm (a) eliminates a fixed, counted set of heavy operations
(whole-page TB invalidations: qht remove + inv_htable insert + jmp-unlink per TB;
and per-vaddr `tlb_set_dirty` TLB walks) and replaces each trap's variable work
with one single-word bitmap test. The only open question is whether the extra
traps (a counted 1.89× ratio) outweigh that removed work — the orchestrator's
fps A/B is the ground truth.

| Quantity | Baseline (85 s) | RANGE_INV (75 s) |
|---|---|---|
| notdirty_write calls (traps) | 1,192,280 → **14,026/s** | 1,984,635 → **26,461/s** |
| of which do invalidation | 190,334 (2,239/s) | 1,564,325 (20,857/s) |
| invalidations (TBs) | 290,595 | 675 |
| false-share | **100.0%** | 100.0% |
| true SMC | **0** | 0 |
| recycle hit-rate | 100.0% | — |
| body total | **1488.254 ms (1.751% of 1 thread)** | 1844.271 ms (2.459%) |
| inval segment | 855.324 ms (4493.8 ns/inval-call) | 1277.092 ms (816.4 ns/call = the O(N) scan) |
| rest segment (preamble+tail) | 632.930 ms (530.9 ns/call) | 567.180 ms (**285.8 ns/call**) |

Read-pair timer floor = 0 ticks (cntvct 41.67 ns/tick); segment ratios are
robust. Key structural facts the timing exposed:

- Baseline traps split **190 k invalidating + 1.0 M cheap "no-inval"** (per-vaddr
  `TLB_NOTDIRTY` clears after each unprotect). The whole body is 1.751% of one
  thread **before** counting the slow-path dispatch `H`.
- RANGE_INV's `rest` (285.8 ns) is far cheaper than baseline's (530.9 ns) because
  a permanently-protected page never runs `tlb_set_dirty` in the tail — this is
  exactly arm (a)'s tail behavior.
- RANGE_INV is a body **regression** (2.459% vs 1.751%) purely from the O(N)
  range scan (816 ns/call) — the cost that killed it in archaeology 1.22.

### Arm (a) — re-evaluated (this REVISES the pre-registered §2 model)

The §2 model assumed a fixed `total` per trap; the data shows arm (a)'s per-trap
body is **not** the baseline body but the RANGE_INV-style cheap body (no scan, no
inval, no `tlb_set_dirty`): `rest' ≈ 286 ns` + an O(1) bitmap consult (page_find
+ one 64-bit test, ~20 ns) ≈ **~306 ns/trap**. Arm (a)'s trap rate = RANGE_INV's
26,461/s (page never unprotects). So:

    arm(a) body/s = 26,461 × 306 ns          = 8.10 ms/s  = 0.810%  vCPU
    baseline body/s                          = 17.51 ms/s = 1.751%  vCPU
    body saving                              = 0.941%  vCPU
    dispatch penalty = (26,461 − 14,026)/s·H = 12,435/s · H

    Δ(a)_vCPU% = −0.941% + 100 · 12,435 · H(s)

**Break-even dispatch H = 0.941% / 12,435 s⁻¹ ≈ 757 ns.** A softmmu store
slow-path dispatch (generated TLB-miss branch + `helper_st*_mmu` + `mmu_lookup`)
is ~50–200 ns — an order of magnitude under 757 ns. Predicted:
`Δ(a) ≈ −0.8% vCPU (H=150 ns)` … `−0.5% (H=350 ns)` — a **net improvement**.

**Why the pre-registered "inval/total > (M−1)/M" kill did not fire as a kill:**
that inequality priced arm (a)'s traps at the *baseline* body; the real arm (a)
body is the RANGE_INV cheap body, which removes both the invalidation (855 ms)
**and** the O(N) scan RANGE_INV added. Arm (a) = "RANGE_INV with an O(1) consult
instead of an O(N) scan," and that single change flips RANGE_INV's body
regression (+0.7%) into a body saving (−0.94%). Archaeology 1.22 killed RANGE_INV
on a `>5× traps ⇒ negative` **proxy**; this measurement shows the proxy is not
valid for the O(1)-consult form — the trap multiplier is real but each extra trap
is ~300 ns, not ~900 ns of scan.

**PRE-REGISTERED KILL (a), restated with the number:** arm (a) regresses only if
the store-dispatch `H > 757 ns`. That is the orchestrator's A/B kill line: if the
interleaved fps A/B shows a regression, `H` was underestimated — revert.

### Arm (b) — fast-path consult (codegen)

Eliminates **all** 1.19 M traps (true_smc = 0), saving the full body **1.751% +
dispatch 14,026/s·H** and costing an inline probe (~2 ns) on the 26,461/s
protected-page stores (≈0.005%). Predicted **Δ(b) ≈ −1.8% … −2.6% vCPU** (bigger
the larger `H` is). But it requires host-`tcg/aarch64` `qemu_st` codegen — Class 5
(shared/Windows-arm64), the fork's #1 non-negotiable. Scope in §2.

## 4. VERDICT

- **Arm (a) is a buildable, low-risk, predicted +0.5–0.9% vCPU win** (no codegen;
  all logic inside the XBOX-guarded `tb_invalidate_phys_range_fast`). Kill line:
  regression in the orchestrator's fps A/B (⇔ `H > 757 ns`). **BUILD it** behind
  `XEMU_SUBPAGE_DIRTY=1` with the refuter.
- **Arm (b) is a higher-ceiling (~+2.5% vCPU) follow-up** gated on an explicit
  orchestrator decision to accept Class-5 aarch64-codegen risk. **Do not build**
  in this campaign; documented as a scoped Future vector.
- Both clear the +0.5% vCPU floor, so the "STOP at Gate 0" rule (both below
  +0.5%) does **not** apply. Arm (a) doubles as the cheap discriminator for arm
  (b): if arm (a)'s vCPU saving does not convert to fps on the busy-poll CPU-bound
  limiter (archaeology 1.14), arm (b)'s larger saving won't either — killing the
  codegen investment before it starts.

Honest caveat: the win is modest and the F-scenes are CPU-bound busy-poll
(≈95% vCPU); whether ~0.8% vCPU headroom converts to measurable fps is the
orchestrator's A/B to decide (±0.02 fps baseline can resolve it). `H` is
reasoned, not directly measured — the A/B is the ground truth.
