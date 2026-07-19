# xemu-macos performance & feature roadmap

The open candidates: work not yet attempted, or measured and parked
behind an explicit bar. Every entry carries its decision inputs — what
is known, the measurement that would confirm or kill it, and the bar it
must clear. Landed and settled work lives in the
[optimization ledger](optimizations.md); nothing here is planned work
until it graduates into a design doc or a landing.

State when last re-ranked (2026-07-18, v0.11.2+PGO shape): F8 heavy
anchor 37.2-37.6 fps @ ~740 draws/flip windowed (M2 Ultra), F5 ~59, F6
at the 60 vblank cap. The scene is CPU-bound on the guest vCPU thread:
guest-TCG generated code is 55.3% of vCPU samples (top TB 3.2% — no hot
loops), notdirty traps ~90 k/s ≈ 2% of the thread, locks architectural,
tb cache unpressured (`tb_flush` = 1/run). The 60 fps goal on F8 needs
~+60%; there is no quantization wall below the cap (fps rises
continuously with vCPU throughput until the fixed 60 Hz vblank IRQ
gates it). Re-profile (`XEMU_GUEST_PROF`, `XEMU_INV_PROF`,
`XEMU_TB_PROF`) before re-ranking after any landing.

## The ranked performance roadmap

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
2. **Ret-memo remainder (sub-noise, optional).** The fill path's
   g_tree reversal was removed 2026-07-18 (ledger, Testing & tooling);
   what remains is the `XEMU_RETC_BITS` 12→13 experiment — one
   `#define`, predicted +0.3-0.5 fps *at the measurement floor* with a
   real 128→256 KiB cache-footprint downside. Cheap experiment, not a
   known win; kill unless 13-bit ≥ 12-bit across 4 unanimous pairs.
3. **Sub-page arm (b) — IMPLEMENTED DARK 2026-07-18** (owner accepted
   the risk exploration; ledger has the full entry). Correctness is
   refuter-proven (6.35M decisions, 0 violations, ideal demote
   histogram); the +0.3-0.7 fps prediction never got its quiet-machine
   A/B. **Promotion recipe:** on a quiet machine, run
   `scripts/bench-savestate-ab.sh vm-<f8> XEMU_SUBPAGE_FAST 3 75 out`
   — promote the default only on ≥3 consistent-sign positive pairs
   plus a loadvm soak, per the house bar.
4. **Cross-page chaining, page-spanning-dest coverage gap.** The
   dispatch loop refuses to direct-chain into a TB that itself spans
   two pages (`cpu-exec.c` tb_page_addr1 guard). Sizing counter landed
   2026-07-18 (`xemu_inv_span_nochain`, INV_PROF dump (d')): read it
   in-game before considering the registry extension — closing the gap
   means keying the xpage backstop on *both* dest pages, an
   archaeology-1.24-class hazard for what is expected to be a
   sub-noise share. Bar: counter ≥ ~1M/s in-game.
5. **Real-hardware validation debt (not fps; the largest open
   correctness item).** Windows/Linux runtime proof for the shipped
   cross-platform wins: the gating-audit needs-real-HW list
   (`docs/windows-gating-audit.md`), an in-game `XEMU_DSP_JIT_DIFF`
   run on non-Apple aarch64, `XEMU_SSE_HOST=2` clean on real x86_64
   silicon (one un-root-caused ±0-sign divergence under Rosetta keeps
   that arm dark), and per-platform savestate baselines before any
   perf claim there. Excluded from the 2026-07-18 session scope by
   owner decision.

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
attribution. Method: `.claude/skills/xemu-testing`; thresholds:
`.claude/skills/xemu-validation-and-qa`.
