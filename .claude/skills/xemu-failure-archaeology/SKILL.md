---
name: xemu-failure-archaeology
description: >-
  The chronicle of every settled battle in the xemu-macos fork: failed
  experiments, reverts, fenced-off designs, regressions, and incidents, each
  recorded as symptom -> root cause -> evidence -> status. Load this skill
  BEFORE attempting or proposing any optimization or design change (it may be
  a known dead end: dynamic rendering, VK_EXT_external_memory_host, MoltenVK
  prefill, vblank retiming, EPI_NO_PC, cur_inst skip, HLT recovery, rate==1.0
  resampler, BQL batching, spec constants, GPU S3TC, barrier batching,
  -fzero-call-used-regs hardening removal, notdirty_write dirty-check
  pricing...);
  before reverting or retrying anything; when investigating the history of a
  regression, crash, freeze, or pink/magenta artifact; when asking "has this
  been tried?", "why was X reverted?", "why is this flag/guard here?"; when a
  git log revert pair needs its story; or when adding a new failed-experiment
  record. Contains full sagas: pink-tile, visibility-buffer crash (two acts),
  streamed-vertex campaign, occlusion rework, level-transition freeze, DSP JIT
  pin/PC battles, release-CI hardening, tag-deletion incident, refuter-stacking
  boot wedge, and benchmarking traps.
---

# xemu failure archaeology — the settled battles

The fork's institutional memory of what did NOT work, what was reverted, what
is fenced off, and what was proven not worth doing — so nobody re-fights a
settled battle or silently re-attempts a fenced-off design. Every entry is
grounded in the repo: `README.md` "Failed / reverted experiments" (~line 803
as of 2026-07-11), commit messages, and dated measurements.

Repo: `/Users/michaelsrouji/Documents/Xemu/tools/xemu-macos`, branch
`macos-optimizations`. Commit messages in this fork are unusually rich — for
any entry below, the full story is one command away. Read it before acting:

```bash
git show --no-patch --format='%h %ad%n%s%n%n%b' --date=short <hash>
```

**Status vocabulary** (used in every entry):

| Status | Meaning |
|---|---|
| shipped-after-fix | Attempt initially failed/regressed; a corrected form is now in the tree |
| reverted | Landed, regressed, removed. Do not re-land as-is |
| fenced-off | Removed/disabled, but plumbing or a harness is kept in-tree for a stated re-entry path |
| settled-negative | Evaluated (sometimes without landing) and closed with data. Don't reopen without new evidence |
| incident | Process/operational failure; the lesson is a rule, not code |

Every entry ends with **Reopen if** — the falsifiable condition under which
re-attempting is legitimate. If you cannot meet the condition, do not reopen.

## Index

| # | Entry | Status |
|---|---|---|
| 1.1 | Occlusion/report rework + its two regressed intermediates | shipped-after-fix |
| 1.2 | Streamed-vertex stall campaign (GPU-bound conclusion) | settled-negative |
| 1.3 | Per-flight vertex-RAM mirrors | reverted |
| 1.4 | Level-transition freeze (defensive-revert cascade) | shipped-after-fix |
| 1.5 | VK_KHR_dynamic_rendering full lowering (B2) | fenced-off |
| 1.6 | VK_EXT_external_memory_host direct-VRAM (α2/α3) | fenced-off |
| 1.7 | Surface-expiry throttle keyed on frame_time | shipped-after-fix |
| 1.8 | Surface-range binary search on `end` | settled-negative |
| 1.9 | Texture-hash lineage (quick hash, dirty-range flush, misaligned) | mixed |
| 1.10 | Depth export (CPU readback / vkExportMetalObjectsEXT) | settled-negative |
| 1.11 | Separate compute queue | settled-negative |
| 1.12 | Measured "not worth it" set (barriers, S3TC, spec constants, input ring, ext-mem-host bar, async pipeline compile) | settled-negative |
| 1.13 | Invalid-surface destruction raced pending submissions | shipped-after-fix |
| 1.14 | GPU frame-cost campaign menu (A1/A2/B/C/D) on 2026-07 fixtures | settled-negative |
| 1.15 | Eager report submit (front-run the poll stall) | settled-negative |
| 1.16 | Vertex copy-on-conflict transient remap (XEMU_VTX_TRANSIENT) | settled-negative |
| 1.17 | JIT write-protect flip caching (barrier-skid mirage) | settled-negative |
| 1.18 | Per-depth return-address ring -> eip-keyed ret memo | superseded-shipped |
| 1.19 | Sticky host-FPU bracket: SSE parity -> +1.9 | shipped |
| 1.20 | L2 victim jump-cache | settled-negative (instrument-killed) |
| 1.21 | Texture/sampler LRU eviction raced pending submissions | shipped-after-fix |
| 1.22 | TB range-check re-add (XEMU_TB_RANGE_INV) | settled-negative (instrument-killed) |
| 1.23 | Sub-page code dirty tracking (+14-25% fps; model undershot 15×) | shipped |
| 1.24 | xpage full-flush backstop = the v0.11 "new-area lag" (139 tb_flushes/20 s; unlink registry fix) | shipped-after-fix |
| 7.5 | BQL-free MMIO dispatch (PFB/USER lockless_io) | shipped (fps-parity, jitter win) |
| 8.4 | build.sh PGO merge-skip + set -e ls-glob CI red | shipped-after-fix |
| 8.5 | -a x86_64 cross-build failure chain (stale build/, arch-blind MoltenVK tiers, unguarded MetalFX refs) | shipped-after-fix |
| 8.6 | grep -m1 under pipefail poisoned the MoltenVK provenance version | shipped-after-fix |
| 8.7 | `-fzero-call-used-regs=skip` hardening removal (−411 kB `__text`, fps sign-mixed) | settled-negative (fenced-off dark) |
| 2.1 | Pink-tile corruption (MoltenVK prefill) | shipped-after-fix |
| 2.2 | Visibility-buffer crash, two acts | shipped-after-fix |
| 2.3 | Pink-flash (torn texture snapshot) | shipped-after-fix |
| 3.1 | Async MetalFX under GL presentation | reverted, landed elsewhere |
| 3.2 | Vblank cadence aligned to host refresh | reverted |
| 3.3 | Push-present strict-FIFO consume ("frames double playing" judder; bounded-debt catch-up fix) | shipped-after-fix |
| 4.1 | DSP JIT round-4 EPI_NO_PC | settled-negative |
| 4.2 | DSP JIT round-4 cur_inst preset skip | fenced-off |
| 4.3 | A/B accumulator pinning (revert → audit → reapply) | shipped-after-fix |
| 4.4 | Chain repatch outside the JIT-write window (W^X) | shipped-after-fix |
| 4.5 | Diff-validator starvation → async validator | shipped-after-fix |
| 4.6 | EP retranslation churn → auto-throttle | shipped-after-fix |
| 4.7 | Upstream-merge env-var rename collateral | shipped-after-fix |
| 5.1 | APU rate==1.0 resampler fast path | reverted |
| 5.2 | Voice register `__thread` cache | settled-negative |
| 5.3 | HRTF hand-NEON | settled-negative |
| 5.4 | SE2FE_IDLE_VOICE re-notify skip | settled-negative |
| 5.5 | APU / snapshot-load race | shipped-after-fix |
| 6.1 | floatx80 union overlay on ARM64 | settled-negative |
| 6.2 | FSIN/FCOS libm-helper inline | reverted |
| 6.3 | pthread_jit_write_with_callback_np (A3) | reverted |
| 6.4 | TLS-cached pthread_jit_write_protect_np | settled-negative |
| 6.5 | Cross-TB FPCR elision | settled-negative |
| 6.6 | HLT BSOD recovery | reverted |
| 6.7 | Superblock seam-following (M1 3.7% capture; M2 both policies −1.6/−2.3% throughput; spill-before-exit theory) | settled-negative (mechanism fenced-off dark) |
| 6.8 | Region/diamond formation + recorded-label liveness elision (census gate passed 21.4%; both windows −4.5%/−0.69 3/3; drain-demotes 72%/64% — THE closing campaign) | settled-negative (full stack fenced-off dark) |
| 6.9 | Arm (b) promotion: prediction sized on a 20x-undercounting proxy; fps masked a +12% throughput win via effect-load feedback | shipped-after-proof |
| 6.10 | Audit §1.2's benefit base had already collapsed 34-57x (proxy collapse, mirror of 6.9) | settled-negative (claim killed, code kept) |
| 7.1 | PFIFO untimed wait (A1): revert → proof-backed re-add | shipped-after-fix |
| 7.2 | Descriptor bind-skip (C1): same arc | shipped-after-fix |
| 7.3 | BQL event batching | settled-negative |
| 7.4 | `can_fifo_access` hoist out of pusher loop | settled-negative |
| 8.1 | Release-CI hardening: five pre-existing portability bugs | shipped-after-fix |
| 8.2 | XEMU_INPUT_PIPE Windows build break | shipped-after-fix |
| 8.3 | Tag-deletion incident | incident |
| 9.1 | The "-30%" invalid-baseline misread | incident |
| 9.2 | Bimodal attract-reel scenes | incident |
| 9.3 | Portable-config wipe / harness at `dist/xemu.app` | incident |
| 9.4 | OS-level input injection false-verify | incident |
| 9.5 | Running process keeps its old binary | incident |
| 9.6 | Bare cp+codesign bundle refresh dies at dyld | incident |
| 9.7 | Bench gate counted boot-menu intervals (cold-vs-warm boot asymmetry) | shipped-after-fix |
| 9.8 | Helper appends to a live process's stdout log get overwritten (non-append fd) | incident |
| 9.9 | Bench harness socket under a deep outdir blows the 104-byte AF_UNIX cap — all runs DEAD "no monitor socket" | shipped-after-fix |
| 9.10 | Three refuters in one session wedge the guest at boot (shadow-instrumentation cost on `d->lock`) | incident |

## When NOT to use this skill

- **Triaging a live symptom** (first checks, discriminating experiments): the
  `xemu-debugging-playbook` skill. Its rows point back here for full stories.
- **Invariants distilled from these battles** (what must stay true): the
  `xemu-architecture-contract` skill.
- **Methodology lessons** (evidence bar, predict-before-run, idea lifecycle):
  the `xemu-research-methodology` skill. This file records what happened;
  that one teaches how to work.
- **Running the A/B protocol** cited by entries: the `xemu-testing` skill.
- **Change classes, release rules, README duties**: the `xemu-change-control`
  skill (the tag invariant and failed-experiments-row duty live there).
