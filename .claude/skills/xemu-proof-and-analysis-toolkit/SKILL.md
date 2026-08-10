---
name: xemu-proof-and-analysis-toolkit
description: >-
  Eight first-principles analysis recipes for proving WHY something is slow,
  wrong, or crashing in this xemu fork — and proving a fix actually worked.
  Wall-time attribution (nsprof), the wait-relocation test for CPU-bound vs
  GPU-bound, single-variable bisects against an automated oracle,
  read-the-driver-source proof for third-party crashes, bit-exact
  differential validation design, adversarial self-refutation harnesses,
  statistical benchmark discipline, and cadence-change capacity analysis for
  fixed-size pools. Load when a claim needs evidence, not confidence.
  Triggers include "prove it", "is this really GPU-bound", "why does this
  only happen sometimes" / only for one person / only from Finder, "bisect
  this", "read the driver source", "differential validation",
  "self-refutation harness", "is this a real regression", "capacity guard",
  "wall-time attribution", "prove headroom", "how do I know this is fixed".
---

# xemu proof-and-analysis toolkit

This project ships without a test suite backstop: grep every workflow
under `.github/workflows/` for `make check` / `meson test` / `pytest`
and you get nothing — CI here is compile-and-package only (verified
2026-07-04). Every performance and correctness claim that ships is
instead the product of someone deliberately proving it, with a method
that would have caught the claim being wrong. "Prove it, don't just
install it": build the fix, then build (or reuse) the technique that
would falsify it, and run that technique before believing the fix.

This skill is eight such techniques, each mined from this repo's own
history, each with a real worked example (commit hash, dated numbers)
and — just as important — the ways the technique itself lies to you if
used carelessly. Pick the recipe that matches your claim, follow its
steps, and cite its worked example's shape when you write up your own
result.

## Quick lookup

| Your situation | Recipe |
|---|---|
| A frame/operation is slow; several plausible causes, none confirmed | 1. Whole-frame wall-time attribution |
| Is this CPU-side sync overhead, or genuinely downstream (GPU) work? | 2. The wait-relocation test |
| A bug reproduces "sometimes" / "only for one person" / "only from Finder" | 3. Single-variable bisect against an automated oracle |
| A driver/library crash defies local repro, or your structural fix didn't fully work | 4. Read-the-source proof for third-party behavior |
| A new engine must match a trusted reference bit-for-bit, cheaply enough to leave on | 5. Bit-exact differential validation design |
| You're about to trust an invariant with no existing failure signal | 6. Adversarial self-refutation harnesses |
| You need an fps/timing number that survives scrutiny | 7. Statistical benchmark discipline |
| A change alters how often a pooled/fixed-size resource gets allocated | 8. Cadence-change capacity analysis |

---

## 1. Whole-frame wall-time attribution

**Use when** a frame or operation is measurably slow but the cost is
spread across many candidate sites (shader generation, texture upload,
fence waits, idle time...) and no single existing measurement explains
the gap between "what you've measured" and "how slow it actually is."

**Steps**

1. Identify the single thread that serializes the work you care about.
   In this renderer that is the PFIFO thread (NV2A's command-FIFO
   pump/puller thread, also driving PGRAPH, the 3D engine state
   machine) — nearly every rendering-side cost lands there.
2. Instrument **every** candidate wait/cost site on that thread, not
   just your prime suspect. This project's pattern
   (`hw/xbox/nv2a/nsprof.[ch]`, `XEMU_NV2A_NSPROF=1`): wrap a region
   with `nsprof_begin()`/`nsprof_end(counter, t0)` to accumulate
   total/max nanoseconds and a hit count per named counter, or call
   `nsprof_event(event)` for a plain tally. Current counters
   (`hw/xbox/nv2a/nsprof.h`): `SHADER_GEN`, `PIPELINE_GEN`,
   `TEX_UPLOAD`, `TEX_HASH`, `TEX_SNAPSHOT`, `GEOM_UPDATE`,
   `FENCE_WAIT`, `AUX_FENCE_WAIT`, `MFX_DRAIN`, `SURF_DOWNLOAD`,
   `FLIP_IDLE`, plus tagged events (`FINISH_*` reasons, `DRAW`).
3. Track a denominator alongside the costs — flips (frame/swap
   boundaries) and elapsed interval time — so totals become ms/flip.
   nsprof reports on a rolling 5 s interval.
4. **Sum every category's total for the interval and compare it to the
   actually-measured frame time** (interval time × flips × observed
   ms/flip). A short sum means uninstrumented sites — add more wraps
   before trusting any ranking; a partial budget is a confidently
   wrong "top cost."
5. Once the budget is close to the real total, sort by ms/flip and
   attack the largest line item first.
6. When a category's `max_ns` dwarfs `total_ns / events`, suspect one
   resource doing something pathological, not a uniform cost — the
   fix is a special case, not a general speed-up.

