# xemu-macos roadmap

Open candidates only: work not yet attempted, or measured and parked
behind a bar. Each entry gives what is known, the measurement that
would settle it and the bar. Landed and settled work is in the
[optimization ledger](optimizations.md); closed candidates, with their
numbers, are in
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md).
The live frontier is the `xemu-history` skill's Open frontier. Nothing
here is planned work until it gets a design doc or lands.

## Ranked performance items

Items keep their numbers so citations resolve. Ranked items 1-3
(region formation, ret-memo remainder, sub-page arm (b)) and the
2026-08-04 top candidates 1-4 (surface callback churn, async/prewarm
pipeline creation, x87 write-back elision, reports idle-budget scan)
are closed; they live under those numbers in
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md).
The newest profile
([2026-10-11](Finished/roadmap-closed-items.md#profile-at-the-primary-anchor-2026-10-11))
predates the 2026-10-15/16 TCG, x87 and threading work; re-profile
before ranking from it.

4. **Chaining into page-spanning TBs: parked.** The dispatch loop
   refuses to direct-chain into a TB that spans two pages (`cpu-exec.c`
   `tb_page_addr1` guard). Since 2026-10-15 `gen_jmp_rel` sends every
   direct exit to a target in the last 14 bytes of a page through the
   inline jump-cache probe, which may enter a spanning TB: declines
   (`xemu_inv_span_nochain`) fell from 3.1M to under 1K per 1200
   heavy-f1 det frames. Chaining into such TBs (keying the xpage
   backstop on both dest pages, an archaeology-1.24-class hazard) would
   now save only the probe.
5. **Real-hardware validation debt** (not fps; the largest open
   correctness item). Windows/Linux runtime proof for the shipped
   cross-platform wins: the needs-real-HW list in
   `docs/windows-gating-audit.md`, an in-game `XEMU_DSP_JIT_DIFF` run on
   non-Apple aarch64, and per-platform savestate baselines before any
   perf claim there. Settled on Windows x86_64 (i7-4770K, NVIDIA TITAN
   Xp, 2026-10-18): `XEMU_SSE_HOST=2` runs clean and is the x86_64
   default (`57cfe4fb51`; the +/-0-sign divergence seen only under
   Rosetta stays un-root-caused); the zeta-quarantine and
   texture-retirement gates soak clean; 0 VUIDs on NVIDIA; det and
   windowed baselines exist; the lockless MMIO path passed 30 of 30 soak
   cycles; windowed heavy-f1 went from 11.1 to 38.7 fps. Still owed: AMD
   and Intel GPUs, Windows ARM64, Linux (including a lockless soak), a
   CI-trained Windows PGO profile, and the portable LLVM DSP JIT from the
   Better-xemu4macOS merge on the hosts that build it.

   Windows and Linux take the same lockless MMIO path for
   PFB/USER/APU-VP/PGRAPH (`docs/Finished/pgraph-lockless-audit.md`
   §I.3). With `XEMU_MMIO_BQL` and `XEMU_PGRAPH_LOCKLESS` removed,
   bisecting a lockless-suspected failure takes a rebuild without the
   `memory_region_enable_lockless_io` calls (three in `nv2a_realize`,
   one in `mcpx_apu_realize`) or a revert of the removal commit;
   `XEMU_PFIFO_PUT_LOCK=1` still covers the DMA_PUT doorbell.
   Converting the remaining blocks is closed
   ([lockless-mmio-verdict.md](Finished/lockless-mmio-verdict.md)).

## Feature and quality vectors

- **BINK video via VideoToolbox.** BINK decode is guest-CPU work under
  TCG and no hook exists. First confirm a fixture title's FMV is
  decode-bound on an M-series core, then find the narrowest
  interception point (`surface-compute.c`'s YUV handling is the closest)
  before considering HLE.
- **PAL 50 Hz from the guest's video timing.** `[sys] vblank_rate`
  ships: `'eeprom'` derives 50 Hz from a PAL50 EEPROM, `'50hz'` forces
  it, the default `'60hz'` keeps 60 Hz. A PAL title that picks 50
  Hz on a PAL-60 console, or 60 Hz modes on a PAL50 one, would need the
  programmed encoder and CRTC timing, which no snapshot holds today.
- **Real depth on AA and super-width titles.** `XEMU_MFX_REAL_DEPTH=2`
  disengages when no zeta matches the scaler input (AA titles render 3D
  at 2x scanout width). A resolve-blit of the bound zeta to scanout dims
  at flip would engage it; the open question is whether resolved AA
  depth beats the exact-match fallback. Quality A/B, about 0 fps.
- **GL NV2A renderer under the Metal window.** Switching away from
  Vulkan under the Metal backend needs a restart; unifying would render
  the GL display buffer into an IOSurface-backed FBO feeding the same
  present handoff. Compatibility only.
- **`VK_KHR_dynamic_rendering` with explicit barriers.** The fenced B2
  lowering (archaeology 1.5) reopens only with explicit
  `VK_SUBPASS_EXTERNAL`-equivalent barriers around every
  Begin/EndRendering and a level-transition repro. No measured upside
  on TBDR; research only.

## Measuring

Re-rank inputs: `XEMU_GUEST_PROF=1`, `XEMU_INV_PROF=1` (with
`XEMU_INV_TIMING=1`), `XEMU_NV2A_NSPROF=1`, `XEMU_X87_CENSUS=1` with
`XEMU_X87_REFUTE=1`, `XEMU_MMIO_PROF=1`, and `scripts/movement-probe.sh`
for first-visit streaming. Method and thresholds: the `xemu-operate`
skill. `display.frame_interpolation` (MetalFX) delivers displayed
60/120 from a real 30+ without changing simulation rate or input
latency, so benches read real flips. An
"archaeology N.M" citation is section N.M of
`git show ad569d78c5:.claude/skills/xemu-failure-archaeology/SKILL.md`.
