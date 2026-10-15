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
| Occlusion-rework intermediates (in-pass queries + sync drains; submit per report) | reverted | 6.86 / ~11.2 fps vs 15.78. Placement and delivery are one policy; shipped form: 38.57 fps, passes 376→14. Never re-add per-draw pass teardown (TBDR) | `6bfbc22863` |
| CPU-side wait/fence elimination for streamed vertices (byte-exact refine; per-flight 128 MiB mirrors) | closed | fps flat twice: the wait is GPU time changing call site. The byte-exact refine shipped alone (+5.4%) | `099efcb734`, `268daf69ac` |
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
| Push-present strict FIFO consume | fixed | Backlog dropped real frames ("double playing"); bounded debt | `78c4573dfb` |
| Drawable-first present order | reverted | +0.50 fps, under noise | `1a8f466e2b` |
| **DSP / APU** | | | |
| DSP JIT EPI_NO_PC (skip the PC-mismatch exit) | closed | Sentinel: 9.75% legitimate divergence (mode-6 parmoves, REP/DO); fixed the classifier instead | `d0e1f0b27d`, `ca7fe51a0b` |
| DSP JIT `cur_inst` preset skip | reverted | A hidden runtime `cur_inst` reader (`op=0x001000`); harness removed, `git show 1f444fa272 4668947cc9` to reopen | `aa9eed38e8` |
| A/B, then X/Y register pinning | fixed | Direct-slot parmove writes skipped the pin; new pins run `XEMU_DSP_JIT_PIN_AUDIT=1` | `bf82c37fba`, `54d1543d50` |
| Inline diff validator | fixed | Held `d->lock` → startup hang; now async, one check per translation | `5c093dace1` |
| JIT on the EP core | closed | P-space overlays → retranslation churn (~1 per 10 block executions); the EP stays on the interpreter, the GP keeps the churn throttle | `21e9045344`, `d287c6cb23` |
| Merges: a rename rewrote `getenv` strings; `use_dsp_jit` flipped to false | fixed | Grep `getenv(` after renames; diff `config_spec.yml` after every merge | `21e9045344`, `2e0aad3e18` |
| Parmove+ALU fusion | closed | 0 instructions saved in every source class; x28 is the SR pin | ledger Settled |
| Inline DO-loop `postexecute_update_pc` in the JIT epilogue | deferred | The GP JIT is ~2.6% of the vCPU and the helper ~0.5%: under A/B resolution for a pin-reload risk | not committed |
| APU `rate==1.0` resampler fast path | reverted | No silence pad on drain → `voice_process` spins | `68d5afe7fa` |
| Voice-register cache, SE2FE re-notify skip, voice writeback batching | closed | DMA and the guest use RAM voice state; no write hook | ledger |
| HRTF hand-written NEON | reopened, shipped | The gather-over-taps form lost to autovectorization; lanes over outputs on settled frames (96% of heavy-f1 calls) won, 2.9% fewer det cycles per frame | `aff8c229e9`, HRTF settled path |
| Vectorizing the unsettled HRTF path | closed | After the settled fast path, VP is 1.2% of the vCPU on stream-e2 and HRTF leaves the top list | `75a295fdbd` |
| EP DSP on the JIT (retested 2026-10-15) | closed | Still churns: 108,788 EP translations per 1200 heavy-f1 frames, Minsns/frame 125.4 vs 120.7; overlays are re-DMAed with different code. Reopen only with a P-space-content-keyed translation cache | `d287c6cb23` |
| APU thread resumed by reset during loadvm | fixed | Resume only if `runstate_is_running()` | `d6da581d4b` |
| **TCG / x87 / CPU** | | | |
| floatx80 union overlay | closed | Layout mismatch; segfaults | — |
| FSIN/FCOS as thin libm helpers | reverted | Crackly audio; the floatx80 helper was an implicit barrier | `778db912ed` |
| W^X shortcuts (`..._with_callback_np`, TLS-cached state) | reverted / closed | Break nested writes; bracket every JIT-code store | `d4f2e3e226` |
| Cross-TB FPCR elision through a TCG global | closed | Globals reload per TB | `aff8c229e9` |
| HLT BSOD recovery (force IF=1) | reverted | Masked a blocked thread; cured nothing | `6eea148914` |
| icount warp from the main loop | fixed | Warped from a clock missing the slice's instructions: a timer fired late by host scheduling, ~3% of `vanilla-exit` replays diverged. cc5d72113f only made it rarer | `icount_vcpu_warps` |
| Caching JIT write-protect flips | reverted | Parity; the 9.7% was sampler skid | ledger |
| Return-address ring; jump cache at 16 bits; L2 victim tier; ret memo at 13 bits | replaced / reverted | Per-depth key thrashes (the eip ret memo shipped); −1.10 fps; 11.2% hits; −0.23 fps | `02364e6960` |
| TB byte-range invalidation filter | closed | Trap storm; sub-page tracking shipped instead (+14-25%) | `b8ee034131`, `683b919631` |
| xpage full-flush backstop | fixed | Flushes aren't rare (139 per 20 s streaming) → the v0.11 lag; unlink registry | `f7d9cc2ff1` |
| Full TLB flush per MemAccessCallback change (~190/s in normal mode) | fixed | Its old 0.2%-of-vCPU estimate missed the refills and the rebuilt jump cache and near-return memo: 2.3% of host instructions per frame (det offscreen); now a ranged refill | `tlb_flush_host_range` |
| Superblock seams, then region formation | closed | −1.6% to −4.5%: TCG spills before any exiting branch | `ee1de9f654`, `d65fc5f83e` |
| SSE on NEON, strict brackets | shipped | Parity until the leave bracket went sticky (+1.90 fps) | `d76ddd7b20` |
| Arm (b) store prefilter | shipped | fps flat, draws/s +12% | `ede35b84f9` |
| `notdirty_write` dirty-check fps claim | closed | Base collapsed 34-57×; the code stays | `4e7531cfae` |
| x87 clean write-back elision | reverted | −0.23 fps 3/3 | `987a2aae24` |
| Inline FUCOMI on the write-through path | closed | Helper gone, det heavy-f1 88.25 vs 88.08 fps: its sampled share was the shared ld80f conversions | not committed |
| Normal mode: drop the store barriers too, or weaken them to `dmb ishst` | closed | +1.3% more frames/Gcycle than load-only (det, 5 pairs), and OHCI needs store-store order; `ishst` measured no cheaper than `ish` | `XEMU_GUEST_MO=1` |
| `-fzero-call-used-regs` off on macOS arm64 | reopened, shipped | The 2026-08 fps sign tests (3+/2−, 3+/5−) couldn't resolve it; det counters did: host instructions −3.2% per frame; cycles −1.2% (load 17-28) to −1.7% median (load 39-78); `__text` −769 kB | `meson.build` hardening |
| COMISS lowering, dirty-clear coalescing, xpage spanning dests | closed / parked | Census empty; <1% of vCPU; under 1M/s | ledger |
| Chaining icount step TBs (step→step `goto_tb`, non-PCREL step TBs) | closed | Loop dispatches per cut 22→2.6, but Minsns/frame 123.33→123.30 and Mcycles/frame flat: the loop round trip is ~0.2% of the det vCPU (an earlier "+1%" was wall-fps noise). Trap: a non-`CF_PCREL` TB skips the EIP update on a linked exit, so it must never chain into a `CF_PCREL` TB | not committed |
| REP bulk-helper minimum run 4 / 12 / 16 / 32 | closed | Minsns/frame 121.5 / 121.4 / 121.4 / 121.9 vs 121.2 at 8 (helper off: 123.4); 8 shipped | `51e7e7fe1e` |
| Guest spin/idle skipping under icount | closed | Spinning is <0.5% of guest instructions (heavy-f1 0.38% at 0x13a136); halted: stream-e2 1.24%, cold boot 0.51% | not committed |
| SHUFPS non-broadcast as four lane moves | closed | Minsns/frame −0.16% (4/4) but cycles leaned worse (~8/20 better): likely a store-forwarding stall (four narrow stores, then the helper's 128-bit load). The broadcast form shipped | `9947de1db3` |
| Guarded inline `cc_compute` for flag reads before any flag setter in a block | closed | Translation-time cc_op predicts runtime 97.6-99.98%; the guard removed 97% of helper calls (47M→1.3M per 1200 frames) but only 0.19% of instructions, lost to spills around the guard branch. Only keying TBs on entry cc_op would pay (risky: chaining, fork lookups) | not committed |
| SSE packed-single as vector TCG ops + an MXCSR hflag | closed for now | mulps+addps are 0.8-2.0% of the det vCPU, shufps 0.1-0.2%; needs new TCG vector-FP opcodes for at most 1-1.5% | not committed |
| **PFIFO / threading** | | | |
| Report delivery under `pgraph.lock` instead of `pfifo.lock`, plus doorbell broadcast after unlock | closed | heavy-f1 17.15 -> 10.67 fps (2/2): the fence wait then blocks the vCPU's PGRAPH reads and surface callbacks instead of the doorbell | 2026-10-14, not committed |
| PFIFO untimed wait, descriptor bind-skip | re-added | Innocent; a new `pfifo_kick` site must hold `pfifo.lock` | `a005bb1560` |
| Freeze hunt by reverting suspects | lesson | Build the discriminating diagnostic first | `XEMU_PFIFO_HEARTBEAT=1` |
| BQL event batching; `can_fifo_access` hoist | closed | Starves the vCPU; the lock drops mid-loop → `ERROR_CALL` | ledger |
| BQL-free MMIO (PFB/USER/PGRAPH/APU-VP) | shipped | Parity, less jitter; the remaining blocks are closed | `ee9100e538`, `4b92b2bc40`, `a477ad9f91` |
| Kick suppression, semaphore kick, ImGui lock | closed | Empty population / wrong premise / harmless | ledger Settled |
| Post-loadvm OHCI/`flatview` SIGSEGV | fixed | A freed `MemAccessCallback`, not OHCI; RCU-deferred free | `4161ab5f24`, `13afd5f244` |
| **Build / CI / release / harness** | | | |
| Release CI portability (CRC32, CXXFLAGS, SDK-26 guard, Windows LTO, FILEVERSION, POSIX-only input pipe) | fixed | The CI matrix is the test; tagged builds hit new paths | `378915e067`, `497c1ff3e6` |
| Deleting or re-pushing a published tag | incident | The release flips to draft; tags are immutable | `docs/RELEASING-macos.md` |
| build.sh shell traps (PGO merge-skip, `set -e` glob, pipefail) | fixed | Test edits under `bash -e` | `fcb874d256` |
| `-a x86_64` failure chain | fixed | Run `file` on the artifact | `e5134cc906`, `ae94399cd3` |
| cp+codesign bundle refresh | incident | Dies at dyld; replay the `package_macos` fixups | `8db8f61b4d` |
| Three refuters at once | incident | Wedges boot; use two at most, or `XEMU_DSP_JIT_DIFF=10` | ledger |
| Bundle binary run with `-machine help` and no `-config_path` (2026-10-15) | incident | The exit-time save rewrote the personal `xemu.toml` (same settings). Even `-help` runs need `-config_path` | — |

## GPU frame-cost campaign (2026-10-13)
- Anchor `azurik-heavy-f1` (replaced F8 on 2026-10-11): ~679 draws/flip, ~28.5 passes/flip, fence_wait ~3.9 ms/flip, `pipeline_gen`≈0, 28.9 fps on a loaded box. `azurik-heavy-w2`: 323 draws/flip, ~22 passes, 23.3 fps, cost not per-draw (`../fixtures/README.md`).
- The 2026-07-04 F5 run (`038f596332`, `9b625f1126`) killed every mechanism. A1 subrange merge: zero opportunity. A2 cross-block merge: 10.6% candidates at ~0 PFIFO cost per call, since prefill=0 encodes on MoltenVK's thread. B per-draw CPU: <1 ms/flip. C pass cuts: savings land in ~14 ms/flip of GPU slack (clears are the top cause, 5.0/flip).
- `da_multi_subrange` = 0 on heavy-f1 too, so A1 is closed. D `vkCmdDrawMultiEXT`: never wire it; MoltenVK unrolls it per draw. Re-check on a pin bump.
- The scene is guest-CPU bound. vCPU% says nothing (the title busy-polls); PFIFO starvation is the signal.
- If A2 is ever built: compare inline vertex-attribute values (no dirty flag), and never merge across a report or zpass-enable boundary.
- Next: two Phase 0 captures (load quoted) plus the merge/pass-cause nsprof counters on heavy-f1, then Metal System Trace on heavy-f1 and W2. Reopen A2/C only if GPU time ≈ frame time.

## Open frontier (ranked)
1. **Reports safety valve (`REPORTS_SUBMIT`).** The doorbell no longer waits on it (lockless doorbell, 2026-10-15), but it still fires 6-30 times per flip on heavy-f1: Azurik issues ~600 GET_REPORTs per flip and the FIFO idles ~90 times per frame with one pending, so each idle 300 µs after the first one submits, splits the pass and waits the other slot's fence. Under GPU contention it feeds back (33-77 per flip, 4-8 fps runs). Not a free knob: a 100 ms budget drops draws/flip 693 → 601 and fps, so the title consumes reports mid-frame. Open: a submit that never waits on the PFIFO thread.
2. **x87 helpers.** The inline cache never ran after 3eb1b2de10 (its `TCG_TARGET_HAS_fpu` gate is private to tcg/) and its fork layers are removed; the exact write-through path (`4d2466d106`) replaced it in both modes. What is left are fucom*/fcomi, fist*, transcendentals and PC=single blocks, ≈4% of the deterministic vCPU.
3. **APU tick remainder.** GP DSP JIT ~2% plus EP interpreter ~1.5% of the deterministic vCPU. The EP stays off the JIT until a P-space-content-keyed translation cache exists (DSP/APU dead ends).
4. **Short REP MOVSD.** Runs of 1-3 dwords stay in the per-element loop; the hottest site (eip 0x12378b, 6.2M runs, 3 dwords on average) is ~0.6% of the vCPU. Wants a cheaper inline loop, not the helper.
5. **Is W2 GPU-bound?** Fewest draws, slowest anchor. Run Phase 2.
6. **Real-hardware validation debt.** `docs/windows-gating-audit.md`'s list, geometry-shader vs CPU-emulated primitive parity, non-Apple aarch64 `XEMU_DSP_JIT_DIFF`, `XEMU_SSE_HOST=2` on real x86_64, a lockless-MMIO soak.
7. **Real depth on super-width titles.** Resolve the zeta to scanout size at flip so `XEMU_MFX_REAL_DEPTH=2` engages. A quality change, ~0 fps.
8. **Apple default for `audio.dsp_jit.enabled`** (still false; the APU numbers here were all measured with it on). Wants an `XEMU_APU_PROF` receipt.
9. **PAL 50 Hz guest vblank.** Present no longer uses the vblank timer. Needs region detection, an NTSC no-change A/B and a PAL fixture.
10. **BINK via VideoToolbox.** Needs theory: there's no hook, and Azurik's movie never touches PVIDEO.
11. **GL renderer under the Metal window.** Compatibility only.
12. **Unexplained white window** in 2 of 13 warm `azurik-stream-e2` walks while nsprof looked normal. Needs an in-app screenshot hook.

Parked, with bars: pipeline prewarm (cache misses only; reopen on a ≥16 ms compile that survives a warm rerun); dynamic rendering (no measured upside); MTLResidencySet (MoltenVK has none).

## Benchmarking traps
- Scene identity: check draws/flip in both arms (a ~3 draws/flip menu baseline once faked "−30%"). Select intervals by content, not log position.
- Composition shift: Azurik spends freed CPU on more effect draws. When the arms' draws/flip differ, compare draws/s.
- A loaded box or leftover harness loops: run `pgrep -fl xemu`, quote the load and add pairs rather than waiting. A 3+/2− sign split is noise.
- Warm vs cold caches: shared pipeline/shader caches hide first-visit hitches (`movement-probe.sh --isolate-caches`). Static A/Bs miss streaming costs.
- Wrong binary or config: a running process keeps its old binary; Finder and shell launches get different environments; harnessing `dist/xemu.app` loses the config on rebuild.
- Cheap-helper deletions: removing cheap, high-IPC helper calls shows in Minsns/frame (1-2%) but not in Mcycles/frame on this box, and heavy load widens the Mcycles/frame spread (28-40): interleave and add pairs.
- Stale instruments: re-measure proxies on today's binary. Suspend samplers park on barriers (`pthread_jit_write_protect_np` ~10% is skid). Unfocused windows present unpaced.

Full pre-2026-10-13 sagas: `git show ad569d78c5:.claude/skills/<old-skill-name>/SKILL.md` for `xemu-failure-archaeology`, `xemu-gpu-frame-campaign`, `xemu-frontier-and-positioning`, `xemu-research-methodology`.
