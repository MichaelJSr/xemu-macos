# Cross-page direct-chaining, Xbox-relaxed — soundness analysis (Gate 0)

Campaign: README roadmap item 2. Branch: `macos-optimizations`, isolated
worktree. Guest: i386 Pentium III, single vCPU, CF_PCREL always set
(`target/i386/cpu.c:9326`, `tcg_cflags_set(cs, CF_PCREL)`).

Status of this doc: **Gate 0 — written BEFORE implementation.** Refuter-first
is mandatory; every claim below is either code-cited or flagged as a
hypothesis the Gate-1 refuter must confirm.

The one-sentence goal: cross-page **direct** relative jumps currently pay the
~20-op inline jump-cache probe (they fall out of `gen_jmp_rel`'s same-page
`goto_tb` gate). If the Xbox's mapping model lets us patch a direct host
branch for them instead, we save the probe on the "other" slice of the exit
census. Worth +1–2 fps *iff* the taken rate clears the kill threshold below.

---

## 1. The two correctness hazards, adjudicated against code

### Hazard (a): SMC / invalidation of the TARGET page — ALREADY SOUND

**Claim (task):** QEMU's `tb_phys_invalidate` unlinks incoming direct jumps
via `jmp_list` regardless of which page the jumper lives on, so plain
invalidation already severs cross-page chains.

**VERIFIED in code.** The unlink is a pure TB-graph walk with **no page
dependency**:

- `do_tb_phys_invalidate` (`accel/tcg/tb-maint.c:1047`) calls, for the TB
  being invalidated:
  - `tb_remove_from_jmp_list(tb, 0/1)` (`tb-maint.c:1081-1082`) — removes
    *this* TB from the jump-lists of its own targets.
  - `tb_jmp_unlink(tb)` (`tb-maint.c:1085`) — walks
    `TB_FOR_EACH_JMP(dest, tb, n)` over every TB that jumps *into* this one
    and calls `tb_reset_jump(tb, n)` (`tb-maint.c:1003-1018`), resetting the
    patched host branch back to `tb->tc.ptr + tb->jmp_reset_offset[n]` (the
    epilogue). The iteration key is `jmp_list_head`/`jmp_list_next` — a
    linked list of `(TB, slot)` pairs, entirely page-agnostic.
- The Xbox guest-data-write → code-page path (`XEMU_INV_SRC_NOTDIRTY`) routes
  through this same `do_tb_phys_invalidate`, as does every other
  invalidation source.

**Conclusion:** when the target page is written (SMC) or otherwise
invalidated, *all* incoming direct chains are severed — cross-page ones
exactly like same-page ones. Hazard (a) needs **no new code**. The ban is
therefore purely about hazard (b).

### Hazard (b): pure VIRTUAL REMAP without invalidation — THE real hazard

A patched cross-page chain `P_s → T_t` binds "leaving `P_s` toward vaddr
`V_t`" to "jump to `T_t`", where `T_t` is code translated for the *physical*
page that `V_t` mapped to at translate time. If the guest later remaps `V_t`
to a **different physical page** *without* writing the old physical page
(i.e. edits a PTE, then `INVLPG`/`MOV CR3`), then:

- The old physical page is untouched → `T_t` is **not** invalidated → hazard
  (a)'s unlink never fires.
- `V_t` now resolves to different code, but the stale chain still jumps into
  `T_t` = code for the *old* mapping. **Silent wrong code.** This is the
  worst failure class (a rare wrong-code hang).

**Upstream's exact rationale, cited.** `accel/tcg/cpu-exec.c:1451-1461`:

```c
#ifndef CONFIG_USER_ONLY
    /*
     * We don't take care of direct jumps when address mapping
     * changes in system emulation.  So it's not safe to make a
     * direct jump to a TB spanning two pages because the mapping
     * for the second page can change.
     */
    if (tb_page_addr1(tb) != -1) {
        last_tb = NULL;   /* refuse to chain into a page-spanning target */
    }
#endif
```