- **Planning new GPU work**: the `xemu-gpu-frame-campaign` skill (builds on
  entry 1.2's conclusion).

---

# 1. Vulkan renderer (hw/xbox/nv2a/pgraph/vk)

Jargon: **PFIFO thread** = the NV2A command-FIFO thread, i.e. the renderer
thread. **finish** (`pgraph_vk_finish`) = submit-and-rotate sync point on
that thread — it submits the CURRENT command buffer but then waits only the
PREVIOUS flight slot's fence, so the just-submitted CB keeps executing
(pipelined; verified 2026-07-04, see entry 1.13 — "full submit + CPU wait"
is only true of upstream's pre-flight-slot design). **flip** = guest frame
boundary. **flight slot** = one of the 2 in-flight command-buffer
generations. **TBDR** = tile-based deferred renderer (Apple GPUs), where
every render pass costs a whole tile load/store.

## 1.1 Occlusion/report rework — the 2.4x win and its two regressed intermediates

**Status**: shipped-after-fix (`6bfbc22863`, 2026-07-03). Two intermediate
designs REGRESSED and are fenced off — do not retry them.

**Symptom**: report-heavy scenes (Azurik in-game savestate, 408–485
draws/flip) ran at 15.8 fps with the renderer thread mostly waiting.

**Root cause** (two coupled serialization sources, found via the then-new
renderpass/pipeline-bind nsprof instrumentation): (1) `vkCmdResetQueryPool`
is illegal inside a render pass, so every occlusion-query rotation tore the
pass down — **376 render passes per flip**, ~one full tile load/store per
draw on Apple TBDR GPUs. (2) Whenever the FIFO idled with a zpass report
pending, the STALLED path submitted and synchronously drained — 23+ full GPU
syncs per flip.

**Shipped design**: bulk-reset the slot's query partition once at
command-buffer begin (outside any pass; slots own disjoint ranges, fence-
reaped before reuse); begin/end queries *inside* the pass (MoltenVK's native
Metal visibility-buffer path); reports ride the next natural submission
(flip) and deliver at slot reclaim, with a 5 ms continuous-idle fallback
(submit once, poll fence non-blocking) for guests that truly spin-wait.
Escape hatch: `XEMU_REPORTS_SYNC=1` restores legacy synchronous drains.

**Evidence** (2026-07-03, `scripts/bench-savestate-ab.sh`, 3 interleaved
pairs, same binary, env toggle): commit message records 38.57 ± 0.80 fps vs
15.78 pre-change (+144%), 14 render passes/flip, 0 stalled finishes,
2.2 ms/flip fence wait — the protocol receipt. README (~line 351) records
the same change as 15.8 → 35.6 fps / 2.25x, passes 376 → 14, fence waits
31.9 → 2.3 ms/flip — added in the same commit `6bfbc22863` and carrying no
receipt; cite 38.57 ± 0.80 (+144%) per `xemu-frontier-and-positioning` §3.5
until `xemu-docs-and-writing` reconciles the README.

**The two REGRESSED intermediates** (measured 2026-07-03; do not retry):
in-pass queries *keeping the old synchronous drain policy* — 6.86 ± 0.71 fps,
WORSE than the 15.78 baseline (in-pass queries without deferred delivery
concentrate the sync cost); and submit-on-idle per pending report (session
record from the rework, not in a commit message) — ~11.2 fps with ~144 tiny
submissions per flip, submission overhead replacing drain overhead.

**Lessons**: on TBDR, pass count is a first-order cost — any change that
multiplies passes is dead on arrival; and query/report changes must be
evaluated as a *policy pair* (where queries live × when reports deliver),
never piecewise.

**Reopen if**: never as-is. Follow-on work (e.g. satisfying guest report
polls from per-slot drains without a finish) is recorded as a README Future
vector, but `xemu-frontier-and-positioning` §2.8 (the status owner) marks it
**resolved-verify**: the default path already does non-blocking per-slot
drains, and `VK_FINISH_REASON_STALLED` is unreachable under shipped defaults
(`reports.c:298`, sole call site, `XEMU_REPORTS_SYNC=1` only). Run
frontier's one confirming nsprof measurement before treating it as open;
otherwise it's a doc-prune for `xemu-docs-and-writing` — and in no case a
reopening of the regressed designs.

## 1.2 Streamed-vertex stall campaign — attempted twice, both ~neutral: heavy scenes are GPU-bound

**Status**: settled-negative for CPU-side synchronization work (README Future
vectors; `099efcb734`, 2026-07-03). One component later shipped on its own
merits (1.2a).

**Symptom**: heavy scenes showed 440–700 `finish_vtx_dirty` forced finishes
per 5 s interval and ~2.5–3.5 s total fence wait per 5 s — the top item in
whole-frame attribution.

**Attempt 1 — byte-exact conflict refinement** (per-page written-span
tracking + span-restricted memcmp before the recording-CB conflict finish):
eliminated 75–85% of `finish_vtx_dirty` (440–700 → ~100 per 5 s; page-padded
guest stream writes false-share boundary pages with byte-identical content).
**flips/s did not move** — the eliminated finishes reappeared as cross-slot
targeted waits.

**Attempt 2 — attempt 1 + per-flight vertex-RAM mirrors** (cross-slot waits
structurally impossible; see 1.3): fence-wait *events* ballooned while totals
stayed ~2.5–3.5 s per 5 s.

**Conclusion**: in the heavy Azurik attract reel, the ~22–30 ms/flip of waits
is the GPU's *actual frame time*, not CPU-side slack. CPU-side wait
elimination relocates which call site absorbs the wait; it cannot remove it.
Productive direction: reduce GPU work per frame (draw batching, render-pass
merging) — the `xemu-gpu-frame-campaign` skill's charter. Both
implementations are preserved in history (`git show 268daf69ac 099efcb734`).

**1.2a — the salvage**: byte-exact refinement alone later shipped as a
conservative finish-skip (`6e8c6cc481`, 2026-07-03): forced finishes 5.65 →
0.37/flip (−93%), fence wait −11%, process CPU −7.6%, **fps +5.4%** on a 408
draws/flip in-game scene. `XEMU_VTX_EXACT=0` restores page-granular behavior.
A campaign can fail its goal and still yield a shippable component — score
components separately.

**Reopen if**: nsprof shows fence-wait per flip substantially *exceeding*
plausible GPU frame time for the scene (waits that are slack, not GPU
execution) — e.g. after GPU-side work reduction shifts the balance. Until
then, do not spend effort on CPU-side vertex synchronization.

## 1.3 Per-flight vertex-RAM mirrors

**Status**: reverted (`268daf69ac` documents it, 2026-07-03; README row).
**Design**: one 128 MiB host mirror per flight slot; in-flight slots read a
frozen mirror (cross-slot conflict waits disappear); `uploaded_bitmap`
doubles as the rotation delta log applied at slot reclaim.
**Measured** (2026-07-03, Azurik attract): ~neutral vs the single-mirror
build once compared correctly — the initial "-30%" read was an invalid
baseline (entry 9.1). Bisect modes (mirrors + legacy cross-slot waits;
mirrors + no delta apply) showed any regression persists regardless of
conflict policy — alternating large vertex buffers per submission is itself
expensive under MoltenVK.
**Root cause of neutrality**: the dominant cost was the *recording-CB*
conflict cascade (page-boundary false sharing), which mirrors cannot address
— and beneath that, the GPU-bound conclusion of 1.2.
**Reopen if**: same condition as 1.2, plus targeted-wait counters dominated
by in-flight-slot conflicts rather than recording-CB conflicts.

## 1.4 The level-transition freeze — a cascade of defensive reverts and the real culprit

**Status**: shipped-after-fix (`f156b009a1`, 2026-04-17). The hunt
temporarily reverted several *innocent* optimizations; two were later
re-added with proofs. Read this before bisecting any freeze by reverting
optimizations one at a time.

**Symptom**: black-screen freeze during rapid level transitions /
death-reloads (v0.8.142 era). Guest CPU parked in the kernel idle thread
(`CLI; HLT` loop, recovery counter ~1500/s at eip=0x800151ef); xemu UI stayed
responsive.

**The cascade** (2026-04-17, in order): suspicion fell on every recent
change, producing defensive reverts of B2 dynamic rendering (`c37110d0aa`),
α3 direct-VRAM compute + A1 untimed PFIFO wait (`59b85d1e86`), α2
host-imported vertex buffer (`accfcac580`), A3 JIT-write callback
(`d4f2e3e226`), C1 descriptor bind-skip (`2534c9bc1a`), the APU rate==1.0
resampler (`68d5afe7fa`), and finally removal of the HLT BSOD recovery hack
itself (`6eea148914`). The `XEMU_PFIFO_HEARTBEAT=1` diagnostic (added in
`2534c9bc1a`) then proved the freeze was NOT pgraph-side: pfifo thread alive
at ~950 iters/s, all pending flags 0.

**Root cause** (bisecting v0.8.141 → v0.8.142 to `942368e86c` "Audit round
7"): the surface-expiry throttle was keyed on `pg->frame_time`, which only
advances on `NV097_FLIP_INCREMENT_WRITE`. During a burst of draws with no
flip (level transition, death-reload, cold shader cache) the expiry/prune
gate NEVER fired; `r->invalid_surfaces` accumulated unbounded (each entry
holding a live VkImage + view + VMA allocation) until the renderer stalled
and the guest main thread blocked. Repro recorded in the commit: delete
shader cache, launch Azurik, rapidly load between two levels, immediately
die.

**Fix**: host-time throttle (~33 ms) so pruning ticks regardless of flip
cadence (`f156b009a1`). The original commit keyed it on `QEMU_CLOCK_HOST`;
the throttle has since migrated to the monotonic clock —
`qemu_clock_get_ns(QEMU_CLOCK_REALTIME)`, `surface.c:2115` (REALTIME is
CLOCK_MONOTONIC here despite the name; xemu-architecture-contract
Invariant 9).

**Aftermath**: A1 and C1 — reverted on suspicion, never guilty — were
re-added with explicit correctness proofs in `a005bb1560` (entries 7.1/7.2).
The items NOT re-added (α2/α3, B2, FSIN/FCOS, rate==1.0 resampler, misaligned
incremental hash, async-GL MetalFX, A3, HLT recovery) had *real* correctness
bugs independent of the freeze — each has its own entry here.

**Lessons**: (a) "correlated with freezes" is not a root cause — build the
discriminating diagnostic (heartbeat) before reverting good code; (b) any
throttle keyed on guest frame counters starves during non-flip bursts — key
maintenance work on host time via the monotonic clock
(`QEMU_CLOCK_REALTIME` here, never `QEMU_CLOCK_HOST` — Invariant 9); (c)
after the storm, re-adjudicate every
defensive revert explicitly (innocent → re-add with proof; guilty → README
row).

**Reopen if**: n/a (the fix is a correction, not a tradeoff).

## 1.5 VK_KHR_dynamic_rendering full lowering (B2)

**Status**: fenced-off (`c37110d0aa`, 2026-04-17). Probe and plumbing remain;
`r->dynamic_rendering_feature_enabled` forced false at device-create.
**Symptom**: level-transition hangs on a black loading screen — `vkQueueSubmit`
stopped progressing, `pgraph_vk_finish`'s `vkWaitForFences(UINT64_MAX)`
blocked forever, guest in `CLI; HLT` with the recovery counter at ~10k/2s
(IRQs delivered, no PGRAPH completion ever).
**Root cause**: the lowering (landed in `ea23058e1e`) replaced
VkRenderPass/VkFramebuffer with `vkCmdBeginRendering`/`vkCmdEndRendering` but
did not replicate the render pass's implicit `VK_SUBPASS_EXTERNAL → first
subpass` dependency. With dynamic rendering that barrier is the caller's job;
without it, back-to-back passes into the same attachment observe stale writes
and MoltenVK's tile renderer stalls on the Metal side.
**Reopen if**: you add the explicit `vkCmdPipelineBarrier` equivalent around
every Begin/EndRendering — `COLOR_ATTACHMENT_OUTPUT` +
`EARLY/LATE_FRAGMENT_TESTS` stages with COLOR/DEPTH_STENCIL
ATTACHMENT_READ/WRITE access (the commit message spells this out) — and
validate on the repro class (rapid small passes at level transitions). Also a
README Future vector.

## 1.6 VK_EXT_external_memory_host direct-VRAM paths (α2 vertex / α3 compute)

**Status**: fenced-off (α3 reverted in `59b85d1e86`; α2 disabled in
`accfcac580`, both 2026-04-17; extension probe + `create_host_imported_buffer`
kept in-tree). Later attempt `d65cfd70b8` fed the same conclusion.
**Symptom**: intermittent flicker on fast-moving / far-off / translucent
content — particles, HUD, billboards, skinned meshes; GPU vertex fetches
observing two CPU frames mid-primitive.
**Root cause**: with host-imported memory, `BUFFER_VERTEX_RAM` (or the
compute-unswizzle source) IS live guest VRAM. `HOST_WRITE → SHADER_READ`
barriers guarantee visibility of writes *up to submission* but do not stop
the guest CPU writing during GPU execution; on Apple Silicon HOST_COHERENT
memory those writes are immediately GPU-visible. Torn reads are inherent —
no correctness-preserving form exists without a snapshot or cross-API fence.
**Also settled on value** (README Future vectors, 2026-07-03 era): the
snapshot memcpy this would eliminate totals < 0.5 ms per 5 s in-game — far
below the fork's ≥ 1 ms/frame bar for risky rework.
**Reopen if**: a per-flight COW or MTLSharedEvent-keyed snapshot boundary is
designed AND the eliminated memcpy measures ≥ 1 ms/frame in a real scene.
Both conditions; the second currently fails by ~two orders of magnitude.

## 1.7 Surface-expiry throttle keyed on `pg->frame_time`

**Status**: shipped-after-fix — the root cause of 1.4, recorded separately
because it generalizes: `pg->frame_time` advances only on flips, so any gate
keyed on it silently stops during draw bursts without flips. Evidence:
`f156b009a1` vs `942368e86c`; README row.
**Reopen if**: n/a. Key any pgraph cache/prune throttle on the monotonic
clock — `QEMU_CLOCK_REALTIME` (which despite the name is CLOCK_MONOTONIC
here; current code at `surface.c:2115`) — never on `pg->frame_time` and
never on `QEMU_CLOCK_HOST` (the NTP-jumpable wall clock); see
xemu-architecture-contract Invariant 9, the fact owner.

## 1.8 Surface-range binary search on `end`

**Status**: settled-negative (README row). Binary-searching a start-sorted
surface-range array by `end` skipped valid overlaps — `end` is not monotonic
in that ordering. The correct structure shipped later: O(log n) insert on
start + O(1) remove (`e71a85f643`, 2026-04-19).
**Reopen if**: n/a — never order-by-`end` tricks on start-sorted arrays.

## 1.9 Texture-hash lineage — three failures that shaped the current design

All three are README rows; the misaligned case has a full revert pair.

- **Two-level quick hash / hash-skip for >64 KiB textures** —
  settled-negative. 192-byte sampling missed real content changes (YUV/FMV);
  false positives caused GPU re-upload churn. Full-content hashing stayed;
  the shipped speedups came from *incremental* hashing and dirty-tracking.
- **Dirty-range VRAM flush (early version)** — reverted. The dirty bitmap was
  cleared before `flush_memory_buffer` consumed it. Safe equivalent that
  shipped: per-flight `[first, last]` written-range tracking.
- **Incremental hash on misaligned textures** — reverted (`368ae272e6` →
  revert `5ebda9a33c`, 2026-04-17): host-page boundaries straddle 64 KiB
  chunks, so `test_and_clear_dirty` for one chunk *steals* the dirty signal
  belonging to its neighbor — missed updates. Fixed properly the same day by
  `76f4e2072b` (page-aligned chunk layout) plus `d0ad34b48a` (re-hash all
  chunks when externally marked possibly-dirty). General trap:
  `test_and_clear` on shared pages is a destructive read — partition first.

**Reopen if**: n/a for the first two. Sampling-based hashes: only with a
title-wide artifact-oracle soak (per `xemu-testing`) proving zero missed
updates on FMV/YUV content.

## 1.10 Depth export for MetalFX real depth

**Status**: settled-negative (README row; CPU-readback leg reverted in
`248c4d282f`, 2026-04-15). CPU readback: ~2 ms/frame + 18 MiB of copies for
minimal upscale-quality gain. `vkExportMetalObjectsEXT`: deadlocks MoltenVK's
internal mutex. Shipped compromise: opt-in `XEMU_MFX_REAL_DEPTH` feeding the
*live* zeta texture (temporally wrong by one frame); open idea (README Future
vector): flip-time zeta blit.
**Reopen if**: you implement the flip-time GPU copy — not either failed
transport.

## 1.11 Separate compute queue

**Status**: settled-negative (README row; dead plumbing dropped in
`aff8c229e9`). MoltenVK exposes `queueCount=1` — there is no second queue;
single-queue submission order IS execution order.
**Reopen if**: MoltenVK exposes multiple queues here (check `vulkaninfo` /
release notes), and even then only with evidence queue parallelism beats the
current single-queue pipelining.

## 1.12 Measured "not worth it" set (don't reopen without fresh numbers)

Settled-negative on value, not correctness — each measured against the fork's
effort bar and closed (README Future vectors record all; measurements
2026-07-03/04 era, Azurik, M2 Ultra):

| Idea | Measured reality | Why closed |
|---|---|---|
| Texture-upload barrier batching (per-mip pre/post_compute pairs) | total upload ≤ 15 ms per 5 s during streaming-heavy transitions, ~0 steady-state | risk to load-bearing barriers on reused COMPUTE_DST/SRC for ~nothing |
| GPU S3TC decode (compute shader) | upload CPU incl. S3TC ~0 steady, ≤ 15 ms/5 s streaming | no longer a meaningful cost center |
| `VK_EXT_external_memory_host` revival | eliminated memcpy < 0.5 ms per 5 s | ≥ 1 ms/frame bar (entry 1.6) |
| MetalFX *input* ring (drop `metalfx_drain_inflight`) | drain ~0.1 µs/flip steady, max ~4 ms on rare hitches | complexity for noise |
| Shader specialization constants | stale premise: alpha-test/fog-enable are already compile-time GLSL variants keyed via PshState/VshState in the LRU shader cache; only value uniforms remain per-draw and must stay uniforms | premise false; useful reframing = variant-count/compile-stutter work |
| Async pipeline compilation | in-game `pipeline_gen` ≈ 0 after pipeline-cache persistence landed (cold 164 ms worst-case create → ≤ 4 ms warm; README ~line 232) | the stall it would hide no longer exists |

**Reopen any of these if**: `XEMU_NV2A_NSPROF=1` shows the relevant counter
(TEX_UPLOAD, PIPELINE_GEN, MFX_DRAIN) at ≥ 1 ms/frame in a real title —
attach interval logs to the proposal.

---

## 1.13 Invalid-surface destruction raced pending submissions

**Status**: shipped-after-fix (`a045dfc780`, 2026-07-04; found by the
Windows gating audit, not by a crash report).
**Symptom**: none observed — latent. `prune_invalid_surfaces` destroyed
quarantined `VkImage`s once they were outside the *recording* CB, but
`pgraph_vk_finish` pipelines: it submits the current CB and waits only
the previous slot's fence, so the just-submitted CB routinely still
executes while the next records. An image evicted during CB N could be
`vkDestroyImage`d during CB N+1's recording with CB N pending — invalid
usage on every driver. MoltenVK's deferred encode (prefill=0 encodes on
its queue thread after `vkQueueSubmit` returns) makes the window real on
macOS too; the window opened when flight-slot pipelining replaced
upstream's synchronous submit-and-wait finish (upstream unaffected).
**Fix**: evictions stamped with the highest submission index that may
reference them (`evict_submit_seq`); a retirement watermark
(`retired_submit_count`, updated at every slot-fence observation) gates
destruction; `surface_flush` drains all slot fences before its
free-everything prune. REUSE of quarantined images needs no gate — a
migrated image keeps its attachment layout and its first GPU touch is
ordered against all earlier same-queue submissions by the draw render
pass's explicit `VK_SUBPASS_EXTERNAL` dependency (proof comment at
`get_any_compatible_invalid_surface`).
**Evidence**: fps parity on F5 (46.6-47.2 vs 46.83 ± 0.24 pre-fix),
identical finish mix, zero errors; audit table at
`docs/windows-gating-audit.md` §C.
**Lessons**: (a) "not in the recording CB" never proves "not referenced
by the GPU" in a pipelined renderer — destruction needs a fence-derived
watermark, reuse can ride spec-guaranteed ordering; (b) a static audit
with the spec open finds bug classes soak testing structurally cannot
(nothing crashes until a driver actually reuses the freed allocation).
**Family member 2 (`215f243be7`, 2026-07-04)**: the VERTEX_BUFFER_DIRTY
conflict path memcpy'd new guest data over the vertex mirror right
after finish, with a comment claiming "the submit's fence waited" —
true of upstream's single-slot finish (verified against
upstream/master: submit + vkWaitForFences on the same fence), silently
false after flight slots. CPU write racing the just-submitted CB's
vertex fetches; fixed by waiting the submitted slot's fence (+ clearing
its upload tracking). Parity receipt: 24.45±0.58 vs 24.48±1.19 fps,
3 cross-binary pairs. A full sweep of every other finish caller found
no further members: per-slot staging is safe via the rotation wait,
GPU-GPU hazards ride the transition barriers' attachment-stage src
masks, CPU readbacks wait the aux fence, FLUSH/REPORTS_FULL drain via
per-slot waits. **The general rule: pipelined finish is safe for
everything except CPU-side actions on shared GPU-visible state.**
**Reopen if**: n/a.

## 1.14 GPU frame-cost campaign menu — killed by measurement on the 2026-07 fixtures

**Status**: settled-negative for mechanisms A1/A2/B/C/D on the current
savestates (never implemented — Phase 1/2 gates fired first; commits
`038f596332` instrumentation, `9b625f1126` verdict; campaign skill
carries the full status header).
**Evidence**: F5 (471 draws/flip @ 46.6 fps): A1 zero opportunity
(vk_draw_call == draws exactly); A2 10.6% merge candidates × ~zero
per-call PFIFO recording cost (prefill=0 defers Metal encoding to
MoltenVK's queue thread; `vkCmdDraw*` absent from a 10 s PFIFO sample);
B sites all <1 ms/flip with PFIFO 57.6% idle; C's savings land in
~14 ms/flip of GPU slack (fence 2.8 ms/flip); D blocked in MoltenVK.
Frame limiter is CPU-side: guest TCG largest share (≥5.8 ms/flip PFIFO
starvation; vCPU% itself is confounded — the title busy-polls, ~95%
vCPU even at F6's flat 60 fps cap).
**Reopen if**: a genuinely GPU-bound scene re-enters the fixture set —
re-run campaign Phase 0 and Phase 2 gates first (draws/flip in
envelope, fence-wait a large fraction of frame time, encoder time ≈
frame budget). The pass-cause data (clears 5.0/flip) becomes actionable
again only under that condition.

## 1.15 Eager report submit — front-running the poll stall

**Status**: settled-negative (implemented, measured, reverted same day
2026-07-05; README failed-experiments row).
**The idea**: submit the recording CB when its Nth zpass report is
*requested* (`XEMU_REPORTS_EAGER=N` in `pgraph_vk_get_report`), so GPU
execution overlaps remaining guest frame work instead of starting only
after the guest stalls into the idle-budget valve.
**Evidence**: interleaved same-binary A/B, F8 scene, 6 pairs: mean
−0.08 fps (deltas +0.26/+3.04/+0.09/−0.87/−0.65/−2.38), plus a
persistent +4% draws/flip composition shift in the eager arm
(frame-cadence feedback). Two E runs also captured almost no in-game
intervals (harness noise), further weakening the early positives.
**Why dead**: with the idle budget already at 300 µs (same-day ship),
the guest's residual wait is GPU catch-up time — eager submission
relocates the submit without shrinking that, while paying extra
submit/CB-restart overhead.
**Reopen if**: the idle budget ever has to grow again (e.g. a workload
where 300 µs causes measurable submit storms) — eager-at-record is the
natural alternative to re-evaluate.

## 1.16 Vertex copy-on-conflict transient remap — the finish was cheaper

**Status**: settled-negative (designed with trap analysis, implemented
faithfully in a worktree, measured, reverted 2026-07-05).
**The idea**: on a recording-CB vertex conflict, copy the new bytes to a
per-flight-slot transient buffer and remap subsequent overlapping draw
bindings there (newest-first chained entries; retirement-gated apply to
the mirror; rotation copy-forward to close the mirror-lag window;
force-differs exclusion in the exact-refinement path; partial-overlap
and overflow fall back to a drain). Ceiling estimate: ~6
finish_vtx_dirty/flip x ~0.5 ms = ~3 ms/flip on F8.
**Evidence** (interleaved 6 pairs, F8): **-2.09 fps, 6/6 pairs
negative**. Mechanism counters: vtx_remap_hit 226.7/flip (remapped
ranges intersect a huge share of subsequent binds — the newest-first
linear table search became a per-draw fixture), vtx_remap_full
1.47/flip (64-entry / 2 MiB slot budget overflows every frame; each
overflow is an ALL-slot drain, strictly heavier than the ~6 targeted
one-slot waits it replaced), vtx_remap_partial 0.61/flip (more drains).
**Why dead**: Azurik rewrites broad vertex ranges every frame, so (a)
the table can't hold a frame's worth of conflicts at any sane size, and
(b) once entries exist, hundreds of binds/flip pay the search. The
conflict-wait cost is real but its replacement cost more; the legacy
targeted wait (215f243be7's submitted-slot wait + tracking clear) is
the better design point for this workload.
**Reopen if**: a title shows LOW-count, LARGE-range conflicts (few
entries, little bind intersection — the opposite shape), or with an
O(log n) interval structure + frame-sized budget + per-slot targeted
fallback instead of the all-slot drain; re-run the same counters first
to check the shape before writing any code.

## 1.17 JIT write-protect caching — the profiler lied at barriers

**Status**: settled-negative (2026-07-05, same-session revert).
**The idea**: the new guest profiler attributed 9.7% of the vCPU
thread to pthread_jit_write_protect_np (called on every TB entry via
qemu_thread_jit_execute); a thread-local shadow state skipping
redundant flips predicted +1.5-3 fps.
**Evidence**: 6-pair A/B measured parity (-0.10 mean, sign-mixed).
**Root cause of the bad prediction**: thread_suspend-based sampling
(the mach sampler) parks the suspended thread's PC preferentially at
barrier instructions — pthread_jit_write_protect_np is barrier-heavy,
so most of its 9.7% was skid from neighboring code, not real cost.
**Lesson**: in suspend-based profiles, discount symbols whose bodies
are dominated by barriers/serializing instructions (jit-wp, mutex
fast paths); corroborate with a counter-based estimate (call rate x
plausible per-call cost) before predicting from sample share alone.
**Reopen if**: never for fps; possibly as a power/efficiency change
with energy instrumentation.

## 1.18 Return-address ring → eip-keyed ret-target memo

**Status**: superseded-shipped (the memo shipped default-on at
fps-parity; the ring shape was measured and discarded the same day).
**Journey**: guest profiling showed rets = 46% of 15.8M lookups/s and
53% of jump-cache misses → classic shadow stack implemented
(call-site push, ret-pop, lazy TB fill). Smoke: eip prediction paired
at 99.6% BUT fills ran 2.6x hits (196.9M/70 s) — per-depth slots are
shared by every same-depth call site, so the TB memo thrashed; a
512-deep ring changed nothing (not a depth problem, a KEY problem).
Restructured: drop the ring and the per-call push entirely (env->eip
already holds the ret target at dispatch — depth-shaped prediction
adds nothing), direct-mapped 4096-entry eip->TB memo probed in the
ret helper with full jump-cache-equivalent validation. Result: 95.4%
hit, zero mispredicts, fills 17M, fps parity (probe cost ≈ jc probe
cost at helper level, as predicted post-skid-correction).
**Phase 2 SHIPPED (same session)**: the probe inlined at ret sites.
Validation ended up cheaper than the planned epoch design: at a ret
exit the runtime context provably equals the ret-TB's own
translate-time constants (nothing between TB entry and its ret exit
changes cs_base/flags/cflags), so the inline path compares the
entry's fill-time context against immediates, checks tb != NULL
(flush clears entries), and re-loads live tb->cflags to catch
CF_INVALID — no epoch needed. ~17 TCG ops on the hit path; the
helper remains fallback + fill + all special cases (breakpoints,
single-step, logging — -d exec loses inline-hit lines; XEMU_RAS=0
when tracing). Receipts: helper hits collapse 362M→12.8k (inline
absorption signature); 6-pair A/B **+0.77 fps, 6/6 positive**,
inside the registered +0.7-1.8 band, with the enabled arm carrying
more draws/flip. First realized guest-CPU fps win — the exit-kind
census aimed it, two mirages died en route (1.16, 1.17), and the
mechanism landed in-band.
**Phase 3 (same campaign)**: the inline technique generalized to the
real jump cache for non-ret indirect/cross-page exits — emitter in
accel/tcg/xemu-inline-jc.c (sole owner of the CPUJumpCache layout;
mind the rcu_head ahead of the entry array), i386 passes context
constants + the env→CPUState field offset. Family A/B (one hatch,
XEMU_RAS=0): +0.70 fps with +5.5% draws/flip in the enabled arm
(~+8% draw throughput; scene feedback understates fps deltas —
compare draw throughput when arms diverge in composition). Helper
lookups 837M→473M. Also: apu-vp joined the lockless-MMIO set on the
PFB/USER audit pattern (deferred-IRQ discipline verified at
vp.c fe_method → apu.c se_frame's own bql bracket).
**Reopen the ring if**: a workload shows the memo thrashing on
polymorphic returns (same ret eip, alternating targets — impossible:
ret target IS the eip; the memo cannot alias that way. The ring has
no reopen case; this row exists to prevent re-walking it).

## 1.19 Sticky host-FPU bracket — the restore write was the whole margin

**Status**: shipped (+1.90, 3/3 pairs; default ON for aarch64 with
hard_fpu; 2026-07-05).
**History**: the NEON SSE path measured PARITY vs PGO'd softfloat with
strict per-op brackets (enter: set FZ/RN if needed; leave: restore).
The mechanism of parity was identified, not guessed: enter already
compare-skips, but ambient FPCR (x87-leaked modes or IEEE default)
almost always differs from guest-SSE mode, so steady state paid TWO
serializing mode writes per op — worth exactly the SIMD win.
**Fix**: mode-1 leave is now a no-op (host stays in guest-SSE mode
between helpers); =2 differential keeps strict brackets and
re-confirmed zero divergences on the sticky build (70 s). Composition
argument: x87 hard-FPU brackets save/restore around their own ops
(they restore to our sticky state — self-consistent); softfloat and
TCG generated code have no host FP-mode dependence; host code on the
vCPU thread already tolerated x87-leaked rounding modes, so the
incremental exposure is FZ/DAZ on denormals inside the opt-in.
**Receipt**: +2.25/+1.70/+1.75 on F8 (largest single win of the
dispatch campaign era). x86_64 remains dark (unresolved Rosetta ±0
class, no silicon receipt).
**Lesson**: when a fast path measures parity, price its BRACKET
before killing the idea — serializing register writes cost as much as
the work they guard.

## 1.20 L2 victim jump-cache — killed by hit-rate, not by fps

**Status**: settled-negative (instrument-killed same hour, no fps A/B
spent; 2026-07-05).
**The idea**: post-inline residual lookups are miss-rich (12%) and
capacity-shaped (410k TBs vs 4096 L1 entries), so a 64k-entry victim
tier on the miss path — where an extra probe amortizes against the
~100 ns qht walk — looked sound.
**Evidence**: live hit rate 11.2% (normal mode; 11.6% under =2), the
recurring-miss set is tiny — residual misses are one-shot/cold pcs
plus the NULL-lookup class. Ceiling ~0.1 fps ⇒ reverted on the spot.
**Lesson (pairs with the L1-enlargement kill)**: measure the miss
stream's SHAPE (recurrence) before building any cache tier; volume
alone justifies nothing.
**Open observation parked with it**: ~70k/s lookups return NULL at
tb_flush=1 with do_tb_phys_invalidate in the profile top — the
SMC/invalidation-churn vector in README Future vectors item 1.

## 1.21 Texture/sampler LRU eviction raced pending submissions

**Status**: shipped-after-fix (`d9c90165d1`, 2026-07-11; found by the
six-agent audit wave, not by a crash — same discovery mode as 1.13).
**Symptom**: none observed — latent. Third member of the 1.13 family,
which its sweep structurally missed: LRU eviction is not a `finish`
caller, so enumerating finish callers never visited it.
**Root cause**: `texture_cache_entry_pre_evict` protected an entry
only while currently bound or `in_command_buffer && submit_time ==
submit_count` — the *currently-recording* CB. Under pipelined finish
the just-submitted CB still executes with its textures stamped
`submit_count-1` (evictable), and `pgraph_vk_trim_texture_cache` runs
from finish via `check_memory_budget` after `in_command_buffer=false`
(clause disabled entirely), with `post_evict` destroying immediately.
The sampler cache had the same shape with no stamp at all.
**Fix**: surface-watermark mirror — refuse eviction while
`submit_time >= retired_submit_count`; samplers gained the stamp;
`ensure_cache_headroom()` retires slot fences if a pool ever fills
with pinned nodes (release builds strip `lru_evict_one`'s assert, so
exhaustion must be impossible, not unlikely); finalize drains fences
before its cache flushes so a live renderer switch can't leak.
**Evidence**: F5 +0.19 ± 0.14 (3 pairs), F8 quiet +0.12 ± 1.46
(sign-mixed) — parity; 291-capture F8 artifact soak zero flagged.
**Lessons**: (a) when a race family is fixed by enumerating one call
pattern, audit every OTHER site that destroys GPU-visible resources —
the family boundary is "CPU-side action on shared GPU-visible state,"
not "finish callers"; (b) an assert-guarded exhaustion path is a
release-build crash — pair stricter refusal guards with a forward-
progress fallback.
**Reopen if**: n/a (correctness fix, no tradeoff).

## 1.22 TB byte-range invalidation filter — instrument-killed, and it answered why upstream removed it

**Status**: settled-negative (2026-07-11; counters killed it before an
fps A/B was spent — the 1.20 pattern). The knob ships dark
(`XEMU_TB_RANGE_INV=1`) as documentation + A/B hatch; `XEMU_INV_PROF=1`
is the counter set that decided it.
**The idea**: XBOX builds invalidate every TB on a written code page
(`#ifndef XBOX` skips upstream's byte-range overlap check,
tb-maint.c). INV_PROF measured the false-invalidation share at **100%**
on F5 and F8 (notdirty-sourced 100%, true SMC **0**, recycle-hit 100%)
— so re-applying the exact upstream filter looked like removing ~350-650k
pointless invalidate/recycle round-trips per 95 s.
**Evidence that killed it** (counter A/B, F8): invalidations 338k → 675
(0.002x — the filter works) BUT notdirty traps 197k → **4.98M (25.2x)**
and range-scan evaluations 46M — because whole-page invalidation is
what empties the page and fires `tlb_unprotect_code`; keep the TBs
alive and every subsequent data write to that page traps forever, each
trap paying the `physical_memory_test_and_clear_dirty` TLB walk (the
profile's 2.0%) plus a scan of the now-unbounded per-page TB list.
Pre-registered kill threshold ">5x traps = negative"; measured 25x.
**The recovered answer to audit-F3's open question**: base-xemu's
removal of the check (`6ea11938b2e`, no rationale recorded) was
deliberate performance work — on the Xbox's data-shares-a-code-page
pattern, whole-page invalidation makes the next ~25 writes/epoch free.
**The real lever this points at**: sub-page dirty tracking — stop the
non-code write from *trapping* instead of filtering what it
invalidates; design sketch with the archaeology-1.9-class race
analysis parked in the campaign records (wrong-code-hang failure class;
needs refuter-first treatment before any implementation).
**Reopen if**: a workload appears with true SMC or partial-overlap
writes (INV_PROF false-share well below 100%) — the filter's tradeoff
flips only when invalidations stop being pure false sharing.

## 1.23 Sub-page code dirty tracking — the 1.22 lever shipped, and the model undershot 15×

**Status**: shipped default-on (2026-07-11, the same day 1.22 was
killed; `XEMU_SUBPAGE_DIRTY=0` restores whole-page invalidation).
**The mechanism**: per-`PageDesc` 64×64-B code-block bitmap; a notdirty
store missing every code block skips the whole-page invalidation via an
**O(1) bitmap test** — the page stays protected and re-traps cheaply.
Set-before-`tb_link_page`-publish, cleared only on page-empty/tb_flush
(over-approximation is the SAFE direction: a stale bit costs a trap, an
under-set bit would be a wrong-code hang).
**Refuter-first record** (the design doc demanded it; two soaks):
0 violations over 60M+ ground-truth-checked skips across 45 reload
cycles — 28.6M standalone, then 32.0M on the integrated binary with
cross-page chaining simultaneously active (that combined soak also ran
the chain refuter: 3.15B checks, 0 violations).
**Measured** (quiet interleaved A/B, scene-gated intervals): F8
30.35→34.68 (+4.33±1.53, 4/4 pairs, +14.3%); F5 47.33→58.96
(+11.63±0.52, 3/3, +24.6%). Largest single TCG win since the occlusion
rework.
**Two durable lessons**: (1) 1.22's ">5× traps ⇒ negative" proxy was an
artifact of the O(N) per-trap scan — re-deriving the cost model for the
O(1) form (Gate-0, `docs/subpage-gate0-prediction.md`) flipped the
verdict; never carry a kill-proxy across a complexity-class change.
(2) The Gate-0 model still undershot ~15× (+0.5-0.9% vCPU predicted,
+14-25% fps measured) because it priced only the trap body — the
eliminated invalidations also carried recycle round-trips, jump
unlink/relink, jc/ret-memo invalidation and cache pollution downstream.
When pricing a churn-removal, price the whole churn, not the visible
call site. (Also of note: the fps A/B was nearly poisoned by the
loadvm shortcut-vs-tag trap and a scene-gating parser bug — both
caught; see xemu-testing bench mechanics.)
**Reopen if**: n/a (ship). The residual ceiling — removing the ~26k/s
remaining O(1) traps via a host-`qemu_st` fast-path probe (arm (b),
Class-5 codegen) — is scoped in the Gate-0 doc but the trap cost it
would remove is now small; re-rank only with fresh INV_TIMING data.

## 1.24 The xpage full-flush backstop WAS the v0.11 "new-area lag" — a correctness backstop priced on a wrong rarity assumption

**Status**: shipped-after-fix (v0.11.1, 2026-07-12; `XEMU_XPAGE_UNLINK=0`
restores the v0.11 backstop).
**Symptom** (owner report, day after v0.11): heavy multi-second lag on
first entry to new areas — map transitions, death reloads, fast movement
into unexplored space — then fast; revisited areas smooth. Desktop-app
(default-flag) launches.
**Root cause**: the cross-page-chaining promotion's full-flush backstops
(generic `tlb_flush_by_mmuidx` hook + CR3 helper) severed chains with a
**queued whole-`tb_flush`** on the assumption that full-TLB-flush classes
are rare. They are not: level streaming reloads CR3 / fires generic
flushes continuously. Movement probe (F8, 20 s holding forward into
unexplored space): **139 tb_flushes**, worst interval **14.6-15.4 fps vs
38.6-39.6 settled**; same route walked back: **0 flushes, no trough**
(assets resident — exactly the reported asymmetry). Boot alone: 132.
Attribution: `XEMU_XPAGE_CHAIN=0` arm = 1 flush the whole session (the
loadvm). Bonus finding: the CR3 helper's "effectively never taken by a
running Xbox title" comment was **wrong**, and its fires were invisible
to the backstop counter (only the generic hook counted) — 817 backstop
events with CR3 routed through vs 582 counted before.
**Fix (backstop v2)**: register every cross-page link DESTINATION at
`tb_add_jump` (two bits in `tb->xemu_xpage_reg`, pre-`ihash` hole:
CROSS_EMITTER set by gen_jmp_rel on TBs that emit a relaxed slot — the
authoritative trigger, because under CF_PCREL + the Xbox's multiple RAM
aliases a cross-virtual target can share the source's phys page and
evade any phys-only test — plus REGISTERED as dest dedup; 128k registry
in tb-maint.c, generation-checked against `tb_flush_count`); backstop
severs exactly those chains via `tb_jmp_unlink()` synchronously (also
closes the old queue-to-safe-point window). Phys-mismatch-without-flag
links register too (belt-and-braces, `reg-phys-only` counter — an
UNPREDICTED class, thousands per soak even in refute mode with zero
relaxed slots; mechanism unidentified, but severing valid links is
always safe, so it is over-approximation, not hazard — chase it only if
the counter ever explodes). Overflow → old queued flush (correctness
never depends on capacity). Receipt: same-binary interleaved movement
A/B (`XEMU_XPAGE_UNLINK` toggle) — forward-phase flushes 139→0, worst
interval back at the settle floor, settle parity; ~270 unlink walks
severing ~450k dests per session ≈ tens of ms total vs seconds of
retranslation. Init placement trap: the flag byte must be cleared in
`tb_gen_code` PRE-translate — a first draft zeroed it in
translator_loop's post-loop block, which would have wiped the
translator's emitter bits.
**The W^X recurrence** (4.4-class, re-learned): the first fix build
called `tb_jmp_unlink` (host-code patching) from helper context WITHOUT
the manual `qemu_thread_jit_write()/execute()` pair → vCPU thread wedged
in a fault loop at the FIRST boot backstop. Signature for next time:
monitor answers once then returns empty, zero nsprof (no flips), no
crash report — a wedge, not a crash. Every new host-code-patching call
site on Apple Silicon needs the bracket; `tb_phys_invalidate` is the
canonical idiom (and its comment carries the
`pthread_jit_write_with_callback_np` revert story).
**Durable lessons**: (1) a correctness backstop is a PERF feature too —
price its firing rate on real workloads (streaming, boot), not on an
architectural rarity argument; (2) "rare event" claims about guest
behavior need a counter watched during the class of gameplay that would
refute them (here: 100+/20 s during streaming vs "rare"); (3) movement
into unexplored space is a distinct benchmark class the static savestate
A/B structurally cannot see — the fastest-regression-shipping session
(subpage+xpage, +25% day) validated only on static scenes and reload
soaks, and the lag shipped anyway.
**Reopen if**: n/a (fix is strictly better). If a future title overflows
the 128k registry (dump line `reg-overflows`), it degrades to v0.11
behavior on that title — size the registry then, don't redesign.

# 2. MoltenVK / driver level

## 2.1 Pink-tile corruption — the MoltenVK prefill saga

**Status**: shipped-after-fix (`531122e8aa` + `166999fc60`, 2026-07-04).

**Symptom**: giant screen-wide Morton-patterned magenta blocks on ~5% of
frames in streaming-heavy scenes — but ONLY in the user's Finder-launched
sessions; shell-launched harness runs never showed it.

**Root cause of the asymmetry** (the actual breakthrough): `Info.plist`
`LSEnvironment` applies only to Finder/LaunchServices launches. Every
shell-launched test ran MoltenVK *defaults* while every user launch ran the
plist's tuned config — the harness was testing a different driver
configuration than the user's. Replicating the app environment in the harness
reproduced the artifact immediately (18–22 flagged frames per 4-minute run).

**Root cause of the corruption**: single-variable bisection with the
window-capture artifact oracle pinned
`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` (immediate encoding). Prefill
alone reproduces; argument buffers / fast-math / async submits are clean.
Once the rework in 1.1 made command buffers span whole frames, immediate
prefill encoding became strictly worse: it corrupted streamed textures (22
flagged frames vs 0, same run), it was the enabling condition for the Act-2
crash (entry 2.2), and it measured SLOWER — 46.33 vs 48.50 fps — because
encoding lands on the hot PFIFO thread.

**Fix**: prefill = **0** in both `Info.plist` (~line 45, with explanatory
comment) and `ui/xemu.c:1602-1606` `setenv(..., 0)` — the no-overwrite main()
setenv gives launch-path parity so Finder, terminal, and harness launches run
one configuration (`166999fc60`; build.sh also logs the bundled MoltenVK
version+UUID to catch tested-vs-shipped driver drift). Validated on the
user's own pink-spot savestate with the full launch environment: 0 artifact
frames in 388 captures at 48.5 fps (prefill=0) vs 22 at 46.3 (prefill=2).

**Also fixed en route** (same commit): failed snapshot loads (USB topology
drift) left the VM stopped — a silent "freeze" behind a transient toast; the
VM now resumes on failure. And `hw/xbox/xid.c` asserted on emulated pads with
no host binding; they now report neutral input / drop rumble — which is also
what enables headless topology-matched snapshot loads.

**Reopen if** (prefill > 0): a future MoltenVK sets the visibility flag
per-pass rather than per-CB AND an interleaved A/B (per `xemu-testing`) shows
prefill winning fps with the artifact oracle clean. Both failed in 1.4.1.

## 2.2 Visibility-buffer crash, two acts (read before touching queries or trusting a clean soak)

**Status**: shipped-after-fix. Act 1 `bfcf38843f` (2026-07-03), Act 2
`bb33ccaa2e` (2026-07-04). Both defenses remain in-tree deliberately.

**MoltenVK v1.4.2 re-verification (2026-08-04)**: the primer's target
mechanism is verbatim unchanged at the v1.4.2 pin (`db660224`) —
`_needsVisibilityResultMTLBuffer` still resets at every
`vkBeginCommandBuffer` (MVKCommandBuffer.mm:244), is still set only by
`vkCmdBeginQuery`, and the encoder still attaches the buffer only if
the flag is pre-set (:781). The 1.4.2 changelog's visibility fixes
(`c8a9b178`, wrap detection / stale baseline) are a different crash in
the same family, not this one. **Keep the primer**: it is the only
defense correct-by-construction on every launch path (the 2.1
Finder-vs-shell env split is why prefill=0 alone is not sufficient
protection), the crash only ever reproduced in user gameplay, and the
cost is two command calls + one query index per CB. Related triage
lead from the same review: v1.4.2's advertised "channel corruption on
Apple Silicon color RTs used as transfer sources" fix (#2220,
`allowGPUOptimizedContents` copying compressed tile layout on blit
readback) was **reverted before the tag** (`4840a3f2`) — the shipped
driver still has the bug. Zero observed instances in this fork's
artifact-scored campaigns, but if pixel-exact readback corruption ever
surfaces in a title, that is the first suspect, and the custom-build
pipeline can carry the 3-line MVKImage.mm patch surgically.

**Symptom**: user-reported SIGSEGV inside the AGX driver at
`setVisibilityResultMode:offset:` during real gameplay — never reproduced in
bench soaks.

**Act 1 — true but partial**: deferred report delivery (entry 1.1) let one
command buffer span a whole frame while in-pass rotation allocated a query
index per rotation; heavy zpass scenes exceeded the old 512-per-slot
partition mid-recording. The only protection was an assert that release
builds strip — the index ran off the pool. Fix: `begin_draw()` submits
(REPORTS_FULL) before overflow; pool 1024 → 4096; `XEMU_MAX_QUERIES` test
knob. Validated by forcing `XEMU_MAX_QUERIES=64`: guard fired 3312× per 5 s,
zero crashes, steady 30 fps; normal config zero trips at 43 fps. **The crash
recurred in user gameplay anyway** — necessary, not sufficient.

**Act 2 — read the driver source**: MoltenVK 1.4.1 source establishes:
`MVKCommandBuffer::_needsVisibilityResultMTLBuffer` resets on every
`vkBeginCommandBuffer` and is set ONLY by `vkCmdBeginQuery`
(MVKCmdQueries.mm:49, MVKCommandBuffer.mm:242); `beginMetalRenderPass`
attaches the Metal visibility buffer to an encoder only if that flag is
already set (MVKCommandBuffer.mm:776). Under prefill=2 (immediate encoding —
the shipped plist config at the time; see 2.1), the FIRST render pass's
encoder is created the moment the pass is recorded, *before* any in-pass
`vkCmdBeginQuery` can set the flag. A frame whose first pass contains the
frame's first zpass draw got a nil visibility buffer and crashed in AGX.
Bench scenes start frames with clears/non-zpass draws → hours of soaking
could never reproduce it; the user's area starts frames with zpass draws.

**Fix**: record an empty "primer" occlusion query (begin+end outside any pass
— valid usage, zero samples, harmless in report prefix sums) at every
command-buffer begin, setting the flag before any pass exists — correct by
construction for every CB. Validated 2026-07-04: 60 s soak with
`MVK_CONFIG_DEBUG=1`, zero crashes, zero validation errors, 46 fps, ~12
passes/flip.

**Lessons**: (a) when a driver-level crash defies local repro, READ THE
DRIVER SOURCE — MoltenVK is open; the answer was three line references away;
(b) any per-something allocation whose cadence a change alters needs a
runtime guard, not an assert; (c) scene composition is part of the repro —
"same title, same savestate" can differ in the one property that matters
(first-pass content).

**Reopen if**: n/a. Primer + guard stay even at prefill=0 — they are the
defense for anyone forcing prefill back on.

## 2.3 Pink-flash — torn texture snapshot uploaded anyway

**Status**: shipped-after-fix (`8b1b934045`, 2026-07-04). Distinct from 2.1
despite the color (magenta = this fork's texture-path corruption signature).
**Symptom**: single-frame flashes of Morton-tiled magenta on 2/4-bpp
power-of-two textures, increasingly visible after the 2.4x throughput win.
**Root cause**: the snapshot torn-read mitigation *detected* guest writes
landing mid-memcpy but still uploaded the torn bytes, repairing only on next
bind — one bad frame displayed. Rare at ~16 fps; at 2.4x guest throughput the
tear window is hit often. A latent bug made visible by an unrelated win.
**Fix**: retry the copy until clean (`check_texture_dirty()` is
test-and-clear, so copy+check repeats until no write lands mid-copy; bounded
at 4 attempts, exhausted case keeps old repair-next-frame). Validated: 90 s
live-movement run, 569 window captures scored for magenta clusters, zero
artifacts, 60 fps.
**Lesson**: rare-window races become common after throughput wins — re-run
artifact soaks after big perf changes, not just before.
**Reopen if**: n/a.

---

# 3. MetalFX / presentation

## 3.1 Async MetalFX under GL presentation

**Status**: reverted under GL (`ea23058e1e` records attempt+revert,
2026-04-17); the design later legitimately landed under the Metal
presentation backend (`53375edf82` onward).
**Symptom**: ghosting/jitter — Metal encodes raced the previous frame's
still-active GL read on the same IOSurface slot.
**Root cause**: `SDL_GL_SwapWindow` + vsync cannot express GL↔Metal
cross-API completion; GL's IOSurface sampling isn't fenced to Metal GPU
completion. Needs `MTLSharedEvent` interop — exactly what the Metal-native
backend provides on both sides.
**Reopen if**: n/a — solved by moving the boundary, not retrying under GL.
Pattern: when two APIs can't express a fence between them, move the work so
both sides speak the same synchronization primitive.

## 3.2 Vblank cadence aligned to host display refresh

**Status**: reverted (`d8d1f629e8` → revert `649dd265a0`, 2026-04-19). The
revert message is a model root-cause writeup — read it in full.
**Symptom**: "a 30 fps game runs twice as fast at 60 fps" — guest simulation
speed scaled with host refresh (2x at 120 Hz, 4x at 240 Hz).
**Root cause**: `vblank_interval_ns` drives `process_vblank` →
`graphic_hw_update` → `nv2a_vga_gfx_update` (hw/xbox/nv2a/nv2a.c), which
fires `NV_PCRTC_INTR_0_VBLANK` on every call. There is NO separate NV2A
guest-vblank timer — the original commit's claim that there was is explicitly
retracted in the revert. Retuning the host timer retimes the guest IRQ, and
Xbox titles gate simulation advancement on vblank count.
**Cost of the revert**: MetalFX frame interpolation 2x/4x on 120/240 Hz
panels is mostly neutered for 60 fps sources (30→60 smoothing still works).
**Reopen if**: you first build an NV2A-internal fixed 60/50 Hz guest-vblank
timer decoupled from host present cadence (substantial NV2A-model rework; the
PAL-50 Hz README Future vector shares this precondition). Any shortcut that
touches `vblank_interval_ns` alone re-breaks guest timing.

## 3.3 Push-present ring strict-FIFO consume — the "frames double playing" judder

**Status**: shipped-after-fix (same-day as the default-on promotion,
2026-07-12; `XEMU_PUSH_DEBT=0` restores strict FIFO).
**Symptom** (owner, hours after v0.11.1 + the ring promotion): "jittering,
or it looks like sometimes frames are double playing" — 2x interp,
exclusive fullscreen, 60 Hz-class 5K panel.
**Root cause**: the ring consumer took steps strictly FIFO, one per UI
frame. The v0.11.1 speedups pushed the step rate past the display rate
(38-40 fps × 2x = 76-80 steps/s vs 60 Hz) — the queue backlogs to ring
capacity (~100 ms latency) and the producer force-drops UNREAD steps
(~26/s measured under a 60 Hz-simulated consumer), splicing
midpoint→midpoint with the real frame missing. The old pull path was a
wall-time SAMPLER (no backlog possible); the ring turned present into a
QUEUE, and queues at mismatched rates = latency + splices.
**Fix**: bounded consumer debt (interp mode + 2 steps) with catch-up to
the newest unconsumed REAL frame (needs the publisher's is_real byte on
each entry) — freshest-frame sampling semantics when behind, untouched
paced cadence when the display keeps up. Receipt: unread drops
2,253/2,152 per min → **0** (2 interleaved pairs, 60 Hz sim), skips land
on real frames, refuter 0 mismatches / 0 resurrections over the policy.
**Three instrumentation traps burned during diagnosis** (the receipt was
nearly unmeasurable): (1) `present_ring_count` never decreases —
consumption is a CURSOR, not removal — so "count==CAP ⇒ drop-oldest"
fires on every push after warmup; a drop counter must check
`old->frame_seq > consumed` or it reads ~100% drops in perfectly healthy
runs (the first probe did). (2) Unfocused/occluded bench windows present
UNPACED (~200 Hz observed) — no vsync throttle — so ring pacing bugs are
INVISIBLE headless; `XEMU_UI_FRAME_CAP_NS` (test knob) simulates a real
consumer. (3) A too-tight debt bound false-fires on healthy transients:
a flip publishes its whole schedule atomically, so unconsumed depth
legitimately jumps by `mode` per publish — the bound must exceed one
full schedule.
**Reopen if**: n/a (fix is sampling-correct). If a future display path
consumes at exactly the step rate (e.g. 120 Hz panel, 60 fps × 2x), the
policy is inert by construction — no action needed.

---

# 4. DSP JIT (hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c)

Jargon: **parmove** = DSP56300 parallel move executing alongside an ALU op in
one instruction. **GP/EP** = the two DSP cores (Global/Effects Processor).
**block** = translated basic block. **pin** = a DSP register held in an ARM64
callee-saved register for a block's duration. Design doc:
`docs/dsp-jit-design.md` (rounds table records landed/dropped per round).

## 4.1 Round-4 EPI_NO_PC — the 9.75% that killed a "safe" elision

**Status**: settled-negative, dropped permanently (`d0e1f0b27d`, 2026-04-19);
superseded by the *correct* fix `ca7fe51a0b` (same day).

**The idea**: skip the per-op PC-mismatch exit check for parmove stubs and
inlined long-immediate ALU ops — "those ops never mutate pc."

**Symptom when forced**: `XEMU_DSP_JIT_FORCE=2` crashed on startup — blocks
ran garbage past their proper boundary.

**Evidence that killed it**: the runtime-sentinel harness (`1f444fa272`,
logger-placement bug fixed in `1f81e22573`) watched 59.2M parmove + long-imm
ops in one Azurik startup and observed 5.77M pc divergences — a **9.75%
mismatch rate**, from two legitimate sources the static classifier cannot
predict: (a) calc_ea mode 6 makes parmoves 2-word ops (post-exec pc = pc+2
while the translator expected pc+1); (b) ops inside REP/DO hardware loops
have pc *rewound* to repeat the loop body. Both legitimately need the exit.

**The right fix instead**: teach the translator, don't blind the check.
`ca7fe51a0b` made `dsp_jit_helper_inst_length` classify mode-6 parmoves as
2-word (per-parmove-class decode proof in the commit message), eliminating
the dominant mismatch source so blocks stay intact — better inlining density,
no safety loss. The check itself costs 2 insns (SUB+CBNZ imm12 fast path,
landed in `85dc0358dc`; related: `037022a364` fixed mode-6 immediate baking
to full 24-bit).

**Reopen if**: essentially never — the check is 2 instructions. Any future
elision proposal must first produce a sentinel run (harness pattern in
`1f444fa272`) showing ~0% divergence including REP/DO behavior.

## 4.2 Round-4 cur_inst preset skip

**Status**: fenced-off (README row; reverts `aa9eed38e8` / `201c616e02` /
`a4a3dfa131` reverting `fc52303f3d` / `d8da51e359` / `24f9a5b421`, all
2026-04-18; safe round-4 pieces re-landed in `85dc0358dc`).
**The idea**: skip the unconditional `dsp->cur_inst = inst` store for
handlers the classifier deems fully inlined.
**Symptom**: regresses on startup with `op=0x001000` — some inlined path
reads `cur_inst` at runtime that the static classifier doesn't see; the
interpreter (post block exit) observes stale bits and asserts on an unknown
opcode.
**The fence with a gate**: the diagnostic harness is kept in-tree.
`XEMU_DSP_JIT_SENTINEL=1` re-applies the skip with poison value `0x00adbeef`
— any runtime reader of `cur_inst` surfaces as
`lookup_opcode_slow(op=00adbeef)` whose stack trace identifies the missed
reader (pm_4x exempted; `emu_pm_4x` legitimately reads cur_inst — discovered
via `24f9a5b421`). `XEMU_DSP_JIT_FORCE=1` force-enables the skip.
**Reopen if**: a sentinel run identifies the hidden reader(s), each gets an
explicit cur_inst write or inline substitution, and a full
`XEMU_DSP_JIT_DIFF` run passes clean. The tools are waiting; the hunt was
never worth the ~1 insn/op.

## 4.3 A/B accumulator pinning — land, crash, revert, instrument, reapply

**Status**: shipped-after-fix. Sequence (all 2026-04-19, first-parent order):
`91343ad754` land → crash → `67c0e4c204` revert → `5b9c8d329e` reapply →
`be1fe6e242` add `XEMU_DSP_JIT_PIN_AUDIT` → `bf82c37fba` fix the leak; later
`3d0f4867a2` X/Y pinning + `54d1543d50` its own leak fix.

**Symptom**: "Out of bounds read at 0x2806" → `dsp_stack_pop` assertion
shortly after startup with pinning enabled.

**Root cause**: `emit_pm_write_reg`'s non-composite branch — parmoves writing
the *individual* accumulator slots (A0/A1/A2/B0/B1/B2) directly — updated
memory but not the pin. Azurik's init path hits it immediately: a pm_2_2
"Y0,A1" parmove (pc=0x0207, inst=0x220c00) cleared A1 in memory while the pin
kept the stale byte; pinned reads computed on wrong state → corrupt R
registers → bad PC → stack underflow.

**How found**: PIN_AUDIT (`be1fe6e242`) emits a ~30-insn re-pack + compare
after every op and prints the first divergent (pc, inst) pair — it pointed at
the exact instruction **in one 30-second run** (credited in `bf82c37fba`).
The same audit then caught the X/Y pin leak in pm_8/pm_1 direct STRs
(`54d1543d50`) when X0/X1/Y0/Y1 pinning followed.

**Lessons**: (a) for any pinning feature, write-through invariants break at
*direct-memory* write sites — enumerate them exhaustively (the `91343ad754`
message is the checklist pattern); (b) build the audit BEFORE the second
pinning feature, not after the second crash; (c) revert-fast-then-diagnose
beats debugging in-tree — the revert/reapply pair kept the branch shippable
throughout.

**Reopen if**: n/a — shipped. For any new pin, run
`XEMU_DSP_JIT_PIN_AUDIT=1` before calling it done.

## 4.4 Chain-site repatching ran outside the JIT-write window (W^X)

**Status**: shipped-after-fix (fix recorded in `aff8c229e9`, 2026-06-09:
"fix chain repatch running outside the JIT-write window").
**Background**: block chaining (`dec694349f`, 2026-04-19) patches a `B
target->chain_entry` into the source block; on invalidation, each still-live
incoming chain site is re-patched to branch to the source's shared exit.
macOS Apple Silicon enforces W^X on JIT pages — `pthread_jit_write_protect_np`
toggles a thread between write and execute mode.
**The fault**: the invalidation-time repatch wrote JIT code memory outside
the write-enabled window — a latent crash. Moved inside the window.
**Lesson**: on Apple Silicon, EVERY store to JIT code memory — including
"tiny" one-instruction repatches on cold paths — must sit inside an explicit
write-window bracket. Grep for code-buffer stores whenever adding a patch
site. See 6.3/6.4 for the two failed attempts to be clever with the same W^X
API in QEMU's TCG.
**Reopen if**: n/a.

## 4.5 Diff-validator starvation → the async validator architecture

**Status**: shipped-after-fix (`5c093dace1`, 2026-04-18).

**Symptom** (recorded during 2026-04 bring-up): with the original inline
validator at high sampling, the emulator effectively hung at startup (stuck
launch, dock-icon bounce) — the APU thread held `d->lock` across full
interpreter replays of every block.

**Shipped architecture** (know it before touching validation): dedicated
worker thread `mcpx.dsp_diff`; the APU thread snapshots pre/post state into a
16-slot SPSC ring (~2 memcpys, ~160 KB bound per unchecked block) and never
holds `d->lock` across replay; the worker replays the interpreter on a
private `dsp_core_t` with peripheral callbacks swapped to shims; ring-full
drops newest — validation degrades to sampling instead of stalling the
emulator. Key insight bounding total work: **a translated block is
deterministic given its pre-state, so ONE passing validation per translation
proves all future executions** (`DspJitBlock.diff_checked`, cleared on
retranslate/invalidate) — hundreds-to-thousands of checks per session instead
of millions per second. Write-set-narrowed compares (`DSP_JIT_WS_*`) cut
per-check cost ~40 KB → ~2 KB on hot blocks. Knobs: `XEMU_DSP_JIT_DIFF=N`,
`_DIFF_SYNC=1` (bring-up: abort at the divergent block), `_DIFF_MAX=N`.
Chaining is disabled under DIFF (harness assumes one block per dispatch). The
architecture caught a real bug on arrival: pm_5 `y:(r4)+,y0` wrote 32
unmasked bits into Y0 (0xCACACACA yram sentinel leaking through).

**Lesson**: validators must be architecturally incapable of blocking the
thing they validate — budget by *unique translations*, not executions. This
is the fork's reference adversarial-refutation harness; the generalization
lives in `xemu-research-methodology`.

**Reopen if**: n/a.

## 4.6 EP retranslation churn — JIT slower than the interpreter until throttled

**Status**: shipped-after-fix (`21e9045344`, 2026-07-03).
**Symptom**: with the inline JIT on both cores, APU-thread utilization was
WORSE than the interpreter (2026-07-03, Azurik attract, `XEMU_APU_PROF=1`):
inline JIT both cores 25%, interpreter 22%, upstream dsp56300 engine 19%.
**Root cause**: the EP core's program overlays P-space every pass, so every
pass invalidates and retranslates — EP translated 418k blocks while executing
4.2M (1:10) vs the healthy GP core (1.3k:14.5M). Translation cost swamped
execution savings.
**Fix**: per-core churn evaluation per 256Ki-execution window; a core is
permanently handed back to the interpreter when >1/16 of executions caused a
retranslation. EP trips on the first window (26,348 retranslations); GP never
trips. Result: JIT+throttle 20% — best of the four configurations. The 8 MiB
code buffer stays mapped. Escape hatch: `XEMU_DSP_JIT_NO_THROTTLE=1`.
**Lesson**: a JIT's break-even is translation cost / re-execution count —
measure the ratio per *workload region* (here: per core) and give the JIT a
surrender policy.
**Reopen if**: EP-side JIT is worth revisiting only with a design that
survives per-pass P-space overlay (e.g. content-keyed block cache), evidenced
by the 1:10 ratio moving toward 1:1000+.

## 4.7 Upstream-merge collateral: env-var strings silently renamed

**Status**: shipped-after-fix (`21e9045344`; merge `fd467e02b9`, 2026-07-03).
**Symptom**: after the upstream merge (which renamed the fork's JIT symbols
`dsp_jit_*` → `dsp56k_jit_*` because upstream's new engine took the old
namespace), the documented `XEMU_DSP_JIT_DIFF/STATS/SENTINEL/...` knobs
silently did nothing.
**Root cause**: the mechanical rename sed also rewrote the `getenv()` STRING
LITERALS to `XEMU_DSP56K_JIT_*`. `XEMU_DSP_JIT` itself survived; the
sub-flags did not — a user-facing interface changed by a refactor that
"couldn't affect behavior."
**Lesson**: after any bulk rename, grep the diff for `getenv(`, config keys,
and CLI strings — user-facing names are not symbols. (Merge-mediation rules
themselves — naming, engine precedence via `dsp_want_external_jit_engine()`
(the `config_spec.yml:339` comment now names that selector correctly; its
earlier stale wording was fixed — verified 2026-07-11) —
live in `xemu-change-control`; the merge commit message documents them.)
**Reopen if**: n/a.

---

# 5. APU (hw/xbox/mcpx/apu)

## 5.1 rate==1.0 voice_resample fast path

**Status**: reverted (`68d5afe7fa`, 2026-04-17; README row).
**Symptom**: implicated in the level-transition freeze (entry 1.4): games
fade/drain many voices at transitions and wait on an audio-finished signal.
**Root cause**: the libsamplerate callback path pads with SILENCE when a
voice starves, so reads always return a full frame once a voice is active.
The fast path returned `got` (possibly 0) on starvation, and
`voice_process`'s outer loop retries on any return ≥ 0 — a draining voice
could bounce "active, got=0, retry" forever, never signaling completion.
**Lesson**: a fast path must replicate ALL slow-path semantics — including
those that only matter in drain/teardown states. SRC_LINEAR was cheap anyway.
**Reopen if**: only with silence-padding semantics replicated exactly — and
honestly, don't; the saved cost was negligible.

## 5.2 Voice register `__thread` cache

**Status**: settled-negative (README row, early history — no dedicated commit
survives). Cached voice registers went stale: Xbox hardware mutates voice
registers via DMA behind the CPU's back. Per-thread caching of DMA-writable
state is unsound by construction.
**Reopen if**: n/a.

## 5.3 HRTF hand-NEON

**Status**: settled-negative (README row; the shipped alternative — FIR
linearized for autovectorization — recorded in `aff8c229e9`). Hand-written
gather-then-FMA on a circular buffer defeated out-of-order overlap;
`-O3 -mcpu=native` autovectorizes the linearized form better.
**Lesson**: restructure data flow for the autovectorizer before writing
intrinsics; measure both.
**Reopen if**: profile shows the autovectorized FIR as a top-5 APU cost
(`XEMU_APU_PROF=1`) AND a prototype wins an interleaved A/B.

## 5.4 Voice-list SE2FE_IDLE_VOICE re-notify skip

**Status**: settled-negative (README row). No guest-write hook exists on
`NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` transitions, so a "notified this
activation" flag can't be cleared reliably; risking a missed FE-observed idle
transition outweighs the small per-frame saving.
**Reopen if**: a write hook on that register path is added.

## 5.5 APU / snapshot-load race

**Status**: shipped-after-fix (`d6da581d4b`, 2026-07-03).
**Symptom**: SIGABRT ~1 s after a `-loadvm` boot — assert in `read_memory_p`
(DSP PC outside PRAM); latent race on in-app loads too.
**Root cause**: `mcpx_apu_reset_hold` resumed the APU frame thread
unconditionally. Snapshot load resets the machine *while stopped*, then
restores device state — the prematurely-resumed thread executed the DSP
against a half-restored core as vmstate wrote SECTL/GPRST under it.
**Fix**: reset resumes the thread only when `runstate_is_running()`; when
stopped, the RUNNING vm-state transition (which runs `dsp_sync_from_vm`
first) owns the resume.
**Lesson**: device reset hooks run in both live-reset and snapshot-restore
contexts — any thread resume in a reset hook must check runstate.
Generalizes to every device with a worker thread.
**Reopen if**: n/a.

---

# 6. TCG / x87 FPU (target/i386, tcg/aarch64)

## 6.1 floatx80 union overlay on ARM64

**Status**: settled-negative (README row, early hard-FPU history). Overlaying
QEMU's floatx80 (x87 80-bit extended float) with a host union segfaulted —
layout incompatible with IEEE 64-bit on ARM64. The shipped approach is
explicit fast floatx80↔double conversion (`41f9ee13c4` era).
**Reopen if**: n/a.

## 6.2 FSIN/FCOS libm-helper inline

**Status**: reverted (`b6b8dfe6b2` → revert `778db912ed`, 2026-04-17; the
README row carries the analysis — the revert commit itself is bare).
**Symptom**: crackly audio on at least one title after swapping
`gen_helper_fsin`/`_fcos` (floatx80 round-trip) for thin
`helper_sin_fast_f{32,64}` bit-cast libm calls.
**Plausible causes recorded** (not fully proven — reverted on symptom):
(a) `gen_flush_fp` was the de-facto flush point for *other* live x87 temps,
and the new helpers skipped it; (b) `TCG_CALL_NO_RWG_SE` let TCG reorder
across boundaries the floatx80 helper implicitly fenced.
**Lesson**: helpers can be load-bearing as *barriers*, not just as
computations. The same implicit-fence class killed 6.5 at the design stage —
one revert educated a later evaluation, which is exactly this file's job.
**Reopen if**: a design explicitly preserves `gen_flush_fp` ordering (or
proves no live temps cross the call) AND ships with an audio A/B on the
affected title class.

## 6.3 pthread_jit_write_with_callback_np for tb_phys_invalidate (A3)

**Status**: reverted (`d4f2e3e226`, 2026-04-17; landed in `ea23058e1e`).
Apple's callback API has scoped-W semantics — W permission is forcibly
dropped on callback return — which does not compose with QEMU's *nested*
JIT-write paths on the same thread. Correlated with rare freezes under heavy
TB invalidation; perf delta marginal. Manual `qemu_thread_jit_write/execute`
pair restored.
**Reopen if**: QEMU's invalidation paths become non-nested (they won't) —
treat the API as incompatible with QEMU.

## 6.4 TLS-cached pthread_jit_write_protect_np state

**Status**: settled-negative (README row). A per-thread cache of the W^X
toggle drifts from the kernel's actual state on nested `tb_gen_code`/setjmp
paths → SIGSEGV/SIGILL after ~1 minute.
**Lesson** (shared with 4.4 and 6.3): W^X state on Apple Silicon must be
bracketed explicitly at every write site; never cached, never inferred.
**Reopen if**: n/a.

## 6.5 Cross-TB FPCR elision via `cached_fpuc_rc` as a TCG global

**Status**: settled-negative — evaluated and rejected WITHOUT landing (README
row; evaluation recorded in `aff8c229e9`, 2026-06-09). TCG globals are
memory-backed and reloaded per TB, so a global doesn't eliminate the per-TB
`ld16u` in `gen_flcr` — it renames it. A real elision needs TB-chain metadata
(rounding-control equality across chain edges) or relaxing `gen_flush_fp`'s
`flcr_set` reset — the same implicit-fence risk class that bit 6.2. No win at
acceptable risk.
**Reopen if**: TCG grows chain-edge metadata upstream. This entry is the
model for *documenting a rejected design before writing code* — cheaper than
a revert.

## 6.6 HLT BSOD recovery (IF=1 force)

**Status**: reverted — both legs removed (`6eea148914`, 2026-04-17; landed
via `6556192d87`, extended in `b545d9da1f`/`4693ed9ba1`).
**The idea**: guests stuck in `CLI; HLT` ("BSOD pattern") got IF forced on so
pending IRQs could deliver.
**Why it died**: diagnostics during entry 1.4 showed the recovery firing
~1500/s at the kernel idle thread while the game never advanced — the pattern
is a *symptom* of some other thread being blocked; force-delivering the IRQ
runs the idle tick but cannot unblock the real waiter. Removal was the
clean-room test: the freeze persisted identically, proving the recovery
neither caused nor cured anything (true cause: entry 1.7). Masking a
deadlock's signature is worse than the deadlock.
**Reopen if**: never as a recovery hack. If a reproducible `CLI; HLT` hang
appears with `XEMU_PFIFO_HEARTBEAT` clean, root-cause the blocked waiter
(see `xemu-debugging-playbook`).

## 6.7 Superblock seam-following — three receipts and a corrected theory

**Status**: settled-negative for seam-following policies (2026-07-18);
mechanism fenced-off DARK in-tree (`XEMU_SUPERBLOCK=N`, default 0) as
the foundation for the region-formation redesign (roadmap item 1).
**The idea**: harvest the `XEMU_CCOP_CENSUS` finding (~39-44% of block
boundaries carry a dead flag materialization) by not ending TBs at
branch seams: M1 concatenates through unconditional same-page forward
jmps (hook in `gen_JMP` before `gen_update_cc_op`; forward-only
protects `tb->size` contiguity, P0-page gate protects `translator_ld`
and CF_PCREL unwind); M2 continues through conditional fallthroughs
with the taken edge in an out-of-line stub on the lookup path (no
goto_tb slot burn, no xpage registry entries).
**Evidence (all F8, interleaved, scene-gated)**:
(1) M1-alone instrument-killed pre-A/B — the runtime tail-kind census
(`XEMU_SUPERBLOCK_SIZE=1`, 2.397B taken exits) measured jcc_taken
50.4% / jcc_fall 27.3% / call 12.2% / uncond 5.2%, with the
M1-capturable subset **3.7%** vs a pre-registered ≥10% build bar.
(2) M2 all-conditionals, cap 4: **−1.6% draw throughput, 3/3 pairs**
(fps −8.4% after the title's effect-load feedback also thickened the
slower arm's frames ~7% — always compare draws/s on composition
shifts). (3) M2 forward-conditionals-only: **−2.3% throughput, 3/3**,
with ~29k seams/run merged (4,922 jmp + 24,064 fallthrough) — the
loop-back-edge-protection hypothesis refuted.
**The corrected theory (the durable lesson)**: TCG spills dirty
cc/eip globals before ANY branch whose taken edge leaves the TB,
because the exit's successor re-derives state from env. So a
conditional seam can NEVER elide the flag materialization the census
counted, no matter where the stub lives — the census's dead-flag mass
is harvestable only where BOTH successor edges stay inline. That is
region/diamond formation (translate both sides of short forward
conditionals to their join), a different and larger design. Bonus
finding: uncond-jmp seams (where pure concatenation genuinely works,
M1) are only 3.7% of executed transitions on this workload — trace
formation's textbook prey is thin here.
**Reopen if**: only as region formation (both edges inline to a join),
with the movement-phase probe + loadvm soak as REQUIRED gates before
any default-on (TB-lifetime class). Do not re-bench stub-layout
variants of seam-following; two policies died with receipts.

## 6.8 Region/diamond formation — the closing campaign, five receipts deep

**Status**: settled-negative (2026-07-18, same day as 6.7); the whole
stack — TCG recorded-label liveness elision, the `XEMU_REGION_CHECK`
double-pass conformance harness, and the i386 diamond former — stays
in-tree DARK as the reproducible record.
**The design** (the exact reopening 6.7 named): forward jcc arms
internalize — the taken edge branches to an intra-TB label bound at
the join, so NEITHER edge exits — and a liveness-only TCG change lets
globals provably dead into a flagged forward join keep `TS_DEAD`
without the `TS_MEM` forcing at both the brcond and the label, so the
dead flag stores never emit. Verified sound: no allocator edits needed
(`DEAD_ARG` frees dead globals to `TEMP_VAL_MEM` before every seam
assert), `liveness_pass_2` inert for i386, bit-identical arg_life with
zero flagged labels (held over full boot+game runs).
**Traps burned building the harness** (each a lesson): (1) the
double-pass compared op lists BY INDEX, but `liveness_pass_1` also
REMOVES dead ops — misaligned comparisons produced phantom violations
until the diff was keyed by op pointer; (2) the recorded predicate
must be EXACTLY `TS_DEAD` — at an exit-bound tail `la_func_end` marks
everything `TS_DEAD|TS_MEM`, where "dead" only means the TB ends and
the store is still owed; (3) legitimate diffs include added AND
removed deaths (values consumed by the branch live longer in
registers) — the only always-illegal direction is an ADDED sync.
**Receipts**: candidate census PASSED its ≥15% pre-registered gate at
21.4% of executed boundaries (fwd-jcc ≤64 B × head-KILL). A/B window
64 B: **−1.34 fps (−4.5%), 3/3, B arms ±0.30** — mechanism: 72% of
opened regions drain-demoted into 6.7's known-negative stub shape
(11,240 vs 4,290 internalized). The single pre-registered mechanism
iteration (window 16 B, predicted demote ratio <25%): measured **64%**
and **−0.69 fps, 3/3**. STOP fired permanently.
**Durable lessons**: (a) a passed candidate census sizes the POOL, not
the NET — the demote path's cost model must be priced into the gate;
(b) the no-rollback demote fallback means every failed open ships the
prior campaign's negative shape — open-rate policies inherit the dead
design's cost; (c) when a validation harness aborts, suspect the
harness's own comparison model before the mechanism (three "violations"
here were harness bugs; zero were real).
**Reopen if**: only with a fundamentally different attack (persistent
profile-guided region selection; cross-TB IR caching) — never another
window/policy variant of this former. The liveness elision itself is
correct and reusable by any future design that can flag its joins.

## 6.10 Audit §1.2's benefit base had already collapsed 34-57x — the proxy-collapse class, second sighting

**Status**: settled-negative for the fps CLAIM (2026-08-04 wave); the code
ships as a no-claim simplification, default-on with its hatch
(`XEMU_DIRTY_FAST=0` restores both legacy shapes; latch at
`system/physmem.c:1059`). Entry 6.9 is its mirror image — read them
together.
**The candidate**: `docs/fork-optimization-audit-2026-08.md` §1.2 priced
the `notdirty_write` store-slow-path tail at **1.9-2.0% of vCPU wall**
and predicted **+0.2 to +0.35 fps** for (a) deleting the
`physical_memory_is_clean()` re-read whose answer the invalidation
already knew and (b) fusing the five RCU-guarded `find_next_bit` walks
into one guard + one idx/offset + `test_bit`. The price came from the
audit's own committed profile receipt
(`bench-out-profile-f8/ab_E1.log:1218-1220`: **6,726,497** calls with
**5,050,536** code-page traps = **75.1%**, i.e. ~89.7k calls/s over the
75 s window).
**What the Gate-0 census measured on today's binary**: in-scene
`notdirty_write` runs at **~1.6-2.6k calls/s** (60 s differential
1,560/s; two-phase solve 2,622/s) and the code-page trap share is
**2.2%** (5,512 of 247,600). At 2,622 calls/s × 235 ns/call the entire
tail is **~0.06% of one core** — a **34-57x smaller** base than the
audit assumed. Nothing regressed: this tree's own shipped work (6.9's
arm-(b) store prefilter, on top of 1.23/1.24) had already inhaled the
population, completing inline the stores that used to trap.
**Second finding, same session**: §1.2's own `XEMU_INV_TIMING` gate is
**unreadable** at that call rate — two LEGACY-mode runs read
`rest=`235.4 and 179.1 ns/call, a **56.3 ns** self-disagreement larger
than the 56.0 ns effect the gate existed to resolve. An instrument
whose control arm disagrees with itself by more than the effect cannot
adjudicate anything; do not "average the two runs".
**Disposition**: no individual A/B was spent. The change is
behavior-equivalent, soak-validated, hatched, and covered by the wave's
headline cross-binary A/B — so it stays in the tree as a
simplification with **no fps attribution in any document**.
**The durable lesson (pair it with 6.9)**: 6.9 died of a proxy that
UNDERcounted the population 20x; this is the same failure inverted — a
benefit base quoted from a *committed profile* silently OVERcounts once
a shipped optimization eats the population that profile counted. One
rule covers both: re-measure the population **at the gate the emitted
code will actually see, on today's binary**, before pricing any
candidate. A profile is dated evidence, never a standing fact — and an
audit that cites `file:line` of a log is citing a date.
**Reopen if**: `XEMU_INV_PROF=1` on a real scene shows in-scene
`notdirty_write` back above ~50k calls/s (i.e. a workload whose stores
escape the arm-(b) prefilter), AND `XEMU_INV_TIMING`'s legacy arm
reproduces itself within the claimed effect. Both conditions; the
second is what makes the first measurable.

