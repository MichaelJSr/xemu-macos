---
name: xemu-research-methodology
description: >-
  The research culture and evidence discipline behind this fork's
  optimizations — load BEFORE starting a new investigation, chasing a hunch,
  deciding whether a result is "proven enough" to ship, writing up why
  something shipped/stayed dark/got reverted, or when a measurement surprises
  you. Explains the evidence bar (one mechanism must explain every
  observation, including the ones that don't fit), the rule of predicting a
  number and a kill-threshold before running any experiment, the idea
  lifecycle from hunch to shipped/kept-dark/retired, why every invariant gets
  an adversarial checker that hunts it, and where this project's real wins
  actually came from (and where speculative ones didn't). Triggers: "is this
  a good idea", "how do I know this is real", "before I start", "did we
  already try this", "why was this reverted", "why is this flag here",
  "honest kill", "predict then measure", "research discipline", "evidence
  bar", "adversarial refutation", "starting a new investigation".
---

# xemu research methodology

This is the culture skill: not a runbook for running a specific command,
but the thinking pattern that turns a hunch into an accepted result on
this fork. Every other skill in this library assumes you already operate
this way. If you're about to chase a performance idea, explain a past
decision, or decide whether a number is good enough to ship, read this
first.

The short version: **state a mechanism that explains everything, predict
the number before you measure it, implement behind an escape hatch, and
try to prove yourself wrong before anyone else does.** The rest of this
document is that sentence, unpacked with the fork's own history as
evidence.

## 1. The evidence bar: one mechanism, every observation

A theory is not accepted because it explains the headline symptom. It is
accepted only when **one mechanism explains every observation you have —
including the ones that seem irrelevant, and especially the ones that
contradict the easy version of the story.** If your theory needs a second,
unrelated theory to cover the leftover facts, you don't have a root cause
yet.

### Worked case: the pink-tile saga (fixed in `531122e8aa`)

Users launching the app from Finder saw giant, screen-aligned magenta
blocks in roughly 5% of frames. The same binary, launched from a shell
for testing, never showed it — not once in about 40 minutes of capture.
An early theory blamed the texture upload path (plausible: the corruption
looked Morton-patterned, like the Xbox's swizzled texture layout). That
theory explained observation (a) and nothing else:

| Observation | Explained by "texture-path bug"? | Explained by `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`? |
|---|---|---|
| (a) artifacts appear in Finder-launched sessions | yes | yes |
| (b) zero artifacts in ~40 min of shell-launched harness capture | no | yes — `Info.plist` `LSEnvironment` (the mechanism macOS uses to inject env vars into an app) is applied only to LaunchServices/Finder launches; a shell launch of the identical binary ran MoltenVK's *library defaults*, not the tuned config |
| (c) an AGX (Apple's GPU driver) crash in `setVisibilityResultMode` correlated with the same builds | no | yes — immediate ("prefill") command encoding was the enabling condition (see the two-act crash case below) |
| (d) prefill=2 measured *slower*, not faster (46.33 vs 48.50 fps, same savestate scene) | no | yes — encoding work that prefill moves earlier lands on the PFIFO thread (the dedicated thread that feeds NV2A's 3D engine and does nearly all per-frame renderer work) |

`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` explained all four
observations at once; the texture-path theory explained one. That is
what "clears the evidence bar" looks like. The fix (`ui/xemu.c:1602-1606`,
`setenv("MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS", "0", 0)` before
MoltenVK loads, with `overwrite=0` so an explicit user override still
wins) was validated on the user's own environment: 0 artifact frames in
388 captures at 48.5 fps, versus 22 artifacts at 46.3 fps with prefill=2
(commit `531122e8aa`).

### Worked case: the visibility-buffer crash, two acts

A crash inside AGX's `setVisibilityResultMode:offset:` (a Metal call
MoltenVK makes when attaching the occlusion-query visibility buffer to a
render pass) surfaced on a real-gameplay savestate load.

**Act 1** (`bfcf38843f`): the theory was query-pool partition exhaustion
— in-pass query rotation allocates an index per rotation, and heavy
zpass scenes (a "zpass report" is the guest asking "how many pixels
passed the depth test") could exceed the per-slot partition mid-recording.
Release builds strip the assert that would have caught it, so the index
ran past the pool and MoltenVK wrote through a stale offset. The fix
(submit before the partition can overflow, pool `1024 → 4096`) was
validated by *forcing* the failure: `XEMU_MAX_QUERIES=64` made the guard
fire 3312 times per 5-second interval with zero crashes, at a steady
30 fps. That is a real bug, correctly fixed, and rigorously validated —
and it still **failed the evidence bar**, because it did not explain why
hours of bench-scene soak testing never crashed while the user's
gameplay crashed reliably. An explanation that only covers the fixable
bug you found, not the asymmetry in when it appears, is incomplete.

**Act 2** (`bb33ccaa2e`): a second user crash with the identical AGX
symptom proved Act 1 wasn't the whole story. Reading MoltenVK's own
source (see §5) found the real mechanism: `_needsVisibilityResultMTLBuffer`
resets on every `vkBeginCommandBuffer` and is set only by
`vkCmdBeginQuery` (`MVKCmdQueries.mm:49`, `MVKCommandBuffer.mm:242`);
the Metal encoder gets the visibility buffer attached only if that flag
is already set when the pass begins (`MVKCommandBuffer.mm:776`). Under
prefill=2's immediate encoding, the first render pass's encoder is
created the moment the pass is *recorded* — before any of this fork's
in-pass `vkCmdBeginQuery` calls can run. Consequence: any frame whose
**first** render pass contains the frame's first zpass draw crashes,
full stop. Bench scenes start every frame with a clear (a non-zpass
draw), so the first zpass draw always lands in a later pass whose
encoder already has the flag set by an earlier query — hence hours of
clean soaks. The user's area starts frames with zpass draws — hence a
reliable crash. This mechanism explains both the bug *and* the
soak-vs-gameplay asymmetry Act 1 left unexplained, which is why it
passed the bar. Fix: record an empty primer query (begin+end, outside
any render pass) at command-buffer start, so the flag is always set
before any pass exists — "correct by construction," not scene-dependent.
Validated with a 60 s soak under `MVK_CONFIG_DEBUG=1`: zero crashes, zero
MoltenVK validation errors, render-pass count unchanged.

**The pattern to internalize**: if your fix works but you can't say why
the bug didn't fire *everywhere* it structurally could, you have a
patch, not a root cause. Keep asking "what's different about the cases
where this doesn't happen?" until the answer is a mechanism, not a
shrug.

## 2. Predict the number before you run the experiment

State the expected metric delta and the threshold that would kill the
idea **before** you run it. The measurement then confirms or kills the
hypothesis — it never gets to rationalize it after the fact. If you
only decide what "good" looks like after seeing the number, you will
always find a reason the number is good.

**DSP JIT retranslation throttle.** The EP (one of the Xbox APU's two
DSP56300 cores — the Effects Processor) runs per-pass code overlays, so
in churny code it retranslates far more often than the GP (~1 per 10
block executions vs ~1 per 10,000). The design's stated logic
("translating it costs more than interpreting it") is itself the
prediction: handing a thrashing core back to the interpreter should land
its cost *at or below* plain interpretation, not above it — if throttling
ever cost more than the interpreter, the throttle logic would be net
negative and should be reverted. Measured (`XEMU_APU_PROF`, Azurik
attract scene): interpreter-only 22%, JIT-both-cores 25%, JIT with EP
auto-throttled 20% — at or below the interpreter, and within a point of
upstream's own dsp56300 engine (19%) in this DSP-light scene (single
`XEMU_APU_PROF` session reading per configuration — not
interleaved-with-variance; see `xemu-frontier-and-positioning` §3.2 for
what a public comparison still needs). The
prediction held; it shipped as the default, with `XEMU_DSP_JIT_NO_THROTTLE=1`
kept as the A/B escape hatch.

**In-pass occlusion queries + deferred zpass reports** (`6bfbc22863`).
New renderpass/pipeline-bind instrumentation showed 376 render passes
per flip (a "flip" is the guest's frame-swap boundary) — because
`vkCmdResetQueryPool` is illegal inside a render pass, so every query
rotation tore the pass down, and on Apple's tile-based GPUs a render
pass boundary means a full tile load/store. The expectation going in was
that moving the reset to a bulk operation at command-buffer begin (out
of any pass) would collapse pass count toward roughly one pass per
unique render-target bind per flip — in the low teens for this scene, not
the high hundreds. It landed at 14. The measured pair from the shipping
commit's own interleaved A/B (`scripts/bench-savestate-ab.sh`, 3
interleaved pairs, same binary, `XEMU_REPORTS_SYNC` as the toggle):
15.78 → 38.57 ± 0.80 fps (+144%), fence-wait 2.2 ms/flip on the new path
— the protocol receipt (README prose rounds this change to 35.6 fps /
2.25x).

**Per-flight vertex mirrors — the honest kill** (`268daf69ac`). The
mechanism's whole design intent, stated plainly in the code's own
rationale, was that "in-flight slots read a frozen mirror" so "cross-slot
conflict waits disappear" — a falsifiable, specific prediction. The
measurement confirmed the mechanical claim exactly: cross-slot waits did
structurally vanish. But flips/s did not move. That is a clean
falsification of the *actual hypothesis under test* — not "does the
mirror work" (it did) but "are cross-slot waits the bottleneck" (they
weren't). The waits that disappeared reappeared as something else
(boundary-page false-sharing on the currently-*recording* command
buffer, which a mirror can't touch because the conflict isn't with an
in-flight slot). This is what a **correctly executed, honestly reported
negative result** looks like: the thing you built worked exactly as
designed, and that's precisely how you learn the thing you built wasn't
the bottleneck. Full trajectory, including a same-evening
self-correction of the initial reading, in §3 Trajectory C.

**Rule of thumb:** write "expect ≈X; if measured is on the wrong side of
Y, this is dead" in your own notes before you touch the build. If you
can't write that sentence, you don't understand the mechanism well
enough to test it yet.

## 3. The idea lifecycle

Every idea on this fork moves through the same stages, and each stage
has an owning skill for the *mechanics* — this skill only owns the
*discipline* of moving between them honestly.

| Stage | Question to answer | Where the mechanics live |
|---|---|---|
| 1. Idea | Has this exact thing been tried, reverted, or already scoped-and-parked? | `xemu-failure-archaeology` (the chronicle) + README's "Future vectors" and "Failed / reverted experiments" sections |
| 2. Cheapest discriminating measurement | What's the smallest experiment that could kill this idea outright? | `xemu-proof-and-analysis-toolkit` (nsprof, `XEMU_APU_PROF`, forced-failure knobs, bisection recipes) |
| 3. Implementation | Build it behind a default-off flag with a legacy-restore path | `xemu-config-and-flags` (naming convention, `config_spec.yml` vs test-only `getenv`, `MVK_CONFIG_*` plist/`main()` parity checklist) |
| 4. Interleaved A/B | Run it against itself with the flag as the only variable | `xemu-testing` for the savestate-A/B *method*; `xemu-validation-and-qa` for the acceptance thresholds that decide if the number means anything |
| 5. Decision | Ship default-on, keep dark for continued A/B, or revert | this section, plus `xemu-docs-and-writing` for the README template each outcome uses |

Three real trajectories, one per outcome:

### Trajectory A — shipped default-on: byte-exact vertex-conflict refinement (`6e8c6cc481`)

**Origin**: not a fresh hunch — the *failed* per-flight-mirrors experiment
(§Trajectory C below) explicitly named this as the follow-up in its own
retirement text: "the real target this exposed: byte-granular (or
split-at-page) conflict refinement for the current-slot check." A good
retirement points at the next idea.

**Mechanism**: guest dirty bits are page-granular, so vertex-stream sync
writes arrive page-padded, and consecutive writes false-share their
boundary page with the *recording* command buffer — each false conflict
forced a "finish" (`pgraph_vk_finish`: a full submit-and-wait sync point
on the PFIFO thread). A per-page written-span table plus a
span-restricted `memcmp` proves most conflicts are byte-identical and
skips the finish; any genuinely differing byte keeps the old
(conservative) behavior.

**Measurement**: `scripts/bench-savestate-ab.sh`, 3 interleaved pairs,
baseline reproducibility ±0.02 fps, on a heavy in-game scene (408
draws/flip, ~20 fps). Forced finishes 5.65 → 0.37/flip (−93%), fence
wait −11%, process CPU −7.6%, fps **+5.4%**.

**Escape hatch**: `XEMU_VTX_EXACT=0` restores page-granular conflicts.

**Ship**: a README Changes-section bullet with the numbers, the escape
hatch documented alongside it. No config-spec entry was needed — it's a
pure behavior improvement with an already-documented legacy switch.

### Trajectory B — kept dark for ongoing A/B: `XEMU_MFX_REAL_DEPTH`

**Mechanism**: MetalFX's temporal upscaler was being fed synthetic
luminance depth. Feeding it the real zeta buffer (NV2A's depth/stencil
surface) should improve temporal quality — zeta images are created
exportable, and the dims-matched zeta's `MTLTexture` feeds the scaler,
with automatic fallback when the format is rejected or no matching zeta
exists.

**Why it does not get promoted to default-on**: the fix is real and the
mechanism is sound, but by present time "the single guest zeta typically
holds the *next* in-progress frame's depth" — so whether real depth is
an improvement over synthetic depth is title-dependent, not a clean
universal win. README says so explicitly: "Ships dark for A/B... quality
impact is title-dependent." Note also that the *actual* fix for the
underlying timing problem — capturing a GPU copy of zeta at flip-stall
so the depth is temporally correct — is itself still an open Future
vector ("Flip-time zeta snapshot for real depth"). `XEMU_MFX_REAL_DEPTH`
is deliberately parked at "useful, opt-in, honestly labeled" rather than
forced to either a ship or a revert it doesn't deserve.

### Trajectory C — retired with a full record: per-flight vertex mirrors (`268daf69ac`)

**Mechanism attempted**: one 128 MiB host mirror per flight slot (a
"flight slot" is one of the two in-flight command-buffer generations the
renderer pipelines — CPU records slot k+1 while the GPU executes slot
k); in-flight slots read a frozen mirror instead of live VRAM, so
cross-slot conflict waits become structurally impossible, with
`uploaded_bitmap` doubling as the delta log applied at slot reclaim.

**What happened next is itself a lesson.** The revert commit's own
measurement read like a real regression: "60 → ~40 flips/s steady with
collapse spikes to 20." Twenty-one minutes later, in the same session,
a follow-up commit (`099efcb734`) corrected the record after a proper
interleaved comparison: "the initial '-30%' read came from an invalid
baseline (a soak run parked on a 3-draws/flip static screen); properly
compared, mirrors were ~neutral." The team caught its own bad benchmark
before it became a wrong conclusion — see the meta-rule in §6.

**The real, corrected finding**: interval-by-interval flips were within
noise of the non-mirror build. *Why* neutral, not a win: heavy-reel
intervals showed 440–700 `finish_vtx_dirty` events per 5 s **with or
without mirrors** — the dominant cost was never the cross-slot conflict
the mirrors were built to remove. It was page-boundary false-sharing on
the *recording* command buffer's conflict check, which a frozen mirror
of *other* slots structurally cannot touch. The prediction ("cross-slot
waits disappear") came true and the fps didn't move — proof the
targeted mechanism wasn't the bottleneck, not proof the mirror was
buggy.

**Decision**: revert. +128 MiB and swap/delta complexity for a measured
zero win isn't worth carrying. Recorded in README's "Failed / reverted
experiments" table and in "Future vectors" (which redirects to what
*did* work — Trajectory A's byte-exact refinement — and states the
honest ceiling: "Both implementations preserved in this repo's history
for reference." That line is the retirement duty in one sentence: a
revert deletes the code path from `HEAD`, never from the record. The
next person who has this same idea should find the full implementation,
the measurement, and the reason it didn't help — in git history and in
README — not just silence.

## 4. Adversarial refutation as a habit

Every invariant on this fork gets a checker built specifically to hunt
it down, not just a comment asserting it holds. An optimization that an
audit kills before shipping is a **success of the method**, not a wasted
afternoon — it's cheaper to find the bug at "off by default, dev flag
enabled" than at "shipped, someone's save is now corrupt."

| Checker | What it hunts | Cost when idle | What it actually caught |
|---|---|---|---|
| `XEMU_DSP_JIT_PIN_AUDIT=1` | Per-op divergence between the ARM64-pinned A/B/X/Y registers and the in-memory DSP register file | Zero when unset; ~10 extra instructions per op when on | The `pm_2_2 Y0,A1` direct-A1-slot leak (`91343ad754`) and a separate pm_8 X/Y leak — each caught within 30 seconds of enabling the audit (`docs/dsp-jit-design.md`) |
| `XEMU_DSP_JIT_SENTINEL=1` | Whether a deferred optimization's precondition actually holds at runtime (poisons `cur_inst` to expose readers that decode it live) | Zero when unset | Confirmed the round-4 `cur_inst`-preset skip regresses on real code (`op=0x001000` at startup) — kept deferred rather than re-applied blind |
| `XEMU_DSP_JIT_DIFF=N` | Bit-exact divergence between JIT output and the interpreter, per translation, async on a dedicated `mcpx.dsp_diff` worker thread | 0 when unset; 2–10x slowdown at `DIFF=1` (every block); cheap when sampled | Proved `EPI_NO_PC` (a proposed skip of the PC-mismatch exit check) unsafe at a **9.75%** mismatch rate — 5.77M of 59.2M watched ops in Azurik genuinely diverge (calc_ea mode 6 lengthens parmoves to two words; REP/DO loops rewind PC) |
| Forced-failure knobs, e.g. `XEMU_MAX_QUERIES=64` | Whether a capacity guard fires *before* the resource it protects is exhausted | Test-only; never for normal runs | Guard fired 3312×/5 s interval under deliberate starvation, zero crashes, steady 30 fps — validated the Act-1 query-pool guard in isolation (though, per §1, that guard alone didn't explain the full crash) |

**EPI_NO_PC is the flagship story**: a plausible-looking epilogue
optimization (skip the PC-mismatch exit check for parmove stubs and
inlined long-immediates, on the theory that those ops can't diverge)
went in behind a sentinel specifically built to disprove it. The
sentinel measured a 9.75% real divergence rate and the optimization was
dropped **permanently** (`docs/dsp-jit-design.md`, round-4 entry) — not
patched, not special-cased, dropped, because the audit had already done
the work of proving the underlying assumption false. The correct
mechanism (a mode-6 `inst_len` classifier fix, `ca7fe51a0b`) shipped
later on its own merits, addressing the actual root cause the sentinel
had located.

**Practice**: before you believe your own optimization, ask "what
runtime check, if I built it, would catch me being wrong?" — then build
that check and turn it on before you trust the win. Don't wait for a
teammate or a user's crash report to be your refuter.

## 5. Where wins actually came from (mining the record honestly)

Five patterns account for essentially every real win in this fork's
history. None of them is "someone guessed and got lucky."

**Measurement-first attribution.** `XEMU_NV2A_NSPROF=1` (this fork's
release-safe wall-time profiler, printing per-5-second summaries)
did whole-frame attribution and found the dominant in-game cost was
~9 `pgraph_vk_finish` fence cycles per flip (8–19 ms/flip), driven by
the zeta (depth/stencil surface) ping-ponging between two shapes at one
VRAM address every frame. Fixing that, combined with a targeted
vertex-RAM wait fix, took medium scenes to 60 flips/s — the guest's
vblank cap, i.e. the ceiling. Separately, new render-pass/pipeline-bind
counters (the `NSPROF_EV_RENDERPASS` event) found 376 passes per flip in
report-heavy scenes; fixing *that* specific, counted thing produced the
in-pass-queries win (§2): 15.78 → 38.57 fps, +144%. In both cases the
instrumentation came first and named the exact thing to fix — nobody
started from "let's try in-pass queries" as a stand-alone idea.

**Environment parity.** The entire pink-tile class of bugs (§1) existed
because a test harness and a real launch had different environments.
The fix wasn't a code change to the renderer at all; it was making the
harness (and then the binary itself, via the `main()` `setenv` calls)
stop lying about what environment it was testing.

**Reading vendor source.** Act 2 of the visibility-buffer crash (§1) was
unreachable from local repro. It was solved by reading MoltenVK's own
`.mm` source and finding the exact lifecycle of one internal flag
(`_needsVisibilityResultMTLBuffer`, `MVKCmdQueries.mm:49`,
`MVKCommandBuffer.mm:242,776`). When a driver-level crash defies local
reproduction, the fix is in the driver's source, not in more guessing
at your own call site.

**Respecting the platform's grain.** Two facts about Apple GPUs and
MoltenVK are treated as design constraints, not obstacles to work
around: (1) Apple's GPUs are tile-based deferred renderers (TBDR) —
every render pass boundary is a full tile load/store, so pass *count*
is a first-order cost, which is exactly what made the 376→14 win
available once it was counted. (2) MoltenVK exposes a single Vulkan
queue (`queueCount = 1`, `hw/xbox/nv2a/pgraph/vk/instance.c:515`) where
submission order is execution order — the zeta shape-switch fast path
exploits this directly (a quarantined, evicted image is safe to reuse
"afterwards by single-queue submission order," no fence needed). Working
*with* single-queue ordering produced a win; a different attempt to
extract a second, parallel compute queue from MoltenVK was a Failed
Experiment for exactly the reason the platform note in `instance.c:506`
states: MoltenVK "exposes `queueCount=1` anyway" — no amount of Vulkan
API cleverness recovers a queue Metal doesn't have.

**Boring discipline.** Escape hatches exist for a reason beyond
"fallback for users": they are what make bisection *fast*. The in-pass
occlusion query win's own headline numbers (15.78 vs 38.57 fps) came
from `XEMU_REPORTS_SYNC` toggled on the *same binary* — no rebuild, no
"which commit was I on" uncertainty, just a flag flip and 3 interleaved
runs. A codebase where every behavior change ships its own on/off switch
is a codebase where any regression can be bisected in minutes instead of
a `git bisect` across dozens of commits.

**Contrast: what did NOT produce wins.** Several Future-vectors entries
were scoped, measured, and explicitly **not attempted** because the
measured ceiling was too small to justify the risk — this is the flip
side of the same discipline, not a gap in it:

- Texture-upload barrier batching: total upload time measured
  ≤ 15 ms per 5 s even during streaming-heavy transitions, ~0 in steady
  state.
- GPU S3TC (texture compression) decode offload: total upload CPU
  (including S3TC decode) measured ~0 steady-state, ≤ 15 ms per 5 s
  during streaming.
- `VK_EXT_external_memory_host` snapshot scheme: the snapshot memcpy it
  would eliminate measured **< 0.5 ms per 5 s** in-game — against a
  stated, explicit bar of "≥ 1 ms/frame" for even attempting it.

That last one is worth noting for its own sake: the decision rule
(≥ 1 ms/frame or don't bother) was fixed *before* being applied here,
the same predict-a-threshold discipline as §2, just pointed at "should I
even start" instead of "did my change work."

## 6. Practice-derived rules

These three rules are not written down anywhere as owner policy — they
are **inferred from the record**: every shipped change on this fork
follows them, without exception found. Treat them as established
practice, not decree, and if you're ever unsure whether they still
apply, check the record yourself rather than assume.

**1. Perf claims require the interleaved savestate A/B protocol.** Every
optimization shipped since `6e8c6cc481` carries protocol numbers (same
binary, same scene, flag or build toggled, multiple interleaved pairs).
The cautionary incident for skipping this is the per-flight-mirrors
misread in §3 Trajectory C: an initial single-run comparison against an
accidentally-invalid baseline (a static, 3-draws/flip menu screen) read
as a regression; a proper interleaved re-measurement, 21 minutes later,
found the true result was neutral. See `xemu-testing` for the protocol
itself and `xemu-validation-and-qa` for how much data is enough to trust.

**2. Never regress a Windows or upstream code path for a macOS-only
win.** Evidence this is enforced, not just claimed:
- `hw/xbox/nv2a/pgraph/vk/surface.c:1085,1091,1304` gate the narrowed
  `upload_surface_data` sync behind `#if defined(__APPLE__)` — the
  macOS-specific fast path is physically absent from other platforms'
  builds, not just conditionally worse.
- `ui/xemu-input.c:522-574`: the entire `XEMU_INPUT_PIPE` automation
  channel is wrapped `#ifdef _WIN32` / `#endif /* !_WIN32 */` and
  compiles to nothing on Windows. The guard itself is the fork's most
  recent commit as of this writing (`cf85e96597`, "fix Windows build")
  — added after `O_NONBLOCK` and FIFOs turned out not to exist on
  MinGW, i.e. the rule caught a real regression before it shipped.
- `tcg/aarch64/tcg-target-has.h:65-69`: `TCG_TARGET_HAS_fpu` is `0`
  under `#ifdef _WIN32`, `1` otherwise — the inline-x87 win is disabled
  specifically on Windows/ARM64 because llvm-mingw can't constant-fold
  `qemu_build_not_reached()` out of the generic FP paths that
  `TCG_TARGET_HAS_fpu=1` makes reachable; Windows/ARM64 falls back to
  the helper-based hard-FPU path "like x86_64 hosts" (comment, same
  file).
- README states the policy outright (`README.md:31`): "The upstream
  Windows build paths are preserved and CI-tested."

**3. Every behavior change ships an escape hatch.** The entire
`XEMU_*` runtime-knob table (README's "Runtime debug / escape-hatch
knobs" section) exists precisely as a set of legacy-restore switches:
`XEMU_ZETA_SHAPE_READBACK`, `XEMU_TEX_BIND_RECHECK`, `XEMU_VTX_EXACT`,
`XEMU_REPORTS_SYNC`, `XEMU_MFX_REAL_DEPTH` /
`XEMU_MFX_INTERP_ZERO_MOTION`, `XEMU_DSP_JIT_NO_THROTTLE`, and the
`MVK_CONFIG_*` values (explicit env always overrides the fork's
defaults — `setenv(..., 0)` in `ui/xemu.c` never clobbers a value
already set). See `xemu-config-and-flags` for the full catalog and the
add-a-flag checklist.

**Meta-rule: when a result surprises you, first suspect the benchmark.**
Two real illustrations, not a hypothetical:
- The mirrors self-correction above — a same-evening reversal from
  "regression" to "neutral" once the baseline was checked and found
  invalid.
- Azurik's attract-reel entry is bimodal: a title screen at roughly
  3 draws/flip (~60 fps, confirmed as the culprit behind the invalid
  baseline above) versus the actual demo reel at roughly 350–390
  draws/flip (20–48 fps) — a session finding recorded 2026-07-04, not
  yet written into README prose. Two captures of "the same scene" can
  be two different workloads; always confirm draws/flip (or another
  scene-identity signal) before trusting a delta between them. Before
  concluding a surprising number reflects reality, check scene identity,
  environment parity (launch path, harness vs. real usage), and binary
  identity (are you certain you're running the build you think you
  are — a running process keeps its old binary even after a rebuild).

## 7. Starting a new investigation: the one-page checklist

1. **Has this been fought before?** Check `xemu-failure-archaeology` and
   README's "Future vectors" / "Failed / reverted experiments" sections.
   A one-line rejection ("MoltenVK only exposes `queueCount=1`") can
   save a week.
2. **State the mechanism in one sentence**, and check it against every
   observation you have — not just the one that prompted the
   investigation. If any observation doesn't fit, you have a guess, not
   a theory (§1).
3. **Write down the expected number and the kill threshold before you
   touch any code**: "expect ≈X; if measured lands on the wrong side of
   Y, this is dead" (§2).
4. **Find the cheapest measurement that could produce the killing
   number.** `xemu-proof-and-analysis-toolkit` has the recipes (nsprof,
   `XEMU_APU_PROF`, forced-failure knobs, bisection).
5. **Implement behind a default-off flag with a legacy-restore path.**
   Never ship a behavior change with no way back — see
   `xemu-config-and-flags`.
6. **Run the interleaved A/B**, same binary, flag as the only variable
   (`xemu-testing` protocol), and check the result against
   `xemu-validation-and-qa`'s acceptance thresholds before you believe
   it.
7. **Build your own refuter before you trust the win**: what sentinel,
   diff-check, or forced-failure knob would catch you being wrong?
   (§4). If you can't think of one, you haven't looked hard enough yet.
8. **If the result surprises you, suspect the benchmark first**: scene
   identity, environment parity, binary identity (§6 meta-rule) — before
   you suspect the mechanism, and long before you suspect nothing at
   all and ship it.
9. **Decide, and document the decision**: ship default-on (README
   Changes row + keep the escape hatch), keep dark for continued A/B
   (state plainly why — title-dependent, unproven at scale, etc.), or
   revert. A revert gets a Failed-experiments row or a Future-vectors
   entry, not silence — the implementation stays in git history for
   whoever has this idea next (§3).
10. **Never regress a Windows or upstream path for the win, and never
    quietly re-break a documented invariant** (prefill must stay 0,
    published tags are immutable, etc.) — `xemu-change-control` has the
    full list.

## When NOT to use this skill

- Need the exact invocation syntax for nsprof, `XEMU_APU_PROF`, or a
  specific bisection recipe → `xemu-proof-and-analysis-toolkit`.
- Need to know how many interleaved pairs are enough, or whether a given
  delta is noise → `xemu-validation-and-qa`.
- Need to decide *what* to work on next, ranked by opportunity →
  `xemu-frontier-and-positioning`.
- Need the mechanics of actually running a savestate A/B (monitor
  commands, `loadvm`, USB-topology matching) → `xemu-testing`.
- Need to know what evidence a specific change class owes before
  landing, or commit-message conventions → `xemu-change-control`.
- Have a live symptom in front of you right now (crash, artifact,
  freeze) and need first triage steps → `xemu-debugging-playbook`.
- Need the full blow-by-blow of a specific past incident, beyond the
  worked examples here → `xemu-failure-archaeology`.
- Need to know *where* a fact belongs in the docs, or the README
  template for a new row → `xemu-docs-and-writing`.
- Need domain theory (what a zpass report *is*, why TBDR passes are
  expensive, DSP56300 semantics) rather than research process →
  `xbox-hardware-reference`.

## Provenance and maintenance

Facts here are dated 2026-07-04 unless the citation is a specific
commit hash (commits and their messages don't drift). Re-verify with:

- Commit hash still means what this document says:
  `git show -s --format='%h %ad %s' --date=short <hash>`
- Env knobs cited here still exist and are read where stated:
  `grep -rn 'getenv("XEMU_DSP_JIT_PIN_AUDIT\|getenv("XEMU_DSP_JIT_SENTINEL\|getenv("XEMU_DSP_JIT_DIFF\|getenv("XEMU_MAX_QUERIES' hw/`
- README section locations (line numbers drift as the file grows):
  `grep -n '^## \|^### ' README.md`
- The Windows-preservation guards still stand:
  `grep -n '_WIN32' ui/xemu-input.c tcg/aarch64/tcg-target-has.h hw/xbox/nv2a/pgraph/vk/surface.c`
- The single-queue MoltenVK constraint hasn't changed:
  `grep -n 'queueCount' hw/xbox/nv2a/pgraph/vk/instance.c`
- The failed/future-vectors rows cited here still read as summarized:
  `sed -n '803,1026p' README.md` (Failed at 803, Future vectors at 846,
  as of 2026-07-11)
- DSP JIT throttle constants (the 1-in-16 ratio, 256Ki window) are still:
  `grep -n 'DSP56K_JIT_THROTTLE' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
