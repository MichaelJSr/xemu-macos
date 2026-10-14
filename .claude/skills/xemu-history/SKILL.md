---
name: xemu-history
description: Has this been tried? Settled dead ends, reverts, fenced/dark flags, GPU frame-cost campaign state and the open frontier. Load before proposing an optimization or retrying/reverting anything, for "why was X reverted", "why is this flag here", what to work on next, or GPU frame-cost work.
---

# xemu history: what's been tried, what's open

Outcomes: **reverted** (landed, then removed) · **fenced** (removed; probe or harness kept) · **dark** (off by default) · **fixed** (bug fixed; the hatch restores the old behavior) · **closed** (measured or argued dead) · **shipped**. Story: `git show --no-patch <sha>`. Record: `docs/optimizations.md` (Failed/Settled), `docs/roadmap.md`. Reopen only with a fresh number, on today's binary, that beats the row's reason.

## Dead ends and reverts

| Idea | Outcome | Why | Pointer |
|---|---|---|---|
| **Renderer / MoltenVK** | | | |
| Occlusion-rework intermediates (in-pass queries + sync drains; submit per report) | reverted | 6.86 / ~11.2 fps vs 15.78. Placement and delivery are one policy; shipped form: 38.57 fps, passes 376→14. Never re-add per-draw pass teardown (TBDR) | `6bfbc22863`, `XEMU_REPORTS_SYNC=1` |
| CPU-side wait/fence elimination for streamed vertices (byte-exact refine; per-flight 128 MiB mirrors) | closed | fps flat twice: the wait is GPU time changing call site. The byte-exact refine shipped alone (+5.4%) | `099efcb734`, `268daf69ac`, `XEMU_VTX_EXACT=0` |
| Vertex copy-on-conflict remap | reverted | −2.09 fps 6/6: broad rewrites overflow the table into all-slot drains | `64a8a6b543` |
| Eager report submit; idle budget 300→50 µs | reverted / closed | −0.08 fps, signs mixed; −12.1 fps (submit storm). 300 µs stays | `7a9b116680` |
| `VK_KHR_dynamic_rendering` (B2) | fenced | No implicit SUBPASS_EXTERNAL dependency → tile stall, transition hang. Needs explicit barriers | `c37110d0aa` |
| `VK_EXT_external_memory_host` direct VRAM | fenced | Torn GPU reads (flicker); saves <0.5 ms/5 s vs a ≥1 ms/frame bar | `59b85d1e86`, `accfcac580` |
| Prune throttle keyed on `pg->frame_time` | fixed | No flips during loads → black-screen freeze. Key on `QEMU_CLOCK_REALTIME` | `f156b009a1` |
| Texture hash: 192-byte sampling; early dirty-range flush; incremental on misaligned | closed / reverted / fixed | Missed FMV changes; bitmap cleared early; `test_and_clear` stole a neighbor's dirty bit | `5ebda9a33c`, `76f4e2072b` |
| Surface-range binary search on `end` | closed | `end` isn't monotonic in start order | `e71a85f643` |
| Depth export (CPU readback, `vkExportMetalObjectsEXT`) | reverted | 2 ms/frame; the export deadlocks MoltenVK | `248c4d282f` |
| Compute queue; barrier batching, GPU S3TC, MetalFX input ring, spec constants, async pipeline compile | closed | `queueCount=1`; the rest are ~0 in nsprof or a false premise (reopen at ≥1 ms/frame) | ledger Settled |
| Freeing GPU-visible state "not in the recording CB" (surfaces, vertex mirror, texture/sampler LRU) | fixed | Finish is pipelined; destruction waits on the retirement watermark | `a045dfc780`, `215f243be7`, `d9c90165d1` |
| MoltenVK prefill=2 | fixed: keep **0** | Pink tiles, enabled the AGX crash, slower. Finder `LSEnvironment` ≠ shell env hid it | `531122e8aa`, `166999fc60` |
| Visibility-buffer AGX crash | fixed | Query-pool guard (4096) + empty primer query at CB begin. Keep both | `bfcf38843f`, `bb33ccaa2e` |
| Pink flash (torn texture snapshot uploaded) | fixed | Retry the copy until clean | `8b1b934045` |
| Surface-callback reuse pool | reverted | −0.66 fps 3/3 despite clean gates | `bb3fc91080` |
| MetalFX real depth | dark | Azurik's 2×-wide AA → no zeta matches → synthetic fallback | `XEMU_MFX_REAL_DEPTH` |
| **Present / MetalFX** | | | |
| Async MetalFX under GL presentation | reverted | GL↔Metal can't fence; now lives in the Metal backend | `ea23058e1e` |
| Vblank tied to host refresh | reverted | The host timer *is* the guest VBLANK IRQ → game speed scales with Hz | `649dd265a0` |
| Push-present strict FIFO consume | fixed | Backlog dropped real frames ("double playing"); bounded debt | `XEMU_PUSH_DEBT=0` |
| Drawable-first present order | reverted | +0.50 fps, under noise | `1a8f466e2b` |
| **DSP / APU** | | | |
| DSP JIT EPI_NO_PC (skip the PC-mismatch exit) | closed | Sentinel: 9.75% legitimate divergence (mode-6 parmoves, REP/DO); fixed the classifier instead | `d0e1f0b27d`, `ca7fe51a0b` |
| DSP JIT `cur_inst` preset skip | fenced | A hidden runtime reader (`op=0x001000`); `XEMU_DSP_JIT_SENTINEL`/`_FORCE` kept | `aa9eed38e8` |
| A/B, then X/Y register pinning | fixed | Direct-slot parmove writes skipped the pin; new pins run `XEMU_DSP_JIT_PIN_AUDIT=1` | `bf82c37fba`, `54d1543d50` |
| Inline diff validator | fixed | Held `d->lock` → startup hang; now async, one check per translation | `5c093dace1` |
| JIT on the EP core | fixed | P-space overlays → retranslation churn; per-core throttle | `21e9045344`, `XEMU_DSP_JIT_NO_THROTTLE=1` |
| Merges: a rename rewrote `getenv` strings; `use_dsp_jit` flipped to false | fixed | Grep `getenv(` after renames; diff `config_spec.yml` after every merge | `21e9045344`, `2e0aad3e18` |
| Parmove+ALU fusion | closed | 0 instructions saved in every source class; x28 is the SR pin | ledger Settled |
| APU `rate==1.0` resampler fast path | reverted | No silence pad on drain → `voice_process` spins | `68d5afe7fa` |
| Voice-register cache, SE2FE re-notify skip, voice writeback batching | closed | DMA and the guest use RAM voice state; no write hook | ledger |
| HRTF hand-written NEON | closed | The autovectorized FIR is faster | `aff8c229e9` |
| APU thread resumed by reset during loadvm | fixed | Resume only if `runstate_is_running()` | `d6da581d4b` |
| **TCG / x87 / CPU** | | | |
| floatx80 union overlay | closed | Layout mismatch; segfaults | — |
| FSIN/FCOS as thin libm helpers | reverted | Crackly audio; the floatx80 helper was an implicit barrier | `778db912ed` |
| W^X shortcuts (`..._with_callback_np`, TLS-cached state) | reverted / closed | Break nested writes; bracket every JIT-code store | `d4f2e3e226` |
| Cross-TB FPCR elision through a TCG global | closed | Globals reload per TB | `aff8c229e9` |
| HLT BSOD recovery (force IF=1) | reverted | Masked a blocked thread; cured nothing | `6eea148914` |
| Caching JIT write-protect flips | reverted | Parity; the 9.7% was sampler skid | ledger |
| Return-address ring; jump cache at 16 bits; L2 victim tier; ret memo at 13 bits | replaced / reverted | Per-depth key thrashes (the eip ret memo shipped, `XEMU_RAS=0`); −1.10 fps; 11.2% hits; −0.23 fps | `02364e6960` |
| TB byte-range invalidation filter | closed | Trap storm; sub-page tracking shipped instead (+14-25%) | `b8ee034131`, `XEMU_SUBPAGE_DIRTY=0` |
| xpage full-flush backstop | fixed | Flushes aren't rare (139 per 20 s streaming) → the v0.11 lag; unlink registry | `XEMU_XPAGE_UNLINK=0` |
| Superblock seams, then region formation | closed | −1.6% to −4.5%: TCG spills before any exiting branch | `ee1de9f654`, `d65fc5f83e` |
| SSE on NEON, strict brackets | shipped | Parity until the leave bracket went sticky (+1.90 fps) | `XEMU_SSE_HOST=0` |
| Arm (b) store prefilter | shipped | fps flat, draws/s +12% | `XEMU_SUBPAGE_FAST=0` |
| `notdirty_write` dirty-check fps claim | closed | Base collapsed 34-57×; the code stays | `XEMU_DIRTY_FAST=0` |
| x87 clean write-back elision | reverted | −0.23 fps 3/3 | `987a2aae24` |
| `-fzero-call-used-regs=skip` | closed | −411 kB `__text`, fps 3+/2− then 3+/5−. Retest via `CFLAGS` | `7aa746a952` |
| COMISS lowering, dirty-clear coalescing, xpage spanning dests | closed / parked | Census empty; <1% of vCPU; under 1M/s | ledger |
| **PFIFO / threading** | | | |
| PFIFO untimed wait, descriptor bind-skip | re-added | Innocent; a new `pfifo_kick` site must hold `pfifo.lock` | `a005bb1560` |
| Freeze hunt by reverting suspects | lesson | Build the discriminating diagnostic first | `XEMU_PFIFO_HEARTBEAT=1` |
| BQL event batching; `can_fifo_access` hoist | closed | Starves the vCPU; the lock drops mid-loop → `ERROR_CALL` | ledger |
| BQL-free MMIO (PFB/USER/PGRAPH/APU-VP) | shipped | Parity, less jitter; the remaining blocks are closed | `XEMU_MMIO_BQL=1` |
| Kick suppression, semaphore kick, ImGui lock | closed | Empty population / wrong premise / harmless | ledger Settled |
| Post-loadvm OHCI/`flatview` SIGSEGV | fixed | A freed `MemAccessCallback`, not OHCI; RCU-deferred free | `4161ab5f24`, `13afd5f244` |
| **Build / CI / release / harness** | | | |
| Release CI portability (CRC32, CXXFLAGS, SDK-26 guard, Windows LTO, FILEVERSION, POSIX-only input pipe) | fixed | The CI matrix is the test; tagged builds hit new paths | `378915e067`, `497c1ff3e6` |
| Deleting or re-pushing a published tag | incident | The release flips to draft; tags are immutable | `docs/RELEASING-macos.md` |
| build.sh shell traps (PGO merge-skip, `set -e` glob, pipefail) | fixed | Test edits under `bash -e` | `fcb874d256` |
| `-a x86_64` failure chain | fixed | Run `file` on the artifact | `e5134cc906`, `ae94399cd3` |
| cp+codesign bundle refresh | incident | Dies at dyld; replay the `package_macos` fixups | `8db8f61b4d` |
| Three refuters at once | incident | Wedges boot; use two at most, or `XEMU_DSP_JIT_DIFF=10` | ledger |