---

# 7. PFIFO / threading

## 6.9 Arm (b) promotion — the proxy that undercounted 20x, and the fps that hid a +12% win

**Status**: shipped-after-proof (default-on 2026-07-18, same night as the
campaign close; receipts in the promotion commit and the ledger entry).
**What happened**: arm (b)'s +0.3-0.7 fps prediction was sized on the
INV_PROF code-page notdirty trap counter (~90 k/s ≈ 2% of the vCPU
thread). The first quiet-machine A/B read **−0.24 fps (1/3 pairs)** —
a kill on the pre-registered fps bar — but the scene-identity check
flagged an 11% composition shift: every E run rendered ~815 draws/flip
against every B run's ~727, zero overlap, across six pairs in two
independent batches. The protocol's standing composition-shift rule
(the 1.18 correction) says compare draws/s: **+11.8% and +12.0%,
6/6 pairs positive** (batch 2 was also +0.43 fps 3/3 raw).
**Root cause of the misprediction**: the proxy. The stub's own counters
showed **166M eligible slow-path entries per ~85 s in-scene (~2M/s,
100% skipped)** — the population is dominated by stores into
renderer-watched pages (NV2A/NV2A_TEX dirty clients, bits already set),
which the code-page trap counter never counted: a ~20x undercount. At
~2M/s, ~60 ns saved per inline completion ≈ the observed +12%
equilibrium shift. The fps flatness is Azurik's effect-load feedback:
the guest converts freed CPU into more effect draws until frame time
re-saturates (visuals identical, screenshots clean).
**Evidence**: two 3-pair interleaved batches (throughput 6/6 positive;
receipts in `armb-ab`/`armb-ab2` session dirs and the promotion
commit); counter probes FAST=0 vs FAST=1 (5.45M code-page traps vs
166M stub entries); refuter total **~19.5M decisions / 0 violations**
across static, loadvm-cycling (6 reloads), and movement-probe
streaming (tb_flush flat at 1) soaks.
**Lessons**: (1) price a fast path on the population the emitted code
will actually SEE (instrument the gate itself), not on a helper-side
proxy counter — proxies count what reaches the helper, not what the
stub intercepts; (2) on composition-shifting scenes an fps A/B can
bury a real throughput win exactly as it buried the M2 losses —
draws/s is the symmetric metric, and it must be applied for promotion
as readily as for kills; (3) a pre-registered kill bar stated in fps
should say what happens when the scene-identity gate fires — the
correction path was already protocol, which is what kept this from
being a judgment call.
**Reopen if**: n/a (shipped; `XEMU_SUBPAGE_FAST=0` reverts).

