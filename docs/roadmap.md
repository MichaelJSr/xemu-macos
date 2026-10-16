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
resolve: item 1 landed on 2026-10-15, item 2 is parked behind its bar,
items 3 and 4 are closed. The older ranked list below follows the same
convention: items 1-3 are closed stubs, item 4 is parked below its bar,
item 5 is the standing correctness debt. Full write-ups are in
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md)
(moved 2026-10-10; items 1 and 2 on 2026-10-15).

**Profile at the new primary anchor (2026-10-11).** One `probe-run.sh`
session on `azurik-heavy-f1` (80 s, release bundle `91D280C7`, retrained
PGO) with `XEMU_GUEST_PROF`, `XEMU_INV_PROF`, `XEMU_TB_PROF` and
`XEMU_NV2A_NSPROF`, plus a 20 s `/usr/bin/sample` of a second session,
on a box loaded by other jobs:
- **Where vCPU time goes.** 32.3% in translated guest code, 67.7% in
  host code. x87 hard-float helpers ≈ 24% together (`fmul` 5.1, `flds`
  3.7 + 2.6, `fcom` 2.5, `fsts` 2.4, `fadd` 1.8 + 1.7, `fpop` 1.5,
  `fucomi` 1.0, `fsub` 0.9 + 0.4); `pthread_jit_write_protect_np` 10.9%
  (the barrier-skid attribution of archaeology 1.17, not real W^X cost);
  mutex waits 8.3%; `cpu_exec_loop` 2.3, `helper_enter_mmx` 2.2,
  `helper_cc_compute_all` 1.1. `sample` shows where the mutex waits
  come from: `helper_stl_mmu` is 12.8% of vCPU samples inclusive, and
  three quarters of that is `user_write` (the PFIFO doorbell) blocked in
  `qemu_mutex_lock_impl` on `pfifo.lock` while the PFIFO thread holds it
  (~9.6% of vCPU samples); `vp_write` is another ~2%. The lockless
  DMA_PUT doorbell (2026-10-15, `3391b92357`) cut the doorbell from 12%
  to 2.2% of vCPU samples; the REPORTS_SUBMIT valve behind that wait is
  parked in the `xemu-history` skill (0 submits per flip with both
  heavy anchors at the 60 fps cap, 2026-10-16).
- **Guest TLB flush + refill: ~0.2%, under item 1's 1.5% gate.**
  `tlb_flush_by_mmuidx_async_work` 0.07%, `x86_cpu_tlb_fill` 0.05%,
  `get_page_addr_code_hostp` 0.11% of vCPU samples; the slow-path
  load helpers add at most ~0.3%. Item 1 was parked on this number and
  landed on 2026-10-15 anyway: instructions per frame showed what
  samples missed.
- **x87 store traffic:** `helper_fsts_ST0__hard` 2.4% (plus
  `helper_fmov_FT0_STN__hard` 0.4%).
- **Ret-memo:** `xemu: ras hits=45901 (0.5%) fills=10089110` over
  110 s. The counter only sees the helper path, which runs after the
  inline probe at the ret site has missed, so it is the hit rate of
  misses, not of the memo. The v0.13.3 build (before the per-page memo
  flush) reads 45013 / 9945329 on the same scene: the flush costs
  nothing measurable.
- **`da_multi_subrange`: 0** in all 21 intervals (nsprof prints only
  non-zero events), as on F5. The GPU campaign's open fact is closed:
  multi-subrange draws do not occur on the heavy anchor either.

1. **Surface CPU-access-callback churn — ranged flush landed 2026-10-15** (`tlb_flush_host_range`). Deterministic offscreen heavy-f1 runs 2.3% fewer host instructions per frame: the ~0.2% sample estimate that parked it missed the refills and the jump-cache and near-return-memo wipes each full flush caused. The reuse-pool shape stays killed (−0.66 fps, 3/3, 2026-08-04). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md).
2. **Async/prewarm pipeline creation — parked 2026-10-11.** The 45-53 ms first-visit compile is a persistent pipeline-cache coverage miss, gone once warm, so prewarm is not the fix; reopen only on a forward-phase compile ≥ 16 ms that survives a warm rerun. Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md).
3. **x87 clean-ST(i) write-back elision — closed 2026-08-04** (−0.23 fps, 3/3). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: Failed / reverted experiments, which corrects the stated mechanism. The code was removed on 2026-10-10.
4. **Reports idle-budget scan below 300 µs — closed 2026-08-04** (50 µs: −12.1 fps, 3/3). Write-up: [Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md); ledger: Failed / reverted experiments.

## The ranked performance roadmap

Historical ranking (2026-07-19). Items 1-3 are closed (write-ups in
[Finished/roadmap-closed-items.md](Finished/roadmap-closed-items.md)), item 4 is parked below its bar, and
item 5 is the standing correctness debt, still open. The live
frontier is the `xemu-history` skill's Open frontier. The 2026-10-11
profile above predates the 2026-10-15/16 TCG, x87 and threading work
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
   run on non-Apple aarch64, `XEMU_SSE_HOST=2` clean on real x86_64
   silicon (one un-root-caused ±0-sign divergence under Rosetta keeps
   that arm dark), and per-platform savestate baselines before any
   perf claim there. Excluded from the 2026-07-18 session scope by
   owner decision. The shipped BQL-free MMIO dispatch belongs on this
   list too: Windows/Linux take the identical lockless path for
   PFB/USER/APU-VP/PGRAPH and no non-VM soak exists
   (`docs/Finished/pgraph-lockless-audit.md` §I.3). The runtime switches
   that put the BQL back (`XEMU_MMIO_BQL`, `XEMU_PGRAPH_LOCKLESS`) were
   removed 2026-10-15, so bisecting a lockless-suspected failure there
   takes a rebuild without the `memory_region_enable_lockless_io` calls
   (three in `nv2a_realize`, one in `mcpx_apu_realize`) or a revert of
   the removal commit; `XEMU_PFIFO_PUT_LOCK=1` still covers the separate
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

Re-rank inputs: `XEMU_GUEST_PROF=1` sampler,
`XEMU_INV_PROF=1` (+`XEMU_INV_TIMING=1`) invalidation/trap counters,
`XEMU_NV2A_NSPROF=1` frame attribution. Added 2026-08-04 for the candidates above:
`XEMU_X87_CENSUS=1` + `XEMU_X87_REFUTE=1` (x87 elidability and its
adversarial checker), `XEMU_UI_LOCK_STATS=1`, `XEMU_MMIO_PROF=1`,
and `scripts/movement-probe.sh` for the first-visit streaming phase.
Method and thresholds: the `xemu-operate` skill. Anchors and defaults on this
page are dated — re-verify before citing. An "archaeology N.M" citation
is section N.M of the retired failure-archaeology skill:
`git show ad569d78c5:.claude/skills/xemu-failure-archaeology/SKILL.md`.