## GPU frame-cost campaign (2026-10-13)
- Anchor `azurik-heavy-f1` (replaced F8 on 2026-10-11): ~679 draws/flip, ~28.5 passes/flip, fence_wait ~3.9 ms/flip, `pipeline_gen`≈0, 28.9 fps on a loaded box. No quiet-box envelope yet. `azurik-heavy-w2`: 323 draws/flip, ~22 passes, 23.3 fps, cost not per-draw (`../fixtures/README.md`).
- The 2026-07-04 F5 run (`038f596332`, `9b625f1126`) killed every mechanism. A1 subrange merge: zero opportunity. A2 cross-block merge: 10.6% candidates at ~0 PFIFO cost per call, since prefill=0 encodes on MoltenVK's thread. B per-draw CPU: <1 ms/flip. C pass cuts: savings land in ~14 ms/flip of GPU slack (clears are the top cause, 5.0/flip).
- `da_multi_subrange` = 0 on heavy-f1 too, so A1 is closed. D `vkCmdDrawMultiEXT`: never wire it; MoltenVK unrolls it per draw. Re-check on a pin bump.
- The scene is guest-CPU bound. vCPU% says nothing (the title busy-polls); PFIFO starvation is the signal.
- If A2 is ever built: compare inline vertex-attribute values (no dirty flag), and never merge across a report or zpass-enable boundary.
- Next: two quiet-box Phase 0 captures plus the merge/pass-cause nsprof counters on heavy-f1, then Metal System Trace on heavy-f1 and W2. Reopen A2/C only if GPU time ≈ frame time.