## 7.1 PFIFO untimed wait (A1): reverted on suspicion, re-added with proof

**Status**: shipped-after-fix (`ea23058e1e` land → `59b85d1e86` defensive
revert → `a005bb1560` re-add, all 2026-04-17).
The upstream 1 ms `qemu_cond_timedwait` in the pfifo idle loop was replaced
by an untimed wait (killing a ~1 kHz idle wakeup). Reverted during the freeze
hunt "as a safety net"; after `XEMU_PFIFO_HEARTBEAT` exonerated pgraph,
re-added — this time with the proof enumerated in the commit: all 15
`pfifo_kick` call sites hold `d->pfifo.lock` across the kick-set + broadcast
pair, so a wakeup cannot be lost.
**Lesson**: an untimed wait is only as safe as the *complete enumeration* of
its wakers. Adding a 16th kick site requires re-verifying the lock
discipline, or the lost-wakeup class returns. (Enumeration: `a005bb1560`.)

## 7.2 Descriptor-set bind-skip (C1): same arc

**Status**: shipped-after-fix (`2534c9bc1a` revert → `a005bb1560` re-add,
2026-04-17). Skipping redundant `vkCmdBindDescriptorSets` is correct per
Vulkan pipeline-layout-compatibility rules (all draw pipelines share the
(ubo_layout, tex_layout) pair; clear pipelines use a zero-descriptor layout
that doesn't disturb bindings). Reverted only to shrink the freeze suspect
list; re-added with the reasoning written down.
**Reopen if**: n/a — but if pipeline layouts ever diverge per draw type, the
compatibility argument must be re-proven.

## 7.3 BQL event batching

**Status**: settled-negative (README row). Batching events while holding the
BQL (QEMU's Big Lock) breaks QEMU's cooperative scheduling — blocking while
holding it starves vCPU/timer threads. Architectural, not tunable.
**Reopen if**: n/a.

## 7.4 Hoisting `can_fifo_access` out of the pusher word loop

**Status**: settled-negative (README row). The hoist assumed "lock held
throughout the loop" — false: `pfifo_run_puller` drops `pfifo.lock` when
taking `pgraph.lock`. Result: `ERROR_CALL` on boot.
**Lesson**: in pfifo/pgraph code, every hoist over a loop must first map the
lock drop/retake points; the two-lock dance makes "obviously invariant"
conditions mutable mid-loop.
**Reopen if**: n/a.

## 7.5 BQL-free MMIO dispatch for PFB/USER (lockless_io on the TCG path)

**Status**: shipped (`ee9100e538`, 2026-07-05) — with an honest
fps-parity receipt, not a speed claim.
**Mechanism**: `XEMU_MMIO_PROF` (new, default-off) measured 8.2M guest
MMIO ops in 70 s on the heavy scene — PFB 4.85M (3.2M loads =
`NV_PFB_WBC` write-combine-flush polling) + USER 1.58M doorbell
stores = 78% of all traffic — each paying the unconditional
`BQL_LOCK_GUARD()` in cputlb's MMIO helpers. Upstream's
`mr->lockless_io` (added for the address-space path) is now honored on
the TCG fast path too, and the two audited regions opt in (PFB:
constants/plain regs, no IRQs; USER: entire handler under
`pfifo.lock` with the Invariant-10 kick discipline).
**Evidence**: locked crossings 8.2M → 1.64M (−80%, histogram-verified).
6-pair A/B: fps parity (mean +0.54 riding one outlier; the +0.3-1.0
prediction recorded as killed) BUT per-run fps stdev 1.21 → 0.89 with
5/6 E-runs steadier — the BQL-spike class (max 340 µs/acquire) no
longer hits these ops. `XEMU_MMIO_BQL=1` restores locked dispatch.
**Lesson**: average lock cost (~52 ns) was never the story; the tail
was. When a mean-fps A/B reads parity on a lock-scope change, check
intra-run variance before calling it valueless.
**Reopen if**: closed 2026-08-04 — PGRAPH shipped
(`docs/pgraph-lockless-audit.md`), APU-VP shipped, and the remaining
blocks (PMC/PCRTC/PTIMER, APU main/gp/ep) closed by written verdict
(`docs/lockless-mmio-verdict.md`): residual < 0.07% of a core and
BQL-free dispatch there would drive `pci_irq_assert` unlocked. Reopen
only on `XEMU_MMIO_PROF` `BQL wait total` ≥ 500 ms / 75 s (the wave's
2026-08-04 reading was 75.6 ms per 75 s, 6.6x under the bar).

---

# 8. Build / CI / release

## 8.1 Release-CI hardening — five pre-existing portability bugs

**Status**: shipped-after-fix (2026-07-03, the first-ever full CI run of the
fork). None were caused by the CI work — CI *exposed* them. Each is a class
to re-check when touching build flags or adding SIMD/SDK code:

| Bug | Root cause | Fix |
|---|---|---|
| fpng ARM CRC32 hard compile error (Linux aarch64, Win arm64) | `__crc32*` intrinsics are always_inline and need the `crc` target feature; Apple `-mcpu` implies it, generic aarch64 baselines don't | `378915e067`: gate on `__ARM_FEATURE_CRC32`; `-march=armv8-a+crc` on non-Apple aarch64 |
| macOS x86_64 cross-build link failure | only CFLAGS/LDFLAGS carried `-arch/-isysroot/-target`; glslang's CMake C++ try-compile built host-arch objects. Previously masked: with no MoltenVK on the builder, Vulkan silently disabled and glslang never configured — MoltenVK auto-vendoring made clean runners exercise it for the first time | `378915e067`: export CXXFLAGS/OBJCFLAGS |
| MetalFX interpolation build break on CI | `MTLFXFrameInterpolator` types exist only in the macOS 26 SDK; runtime `@available` cannot save compile-time type references — runners ship SDK 15 | `497c1ff3e6`: compile only when `__MAC_OS_X_VERSION_MAX_ALLOWED >= 260000`, inert stubs otherwise |
| Windows link failures under forced LTO | build.sh forced thin-LTO/-O3 everywhere; GCC meson-errors on `b_lto_mode=thin`, and llvm-mingw ThinLTO broke `qemu_build_not_reached` elision on arm64 (release-only undefined symbol) | `ef7637611a`: Windows keeps only LTO-independent knobs; `497c1ff3e6`: disable `TCG_TARGET_HAS_fpu` on `_WIN32` (the FP-TCG paths were the unfoldable branches) |
| windres syntax error on TAGGED Windows builds only | `version.rc` FILEVERSION must be numeric; fork tag `v0.8.153-macos.1` yielded token "macos.1" | `c2860668b7`: sanitize all four fields to digits |

**Meta-lessons**: (a) "works on my machine" for a fork means one toolchain ×
one SDK × one -mcpu — the matrix is the test; (b) tag-derived version strings
are build inputs — tagged builds exercise paths untagged builds never do (the
FILEVERSION bug was unreproducible without a real tag); (c) an environment
improvement (MoltenVK auto-vendoring) can *unmask* latent bugs elsewhere —
that is the improvement working.

**Reopen if**: n/a — but check all five classes when adding intrinsics, new
SDK APIs, or build flags. The Windows-preservation gate itself lives in
`xemu-change-control`.

## 8.2 XEMU_INPUT_PIPE broke the Windows build

**Status**: shipped-after-fix (`cf85e96597`, 2026-07-04 — the v0.9 tag
commit). The FIFO-based test-input channel used `O_NONBLOCK` + FIFOs, which
don't exist on MinGW. Guarded to `#ifndef _WIN32`; compiles to a no-op on
Windows. Same class as 8.1: every POSIX-flavored automation facility needs a
Windows story, even if the story is "absent".
**Reopen if**: someone wants Windows input injection — that's a named-pipe
port, not an unguard.

## 8.3 Tag-deletion incident

**Status**: incident (recorded 2026-07-03/04 release week; the standing rule
lives in `docs/RELEASING-macos.md` "Do not delete and re-push a tag" and in
`xemu-change-control`).
**What happened**: deleting a published release's git tag silently flipped
the GitHub release to DRAFT (vanished from the public releases page — no
warning). Additionally, force-updating a tag re-fires `release-on-tag.yml`,
which must be cancelled before its publish step replaces already-published
assets.
**Standing invariants**: published tags are immutable; one release per tag; a
fresh version every release. `v0.8.153-macos.1` (at `c2860668b7`) remains
published under the old upstream-suffixed scheme; `v0.9` (at `cf85e96597`)
begins the fork-versioned scheme.
**Reopen if**: never. There is no safe tag mutation on a published release.

## 8.4 PGO merge-skip trap + the set -e ls-glob CI red

**Status**: shipped-after-fix (`fcb874d256` + same-day fix, 2026-07-05).
**Trap 1 (silent)**: build.sh use-mode merged profraw → profdata only
when `default.profdata` was ABSENT — so the first use-build after a
retrain silently used the stale committed profile. Detected because the
"retrained" profdata's mtime never moved; cost one wasted 25-minute
build. Fixed: re-merge when any .profraw is newer than the profdata.
**Trap 2 (loud)**: the fix's `newest_raw=$(ls -t ...*.profraw | head -1)`
aborts build.sh under `set -e` when the glob matches nothing — which is
exactly CI's use-mode leg (committed profdata, no raw files in the
source tarball). All-macOS-red on push; fixed with `|| true` inside the
substitution and BOTH paths simulated under `bash -e` before re-push.
**Lessons**: (a) build-script edits are code — simulate them under the
script's own shell options before pushing (the local build had run
BEFORE the edit, so "it built locally" was vacuously true); (b) an
`$(assignment)` is not exempt from `set -e`.
**Reopen if**: n/a.

