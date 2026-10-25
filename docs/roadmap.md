# xemu-macos performance & feature roadmap

The open candidates: work not yet attempted, or measured and parked
behind an explicit bar. Every entry carries its decision inputs — what
is known, the measurement that would confirm or kill it, and the bar it
must clear. Landed and settled work lives in the
[optimization ledger](optimizations.md); nothing here is planned work
until it graduates into a design doc or a landing.

## Top open candidates

These are the candidates the 2026-08-04 wave produced or reopened.
Each has its measurement; none is planned work. Every entry is now a
one-line stub kept under its number so citations elsewhere still
resolve: item 1 landed on 2026-10-15, item 2's prewarm shipped on
2026-10-24, items 3 and 4 are closed. The older ranked list below
follows the same convention: items 1-3 are closed stubs, item 4 is
parked below its bar, item 5 is the standing correctness debt. Full
write-ups are in
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md)
(moved 2026-10-10; items 1 and 2 on 2026-10-15).

The 2026-10-11 profile at the primary anchor moved to
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md#profile-at-the-primary-anchor-2026-10-11)
on 2026-10-25.

1. **Surface CPU-access-callback churn — ranged flush landed 2026-10-15** (`tlb_flush_host_range`). Deterministic offscreen heavy-f1 runs 2.3% fewer host instructions per frame: the ~0.2% sample estimate that parked it missed the refills and the jump-cache and near-return-memo wipes each full flush caused. The reuse-pool shape stays killed (−0.66 fps, 3/3, 2026-08-04). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md).
2. **Async/prewarm pipeline creation — parked 2026-10-11; prewarm reopened and shipped 2026-10-24** (ledger: Pipeline prewarm). The 45-53 ms first-visit compile is a persistent pipeline-cache coverage miss, gone once warm; what reopened it was an effect's first use after an update or with cold driver caches (Azurik's fire-armor cast: 1.9-2.3 s of compiling). It runs on up to 4 workers since 2026-10-24; the parallel compile of first-use miss bursts, the owner's other hitch fix, was gated on that scaling test and closed by it (Metal-cold 1.68× at 8 workers against 3×; `xemu-history`). Async compile stays closed. Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md).
3. **x87 clean-ST(i) write-back elision — closed 2026-08-04** (−0.23 fps, 3/3). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: Failed / reverted experiments, which corrects the stated mechanism. The code was removed on 2026-10-10.
4. **Reports idle-budget scan below 300 µs — closed 2026-08-04** (50 µs: −12.1 fps, 3/3). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: Failed / reverted experiments.

## The ranked performance roadmap