## Open frontier (ranked)
1. **PFIFO doorbell lock wait.** On heavy-f1, `user_write` blocked on `pfifo.lock` is ≈9.6% of vCPU samples, and no list has it yet. Open questions: what PFIFO holds the lock for, and can the put-pointer store publish without it?
2. **x87 hard-float helpers, ≈24% of vCPU.** No candidate yet; derive one from the 2026-10-11 profile, not the 2026-08 audit.
3. **Is W2 GPU-bound?** Fewest draws, slowest anchor. Run Phase 2.
4. **Real-hardware validation debt.** `docs/windows-gating-audit.md`'s list, geometry-shader vs CPU-emulated primitive parity, non-Apple aarch64 `XEMU_DSP_JIT_DIFF`, `XEMU_SSE_HOST=2` on real x86_64, a lockless-MMIO soak.
5. **Real depth on super-width titles.** Resolve the zeta to scanout size at flip so `XEMU_MFX_REAL_DEPTH=2` engages. A quality change, ~0 fps.
6. **Apple default for `audio.dsp_jit.enabled`** (still false). Wants an `XEMU_APU_PROF` receipt.
7. **PAL 50 Hz guest vblank.** Present no longer uses the vblank timer. Needs region detection, an NTSC no-change A/B and a PAL fixture.
8. **BINK via VideoToolbox.** Needs theory: there's no hook, and Azurik's movie never touches PVIDEO.
9. **GL renderer under the Metal window.** Compatibility only.
10. **Unexplained white window** in 2 of 13 warm `azurik-stream-e2` walks while nsprof looked normal. Needs an in-app screenshot hook.