## 8.5 The -a x86_64 failure chain: three defects, one symptom each

**Status**: shipped-after-fix (e5134cc906 arch-clean, ae94399cd3
MoltenVK tiers + CONFIG_VULKAN guard, 2026-07-05).
**Chain** (each layer hid the next):
1. **Stale build/ reuse**: configure wipes build/ only inside its
   marker window, so '-a x86_64' over an arm64-configured build/ kept
   the old config-meson.cross -arch and "succeeded" with a pure arm64
   binary. Fix: build.sh wipes build/ when config-meson.cross
   disagrees with the requested arch.
2. **Arch-blind MoltenVK tiers**: with (1) fixed, the tier-2 check hit
   the custom arm64-only /usr/local dylib (existence test, no arch
   test) → skipped vendoring → meson found no Vulkan → the ENTIRE vk
   renderer silently dropped (vk/meson.build is wrapped in
   `if vulkan.found()`). Fix: a dylib satisfies a tier only if
   `lipo -archs` shows the target arch; the official release tar is
   universal so the vendored copy serves any arch.
3. **Unguarded cross-subsystem symbol**: with the vk renderer absent,
   the link died on metal-helpers.mm's unconditional references to
   the MetalFX present-event provider (which lives in the vk dir).
   Fix: include config-host.h + CONFIG_VULKAN guard. Trap within the
   trap: a bare #ifdef WITHOUT the include compiles everywhere and
   silently disables the present-ordering wait on every build —
   metal-helpers.mm's TU does not see config-host.h transitively
   (verified by include-walk before choosing the form).
