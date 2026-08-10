# xemu-macos performance & feature roadmap

The open candidates: work not yet attempted, or measured and parked
behind an explicit bar. Every entry carries its decision inputs — what
is known, the measurement that would confirm or kill it, and the bar it
must clear. Landed and settled work lives in the
[optimization ledger](optimizations.md); nothing here is planned work
until it graduates into a design doc or a landing.

**State as of 2026-08-04 (post-wave): the F8 heavy anchor now reads
~48.1 fps.** The fork-wide optimization wave landed and measured
**38.12 ± 0.59 → ~48.1 fps (+10.0 mean, +26% fps, +20.4% draws/s), 5
clean interleaved cross-binary pairs, unanimous**; the biggest single
contributors were the `can_do_io` elision (+6.53, 3/3) and the x87
exception-pointer deferral (+7.46, 3/3), sub-additive in combination.
Two attribution notes that matter for anything ranked below:

- **The prewave anchor was 38.1, not 46-48.** It lands inside the 36-39
  band the 2026-07-19 header and the audit were written against, so the
  fixture did not drift — the wave moved the anchor. Every fps figure in
  the items below that predates 2026-08-04 was sized against ~38 fps and
  is now a smaller *share* of the frame.
- **Re-profile before re-ranking.** The wave removed the top-25 vCPU
  symbol (`cpu_io_recompile` and its `tcg_tb_lookup` cluster) and a
  large slice of x87 store traffic, so the shape of the remaining vCPU
  time is unknown at the new equilibrium. Re-run `XEMU_GUEST_PROF`,
  `XEMU_INV_PROF` and `XEMU_TB_PROF` at ~48 fps before trusting any
  ranking here — including this page's own.

Landed receipts and every honest kill from the wave are in the
[ledger](optimizations.md); the candidate list it worked from is
[`fork-optimization-audit-2026-08.md`](fork-optimization-audit-2026-08.md).

**Same-day follow-up (2026-08-04, post-upstream-merge): the audit's
remaining candidates are now all measured-dead.** Four idle-gated
campaigns closed the queue — surface-CB reuse pool **−0.66 fps 3/3**,
`XEMU_X87_ELIDE_CLEAN` **−0.23 3/3**, hardening-skip retest **−0.39
over 8 pairs** (the earlier +0.52 was noise; upstream's register
zeroing stands permanently), reports-budget 50 µs **−12.1 3/3**
(submit-storm cliff; 300 µs validated). Items 1/3/4 below carry the
full receipts. The honest state: at the ~48 fps equilibrium this page
has **no live fps candidate** — the next lever must come from a fresh
profile at the new shape, the ranged-flush FIXME with a post-wave
number, or the prewarm/hitch class (item 2), which is
latency-not-throughput.

*Historical state header, superseded by the block above — kept because
the items below were ranked against it.* State when last re-ranked
(2026-07-19, arm-(b)-on shape): F8 heavy
anchor ~36-39 fps @ ~831 draws/flip windowed (M2 Ultra, local no-PGO
build) — the sub-page store-skip promotion moved the scene equilibrium
from ~740 draws/flip, so compare draws/s throughput (~30-31.6k, up
from ~28.3k) across that boundary, not raw fps. F5 ~59, F6 at the 60
vblank cap. The scene is CPU-bound on the guest vCPU thread: guest-TCG
generated code ~55% of vCPU samples (top TB 3.2% — no hot loops),
locks architectural, tb cache unpressured (`tb_flush` = 1/run). The
old "notdirty traps ~90 k/s ≈ 2%" line was a ~20x-undercounting proxy
(archaeology 6.9): the store slow path saw ~2M eligible stores/s,
now completed inline by default. The 60 fps goal on F8 needed ~+60% at
that anchor — from the post-wave ~48.1 it is ~+25%; there is no
quantization wall below the cap. Re-profile
(`XEMU_GUEST_PROF`, `XEMU_INV_PROF`, `XEMU_TB_PROF`) before re-ranking
after any landing.