Parked, with bars: ranged TLB flush (~0.2% of vCPU vs a 1.5% gate); pipeline prewarm (cache misses only; reopen on a ≥16 ms compile that survives a warm rerun); dynamic rendering (no measured upside); MTLResidencySet (MoltenVK has none).

## Benchmarking traps
- Scene identity: check draws/flip in both arms (a ~3 draws/flip menu baseline once faked "−30%"). Select intervals by content, not log position.
- Composition shift: Azurik spends freed CPU on more effect draws. When the arms' draws/flip differ, compare draws/s.
- A loaded box or leftover harness loops: wait for load <4 and run `pgrep -fl xemu`. A 3+/2− sign split is noise.
- Warm vs cold caches: shared pipeline/shader caches hide first-visit hitches (`movement-probe.sh --isolate-caches`). Static A/Bs miss streaming costs.
- Wrong binary or config: a running process keeps its old binary; Finder and shell launches get different environments; harnessing `dist/xemu.app` loses the config on rebuild.
- Stale instruments: re-measure proxies on today's binary. Suspend samplers park on barriers (`pthread_jit_write_protect_np` ~10% is skid). Unfocused windows present unpaced.

Full pre-2026-10-13 sagas: `git show ad569d78c5:.claude/skills/<old-skill-name>/SKILL.md` for `xemu-failure-archaeology`, `xemu-gpu-frame-campaign`, `xemu-frontier-and-positioning`, `xemu-research-methodology`.