**Worked example.** Commit `a74efb21fb` (2026-06-11) moved nsprof to
the renderer-agnostic `hw/xbox/nv2a/` and added flight-slot/aux-CB
fence-wait counters, MetalFX drain time, surface-readback timing, the
`FLIP_STALL`→vblank idle gap, finish-reason/draw counts, and tagged
trigger sites — specifically to close the gap between previously
measured paths (~2-4 ms/flip) and actual 22-60 ms frames. The first
full-coverage soak attributed the frame to roughly 9
`pgraph_vk_finish` (a full submit-and-wait sync point on the PFIFO
thread; `hw/xbox/nv2a/pgraph/vk/draw.c:1858`) cycles per flip, 8-19 ms,
and pinpointed a zeta (depth/stencil) surface repeatedly switching
between two shapes (dimensions/format/layout) at one VRAM address,
each switch forcing a full GPU→CPU readback, twice per flip. The very
next commit, `fc58c637c4` (same day), attacked that finding plus two
siblings the same soak surfaced (a vertex-conflict wait forcing a full
finish instead of a single-slot wait; a texture re-bind dirty-check
that ran even when nothing had changed), and measured medium scenes
going from 22-26 to 60 flips/s (guest vblank cap) and heavy scenes
from ~22 to ~26-30, with per-flip fence waits falling from 8-19 ms to
1-3.5 ms.

**Failure modes of the method itself**

- *Instrumentation overhead inflates the measurement* on a
  high-frequency site. Disabled cost here is one cached-flag branch,
  but *enabled* cost on a hot call site is real — compare instrumented
  vs. uninstrumented fps if in doubt.
- *Budget-sums-short, silently* — stopping once the top item "looks"
  dominant, without checking the sum against measured frame time, lets
  a second uninstrumented cost hide directly behind it.
- *Interval-boundary artifacts* — a 5 s window can bisect one spike
  across two intervals, diluting its apparent share; corroborate with
  `max_ns`, not only `total_ns / events`.
- *Attribution is not causation for shared resources* — a high count
  at one site can be a symptom of a decision made elsewhere; trace the
  trigger, don't just clock the symptom.

**Copy this pattern when:** you have a global fps/latency complaint,
more than one plausible culprit, and existing point-instrumentation
doesn't add up to the observed cost.

---

## 2. The wait-relocation test (CPU-bound vs. GPU-bound)

**Use when** you suspect a workload is bound by waiting on the GPU (or
some other downstream resource) rather than by CPU-side synchronization
overhead, and you're deciding whether more CPU-side wait-elimination
work is worth attempting.

**Steps**

1. Pick a wait class you can eliminate or relocate *structurally*, not
   shrink — e.g. swap a full pipeline finish for a single targeted
   resource wait, or remove the wait's precondition entirely (give
   every consumer its own private copy of the data).