Historical ranking (2026-07-19). Items 1-3 are closed (write-ups in
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md)), item 4 is parked below its bar, and
item 5 is the standing correctness debt, still open. The live
frontier is the `xemu-history` skill's Open frontier. The 2026-10-11
profile ([Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md#profile-at-the-primary-anchor-2026-10-11)) predates the 2026-10-15/16 TCG, x87 and threading work
(inline MMX entry, x87 doubles and write-through, no per-TB W^X flip,
lockless doorbell, vCPU QoS); re-profile before ranking work from it.

1. **Trace/superblock/region formation in TCG — campaign closed 2026-07-18.** Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: Failed / reverted experiments. The machinery was removed on 2026-10-10.
2. **Ret-memo remainder — closed 2026-07-18** (13-bit index −0.23 fps, 4/4). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: Failed / reverted experiments. The `XEMU_RETC_BITS` knob was removed on 2026-10-10; the width is fixed at 12 bits.
3. **Sub-page arm (b) — promoted default-on 2026-07-18** (`ede35b84f9`). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: CPU / JIT (ARM64).
4. **Cross-page chaining, page-spanning-dest coverage gap — parked
   2026-07-18; its exits are routed since 2026-10-15.** The dispatch
   loop refuses to direct-chain into a TB that itself spans two pages
   (`cpu-exec.c` tb_page_addr1 guard). The sizing counter
   (`xemu_inv_span_nochain`, INV_PROF dump (d')) read 0.26-0.54M
   declines/s in-game on F8, and the item was parked on the belief that
   a decline costs a jump-cache-class lookup. It cost a full exit
   through `cpu_exec_loop`. `gen_jmp_rel` now sends every direct exit
   to a target in the last 14 bytes of a page through the inline
   jump-cache probe, which may enter a spanning TB: declines fell from
   3.1M to under 1K per 1200 heavy-f1 det frames. Chaining into such
   TBs (keying the xpage backstop on *both* dest pages, an
   archaeology-1.24-class hazard) would now save only the probe.
5. **Real-hardware validation debt (not fps; the largest open
   correctness item).** Windows/Linux runtime proof for the shipped
   cross-platform wins: the gating-audit needs-real-HW list
   (`docs/windows-gating-audit.md`), an in-game `XEMU_DSP_JIT_DIFF`
   run on non-Apple aarch64, and per-platform savestate baselines
   before any perf claim there. **Windows x86_64 had its first
   plan-driven run on 2026-10-18** (i7-4770K, NVIDIA TITAN Xp; the
   audit's 2026-10-18 report). Settled there: `XEMU_SSE_HOST=2` runs
   clean on real x86_64 silicon, so the host SSE path is on by default
   on x86_64 (`57cfe4fb51`; the ±0-sign divergence seen only under
   Rosetta stays un-root-caused); the zeta-quarantine and texture
   retirement gates soak clean and the validation layers report 0
   VUIDs on NVIDIA; det and windowed Windows baselines exist. The run
   fixed an SDL startup crash (`058431f83b`, also the 2026-09 crash
   blamed on `-O3`), the null-mode quit crash and two x86_64 hard-FPU
   defects (the guest rounding mode, `cfcc8e68ab`; the vCPU thread's
   x87 precision, `deb98ee93f`), and its optimization pass took
   windowed heavy-f1 from 11.1 fps to 38.7 fps (W2 8.5 → 40.7) on that CPU (the
   x87 helpers inline on the host x87 unit, TOP-keyed TBs, a
   waitable-timer main loop). Still owed: AMD and Intel GPUs, Windows
   ARM64, Linux, a CI-trained Windows PGO profile. The shipped BQL-free MMIO dispatch belongs on
   this list too: Windows/Linux take the identical lockless path for
   PFB/USER/APU-VP/PGRAPH
   (`docs/Finished/pgraph-lockless-audit.md` §I.3); Windows passed its
   first non-VM soak on 2026-10-18 (plan 3.11, 30 of 30 cycles), Linux
   has none. The runtime switches that put the BQL back
   (`XEMU_MMIO_BQL`, `XEMU_PGRAPH_LOCKLESS`) were removed 2026-10-15,
   so bisecting a lockless-suspected failure there takes a rebuild
   without the `memory_region_enable_lockless_io` calls (three in
   `nv2a_realize`, one in `mcpx_apu_realize`) or a revert of the
   removal commit; `XEMU_PFIFO_PUT_LOCK=1` still covers the separate
   DMA_PUT doorbell. Converting the *remaining*
   blocks is closed by written verdict —
   [`lockless-mmio-verdict.md`](Finished/lockless-mmio-verdict.md) (2026-08-04):
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
- **PAL 50 Hz guest vblank: shipped opt-in** (`[sys] vblank_rate`,
  README "PAL 50 Hz"). `'eeprom'` derives 50 Hz from a PAL50 EEPROM,
  `'50hz'` forces it; the default stays 60 Hz. Open: a PAL title that
  picks 50 Hz on a PAL-60 console, or 60 Hz modes on a PAL50 one
  (HDTV packs), would need the guest's programmed video timing
  (encoder and CRTC registers), which no snapshot holds today.
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

Re-rank inputs: `XEMU_GUEST_PROF=1` sampler,
`XEMU_INV_PROF=1` (+`XEMU_INV_TIMING=1`) invalidation/trap counters,
`XEMU_NV2A_NSPROF=1` frame attribution. Added 2026-08-04 for the candidates above:
`XEMU_X87_CENSUS=1` + `XEMU_X87_REFUTE=1` (x87 elidability and its
adversarial checker), `XEMU_MMIO_PROF=1`,
and `scripts/movement-probe.sh` for the first-visit streaming phase.
Method and thresholds: the `xemu-operate` skill. Anchors and defaults on this
page are dated — re-verify before citing. An "archaeology N.M" citation
is section N.M of the retired failure-archaeology skill:
`git show ad569d78c5:.claude/skills/xemu-failure-archaeology/SKILL.md`.