**2026-08-04 audit note — written pre-campaign; the closeout it defers
to is the "Top open candidates" section below.** A fork-wide optimization
audit ran at this equilibrium and is committed as
[`fork-optimization-audit-2026-08.md`](fork-optimization-audit-2026-08.md)
(49 agents; 40 findings, each carrying `file:line` evidence and a
pre-registered prediction + kill threshold, then an adversarial
verifier per finding — 8 confirmed, 17 plausible, 15 refuted, with the
verifiers' corrections folded into the published numbers). Nothing in
it is planned work, and the ranking below is deliberately **unchanged**:
its items are candidates gated on their own measurements, and
re-ranking waits on the campaign's numbers (closeout pass). What it
feeds: item 1 gets the vCPU-thread constant-factor candidates (§1.1
`can_do_io` elision, §1.2 `notdirty_write` dirty-check fast path, §1.4
the x87/SSE translator pack, §1.6/§1.8 the micro-gates) — none of them
a region-formation reopen, so item 1's "fundamentally different attack"
bar stands untouched. Item 4 is adjacent to §1.3's
surface-callback TLB-flush rate question (same TB-lifetime /
invalidation class, same required movement probe). Item 5 gets §3.2
(converge the shipped MoltenVK with the tested one) and §3.3 (pin the
CI `-mcpu` distribution floor). The feature/quality vectors below get
§2.1-§2.3 (MetalFX output-size quantization, drawable-acquire
ordering, UI-thread timing instrumentation) and §2.4 (the
movement-probe gate that closes or reopens arch 1.12). Its §5 records
15 verified negatives so the next sweep doesn't re-derive them, and its
§4 record-hygiene closures have already landed: DSP parmove+ALU fusion
and lockless MMIO for the remaining BQL blocks are both settled in the
ledger, the latter with its own verdict page
([`lockless-mmio-verdict.md`](lockless-mmio-verdict.md), whose owed
measurement is now filled in at §A.1).

**Closeout of that note (2026-08-04, same day).** The campaign ran and
the numbers exist. Disposition of everything the note routed: §1.1,
§1.4a/b, §1.6 and §1.8 shipped default-on with receipts (ledger); §1.2's
code shipped but its **fps claim was killed** (benefit base collapsed
34-57x under the arm-(b) promotion); §1.4c COMISS, §1.5 kick
suppression and §1.9 the ImGui lock rework were all **closed by their
own censuses** and are in Settled vectors; §1.3 measured *for* the
per-flip theory and is candidate 1 above; §1.7 cleared its gate and is
deferred as candidate 4; §2.1/§2.3 shipped, §2.2 stays opt-in with its
characterization, §2.4 **reopened arch 1.12** as candidate 2, §2.5
shipped; §3.1's hardening skip was **killed at the evidence bar** (a
Failed row), §3.2-§3.5 shipped. Item 1's "fundamentally different
attack" bar still stands untouched — nothing in the wave was a
region-formation reopen.

## Top open candidates (2026-08-04 wave closeout)

These are the live candidates the wave produced or reopened, ranked by
expected value at the ~48.1 fps equilibrium. Each already has its
measurement; none is planned work. The older ranked list below is kept
intact — items 1-4 there are closed or parked records, item 5 is the
standing correctness debt.