and the translate-time gate `translator_use_goto_tb`
(`accel/tcg/translator.c:112-121`) which only returns true for
`translator_is_same_page`. The load-bearing sentence is *"We don't take care
of direct jumps when address mapping changes in system emulation."*

**Why same-page is safe (the precise argument).** A same-page `goto_tb` keeps
execution inside one physical page. The subgraph of TBs reachable purely by
`goto_tb` chains from a lookup-entry is entered *only* via `tb_lookup`
(`cpu-exec.c:1432`), which recomputes `phys_pc = get_page_addr_code(...)`
against the *live* mapping. Every edge inside a same-page subgraph targets the
same physical page as the entry, so validating the entry validates them all.
Crucially, to change that page's mapping the guest must `INVLPG`/`MOV CR3` —
and both **end the TB with a full exit** (`DISAS_EOB_NEXT`, not a chain;
`translate.c:4266`/`4359-4360` INVLPG, `4349-4350` `write_crN` for CR3),
returning to the dispatch loop, which re-looks-up. So a same-page chain can
never carry execution across the very remap that would invalidate it.

**Why cross-page breaks the argument.** A cross-page subgraph spans pages
`{V_s, V_t, ...}`. The consistency argument above only covers the *entry*
page. A stale `P_s → T_t` fires whenever execution reaches `P_s` (via any
still-valid path) and takes the patched branch — **without any fetch of
`V_t`** (the raw host jump bypasses instruction fetch and thus bypasses the
TLB). So a `tlb_set_page` observer keyed on code-fetch refills has a **bypass
hole**: the wrong execution never triggers a refill of `V_t`. This is why the
refuter (Gate 1) must probe the **page tables the slow way**
(`cpu_get_phys_page_debug`, a walk — authoritative regardless of TLB/chain
state), from inside the *source* TB body, on *every* cross-page transfer.

### Also handled for free

- **TBs spanning pages** (`tb_page_addr1 != -1`): the runtime guard at
  `cpu-exec.c:1458` already refuses to chain into a page-spanning target,
  *independently* of our translate-time relaxation. The second page could
  remap independently; upstream already declines it and so do we. No action.
- **`tb_flush`**: nukes the whole code cache (`do_tb_flush`), so every chain
  dies. Trivially safe; we also reset our observation structures on a
  `tb_flush_count` change so cross-epoch comparisons never false-positive.
- **`MOV CR3` / page-table base switch**: on the Xbox this is *rare* — XDK
  titles are single-process; kernel + title share **one address space / one
  page directory** (see §2). CR3 reload ends the TB with a full exit anyway.

---

## 2. Xbox memory model — the soundness substrate

Sourced from `xbox-hardware-reference` (§1 UMA, fixed machine) plus the Xbox
kernel's public memory-map model; the emulator does not dictate the map (the
guest kernel from flash sets up the page tables), so these are guest-OS facts
the refuter must confirm empirically, not code constants.

