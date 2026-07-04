---
name: xemu-failure-archaeology
description: >-
  The chronicle of every settled battle in the xemu-macos fork: failed
  experiments, reverts, fenced-off designs, regressions, and incidents, each
  recorded as symptom -> root cause -> evidence -> status. Load this skill
  BEFORE attempting or proposing any optimization or design change (it may be
  a known dead end: dynamic rendering, VK_EXT_external_memory_host, MoltenVK
  prefill, vblank retiming, EPI_NO_PC, cur_inst skip, HLT recovery, rate==1.0
  resampler, BQL batching, spec constants, GPU S3TC, barrier batching...);
  before reverting or retrying anything; when investigating the history of a
  regression, crash, freeze, or pink/magenta artifact; when asking "has this
  been tried?", "why was X reverted?", "why is this flag/guard here?"; when a
  git log revert pair needs its story; or when adding a new failed-experiment
  record. Contains full sagas: pink-tile, visibility-buffer crash (two acts),
  streamed-vertex campaign, occlusion rework, level-transition freeze, DSP JIT
  pin/PC battles, release-CI hardening, tag-deletion incident, and
  benchmarking traps.
---

# xemu failure archaeology — the settled battles

The fork's institutional memory of what did NOT work, what was reverted, what
is fenced off, and what was proven not worth doing — so nobody re-fights a
settled battle or silently re-attempts a fenced-off design. Every entry is
grounded in the repo: `README.md` "Failed / reverted experiments" (~line 730
as of 2026-07-04), commit messages, and dated measurements.

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
| 2.1 | Pink-tile corruption (MoltenVK prefill) | shipped-after-fix |
| 2.2 | Visibility-buffer crash, two acts | shipped-after-fix |
| 2.3 | Pink-flash (torn texture snapshot) | shipped-after-fix |
| 3.1 | Async MetalFX under GL presentation | reverted, landed elsewhere |
| 3.2 | Vblank cadence aligned to host refresh | reverted |
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
comment) and `ui/xemu.c:1610` `setenv(..., 0)` — the no-overwrite main()
setenv gives launch-path parity so Finder, terminal, and harness launches run
one configuration (`166999fc60`; build.sh also logs the bundled MoltenVK
version+UUID to catch tested-vs-shipped driver drift). Validated on the
user's own pink-spot savestate with the full launch environment: 0 artifact
frames in 388 captures at 48.5 fps (prefill=0) vs 22 at 46.3 (prefill=2).

**Known stale doc**: README's "MoltenVK runtime config" table (~line 720)
still says PREFILL=2 — code and plist say 0; the README *Changes* text is
correct. Trust `grep -n PREFILL Info.plist ui/xemu.c`.

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
(the `config_spec.yml:339` comment still names a stale selector function) —
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

---

# 7. PFIFO / threading

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
them). Re-verification one-liners for drift-prone facts:

- README failed-experiments table (~line 730, ~27 rows):
  `grep -n '^## Failed / reverted experiments' README.md`
- Any hash here resolves:
  `git show --no-patch --format='%h %ad %s' --date=short <hash>`
- Revert pairs in fork history:
  `git log --first-parent --oneline upstream/master..HEAD | grep -iE 'revert|reapply|disable|remove'`
- Prefill still 0 in both places (entry 2.1): `grep -n PREFILL Info.plist ui/xemu.c`
- README MVK table still stale at PREFILL=2 (drop the 2.1 note once fixed):
  `sed -n '718,727p' README.md`
- Escape hatches cited (REPORTS_SYNC, VTX_EXACT, MAX_QUERIES, ZETA/TEX
  knobs) still documented: `sed -n '109,125p' README.md`
- DSP JIT round history + knobs: `grep -n 'EPI_NO_PC\|SENTINEL' docs/dsp-jit-design.md`
- Sentinel/audit harnesses still in-tree:
  `grep -rn 'XEMU_DSP_JIT_SENTINEL\|PIN_AUDIT' hw/xbox/mcpx/apu/dsp/`
- Tags cited exist: `git tag -l v0.9 v0.8.153-macos.1`
- Tag-immutability rule still documented: `grep -n 'delete' docs/RELEASING-macos.md`
- Settled-negative Future vectors unchanged: `sed -n '766,853p' README.md`