1. **Surface CPU-access-callback churn — pool shape MEASURED AND
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
   swap. The pool stays in-tree dark as the reproducible record
   (Failed row in the ledger). The only live shape is
   (b) the `// FIXME: flush only applicable pages` ranged invalidation
   in `system/physmem.c` / `accel/tcg/cputlb.c` (template:
   `tlb_reset_dirty_range_locked`) — it spares the jump cache and
   costs no per-swap bookkeeping, but needs a deferred/queued flush
   API. Status: needs-theory; reopen only with a predicted number
   derived from a POST-wave profile, not the audit's.
   Measurement gotcha, recorded so nobody re-derives it: HMP `info jit`'s
   **`TLB full flushes` line is structurally unreachable on this guest**
   (it counts only `to_clean == ALL_MMUIDX_BITS`, and the Xbox target
   has `NB_MMU_MODES=22` vs plain i386's 8) — use the partial + elided
   counters.
2. **Async/prewarm pipeline creation — arch 1.12 REOPENED 2026-08-04.**
   The original kill measured steady state only. The audit's zero-code
   gate (movement probe, forward first-visit phase vs the backward
   control, gating on `max=` and event count rather than a per-flip
   average) **fired**: forward-phase `pipeline_gen` runs **468.7
   µs/flip with a single worst compile of 45.09 ms** — about two dropped
   frames at the current anchor, inside the first-visit streaming
   workload class the steady-state kill never sampled. That graduates
   Experiment B option (a), prewarm, from
   [`moltenvk-optimization-experiments.md`](moltenvk-optimization-experiments.md)
   into a ranked item **with a baseline number attached**. First steps:
   re-run the probe with cold and warm `pipeline_cache.bin` to separate
   "compile" from "cache miss", then price prewarm against that 45 ms
   tail. Bar to attempt: the hitch must survive a warm-cache run — a
   warm-cache-only hitch is a cache-coverage problem, not a
   pipeline-creation one.
3. **x87 clean-ST(i) write-back elision (`XEMU_X87_ELIDE_CLEAN`) —
   MEASURED AND CLOSED 2026-08-04.** The fps A/B it was awaiting ran on
   idle-gated pairs: **−0.23 fps, 3/3 negative** against the ≥ +0.20
   promotion bar. The refuter had already come back stronger than
   predicted (3.49e9 checks, 0 violations, truncation signal absent),
   but safe-and-worthless is still worthless: the elidable flush
   population is too thin after the FT0 + FIP/FDP harvests to pay for
   the dirty tracking. Stays dark permanently as the record; do not
   reopen without a census showing the clean-ST class regrew past
   8M/s on some other title.
4. **Reports idle-budget scan below 300 µs — RAN AND CLOSED
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

## The ranked performance roadmap

Historical ranking (2026-07-19). Items 1-4 are closed or parked
records; item 5 is the standing correctness debt and is still open. The
live candidates are in the section above.

1. **Trace/superblock/region formation in TCG — CAMPAIGN CLOSED
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
2. **Ret-memo remainder — CLOSED 2026-07-18.** The 12→13-bit
   experiment ran as a one-binary A/B (the index width is now a
   runtime-latched knob, `XEMU_RETC_BITS`, default 12): **13-bit
   measured −0.23 fps, 4/4 pairs negative, scene identity clean** —
   the 128→256 KiB footprint cost is real and the 12-bit hash already
   captures the recurring ret set (same shape as the jump-cache
   12→16-bit kill). 12 bits stands; the knob stays for re-tests on
   other titles. Ledger has the Failed row.
3. **Sub-page arm (b) — PROMOTED DEFAULT-ON 2026-07-18** (the recipe
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
4. **Cross-page chaining, page-spanning-dest coverage gap — MEASURED
   BELOW BAR 2026-07-18, parked.** The dispatch loop refuses to
   direct-chain into a TB that itself spans two pages (`cpu-exec.c`
   tb_page_addr1 guard). The sizing counter (`xemu_inv_span_nochain`,
   INV_PROF dump (d')) read **0.26-0.54M declines/s in-game on F8**
   (45.9M per ~85 s baseline run; roughly half that with arm (b) on)
   against the pre-registered ≥ ~1M/s bar — and each decline costs
   only a jump-cache-class lookup, so the ceiling is well under 2% of
   the vCPU thread. Closing the gap means keying the xpage backstop on
   *both* dest pages, an archaeology-1.24-class hazard — not worth it
   at this rate. Reopen only if a title shows the counter ≥ 1M/s.
5. **Real-hardware validation debt (not fps; the largest open
   correctness item).** Windows/Linux runtime proof for the shipped
   cross-platform wins: the gating-audit needs-real-HW list
   (`docs/windows-gating-audit.md`), an in-game `XEMU_DSP_JIT_DIFF`
   run on non-Apple aarch64, `XEMU_SSE_HOST=2` clean on real x86_64
   silicon (one un-root-caused ±0-sign divergence under Rosetta keeps
   that arm dark), and per-platform savestate baselines before any
   perf claim there. Excluded from the 2026-07-18 session scope by
   owner decision. The shipped BQL-free MMIO dispatch belongs on this
   list too: Windows/Linux take the identical lockless path for
   PFB/USER/APU-VP/PGRAPH and no non-VM soak exists
   (`docs/pgraph-lockless-audit.md` §I.3). Converting the *remaining*
   blocks is closed by written verdict —
   [`lockless-mmio-verdict.md`](lockless-mmio-verdict.md) (2026-08-04):
   residual under 0.07% of a core (≈ +0.01-0.06 fps, at or below the
   ±0.02 fps baseline resolution), and BQL-free dispatch there would
   drive the PCI IRQ line with no BQL held at all.

Presentation note: `display.frame_interpolation` (MetalFX) already
delivers displayed-60/120 from a solid real-30+ today; it does not
change simulation rate or input latency, and benches must keep reading
real flips.

## Open feature / quality vectors

- **BINK video via VideoToolbox.** Xbox BINK decode is guest-CPU work
  under TCG; no in-repo hook exists yet (verified: zero `bink` matches
  in sources). First steps: confirm a fixture title's FMV moment is
  actually decode-bound on an M-series core, then find the narrowest
  interception point (`surface-compute.c` YUV handling is the closest
  existing hook) before considering HLE. Stays needs-theory until that
  number exists.
- **PAL 50 Hz guest vblank.** The dedicated vblank timer thread is
  fixed at 60 Hz (`ui/xemu.c` `vblank_interval_ns`); deriving 50 Hz
  from the guest video mode would serve PAL titles. The precondition
  the old host-refresh-alignment revert demanded (archaeology 3.2) —
  a guest-vblank source decoupled from host present — **is now met**
  (the Metal present path never references the vblank timer). Needs
  guest region detection + an A/B on an NTSC title proving zero
  behavior change there. No PAL title is currently in the fixture set.
- **Real-depth engagement on AA / super-width titles**
  (`XEMU_MFX_REAL_DEPTH=2` is temporally correct but disengages when
  no zeta matches the scaler input dims — AA titles render 3D at 2×
  scanout width). A resolve-blit of the bound zeta down to scanout
  dims at flip would engage it (proven to render cleanly); the open
  question is whether resolved AA depth improves temporal quality
  enough to replace the exact-match fallback. Quality A/B, ~0 fps.
- **GL NV2A renderer under the Metal window.** A renderer switch away
  from Vulkan under the Metal backend needs a restart today; full
  unification would render the GL display buffer into an
  IOSurface-backed FBO feeding the same present-frame handoff.
  Compatibility feature, 0 fps.
- **`VK_KHR_dynamic_rendering` with explicit barriers.** The fenced
  B2 lowering (archaeology 1.5) reopens only with the explicit
  `VK_SUBPASS_EXTERNAL`-equivalent barriers around every
  Begin/EndRendering and a level-transition repro validation. No
  measured upside is currently attached to it on TBDR — treat as
  research, not a queued item.

## Provenance

Re-rank inputs: `XEMU_GUEST_PROF=1` sampler + exit census,
`XEMU_INV_PROF=1` (+`XEMU_INV_TIMING=1`) invalidation/trap counters,
`XEMU_SUPERBLOCK_SIZE=1` tail-kind census, `XEMU_NV2A_NSPROF=1` frame
attribution. Added 2026-08-04 for the candidates above:
`XEMU_SURFACE_CB_STATS=1` (surface-callback / TLB-flush rate),
`XEMU_X87_CENSUS=1` + `XEMU_X87_REFUTE=1` (x87 elidability and its
adversarial checker), `XEMU_PFIFO_KICK_STATS=1`, `XEMU_UI_LOCK_STATS=1`,
`XEMU_MMIO_PROF=1`, and `scripts/movement-probe.sh` for the first-visit
streaming phase. Method: `.claude/skills/xemu-testing`; thresholds:
`.claude/skills/xemu-validation-and-qa`. Anchors and defaults on this
page are dated — re-verify before citing.