- **UMA, one address space.** RAM *is* VRAM (`hw/xbox/nv2a/nv2a.c`, "xbox is
  UMA"). 64 MiB retail / 128 MiB (`config_spec.yml:381`). `max_cpus = 1`
  (`hw/xbox/xbox.c:452`) — single vCPU, so all counters can be plain
  unsynchronized `uint64` (the fork's established model).
- **Kernel-identity window `0x80000000+`.** The kernel maps physical RAM
  `0x0..RAM` at virtual `0x80000000..` as a **fixed identity alias**, set up
  at boot and never remapped for a running title.
  `MmAllocateContiguousMemory` hands back addresses *inside* this window;
  allocation reserves ranges but the vaddr→paddr binding itself is invariant
  (`0x80000000+X` always → phys `X`). Kernel code executes here
  (kernel image ~`0x80010000`). **Provably static ⇒ safe cross-page target.**
- **Low region `0x00010000..` (title/XBE + heap).** The XBE is loaded at its
  base (commonly `0x00010000`) into committed memory, mapped once at load.
  Xbox has **no demand paging of XBE code** — code mappings are static after
  load. But this region *also* holds `VirtualAlloc`-style heap and the rare
  `XLoadSection`/`XFreeSection` streamed code sections, which *can* change a
  vaddr's backing physical page. So vaddr alone does **not** prove
  static-ness in the low region.
- **Residual remap vectors (the ones the refuter hunts):** `XLoadSection`
  section streaming (title-specific, rare), title-embedded JIT/scripting VMs
  (that is SMC — a write — so hazard (a) covers it), and any bank-switch of a
  fixed vaddr between two resident physical pages without rewriting either
  (exotic; the one true escape from hazard (a)).

---

## 3. Candidate rules and the chosen rule

- **R1 (region-restricted).** Allow cross-page chaining only when the TARGET
  linear address is in the kernel-identity window (`dest >= 0x80000000`).
  Provably static ⇒ **no backstop needed**. Captures kernel cross-page
  directs only; the low-region title→title cross-page directs (likely the
  majority of guest execution) are excluded, so the fps benefit is bounded by
  the kernel share of cross-page exits — measured by the Gate-0 counter's
  region breakdown.
- **R2 (remap-epoch/observation hook).** Allow broad chaining; catch remaps
  with a hook. NB: a `tlb_set_page` observer has the **bypass hole** proven
  in §1 (stale chains skip fetch). The sound observation point is the guest's
  own **TLB-flush** (`INVLPG`/`CR3`) — the mandatory x86 step that makes a
  remap take effect, and which *precedes* any use of the new mapping, so it
  cannot be bypassed by a stale chain.
- **R3 (hybrid).** R1 as the sound-by-construction default; broad + guest-
  flush backstop as an experimental tier; refuter as the acceptance gate.

**Chosen: R3.**

- **`XEMU_XPAGE_CHAIN=1` (default tier): R1-kernel.** Cross-page `goto_tb`
  only when `dest >= 0x80000000`. Sound with zero new invalidation code
  (hazard a via `jmp_list`; hazard b absent because the kernel window never
  remaps). This is the dark-ship candidate.
- **`XEMU_XPAGE_CHAIN=2` (broad tier): all cross-page targets + guest-flush
  backstop.** On guest `INVLPG(V)` of a page that currently backs code,
  invalidate that physical page's TBs (`tb_invalidate_phys_range`) — severs
  every stale chain into it *before* the remapped mapping is used. On `MOV
  CR3`, schedule `tb_flush`. Both hook the existing TB-ending INVLPG/CR3
  sites. Sound for arbitrary titles; cost = code-page-flush frequency, which
  the refuter/counter measure. Recommended only after a clean refuter soak.

  **Backstop v2 (v0.11.1) — link-registry unlink replaces the full flush.**
  The queued-`tb_flush` form above (plus the generic `tlb_flush_by_mmuidx`
  hook added at promotion) was correct but priced wrong: full-TLB-flush
  classes are NOT rare — Azurik's level streaming reloads CR3 / fires
  generic flushes continuously while entering new areas, and every one cost
  a whole-translation-cache flush (measured 139 tb_flushes in 20 s of
  first-visit walking on F8; fps trough 14.6 vs 38.6 settled; 0 flushes
  walking back through explored space — the shipped v0.11 "new-area lag").
  v2 records every cross-page link **destination** at `tb_add_jump` time in
  a 128k-entry registry in `tb-maint.c` (generation-checked against
  `tb_flush_count` so entries never dangle across a real flush). Two bits
  in `tb->xemu_xpage_reg` (a spare byte in the pre-`ihash` hole) drive it:
  `CROSS_EMITTER`, set by `gen_jmp_rel` on any TB that emits a relaxed
  cross-page `goto_tb` slot — every dest such a TB links is registered
  (this is the authoritative trigger: under CF_PCREL, and with the Xbox's
  multiple virtual aliases of RAM, a cross-virtual target can share the
  source's phys page, so a phys-only comparison is NOT sufficient) — and
  `REGISTERED`, the dest-side dedup bit. A phys-page mismatch with no
  emitter flag also registers (conservative belt-and-braces; counted as
  `reg-phys-only` in the dump because its observed size — thousands per
  soak even in refute mode, i.e. with zero relaxed slots emitted — was not
  predicted; severing a valid link is always safe, so this class is
  over-approximation, not hazard). The backstop severs the registered
  destinations' incoming chains with `tb_jmp_unlink()` — synchronously,
  closing the old queue-to-safe-point window, under the same manual
  `qemu_thread_jit_write()/execute()` bracket `tb_phys_invalidate` uses
  (the first build without it wedged the vCPU in a fault loop at the first
  boot backstop) — and translations survive; chains relink lazily. Lock
  order: registry spinlock → `jmp_lock` (registration happens after
  `tb_add_jump` drops `jmp_lock`, so the order never cycles). Overflow or
  `XEMU_XPAGE_UNLINK=0` falls back to the v0.11 queued full flush, so
  correctness never depends on registry capacity. Armed at every chain
  level ≥ 1 (level-1 kernel chains get severed too — strictly safer than
  the level-2-only v0.11 gate, and free when nothing is registered).

Rationale for R1-as-default over broad-as-default: R1 needs no hot-path hook
and no argument beyond "the kernel identity window is invariant," so it cannot
introduce the wrong-code-hang class even in an untested title. Broad is gated
behind refuter evidence.

---

## 4. Gate-0 counter (the denominator) + pre-registered kill threshold

Extends the `XEMU_INV_PROF` exit-census style (`accel/tcg/xemu-inv-prof.h`,
counters defined in `tb-maint.c`). New counters live in the campaign's own
`accel/tcg/xemu-xpage.c` (kept out of the sibling-touched files), armed by
`XEMU_XPAGE_PROF=1` **or** `XEMU_INV_PROF=1`, dumped at `atexit`:

- `xpage_taken` — runtime **taken** cross-page direct exits (inline gated
  increment emitted in `gen_jmp_rel` at the cross-page-direct site; runs
  whether or not chaining is on, so it is the true denominator).
- `xpage_taken_kernel` / `xpage_taken_low` — split by target region
  (`dest >= 0x80000000` vs below): the R1-vs-broad opportunity split.
- `xpage_emitted` — translate-time count of cross-page-direct exit sites.
- Elapsed wall time since arm → derived **exits per 5 s**.

**Kill-threshold arithmetic (pre-registered).** The saving per converted exit
is the inline jc-probe (~20 TCG ops ≈ ~12–18 host instructions + one
`tb_jmp_cache` load, ~L1/L2) minus a patched direct branch (~1 predicted
branch ≈ 0). Call it **~5 ns/exit** saved on M2 (conservative; the probe is
mostly a dependent load + compares). A live scene at ~45 fps has a 22.2 ms
frame; +1 fps (45→46) needs ~0.48 ms/frame ≈ **107 ms saved per 5 s**, i.e.
`107e6 / 5 ≈ 21.5M` converted exits per 5 s; +2 fps ≈ **43M / 5 s**.

- **Kill threshold X = 10M taken cross-page-direct exits per 5 s** (≈2M/s).
  Below X, predicted gain < ~0.5 fps — under the live-scene noise floor
  (`xemu-validation-and-qa`: ±5 fps live, sub-0.5% = noise-suspect). If the
  counter reads < X on F5/F8, **the +1–2 fps claim is dead** and this ships
  (if at all) only as a curiosity, not a perf win.
- For the R1-kernel default specifically, the relevant number is
  `xpage_taken_kernel`; if kernel exits are < X, R1-kernel is sub-noise and
  only broad (=2) could pay — which then rests on the refuter clearing broad.

---

## 5. Gate-1 refuter (`XEMU_XPAGE_REFUTE=1`)

The falsifier for hazard (b). Emitted at **every** cross-page-direct exit
site (in `gen_jmp_rel`, forced onto the safe non-chaining path so execution is
always correct via the normal lookup): a helper
`helper_xemu_xpage_refute(env, cs_base)` that, each time the exit is taken:

1. Computes the target linear addr `= cs_base + (uint32_t)env->eip`.
2. Resolves its **current** physical page via `cpu_get_phys_page_debug` — a
   page-table **walk**, authoritative regardless of TLB or chain state (this
   is what closes the bypass hole in §1).
3. Looks up a direct-mapped shadow `target-vpage → last-seen ppage`. If seen
   before and the ppage **differs**, a real chain to this target *would have*
   executed stale code → **VIOLATION**: increments the violation counter and
   logs loudly (`vaddr / old-phys / new-phys`). Otherwise records/updates.
4. Increments the total-checks counter. Resets the shadow on a
   `tb_ctx.tb_flush_count` change (cross-epoch comparisons excluded).

Also wired: a `tlb_set_page` **observer** (logs exec-fetch remaps) as a
secondary, over-approximating cross-check, and the refuter naturally covers
loadvm/level-transition churn because it runs on *every* cross-page transfer
after the reload, not at a single hook. Acceptance (Gate 3): **0 violations
over N-million checks**, across multiple `loadvm` cycles in one process.

This mode is intentionally slow (a page-table walk per cross-page exit); it
exists to try to prove the whole idea wrong, not to run fast.

---

## 6. Gate-2 implementation (dark, zero-cost off)

- **`translator_use_goto_tb`** (`translator.c`): after the same-page check,
  under `#if defined(XBOX)` only, consult `xemu_xpage_allow(db, dest)`
  (chain-on && rule); non-XBOX and knob-off are byte-identical to upstream.
- **`gen_jmp_rel`** (`translate.c`): the CF_PCREL block keeps its wrap-safe
  `andi` mask unconditionally (correctness), but only force-disables
  `goto_tb` for cross-page when `xemu_xpage_allow` says no. Also emits the
  Gate-0 counter and Gate-1 refuter at the cross-page-direct site.
- **Backstop (broad tier only)** at the INVLPG/CR3 sites (see §3).
- **Unlink** reuses `jmp_list` unchanged (hazard a). The page-spanning-target
  refusal at `cpu-exec.c:1458` is untouched and still applies.

Class-5 gating: every change is `#if defined(XBOX)` or knob-latched; Windows/
Linux and non-i386 targets keep upstream semantics and keep compiling. Diff is
kept out of the sibling-modified files (cc-op census WIP present in the shared
checkout) except `translate.c`, where the edit sits in `gen_jmp_rel`, far from
the sibling's `cc_op`/`tb_stop` hunks.

---

## 7. Open risks (explicit)

- **Broad tier (=2), untested titles.** The guest-flush backstop assumes every
  remap of an executing vaddr is preceded by a guest `INVLPG`/`CR3` (x86-
  mandatory). A title that remaps a code vaddr and jumps to it *without*
  flushing the TLB would be relying on undefined x86 behavior AND would be
  running the old mapping under QEMU anyway (QEMU only changes the mapping on
  the flush) — so no hazard. The one residual: a fixed vaddr bank-switched
  between two *resident* physical pages, flushed each time — the backstop
  handles it (flush → invalidate), but at a re-translation cost that could
  make broad a net loss if frequent. Measured by the counter.
- **R1-kernel default** carries no such residual: the kernel identity window
  is invariant by construction.
- **cross-page targets that span pages** are already declined by
  `cpu-exec.c:1458` — no coverage gap, but they simply won't be chained.
- **Sub-page dirty-tracking sibling / cc-op census WIP.** Diff scoped to
  rebase cleanly; the orchestrator lands subpage first and re-runs this
  refuter on the combination.

---

## 8. Results (Gates 1-3, F8/F5/F7, 2026-07-11)

Measured on the dev M2 Ultra under multi-instance contention (counts stay
valid; per-5s rates UNDERCOUNT the true steady rate). Build green; xbox unit
suite 6/6 (`ninja test-xbox` + `meson test --suite xbox`).

**Gate-0 denominator (F8, prof-only).** 183.8M taken cross-page-direct exits
in 44.5 s ≈ **20.7M/5s — above the pre-registered kill threshold X=10M/5s.**
But the region split is decisive: **kernel = 2.4M (1.3%), low = 181.4M
(98.7%)**; kernel-only ≈ 0.27M/5s (sub-noise). INV_PROF exit census confirmed
scene identity (goto_tb 71.9% / jc 16.0% / ret 12.1%, matching the campaign's
72/16/12).

**Consequence for the chosen rule.** R1-kernel (=1) is sound by construction
but captures <1.5% of cross-page directs → its fps ceiling is ~0.01 fps
(dead). **The perf lever is the BROAD tier (=2)**, contingent on the refuter.

**Gate-1 refuter (F8, 90 s, one loadvm).** 394,502,397 page-table-walked
checks, **0 VIOLATIONS.** No cross-page-direct target's physical page ever
changed. The over-approximate `tlb_set_page` observer *did* see 3468 exec-page
remaps (vaddr 0xbe000 ↔ two phys pages) — so exec-page remaps DO occur on
Xbox, but never on a chain target (else the precise refuter flags it). The
precise/over-approximate split behaves exactly as designed, and the backstop
is a **real, not dormant, safety net.**

**Gate-3 soak (F8→F5→F7, 12 in-process loadvm cycles, 705.9 s ≈ 11.8 min,
`XEMU_XPAGE_CHAIN=2 XEMU_XPAGE_REFUTE=1`).**
**2,684,961,145 checks, 0 VIOLATIONS, 0 aborts/crashes/hangs, 12/12 cycles.**
The observer logged **61,375 exec-page remaps across 5 distinct vpages** —
level transitions churn executable mappings heavily — and **still zero chain
targets remapped.** tb_flush=137 (loadvm + backstop CR3 drains). Both raw
chaining tiers (=1 and =2) separately ran clean on F8 (503.9M cross-page
exits under broad, no fault).

**Verdict.** Hazard (b) is empirically ABSENT for the tested corpus: chain
targets are phys-stable across billions of transfers and a dozen level
transitions, even though the guest remaps *other* exec pages tens of thousands
of times. Broad cross-page chaining is safe on F8/F5/F7 and worth benching for
the predicted +1-2 fps.

**Residual risk (must be stated).** The refuter proves stability only for the
targets exercised on F8/F5/F7. The broad backstop covers the two remap vectors
a running title actually uses — per-page `INVLPG` (the steady-state vector,
which is what the 61k observed remaps go through) and `CR3` — but NOT a
`CR4.PGE` toggle or other whole-TLB flush that isn't `INVLPG`/`CR3` (boot-era /
rare). For those, broad rests on the empirical refuter, not on construction.
Before any default-on: (a) run the refuter across the full title corpus, and
(b) consider extending the backstop to the generic `tlb_flush` path (cheap —
full flushes are rare) to make broad sound by construction. Until then broad
ships DARK and the orchestrator A/Bs `XEMU_XPAGE_CHAIN=2`.

Repro:
`XEMU_XPAGE_PROF=1` (denominator), `XEMU_XPAGE_REFUTE=1` (falsify),
`XEMU_XPAGE_CHAIN=2 XEMU_XPAGE_REFUTE=1` (soak) — via the bench savestate
harness, one loadvm per boot, multiple cycles for transition churn.