2. Measure **total** wait time, summed across every call site the wait
   class can occur at, using the same profiler categories so
   before/after numbers are comparable (recipe 1's instrumentation).
3. Record the *distribution* too (which sites, how many events). A
   real win moves the sum down; a relocation moves count between sites
   while the sum holds still.
4. If the total holds flat (within run-to-run noise — recipe 7) and
   fps also holds flat, the wait is downstream of something your
   change didn't touch — usually actual GPU execution time. Stop
   iterating there; the next fix has to cut GPU-side work.
5. Cross-check with an orthogonal signal when available (is this scene
   actually draw-heavy, per recipe 7's draws/flip anchor, or
   sync-starved with little real work?).
6. Don't conclude "GPU-bound" from one experiment — confirm with a
   second, structurally different elimination attempt landing on the
   same flat total, since one experiment could be neutral for an
   unrelated reason (a bug in the change, an atypical scene).

**Worked example.** The streamed-vertex stall investigation (README's
"Future vectors", numbers dated to the 2026-07 session) ran this test
twice on the same heavy attract-reel content. (a) Byte-exact conflict
refinement (commit `6e8c6cc481`) eliminated 75-85% of the
page-boundary `finish_vtx_dirty` cascade (440-700 down to ~100 per 5 s
interval), but flips/s did not move — the eliminated finishes
reappeared as cross-slot targeted waits elsewhere. (b) Exact
refinement *plus* a 128 MiB host mirror per flight slot (one of
`NUM_FLIGHT_SLOTS`=2 in-flight command-buffer generations,
`hw/xbox/nv2a/pgraph/vk/renderer.h:542`), meant to remove cross-slot
waits structurally by letting in-flight slots read a frozen mirror
instead of racing the recording command buffer: fence-wait *events*
ballooned (more, smaller waits) while total wait time held at
~2.5-3.5 s per 5 s interval, before and after. README's verdict: "the
~22-30 ms/flip of waits is the GPU's actual frame time... real GPU
time, not slack; CPU-side wait elimination just relocates which call
site absorbs it." The mirror change was reverted as not worth its
complexity (+128 MiB, no measured win); both implementations remain in
git history. Attempt (a) was **not** wasted, though — on a
*different*, heavier in-game scene (408 draws/flip @ ~20 fps) it
independently measured +5.4% fps, forced finishes 5.65→0.37/flip
(−93%), −7.6% process CPU, and shipped on those merits. A GPU-bound
verdict on one scene says where *not* to expect more from *that*
scene; it doesn't invalidate a change that helps a different one.

**Failure modes**

- *Incomplete wait inventory* — gaps in your profiler categories
  (recipe 1) can make a "conserved" total mean only that you can't see
  where it moved to.
- *Scene-dependence* — GPU-bound in a content-heavy scene doesn't mean
  GPU-bound everywhere (medium-content scenes here hit the 60 fps
  vblank cap from CPU-side fixes alone, `fc58c637c4`); re-run per
  scene class before generalizing.
- *Two neutral results from the same mechanism aren't independent
  confirmation* — make the two elimination attempts structurally
  different (page-conflict refinement vs. mirror-based slot isolation
  above), not two variations on one idea.

**Copy this pattern when:** an optimization on a synchronization/wait
path measures flat-to-neutral fps and you need to decide "push
harder here" vs. "the ceiling is GPU work now."

---

## 3. Single-variable bisect against an automated oracle

**Use when** a bug (artifact, crash, wrong behavior) reproduces
inconsistently and more than one environment or configuration
variable differs between the reproducing and non-reproducing
conditions.

**Steps**

1. **Before touching any single variable, enumerate the full delta
   matrix** between the reproducing and non-reproducing conditions —
   every environment variable, launch path, config file, working
   directory. Missing an axis means your bisect can chase a red
   herring and never find the real one.
2. Build or obtain an oracle that scores every run automatically and
   consistently. A human eyeballing frames doesn't scale past a few
   minutes and doesn't survive an unattended soak.
3. Reproduce the **failing** condition first, matching the *entire*
   delta matrix from step 1 (not just your prime suspect) — confirm
   the oracle actually flags it before bisecting anything.
4. Flip exactly **one** variable per run, holding every other axis at
   the reproducing condition's value. Re-run the oracle.
5. A variable is implicated only when toggling it alone flips the
   oracle's verdict; run the "everything except X" control too, and
   confirm it stays clean. A bisect that never runs that control is a
   guess, not a bisect.
6. If the fix also claims a performance or correctness delta,
   corroborate with an independent measurement beyond the oracle
   before calling it done.

**Worked example: the pink-tile bisect** (fixed in commit
`531122e8aa`, 2026-07-04). Screen-wide Morton-patterned (Xbox texture
memory's interleaved-locality layout) magenta blocks appeared only in
Finder-launched sessions, never across ~40 minutes of shell-launched
test captures. The missing matrix axis was launch path: `Info.plist`'s
`LSEnvironment` block (the project's `MVK_CONFIG_*` MoltenVK tuning)
is applied by macOS only to Finder/LaunchServices launches, so every
terminal-launched test run had silently been exercising library
defaults, not the shipped configuration. Once the harness exported the
app's actual `LSEnvironment` variables itself, the artifact reproduced
immediately: 18-22 flagged frames per 4-minute run. The oracle: since
QEMU's `screendump` doesn't work against this fork's custom Metal
present path, capture the real presented window with
`screencapture -x -o -l<windowID>`, locate it via Quartz's
`CGWindowListCopyWindowInfo` matched on `kCGWindowOwnerPID`, and score
downscaled frames for magenta (`R>170 and B>140 and G<0.55*min(R,B)`,
clustered on a coarse grid to reject single-pixel noise) — capture method
in the `xemu-testing` skill's "Visual artifact oracle" section; the
scoring half now ships as `score_frames.py` in
`.claude/skills/xemu-diagnostics-and-tooling/scripts/` (the capture loop
itself is still recreated from the description).
Exporting each `MVK_CONFIG_*` variable one at a time under that scorer
isolated `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`: prefill alone
reproduced the corruption (22 flagged frames vs. 0 in the same run);
argument buffers, fast-math, and async queue submits all ran clean in
isolation. The commit cross-checked with an independent signal (step
6): prefill=2 also measured slower (46.3 vs. 48.5 fps, same savestate
scene), and final validation on the user's own environment logged 0
artifact frames in 388 captures at 48.5 fps with prefill=0, vs. 22 at
46.3 fps with prefill=2.

**Failure modes of the method itself**

- *Incomplete delta matrix* — the exact failure that cost this project
  weeks. Skip an axis and you isolate a variable merely correlated
  with the real cause because an unlisted axis moved with it.
- *Noisy oracle* — a nonzero false-positive/negative rate can mislead
  on one run; run enough time per variable that the flagged-frame
  count is clearly bimodal (here: 0 vs. 18-22 per run, not 2 vs. 4).
- *Wrong signal entirely* — verify the oracle observes the actually
  presented frame (this project's `screendump` pitfall); a bisect
  against the wrong buffer confidently isolates nothing real.
- *Stateful residue between runs* (cached pipelines, a lingering
  config file) can leak into the next variable's run — bisect with
  fresh process launches, not toggles in a live session.

**Copy this pattern when:** a bug reproduces "sometimes," "only for
one person," or "only one way of launching the app," and more than
one variable differs between good and bad runs.

---

## 4. Read-the-source proof for third-party behavior

**Use when** a crash or misbehavior involves a closed-loop third-party
component (driver, vendored library) whose internal state you can't
instrument, and empirical bisection has narrowed the trigger but a
structurally reasonable fix didn't fully resolve it — or a symptom
repeats with the exact same signature after that fix.

**Steps**

1. Treat "still crashing after a reasonable structural fix" as the
   signal to stop hypothesizing from external behavior and start
   reading the third-party component's actual source, if a checkout
   is available (vendored, or open-source dependency).
2. Find the exact state variable(s) gating the behavior you're
   fighting, and every call that sets or resets them — grep the
   dependency's source for the API entry points your code calls
   adjacent to the failure.
3. Trace the state machine across the boundary that matters (what
   resets this flag, and *when*, relative to your own calls) rather
   than stopping at the symptom site.
4. Cross-reference the trace against your actual crash evidence
   (stack trace, the precise API call the crash occurred inside) —
   confirm the mechanism explains *this* crash, not merely a
   plausible-sounding one.
5. Derive the fix from the mechanism so it holds **by construction**:
   name the exact moment the library checks the state, and make your
   fix guarantee that state is correct at that moment for every code
   path with the same shape — not just the one instance you observed.
6. Write down explicitly why the fix is provable rather than
   empirical: which state variable, which check, why every future
   case satisfies it.

**Worked example: the AGX visibility-buffer crash, second act**
(commit `bb33ccaa2e`, 2026-07-04). A first fix (`bfcf38843f` — see
recipe 8) closed a real query-pool exhaustion bug, but a second user
crash with the identical `setVisibilityResultMode` NULL-write signature
proved it wasn't the whole story. The mechanism was established by
reading MoltenVK 1.4.1 source directly:
`MVKCommandBuffer::_needsVisibilityResultMTLBuffer` is reset on every
`vkBeginCommandBuffer` and set **only** by `vkCmdBeginQuery`
(`MVKCmdQueries.mm:49`, `MVKCommandBuffer.mm:242`); `beginMetalRenderPass`
attaches the Metal visibility-result buffer to the encoder only if
that flag is already set at pass-begin (`MVKCommandBuffer.mm:776`).
Under this project's `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`
(immediate encoding, since retired — see `xemu-config-and-flags`), a
render pass's Metal encoder is created the instant the pass is
recorded, before any in-pass `vkCmdBeginQuery` inside it can set the
flag. So any frame whose *first* render pass contained the frame's
first zpass (occlusion query — "how many pixels passed the depth
test") draw got an encoder with a nil visibility buffer and crashed
inside AGX on that draw. This also explained months of non-repro:
bench scenes always start frames with clears/non-zpass draws, so the
first query structurally lands in a later pass whose encoder already
sees the flag set by an earlier pass's query, while the user's content
happened to open some frames with a zpass draw as the very first
operation. The fix, still in the tree (`hw/xbox/nv2a/pgraph/vk/draw.c`,
at command-buffer-begin): an empty "primer" query —
`vkCmdBeginQuery`/`vkCmdEndQuery` on a reserved index, outside any
render pass, before any pass in the command buffer begins. Provable,
not empirical: the flag is now set before ANY pass exists in the
buffer, so every frame's first pass — regardless of what it draws —
sees the visibility buffer attached by construction, not by getting
lucky on draw order.

**Failure modes**

- *Version drift* — the source you read must match the linked binary
  version (1.4.1 here); a fix derived from a different version's
  source can be wrong after a dependency bump. This project pins and
  logs the MoltenVK version + build UUID at bundle time for exactly
  this reason (`scripts/build-moltenvk.sh`, `build.sh`).
- *Reading the wrong layer* — closed-source layers below the
  open-source shim (AGX, below MoltenVK) stay invisible to this
  method; the fix here works by never handing AGX invalid state, not
  by understanding AGX's own crash path.
- *Confirmation bias* — a source-derived theory still needs checking
  against the real crash evidence (stack trace, exact call site); a
  plausible-sounding mechanism isn't the same as a confirmed one.
- *Complements, doesn't replace, empirical bisection* (recipe 3) — an
  earlier capacity investigation (recipe 8) was necessary first here,
  just to know which subsystem's source to go read.

**Copy this pattern when:** a crash signature repeats after a
structurally-reasonable fix, or a fix "mostly" works and another
empirical guess feels like it would just be luck.

---

## 5. Bit-exact differential validation design

**Use when** you're building a new execution engine (a JIT, a
reimplemented algorithm) that must match a trusted reference bit for
bit, and naively running both and comparing on every execution is too
expensive to leave enabled.

**Steps**

1. Before building anything, make the affordability argument: is the
   thing under test **deterministic** given its pre-state? If a
   translation unit is a pure function of well-defined inputs, one
   passing comparison *per unique translation* (not per execution) is
   sufficient — this turns an O(executions) validation cost into
   O(unique translations).
2. Gate re-validation on translation identity: stamp a "validated" bit
   per translation unit, clear it exactly when the translation changes
   (self-modifying code, retranslation), skip the compare entirely
   once the bit is set. Guard the stamp against races — only trust it
   if the translation's identity hasn't changed between snapshot and
   stamp.
3. Narrow what you compare: compute a conservative write-set (which
   memory regions a unit could have touched) at build time, and
   byte-compare only those regions. Read-heavy/compute-heavy units can
   see an order-of-magnitude cut in compared data.
4. Move the actual reference re-execution off the hot thread: snapshot
   pre/post state into a fixed-size lock-free (single-producer,
   single-consumer) ring from the hot thread — cheap, a couple of
   memcpys — and let a dedicated worker thread replay the reference
   and compare. On ring-full, drop the newest entry rather than
   blocking; validation degrades to sampling under load instead of
   stalling the product.
5. Handle non-replayable effects explicitly. Any I/O with the outside
   world (peripheral/DMA access) can't be deterministically replayed
   against a private state copy — shim those callbacks in the replay
   copy to a "skip compare" stub, and have the live engine flag any
   block that touched such I/O so both sides agree to skip it.
6. Keep a synchronous, blocking mode behind a separate flag for
   bring-up debugging, so an abort can fire at the exact instant of
   divergence when you're actively hunting a bug — at the cost of the
   full slowdown, so don't run it as the default.

**Worked example: `XEMU_DSP_JIT_DIFF`** (design:
`docs/dsp-jit-design.md` §2.9 "Differential test mode"; mechanisms in
`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`). Determinism
argument: a translated DSP56300 (the Xbox APU's Motorola-family DSP
core) basic block runs the same handler functions every time for a
given pre-state, so the design doc states plainly that one passing
validation proves correctness for every future execution of that
translation. The `DspJitBlock.diff_checked` flag (set on first
enqueue; cleared on `dsp_jit_invalidate` and on fresh `translate_block`,
trusted only if the block's entry pointer hasn't changed since
enqueue) bounds total validator work to roughly the number of unique
block translations per session — hundreds to low thousands for a game
— instead of millions of executions per second. Async mode (default):
the APU thread snapshots pre/post state into a 16-slot SPSC ring for a
dedicated `mcpx.dsp_diff` worker thread to replay and byte-compare;
APU-thread cost is two memcpys (~160 KB) and the DSP's lock is never
held across the slow replay; on ring-full the producer drops the
newest entry. Narrowed compare: each block stores a `write_set`
bitmask (xram/yram/pram/mixbuffer/peripheral) computed at translation
time, cutting FIR/IIR kernel compares from ~40 KB to ~2 KB (20x).
Peripheral shims: the replay's private `dsp_core_t` copy gets stub
`read_peripheral`/`write_peripheral` pointers that mark the slot "skip
compare" (the real callbacks assume a different parent struct); the
live JIT independently sets `core->jit_skip_diff_compare` on
`DMA_CONTROL` writes so both sides agree to skip non-replayable I/O.
**The failure that motivated the gate**: before it existed, continuous
validation drowned the DSP's lock hold time, starving the main
thread's MCPX MMIO access and hanging startup behind an indefinite
bouncing dock icon (§2.9) — the gate is required for the emulator to
boot at all with the validator on, not a performance nicety.

**Failure modes of the method itself**

- *The determinism argument is load-bearing* — if the reference and
  the engine under test can observe different inputs for "the same"
  pre-state (an unshimmed peripheral read, say), the gate silently
  under-validates: a block gets stamped "validated" from a run that
  never exercised the divergent path.
- *Write-set narrowing is only as good as its conservativeness* — an
  under-approximated write-set can hide a real divergence outside it.
- *Sampling-under-drop means silence isn't proof at high load* — "ran
  N hours async, zero aborts" is evidence proportional to the ring's
  throughput, not exhaustive coverage.
- *Sync mode's slowdown* (roughly 2-10x, per the design doc) makes it
  unsuitable for the volume of testing needed to find rare
  divergences — reserve it for pinning down an already-suspected block.

**Copy this pattern when:** you're adding an alternate execution path
for something with a well-defined, trusted reference, and validating
every execution is too expensive to leave switched on.

---

## 6. Adversarial self-refutation harnesses

**Use when** you're about to trust an invariant your change depends on
(a cached value stays in sync with its source of truth, a pool never
overflows, a classifier is complete) and nothing currently in the
build would notice if it broke.

**Steps**

1. Write the specific checker that would fail loudly on a violation —
   not a generic assert, but one that reports enough to debug from a
   single failing run (the exact PC/instruction, or both copies of a
   value under comparison).
2. Make it opt-in (env-flag gated) and provably free when off — a
   single cached-flag check or a compile-time-elided branch — so it
   can live in the normal binary instead of a separate build.
3. Run it against real, varied content, not a synthetic unit test —
   the invariant you're worried about usually only breaks under rare
   opcodes, rare timing, or rare pool pressure that a hand-written
   test won't happen to hit.
4. When it fires, use the *exact* location it reports to find every
   other place with the same shape of bug, not just the one instance.
5. If the invariant is violated at a nontrivial rate — not a rare edge
   case — treat that as grounds to retire the optimization it was
   protecting, not just patch the one instance. Measure the rate
   before deciding.
6. For capacity/pool invariants specifically, prove the guard fires
   under deliberately hostile parameters (shrink the pool far below
   expected steady-state) rather than trusting it silently never gets
   hit in normal operation.

**Worked examples** (all in
`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c` unless noted):

- **`XEMU_DSP_JIT_PIN_AUDIT`** (added `be1fe6e242`, 2026-04-19 04:04).
  A/B accumulator register pinning (`91343ad754`, 03:21) was reverted
  25 minutes later on a startup crash (`67c0e4c204`, 03:46) and
  reapplied 4 minutes after that (`5b9c8d329e`, 03:50) once the audit
  tool was on the way. The audit emits ~30 ARM64 instructions after
  every JIT instruction's epilogue that re-derive the pinned
  accumulator from memory as the interpreter would, compare against
  the pinned register, and print pc/inst/pin/memory on mismatch. It
  caught two independent pin-leak bugs, each within about a 30-second
  run: a direct accumulator-slot write in a `pm_2_2` parmove
  (`bf82c37fba`, 04:16 — "first divergence was at pc=0x0207,
  inst=0x220c00") and a dual X/Y register load in a `pm_8` parmove
  (`54d1543d50`, 04:42 — "Diagnosis time: ~30 seconds from the audit
  log pointing at pc=..."). Both fixed today; the audit hook stays
  available at zero cost when unset.
- **`XEMU_DSP_JIT_SENTINEL` poison-value harness.** Built to bisect why
  two "round-4" JIT optimizations — a `cur_inst` preset skip, and
  eliding the PC-mismatch exit check (`EPI_NO_PC`) — couldn't land
  cleanly. Instead of guessing which handler secretly depended on the
  elided state, the harness substitutes a poison constant
  (`DSP56K_JIT_SENTINEL_POISON = 0x00adbeef`, `dsp56k_jit_arm64.h:76`)
  for the skipped write, so any handler that reads it crashes loudly
  and identifiably. **The harness itself needed a self-check**: an
  early version emitted the divergence logger *after* the
  PC-mismatch exit branch, so on an actual divergence the block
  jumped straight to the exit stub and the logger never ran — it
  silently reported "389M ops watched, 0 divergences" on a run that
  provably crashed with the same optimization forced on (`1f81e22573`).
  A harness that never fires is ambiguous between "invariant holds"
  and "harness can't see a violation"; only a forced-failure control
  caught this one. Fixed and re-run (`d0e1f0b27d`), the sentinel
  measured 5.77M PC divergences out of 59.2M watched parmove/
  long-immediate ops in one Azurik boot — 9.75%, not a rare corner
  case. `EPI_NO_PC` was dropped permanently: **the optimization was
  disproven by the very audit built to justify keeping it** (the two
  real causes, `calc_ea` mode-6 two-word parmoves and PC rewinds
  inside REP/DO loops, were later handled correctly by a static
  instruction-length classifier, `ca7fe51a0b`, instead of skipped).
- **Forced-failure guard validation, `XEMU_MAX_QUERIES`** (`bfcf38843f`,
  2026-07-03). A capacity guard against occlusion-query pool
  exhaustion is silent by design on the happy path — at the shipped
  pool size (4096) it should rarely or never fire. To prove the guard
  *works*, not merely exists, the pool ceiling was forced to 64 via
  the same env override used for production tuning: the guard fired
  3312 times in a 5-second interval with zero crashes at a steady
  30 fps, versus zero guard trips at 43 fps with the normal pool. The
  guard's presence in the diff was not evidence it worked; the
  forced-failure run was.

**Failure modes of the method itself**

- A harness that never fires is ambiguous between "holds" and "can't
  observe a violation" — the sentinel's placement bug is the canonical
  example; validate the harness's own failure path (force the
  condition it should catch) before trusting its silence.
- Audit overhead can mask or shift timing-dependent bugs; keep its
  cost isolated to the flagged path so un-audited timing is unaffected.
- A check scoped to the one op that crashed once misses the next
  instance of the same bug class in a different opcode — generalize
  the check or grep for similar code before declaring victory.
- Rate matters: near-0% might justify a narrow fix; double digits
  (9.75% here) says rethink the whole approach.

**Copy this pattern when:** you're relying on an invariant with no
existing failure signal — especially register/pin-sync correctness,
pool/capacity ceilings, or "this optimization is safe because X never
happens."

---

## 7. Statistical benchmark discipline

**Use when** you have two variants (with/without a change, old/new
flag value) and need an fps or timing number that survives "that's
just noise" or "you got lucky on scene content."

**Steps**

1. Interleave the variants (A, B, A, B, A, B, ...) rather than
   blocking all of A then all of B — this cancels thermal drift and
   other system-state confounds that would otherwise bias whichever
   variant runs second (hotter machine) or first (cold caches).
2. Run at least 3 interleaved pairs before trusting a mean; report the
   spread, not only the average.
3. Drop load/transient samples — the first interval after a scene
   loads includes asset streaming and pipeline warm-up unrelated to
   the variant under test.
4. Anchor scene identity on a content metric, not wall-clock position
   or a level name. This project uses draws-per-flip (draw calls
   submitted between flip/swap boundaries) specifically because a
   nominally single "scene" can be internally bimodal (next point).
5. State the variance alongside the number, and know which regime
   you're in: this project's reproducibility bar is about ±0.02 fps
   for a fully static scene replayed from a savestate (a QEMU
   `savevm` snapshot restored from the game's qcow2 hard disk image),
   versus roughly ±5 fps for a live-movement route. A claimed delta
   below the relevant noise floor is not a result.
6. Detect bimodal content **before** comparing variants — histogram
   the content metric across intervals within a single run. A scene
   that silently alternates between two very different draw-call
   regimes will produce a comparison that depends on which regime
   each variant happened to sample more of, not on the variant.

**Worked example — the cautionary tale.** A per-flight vertex-RAM
mirrors experiment initially measured "-30%" and nearly got treated as
a regression, until the number was traced to an invalid baseline: that
run had been parked on a 3-draws/flip static screen (effectively a
menu frame) while the comparison run sampled actual attract-reel
content (README.md, "Per-flight vertex-RAM mirrors" row) — the two
runs weren't comparing the same scene at all. Measured properly
(interleaved, content-matched via draws/flip), the change was
~neutral, not -30%. Companion trap (dated 2026-07-04 session record):
Azurik's attract-reel entry is itself bimodal within one continuous
capture — a title-screen interval at ~3 draws/flip and 60 fps, versus
a demo-reel interval at 350-390 draws/flip and 20-48 fps — so any A/B
comparison that skips the draws/flip check can silently pit a menu
frame against a rendering-heavy one and report a meaningless delta.
The project's own harness (`scripts/bench-savestate-ab.sh`) encodes
several of these disciplines directly: it interleaves pairs (alternating
env-value runs), its usage comment says explicitly to "drop the first
(load transient)," and its documented baseline reproducibility for a
fully static scene is ±0.02 fps.

**Failure modes**

- Interleaving cancels drift, not a bias shared by both variants (e.g.
  background load correlated with time of day) — longer soaks reduce
  this, never eliminate it.
- "≥3 pairs" is a floor — a genuinely marginal effect near the noise
  floor needs more pairs or a redesigned experiment.
- Draws/flip is an approximation, not a full fingerprint — two frames
  with equal counts can still differ in overdraw/complexity; use it as
  a fast identity/bimodality check, not proof of identical frames.
- A mean without spread invites both false confidence (a tiny real
  effect misread as solid) and false alarm (a noisy effect misread as
  strong).

**Copy this pattern when:** about to report or accept any fps/timing
delta. The mechanics of actually running the interleaved harness
(loading a savestate, capturing nsprof, the monitor-socket dance) live
in the `xemu-testing` skill — this recipe is the statistical reasoning
for why that protocol is shaped the way it is.

---

## 8. Cadence-change capacity analysis

**Use when** a change alters how *often* a per-frame/per-pass/per-draw
resource gets allocated from a fixed-size pool — new call sites, finer
rotation granularity, deferred consumption that changes batch size —
even if the change doesn't touch the pool's size at all.

**Steps**

1. Identify every fixed-capacity pool or partition the changed code
   path allocates from, directly or indirectly.
2. Work out the allocation cadence **before** your change (units per
   frame/pass/draw) and **after**. The risk is specifically in a
   *change to the rate*, not in the pool having been mis-sized from
   day one.
3. Multiply the new cadence by the expected **worst-case** units per
   frame (not the average) to get peak demand, and compare it against
   the pool's actual capacity — per-partition, if the pool is sharded
   (e.g., one slice per flight slot).
4. If any guard protecting the pool is implemented as an assert,
   check explicitly whether it survives your build configuration. In
   this renderer, checks wrapped in `nv2a_vk_assert` compile to
   `((void)0)` whenever `NV2A_VK_PERF_BUILD` is defined
   (`hw/xbox/nv2a/pgraph/vk/debug.h`) — and as of 2026-07-04 that
   macro is unconditionally `1` in this tree, meaning these
   particular checks are **always** stripped, not just in an
   optimized/release configuration. An assert that's compiled out is
   documentation, not a guard.
5. Where headroom can't be comfortably proven, add a runtime guard
   that degrades gracefully **before** the pool is exhausted (e.g.,
   force an early flush/submit), rather than only enlarging the pool
   — sizing alone protects today's content, not the next heavier
   scene.
6. Validate the guard by forcing the failure condition deliberately
   (shrink the pool via a test knob far below expected demand) and
   confirm it fires cleanly with no crash — this is recipe 6's
   forced-failure discipline, applied specifically to capacity.
7. Re-size the pool with a documented margin above measured peak
   cadence, and record the sizing arithmetic in a code comment so the
   next person who changes the cadence again has the numbers to redo
   this analysis.

**Worked example: occlusion-query partition exhaustion** (2026-07-03/04).
The occlusion/report rework (`6bfbc22863`) changed query rotation from
"reset per query, forcing a render-pass teardown" (one full
tile-based-deferred-renderer pass per draw on Apple GPUs — TBDR
loads/stores the whole tile per pass, making pass count a first-order
cost — measured at ~376 passes/flip) to "bulk-reset the whole
partition once at command-buffer begin, then begin/end queries inside
one long-lived pass." That's a cadence change: instead of a bounded
number of resets per flip, the renderer now allocates a fresh query
*index* per rotation inside a command buffer that could span an entire
frame. The existing partition was sized at 512 per flight slot (1024
total ÷ `NUM_FLIGHT_SLOTS`=2) for the old, pass-per-query cadence; the
new cadence measured ~200-400 index rotations per frame in heavy zpass
scenes — close enough to the old ceiling that heavy scenes exceeded it
mid-recording. The only protection was `nv2a_vk_assert`, stripped in
this tree (step 4) — so the allocation index ran past the pool's
backing array in practice, and MoltenVK wrote through a bad
visibility-result offset, crashing in
`AGXMetal setVisibilityResultMode:offset:` on a real user's savestate
load (`bfcf38843f`). The fix matched steps 5 and 7: (a) a runtime
guard — `begin_draw()` now submits (`VK_FINISH_REASON_REPORTS_FULL`)
and starts a fresh recording before the partition can overflow,
mirroring an existing guard on the report-allocation side; (b) the
pool was resized 1024→4096 (2048/slot), with the code comment
recording the sizing arithmetic directly ("heavy scenes measure
~200-400 rotations per frame; 2048 per slot leaves ample headroom").
Validated per step 6 with `XEMU_MAX_QUERIES=64`: the guard fired 3312
times in a 5-second interval with zero crashes at a steady 30 fps,
versus zero guard trips at 43 fps with the normal pool. This guard was
not the whole story — a second, independent crash mechanism triggered
by the *same* cadence change is recipe 4's worked example; capacity
analysis and source-level driver analysis were both necessary.

**Failure modes**

- Sizing against *average* cadence instead of worst-case peak — a pool
  sized for the typical scene still blows through on the heaviest
  content a player reaches; prefer a guard that degrades over a pool
  that's merely "big enough for now."
- Trusting an assert as a guard without checking build flags — the
  single most expensive mistake in the worked example: the check
  existed for a long time before it mattered, and gave zero real
  protection in the shipped binary.
- Treating a pool resize as sufficient without a runtime guard — a
  bigger pool only postpones the same failure to a heavier future
  scene.
- Stopping at the first plausible capacity mechanism — a correct
  capacity guard can still not be the only cause of a given crash
  signature; a recurring, identical crash after a capacity fix should
  reopen the investigation (recipe 4), not be assumed fixed.

**Copy this pattern when:** reviewing or authoring a change that adds
a new allocation call site to an existing pool, increases the
rotation/rate of an existing one, or changes what triggers a
per-frame resource's lifecycle boundary.

---

## When NOT to use this skill

- **Running the standard interleaved savestate A/B benchmark**
  step-by-step (harness invocation, monitor-socket dance, snapshot
  loading, artifact capture mechanics) — that's `xemu-testing`. This
  skill's recipe 7 explains *why* the protocol is shaped the way it
  is; it assumes the mechanics already exist.
- **Deciding what evidence a class of change owes before it can
  land**, or the lifecycle policy for an idea (env-flag → measured →
  adopt-or-document-retirement) — that's `xemu-research-methodology`
  (evidence bar, predict-before-run discipline) and
  `xemu-change-control` (what each change class must ship with).
- **Looking up whether a specific bug or optimization has already
  been tried and what happened** — that's `xemu-failure-archaeology`,
  the full chronicle. This skill teaches the *methods* used to reach
  those verdicts (several of which are cited here as worked
  examples), not a browsable index of verdicts.
- **Symptom-first triage** ("I'm seeing pink tiles, what do I check
  first?") — that's `xemu-debugging-playbook`. Come here once you
  already know which analysis technique the symptom calls for, or
  after the playbook's first checks haven't resolved it.
- **Looking up a specific env var's name, default, or current
  effect** — that's `xemu-config-and-flags`.

## Provenance and maintenance

Every fact above was verified against this repo on 2026-07-04.
Volatile ones, with a one-line re-check each:

- nsprof counters/events list: `grep -n "NSPROF_" hw/xbox/nv2a/nsprof.h`
- `pgraph_vk_finish` definition: `grep -n "^void pgraph_vk_finish" hw/xbox/nv2a/pgraph/vk/draw.c`
- Flight slot count: `grep -n "NUM_FLIGHT_SLOTS" hw/xbox/nv2a/pgraph/vk/renderer.h`
- `FinishReason` names: `grep -n "VK_FINISH_REASON_" hw/xbox/nv2a/pgraph/vk/renderer.h`
- `nv2a_vk_assert` strip status: `grep -n "NV2A_VK_PERF_BUILD\|define nv2a_vk_assert" hw/xbox/nv2a/pgraph/vk/debug.h`
- Query pool sizing / `XEMU_MAX_QUERIES`: `grep -n "max_queries_in_flight\|XEMU_MAX_QUERIES" hw/xbox/nv2a/pgraph/vk/reports.c`
- Primer query: `grep -n "primer" hw/xbox/nv2a/pgraph/vk/draw.c`
- DSP JIT diff/audit/sentinel knobs: `grep -n "XEMU_DSP_JIT_DIFF\|XEMU_DSP_JIT_PIN_AUDIT\|XEMU_DSP_JIT_SENTINEL" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- Differential-validation design: `sed -n '164,227p' docs/dsp-jit-design.md`
- Sentinel poison constant: `grep -n "SENTINEL_POISON" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h`
- README wait-relocation / GPU-bound verdict: `grep -n "real GPU time, not slack" README.md`
- README byte-exact vertex refinement numbers: `grep -n "forced finishes 5.65" README.md`
- README occlusion-rework numbers: `grep -n "render passes" README.md`
- Bench harness interleaving/transient-drop convention: `sed -n '1,20p' scripts/bench-savestate-ab.sh`
- Visual artifact oracle description: `grep -n "Visual artifact oracle" -A 15 .claude/skills/xemu-testing/SKILL.md`
- No CI workflow runs a test suite: `grep -rniE "make check|meson test|ninja test|pytest" .github/workflows/*.yml` (expect no output)
- Any commit hash cited above: `git show --stat <hash>` — every hash in
  this skill was checked this way: `a74efb21fb`, `fc58c637c4`,
  `531122e8aa`, `bfcf38843f`, `bb33ccaa2e`, `6bfbc22863`, `ca7fe51a0b`,
  `91343ad754`, `6e8c6cc481`, `67c0e4c204`, `5b9c8d329e`, `be1fe6e242`,
  `bf82c37fba`, `54d1543d50`, `1f81e22573`, `d0e1f0b27d`