**Bonus catch**: the restored x86_64 leg let the SSE =2 differential
run execute for the first time — it immediately caught a real leaked
rounding-mode bug in the host-FP brackets (see the XEMU_SSE_HOST
README row and e7c2e9cfb8): usable() verified the GUEST's SSE RC=RN
but the live host MXCSR carried x87-hard-FPU-leaked round-down →
host -0.0 vs softfloat +0.0 on exact cancellation. Both brackets now
force RN. A second ±0-sign class under Rosetta remains un-root-caused
→ x86_64 path stays dark with no bit-exactness claim.
**Lessons**: (a) "the build succeeded" proves nothing about WHICH
build you got — check `file` on the artifact after any cross build;
(b) meson's `if dep.found()` wrapping a whole subsystem turns a
missing dependency into a silent feature drop — the failure surfaces
arbitrarily far away (here: a UI link error); (c) pipe a long build
through `head` and SIGPIPE kills it mid-flight — grep the log file
afterwards instead.

## 8.6 grep -m1 under pipefail poisoned the MoltenVK provenance version

**Status**: shipped-after-fix (2026-07-05, found while validating the
build.sh consolidation).
**Symptom**: `package_macos`'s provenance line printed
`version 1.4.2 unknown` — the matched version AND the fallback.
**Root cause**: `strings | grep -m1 <pat> || echo unknown` under
`set -o pipefail`: `grep -m1` exits after the first match and closes
the pipe, `strings` dies with SIGPIPE, pipefail marks the whole
pipeline failed — so the `||` fallback fires despite a successful
match, appending "unknown" after the version.
**Fix**: an awk that reads ALL input and carries the fallback inside
(`/pat/ && !v {v=$0} END {print (v ? v : "unknown")}`) — no early
pipe close, no `||` on the pipeline.
**Lesson** (sibling of 8.5 lesson (c) and 8.4's `set -e` traps):
under pipefail, any early-exit reader (`grep -m1`, `head`) makes the
pipeline's exit status lie about success — never hang an `||`
fallback off such a pipeline.
**Reopen if**: n/a.

## 8.7 `-fzero-call-used-regs=skip` — a real code-size win the fps A/B refused to confirm

**Status**: settled-negative under the pre-registered bar (2026-08-04
wave); the mechanism stays in-tree DARK as the experimental arm
`XEMU_HARDENING=0` (`build.sh:719-725`). The default **keeps upstream's
register zeroing**, so the fork takes no security trade.
**The idea** (audit §3.1, the flagship build finding): upstream's
hardening block applies `-fzero-call-used-regs=used-gpr` globally with
no opt-out, so every return re-zeroes its used GPRs — measured at
**3.4-3.8% of this fork's dynamic compiled-instruction stream**, and
the flag also defeats tail-call optimization (`bl`+`ret` where a `b`
would do). clang is last-flag-wins and the fork's extra-cflags land
after meson's globals, so one appended `-fzero-call-used-regs=skip`
disables it. Pre-registered expectation: **+0.11 to +0.83 fps, central
~+0.34**, with the audit itself flagging that the band's lower half sits
inside the noise-suspect zone and demanding **5-6 interleaved pairs**.
**Evidence that killed it**: cross-binary interleaved **5-pair** A/B on
the F8 heavy anchor read **mean +0.52 fps but signs 3+/2−**. The
pre-registered bar kills on sign-mixed — and this shape is *worse* than
the sign-mixed-by-one results that have killed candidates before, so
the verdict was applied as written rather than re-argued from the
positive mean. Receipts: wave scratchpad `b4/h2`.
**The static receipt that did NOT override the bar**: the mechanism
provably did what it claimed — `__text` **8,741,512 B** hardened vs
**8,330,592 B** skipped, **−410,920 B**. Recorded in the ledger's
Failed row precisely because a confirmed code-size win is not evidence
of an fps win, and a kill bar that can be talked out of by a
*different* metric is not a bar.
**Lessons**: (a) when a candidate's predicted band overlaps the noise
floor, the sign test is the whole experiment — pre-register it and then
honour it, because a positive mean carried by 3 of 5 pairs is exactly
what noise looks like; (b) keep the losing arm as a knob: the
mechanism is one flag, so `XEMU_HARDENING=0` costs nothing and makes
the retest a one-line change instead of an archaeology dig.
**Reopen if**: a quiet-machine batch of **≥ 8 interleaved pairs** on the
current binary comes back **unanimous-sign** positive, or draws/s (the
composition-symmetric metric of 6.9) separates the arms where fps
cannot. Use `XEMU_HARDENING=0` as the E arm; `build.sh`'s
`check_zero_call_regs_order` (`build.sh:234-265`) already asserts the
appended flag actually wins in the generated compile database, so a
null result cannot be a silently-unapplied flag.

---

# 9. Benchmarking / method incidents

The protocol these incidents motivated (interleaved savestate A/B, monitor
`loadvm`, artifact oracle) is the `xemu-testing` skill. These entries are the
scar tissue — cite them when someone proposes a shortcut.

## 9.1 The "-30%" invalid-baseline misread

**Status**: incident, corrected in the record (`268daf69ac` recorded
"60 → ~40 flips/s" for vertex mirrors; `099efcb734` corrected it the same
day: the baseline run had been PARKED ON A MENU — a 3-draws/flip static
screen — so the comparison was menu-vs-gameplay, not A-vs-B). Properly
compared, mirrors were ~neutral (entry 1.3).
**Lesson**: a perf comparison is invalid unless scene identity is verified —
check draws/flip in the nsprof interval log for BOTH runs before believing
any delta. A wrong number nearly fenced off a neutral design *and* nearly
justified more work on a doomed one.

## 9.2 Bimodal attract-reel scenes

**Status**: incident (2026-07 campaign records). The Azurik attract entry is
bimodal: title screen ~3 draws/flip at 60 fps vs demo reel 350–390 draws/flip
at 20–48 fps. A soak sampling across the boundary averages two different
workloads. Anchor every measurement to scene identity via draws/flip (heavy
in-game 408–485; menus ~3; reel 350–390 — 2026-07 records, Azurik, M2 Ultra);
prefer savestate-pinned scenes (per `xemu-testing`).

## 9.3 Portable-config wipe / harness pointed at `dist/xemu.app`

**Status**: incident. Two coupled traps: (a) with SDL3 on macOS the bundle
base dir for portable mode is `Contents/Resources/`, NOT `Contents/MacOS/` —
and a full `./build.sh` re-runs packaging and WIPES
`dist/xemu.app/Contents/Resources`, so a planted portable `xemu.toml`
silently vanishes on rebuild; (b) a harness pointed at `dist/xemu.app`
therefore silently switched from the planted config to the user's real
`~/Library/Application Support/xemu/xemu/xemu.toml` — the "config hijack"
incident: runs *looked* fine while testing the wrong configuration.
**Rule**: never point a harness at `dist/xemu.app`; copy the bundle out, or
pass `-config_path` explicitly. Environment parity details: `xemu-testing`.

## 9.4 OS-level input injection false-verified

**Status**: incident (2026-07 harness build-out; motivated `38e5e42bbd`,
XEMU_INPUT_PIPE). OS-level key injection appeared to work in early tests —
but only because the app had self-activated; AppKit drops key events for
inactive apps, so unfocused automation silently sent nothing. A verification
that passes for the wrong reason is worse than a failure. The fix was an
in-process channel (`XEMU_INPUT_PIPE` FIFO, OR'd into SDL keyboard state,
works unfocused).
**Lesson**: verify the *mechanism*, not the outcome — prove input arrived
(observable game reaction with the window deliberately unfocused).

## 9.5 A running process keeps its old binary

**Status**: incident (2026-07 records). Rebuilding while an xemu instance
runs does not change what that instance executes — A/B runs comparing "new
build" against a stale live process measure nothing. Kill and relaunch; the
window title carries the git-describe version — read it to confirm what is
actually running.

---

## 9.6 Bare cp+codesign bundle refresh dies at dyld

**Status**: incident (2026-07-04; recipe fixed in `8db8f61b4d`).
**What happened**: refreshing `dist/xemu.app` (and any APFS clone of
it) by copying `build/qemu-system-i386` over the bundle executable +
re-codesigning — the then-documented loop — produced a bundle that
died at launch, twice, two layers deep: (1) the raw link output still
carries `/opt/local/...` install-name references (dylibbundler
rewrites them only in the packaged copy) → `Library not loaded:
libSDL3`; (2) after fixing install names, the linker's own `LC_RPATH`
entries (`macos-libs/.../opt/local/lib`, `/usr/local/lib`) made
bundled dylibs' `@rpath/` deps resolve to un-fixed MacPorts copies →
`libiconv` failing out of macos-libs' glib. The correct refresh
replays all four `package_macos` executable steps: cp, `xattr -c`,
`install_name_tool -change` loop, rpath strip-and-re-add-one, then
codesign, with a trailing `/opt/local` check (recipe:
`xemu-build-and-env` §5.3).
**Lesson**: the packaged executable and the build-tree executable are
different artifacts; any "fast refresh" must replicate the packaging
fixups or the bundle silently depends on developer-machine paths.
**Reopen if**: n/a.

## 9.7 The bench gate counted boot-menu intervals — cold-vs-warm boot asymmetry

**Status**: shipped-after-fix (2026-07-18, `scripts/bench-receipt.py`).
**Symptom**: every timed run of a healthy F8 baseline gated INVALID
("intervals [0..3] outside band; bimodal CV 0.51") while the warmup run
gated OK at the true floor.
**Root cause**: interval selection was position-based (`--drop-first 1`
from the log head). A COLD first launch boots flip-silent — nsprof
prints on flips, so no boot intervals appear and the log starts
in-game; WARM launches reach the flipping boot menu in seconds and
print 4-5 menu intervals before `loadvm`. Position-based dropping was
calibrated on the cold shape and misclassified every warm run (or,
worse, would have averaged 60 fps menu intervals into fps means).
**Fix**: anchor on scene identity — keep the TRAILING contiguous
in-band run of intervals, then drop that run's first interval as the
post-load transient; runs that never enter (or fall out of) the scene
keep nothing and still gate INVALID. Incident-9.1/9.2 selftest
fixtures still fire.
**Lesson**: select measurement windows by CONTENT (draws/flip regime),
never by log position — process warm-up state changes what precedes
the workload.
**Reopen if**: n/a.

## 9.8 Helper appends to a live process's stdout log get silently overwritten

**Status**: incident (2026-07-18, screenshot-verdict integration).
**What happened**: the mid-run screenshot checker appended its verdict
line to the xemu run log; the PNG existed but the verdict line never
did. xemu's stdout was opened by the shell with `>` (no `O_APPEND`),
so its file offset advances only with its own writes — the helper's
appended bytes at EOF sat exactly where xemu's next buffered flush
landed, and were overwritten byte-for-byte.
**Fix**: sidecar files (`shot_<label>.verdict`), never appends to a
live process's redirect target.
**Lesson**: two writers on one log require both fds in append mode;
a harness helper must assume the main process's redirect is not.
**Reopen if**: n/a.

## 9.9 Harness monitor socket under a deep outdir — every run DEAD at launch

**Status**: shipped-after-fix (2026-07-18, `bench-savestate-ab.sh`).
**What happened**: the hardened A/B harness derived its monitor socket
path from the per-invocation work dir (`$WORK/mon-$label.sock`), which
lives under the caller's outdir. Invoked with a session-scratchpad
outdir (~130 chars), every run died as `DEAD (launch failed (no
monitor socket))` — xemu exits before creating a socket whose AF_UNIX
path exceeds macOS's 104-byte cap. The movement probe had already
learned this (its sockets live in `/tmp`); the bench harness re-tripped
the same trap in a different home.
**Fix**: `local sock=/tmp/xemu-bench.$$.$label.sock`, removed after
each run. Only the socket moves to `/tmp`; logs and clones stay in the
outdir.
**Lesson**: the DEAD-run robustness worked exactly as designed (batch
completed, receipt said DEAD-RUNS instead of garbage) — but path-length
constraints are per-artifact, not per-harness: every AF_UNIX socket a
tool creates needs the short-path rule applied at ITS creation site.
**Reopen if**: n/a.

## 9.10 Three refuters in one session wedge the guest at boot — cumulative shadow-instrumentation cost, not a wave defect

**Status**: incident (2026-08-04 wave, campaign C3). Bisected to a
harness rule; no code changed, no shipped default affected. Same family
as 4.5 (a validator starving the thing it validates), rediscovered
across *three* independent validators instead of one.
**Symptom**: a soak launched with the wave's full refuter set —
`XEMU_X87_REFUTE=1` (+`XEMU_X87_CENSUS=1` +`XEMU_X87_ELIDE_CLEAN=1`)
AND `XEMU_SUBPAGE_FAST_REFUTE=1` AND `XEMU_DSP_JIT_DIFF=1` — never
reaches a single flip. **Zero nsprof intervals, `XEMU_PFIFO_HEARTBEAT`
frozen at `iters=1`, monitor unresponsive.** Reproduced **2/2** (a
12-minute soak run and a 90-second boot probe). This is the *wedge*
signature (1.24, xemu-testing), not a crash: no DiagnosticReports entry
is written, so a harness that only watches for crashes reports nothing.
**Root cause**: three-way wait on the APU `d->lock`, per `/usr/bin/sample`
(receipt `c3/run1-wedge.sample.txt`): `mcpx_apu_frame_thread` sits inside
`mcpx_apu_dsp_frame` → `dsp56k_jit_execute_block` for **4173 of 4175
samples** while holding `d->lock` across `se_frame()`; the vCPU thread is
blocked in `cpu_tb_exec` → `do_st_mmio_leN` → `vp_write` →
`qemu_mutex_lock_impl` on that same lock (a voice-processor register
write); the main loop is parked in `hmp_loadvm` → `vm_stop` →
`pause_all_vcpus`. The coupling lock lives in `hw/xbox/mcpx/apu/apu.c`,
which **this wave does not touch** — the input that changed is the
aggregate cost of three simultaneous shadow checkers, not any one
mechanism.
**Evidence (the bisect that makes it a rule, not a suspicion)**: every
knob boots clean **alone** (DIFF; the x87 trio; subpage-fast refute) and
**all three pairs** boot clean (x87+subpage, x87+DIFF, subpage+DIFF — 17
nsprof intervals each). Only the triple wedges (0 intervals). The triple
with **sampled** `XEMU_DSP_JIT_DIFF=10` boots clean (17 intervals at
60 fps). Receipts: `c3/bp0..bp8/run.log`, the sample above, and the
campaign record's `anomalies` section.
**The standing rule** (owned by `xemu-testing`, "Refuter combination
limit"): **never arm all three refuters in one session — two is the
maximum.** If a soak genuinely needs all three, downgrade the DSP
validator to sampling (`XEMU_DSP_JIT_DIFF=10`), which is the
demonstrated-clean form; splitting the soak into two sessions is the
other accepted answer and is what this campaign did (DIFF=1 was
preserved, not weakened).
**Lessons**: (a) refuters compose *additively in cost* and
*multiplicatively in risk* — each is individually budgeted against the
thread it instruments, and nobody budgets the sum; (b) 4.5's rule
("a validator must be architecturally incapable of blocking what it
validates") holds per-validator and still fails in aggregate when two
validators land on threads that share a lock; (c) a boot wedge under
instrumentation is a *test-infra* result and must be recorded as one —
the alternative is a wave spending hours suspecting its own mechanisms.
**Reopen if**: n/a as a bug. If a future campaign must run the triple
un-sampled, the prerequisite is a measurement of DSP-validator hold time
against `d->lock` (not a retry) — the bp8 receipt already shows sampling
buys the headroom.

# How to add an entry (the recording duty)

When an experiment fails or a change is reverted, the record is part of the
change — per `xemu-change-control`, a README "Failed / reverted experiments"
row is REQUIRED for every reverted attempt (enforced practice: nearly every
revert commit above updates the table in the same commit). Then extend this
chronicle:

1. **README row** (concise): Attempt | Reason — include the mechanism, not
   just "was slower". Diagnostic signatures (heartbeat patterns, counter
   values) belong in the row so future hunts pattern-match instead of
   re-bisecting (`68d5afe7fa` is the exemplar).
2. **Entry here**: pick the subsystem section; write Status / Symptom / Root
   cause / Evidence (hashes + dated numbers) / Reopen-if. Verify every hash
   with `git show --no-patch` before citing. Date-stamp every measurement.
3. **Index row** at the top of this file.
4. If the failure produced a *rule* (an invariant future code must respect),
   propose it for `xemu-architecture-contract`; if a *method*, for
   `xemu-research-methodology`. One home per fact — link, don't duplicate.

Template:

```markdown
## N.M Title
**Status**: reverted (`<hash>`, YYYY-MM-DD).
**Symptom**: what was observed, where, under what config.
**Root cause**: the mechanism (or "not fully proven; reverted on symptom").
**Evidence**: hashes, dated numbers, escape-hatch flag if any.
**Reopen if**: falsifiable condition, or "n/a".
```

---

## Provenance and maintenance

All facts verified against the repo on 2026-07-04 unless carrying an earlier
date (those are recorded historical measurements — do not re-measure to cite
them). README anchors and row counts below refreshed 2026-07-11 at
`7e2e6e7256`. Entries 6.10, 8.7, 9.10 and 7.5's closure were added
**2026-08-04** from the fork-wide optimization wave that implements
`docs/fork-optimization-audit-2026-08.md`; their measurements are that
day's, not current readings. Re-verification one-liners for drift-prone
facts:

- 6.10 hatch still present and still default-on:
  `grep -n 'XEMU_DIRTY_FAST' system/physmem.c include/system/physmem.h`
- 6.10's audit-side numbers (the base that collapsed):
  `sed -n '67,97p' docs/fork-optimization-audit-2026-08.md`
- 8.7 dark arm still one flag, default still hardened:
  `grep -n 'XEMU_HARDENING\|zero-call-used-regs' build.sh`
- 8.7 flag-order checker still runs after every build:
  `grep -n 'check_zero_call_regs_order' build.sh`
- 9.10's three refuter knobs still exist (the rule is only meaningful while
  they do): `grep -rn 'XEMU_X87_REFUTE\|XEMU_SUBPAGE_FAST_REFUTE' accel/tcg target/i386/tcg`
- 7.5's closure page and its reopen number:
  `grep -n 'BQL wait total' docs/lockless-mmio-verdict.md`

- README failed-experiments table (~line 803, 34 rows):
  `grep -n '^## Failed / reverted experiments' README.md`
- Any hash here resolves:
  `git show --no-patch --format='%h %ad %s' --date=short <hash>`
- Revert pairs in fork history:
  `git log --first-parent --oneline upstream/master..HEAD | grep -iE 'revert|reapply|disable|remove'`
- Prefill still 0 in all three homes (entry 2.1; README fixed
  2026-07-05): `grep -n PREFILL README.md Info.plist ui/xemu.c`
- Escape hatches cited (REPORTS_SYNC, VTX_EXACT, MAX_QUERIES, ZETA/TEX
  knobs) still documented: `sed -n '109,125p' README.md`
- DSP JIT round history + knobs: `grep -n 'EPI_NO_PC\|SENTINEL' docs/dsp-jit-design.md`
- Sentinel/audit harnesses still in-tree:
  `grep -rn 'XEMU_DSP_JIT_SENTINEL\|PIN_AUDIT' hw/xbox/mcpx/apu/dsp/`
- Tags cited exist: `git tag -l v0.9 v0.8.153-macos.1`
- Tag-immutability rule still documented: `grep -n 'delete' docs/RELEASING-macos.md`
- Settled-negative Future vectors unchanged: `sed -n '846,1026p' README.md`
