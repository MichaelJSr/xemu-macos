---
name: xemu-diagnostics-and-tooling
description: >-
  How to MEASURE this xemu-macos fork instead of eyeballing it.
  Interpretation guide for every diagnostic channel (XEMU_NV2A_NSPROF
  PFIFO-thread profiler, XEMU_APU_PROF audio-thread utilization,
  XEMU_PFIFO_HEARTBEAT hang triage, the DSP56300 ARM64 JIT's
  STATS/DIFF/DUMP/PIN_AUDIT/SENTINEL knobs, plus host tooling: ps %cpu,
  /usr/bin/sample, Instruments/xctrace) with exact output formats and
  healthy-vs-pathological number ranges. Also ships four smoke-tested
  scripts under the skill's own scripts/ directory: nsprof_summarize.py
  (parse/A-B nsprof logs), list_snapshots.py (read qcow2 savestate tables
  without qemu-img), score_frames.py (magenta/pink-tile artifact scorer
  for captured PNGs), inject_input.sh (XEMU_INPUT_PIPE key-event helper).
  Load this skill when asked to interpret nsprof/apu_prof/heartbeat/DSP-JIT
  output, when you need a number instead of a guess, when a perf claim
  needs supporting data, or when reaching for any script under
  .claude/skills/xemu-diagnostics-and-tooling/scripts/.
---

# xemu diagnostics and tooling

This fork's optimization work runs on measurement, not impressions — every
number in README's Changes section came from one of the channels below. This
skill is the interpretation guide: what each channel prints, what the fields
mean mechanically, and what range separates "healthy" from "something is
wrong." It also ships four working scripts (under this skill's own
`.claude/skills/xemu-diagnostics-and-tooling/scripts/`, not the repo's
top-level `scripts/`) that turn raw diagnostic text into tables you can
actually read.

All facts below were verified against the repo on 2026-07-04 by reading the
implementing source directly (file:line references throughout so you can
re-verify after the tree moves).

## Route finder — which channel answers which question

| You want to know | Use | Section |
|---|---|---|
| Is the renderer doing more GPU work than it should this frame? | `XEMU_NV2A_NSPROF=1` | §1 |
| Did my change actually change fps, or is this a different scene? | `XEMU_NV2A_NSPROF=1`, anchor on draws/flip | §0, §1 |
| Is the audio thread keeping up in realtime? | `XEMU_APU_PROF=1` | §2 |
| The game is "frozen" — is the renderer thread alive? | `XEMU_PFIFO_HEARTBEAT=1` | §3 |
| Is the DSP JIT covering this game's instruction mix, or falling back a lot? | `XEMU_DSP_JIT_STATS=1` | §4 |
| Did a JIT change break DSP correctness? | `XEMU_DSP_JIT_DIFF=N` | §4 |
| Where is wall-clock time actually going on the host CPU? | `ps -o %cpu=`, `sample`, Instruments | §5 |
| Are captured frames showing pink/magenta corruption? | `score_frames.py` | §6 |
| What savestates exist in this qcow2? | `list_snapshots.py` | §6 |
| I need to send key events without focusing the window | `inject_input.sh` | §6 |

## §0 Universal precondition: anchor scene identity first

Every counter below is reported **per flip** (one guest frame boundary; see
glossary in §1). Comparing per-flip numbers across two runs is meaningless
unless both runs are rendering the same scene, because draw count dominates
almost every other counter. This fork's benchmark title (Azurik) is
famously bimodal: its attract-reel entry sits on a title card at ~3
draws/flip, 60 fps, then jumps to a demo reel at 350-390 draws/flip, 20-48
fps — a run that starts sampling half a second early or late silently
compares two different scenes and produces a bogus delta. A real invalid
"-30%" regression was once traced to exactly this (a baseline accidentally
parked on a static 3-draws/flip screen).

**Rule:** before trusting any nsprof-derived comparison, check the `draws`
event's `per_flip=` value in both logs. If they differ by more than roughly
15%, the comparison is invalid — re-run from the same savestate. Typical
anchors (Azurik, this dev machine, 2026-07): menus/title cards ~3
draws/flip; attract-reel demo 350-390; heavy in-game 408-485.
`nsprof_summarize.py`'s two-log mode (§6) checks this automatically and
prints a warning.

The full reproducible-benchmark protocol (interleaved A/B pairs, monitor
`loadvm`, boot timing) is owned by the `xemu-testing` skill — this skill
covers reading the numbers it produces, not the harness around them.

## §1 XEMU_NV2A_NSPROF=1 — PFIFO-thread wall-time profiler

Source: `hw/xbox/nv2a/nsprof.c` / `nsprof.h`. A minimal nanosecond
accumulator for the costs that drive this fork's rendering optimizations:
shader/pipeline compiles, texture upload/hash, vertex copies, GPU fence
waits, surface readback, and the flip-to-vblank idle gap. It is designed to
stay cheap in release builds (the older `NV2A_PROF_*` counters are
compiled out there) and print a periodic summary instead of live UI.

Enable with `XEMU_NV2A_NSPROF=1` in the environment before launch. Every
instrumented call site is on the PFIFO thread (the renderer thread —
pusher/puller feeding PGRAPH), so accumulation is unsynchronized by design;
don't read these fields from another thread.

### Output format

A summary prints to stderr roughly every 5 seconds of guest flips
(`nsprof_flip_tick()`, called once per `NV097_FLIP_STALL` completion in
`pgraph/vk/renderer.c:206` — i.e. once per **guest** frame, not once per
*displayed* frame; with `display.frame_interpolation` = 2x/4x the on-screen
fps can exceed nsprof's flips/s, because MetalFX interpolation happens
downstream of PGRAPH). Verified exact format (`nsprof.c:148-171`):

```
nsprof: 5.0s interval, 193 flips (38.6/s)
nsprof:   fence_wait   ev=386     total= 443.90ms per_flip= 2300.0us max= 8123.0us
nsprof:   tex_hash     ev=5824    total=  67.30ms per_flip=  348.7us max=  912.0us
nsprof:   draws               ev=78088   per_flip=  404.6
nsprof:   renderpass          ev=2703    per_flip=   14.0
nsprof:   finish_flip_stall   ev=193     per_flip=    1.0
```

Two line shapes:
- **Counter lines** (timed accumulators): `ev=` count of events, `total=`
  summed nanoseconds for the interval (printed in ms), `per_flip=` that
  total divided by flip count (printed in us), `max=` the single slowest
  event in the interval (printed in us). A counter with zero events that
  interval is omitted entirely.
- **Event lines** (plain counts, no timing): `ev=` count, `per_flip=` count
  divided by flips. Also omitted when zero.

Counters and events both reset to zero after each print — every interval is
independent, not a running total.

### Counters (timed, ns-accumulating)

| Counter | Meaning | Call-site cost it measures |
|---|---|---|
| `shader_gen` | Shader cache miss: GLSL generation + SPIR-V compile | New/changed shader combination |
| `pipeline_gen` | Pipeline cache miss: `vkCreateGraphicsPipelines` | New PSO permutation (blend/depth/vertex-layout state) |
| `tex_upload` | Texture upload: layout conversion + copy + unswizzle | Guest texture changed, needs re-upload |
| `tex_hash` | Texture content hashing (dirty-check) | Every bound texture whose dirty flag needs verifying |
| `tex_snapshot` | Guest VRAM snapshot memcpy before upload | Pre-upload copy-out of guest memory |
| `geom_update` | Vertex RAM / inline / index buffer copies | Per-draw geometry data movement |
| `fence_wait` | `vkWaitForFences` reclaiming a flight slot | CPU stalls waiting for the GPU to free slot k so slot k+1 can record |
| `aux_fence` | Aux command-buffer fence: sync end + lazy async reclaim | Secondary CB bookkeeping (surface downloads, etc.) |
| `mfx_drain` | `metalfx_drain_inflight` CPU spin | MetalFX upscale/interpolation pipeline drain, still on PFIFO |
| `surf_down` | GPU→CPU surface readback | vCPU/blit needs surface contents back in guest RAM |
| `flip_idle` | `FLIP_STALL` → guest vblank release | See below — the display-pacing idle gap |

`flip_idle` is special: it is NOT wasted CPU time in the usual sense. It
starts when the guest issues `NV097_FLIP_STALL` (`pgraph.c:989-996`) and
ends when `pfifo_stall_for_flip()` sees the flip complete
(`pfifo.c:141-153`) — i.e. it is the PFIFO thread waiting for vblank
pacing, which is expected and roughly `1000/target_fps` ms per flip at a
capped frame rate. A `flip_idle` that shrinks to near-zero usually means
the guest is now GPU/CPU-bound and no longer waiting on vblank pacing (fps
below the cap), not that something got faster.

### Events (plain counts)

Grouped by what they diagnose:

**Finish reasons** (`finish_*` — every one is a full submit+wait sync point
on the PFIFO thread, `pgraph_vk_finish`; the “why” mirrors `FinishReason` in
`pgraph/vk/renderer.h`):

| Event | Trigger |
|---|---|
| `finish_vtx_dirty` | Vertex buffer region about to be overwritten while still in-flight |
| `finish_surf_create` | A new surface (render target) is being created |
| `finish_surf_down` | A surface readback is about to happen |
| `finish_buf_space` | Need buffer space, must reclaim before continuing |
| `finish_present` | Present-path synchronization |
| `finish_flip_stall` | Guest issued `FLIP_STALL` (should be ≈1 per flip — if it's not, flips and finishes have decoupled, worth investigating) |
| `finish_flush` | Explicit renderer flush (e.g. savestate) |
| `finish_stalled` | FIFO sat idle with a pending report — full GPU sync instead of the fence-poll fallback |
| `finish_reports_full` | Occlusion-query/report ring is full, must drain |
| `finish_reports_submit` | Report submitted without a full sync (the non-blocking fence-poll path) |

**Everything else:**

| Event | Meaning |
|---|---|
| `draws` | `draw_end` calls — the scene-identity anchor (see §0) |
| `sdown_access_r` / `sdown_access_w` | vCPU read/write of dirty surface VRAM triggered a readback |
| `sdown_vtxram` | Vertex RAM sync overlapped a surface, forcing readback |
| `sdown_texbind` | A texture bind indexed into surface VRAM |
| `sdown_blit` | `NV097` image blit src/dst needed readback |
| `sdown_evict` | Surface eviction/invalidation |
| `sdown_incompat` | Surface shape changed: evict + readback |
| `sdown_flush` | Renderer flush / savestate path readback |
| `supload_color` / `supload_zeta` | RAM → color / zeta (depth-stencil) surface upload |
| `texbind_skip` | Once-per-frame verified-bind fast path taken (texture already known bound+valid — this is the win path, high counts are good) |
| `vtx_exact_skip` | Byte-identical vertex conflict detected, finish skipped (also a win path; see `XEMU_VTX_EXACT`) |
| `renderpass` | `vkCmdBeginRenderPass` on the main command buffer — the first-order TBDR cost (see below) |
| `pipeline_bind` | `vkCmdBindPipeline` (graphics) |
| `vk_draw_call` | Actual `vkCmdDraw`/`vkCmdDrawIndexed` calls in `flush_draw` — vs `draws`, which counts guest begin/end blocks; the gap is the multi-subrange coalescing opportunity |
| `da_multi_subrange` | Non-emulated `DRAW_ARRAYS` block issued with >1 start/count subrange (one `vkCmdDraw` per subrange) |
| `merge_identical` / `merge_candidate` / `merge_cand_udiff` / `merge_state_changed` | Per non-clear block, classification vs the previous block (GPU frame-cost campaign Phase 1): fully deduped already / mergeable — same pipeline+descriptors+buffers, only vertex offsets differ (`_udiff` subset: push-constant payload changed) / unmergeable state change |
| `rpcause_surface` / `rpcause_clear` / `rpcause_texupload` / `rpcause_other` | Which site ended a *live* render pass: RT rebind, `NV097_CLEAR_SURFACE` boundary, compute-unswizzle interleave, surface-create/RTT nondraw. The submit path (`pgraph_vk_finish`) is untagged, so `renderpass` − Σcauses ≈ submit/flip-boundary passes |

### Healthy vs. pathological ranges (Azurik, this dev machine, dated)

| Metric | Healthy | Pathological | Source |
|---|---|---|---|
| draws/flip | scene-dependent: menu ~3, attract-reel 350-390, heavy in-game 408-485 | — (this is identity, not pass/fail — see §0) | README "Changes"; historical session records |
| passes/flip (`renderpass` per_flip) | ~14 | 376 | Apple GPUs are TBDR — every render pass is a full tile load/store, so pass count is a first-order cost. The 2026-07 in-pass-occlusion-queries + deferred-zpass-reports change (zpass = the guest's "how many pixels passed depth" report — see xbox-hardware-reference) cut per-query `vkCmdResetQueryPool` (illegal mid-pass, forcing pass teardown every query rotation) from 376→14 passes/flip on the savestate bench scene. |
| `fence_wait` ms/flip | ~1-3.5 | 8 and up (measured as high as ~32 pre-fix) | Same change: fence wait fell 31.9→2.3 ms/flip. High fence-wait with low draws/flip usually means a sync-policy bug, not GPU load. |
| `pipeline_gen` events | ≈0 once warm (async pipeline creation isn't worth building — this is rare enough already) | Nonzero bursts mid-session = compiling now = visible stutter | `pipeline_gen`'s `max=` field shows the worst single `vkCreateGraphicsPipelines` call: 164 ms measured cold (empty `pipeline_cache.bin`), ≤4 ms once `pipeline_cache.bin` is warm and persisted. |
| First interval after a `loadvm` | — | Always transient — shader/pipeline gen and texture uploads spike right after a snapshot loads because everything looks "new" to the caches | Drop the first interval in any comparison; `nsprof_summarize.py` does this by default (`--drop-first 1`). |

The 2026-07 occlusion/report rework's flagship figure: cite
**38.57 ± 0.80 fps (+144%)** vs the 15.78 fps legacy baseline, per commit
`6bfbc22863`'s protocol receipt (README prose rounds this change to
35.6 fps / 2.25x — added in the same commit, never reconciled; the
stale-doc item is owned by `xemu-docs-and-writing`). Which figure to cite
is `xemu-frontier-and-positioning` §3.5's claiming rule: the
commit-message number carries the receipt — don't average the two or pick
whichever sounds better.

### Tool: `nsprof_summarize.py` (shipped in this skill's `scripts/`, §6)

Parses a captured nsprof stderr log into a per-interval table (fps,
draws/flip, passes/flip, fence ms/flip, finish-reason counts), or diffs two
logs in A/B mode with the draws/flip scene-identity check built in. See §6.

## §2 XEMU_APU_PROF=1 — APU-thread utilization

Source: `hw/xbox/mcpx/apu/apu.c:225-274` (`se_frame`). The MCPX APU
(voice processor + DSP mixdown) runs on its own thread, producing one audio
frame every `EP_FRAME_US` = 5333 us (256 samples / 48000 Hz,
`apu_regs.h:363`). Once a second, xemu measures what fraction of that
budget the thread actually spent working:

```c
g_dbg.utilization = frame_work_acc_us / elapsed_us;
```

`frame_work_acc_us` accumulates wall time across
`mcpx_apu_vp_frame` (voice processor) + `mcpx_apu_dsp_frame` (DSP
GP/EP cores, whichever engine is active) + `mcpx_apu_monitor_frame` for
every frame built in that 1-second window — this is **whole-pipeline**
utilization, not DSP-engine-only. That makes it a legitimate way to A/B
DSP engine choice (interpreter vs. fork ARM64 JIT vs. upstream engine): the
voice-processor cost is constant across those three, so a change in this
number isolates the DSP engine's share.

With `XEMU_APU_PROF=1`, the same number prints to stderr instead of only
feeding the debug UI (verified format, `apu.c:252-255`):

```
apu: utilization  20.3% (188 frames/s)
```

Reading it: **1.0 (100%)** means the thread is not sleeping and is likely
falling behind realtime (audio glitches/underruns are the symptom); **< 1.0**
means it completes each frame with time to spare. `frames/s` should track
the 1000000/5333 ≈ 187.6 target; a value well below that means the thread
is dropping frames, not just running hot.

Reference utilizations (Azurik attract-reel, this dev machine, dated
2026-07): fork ARM64 JIT ~20-25%, interpreter ~22%, upstream
(dsp56300-subproject) engine ~19%. These numbers move with scene/workload —
treat them as a sanity-check range, not a target to hit exactly. Engine
selection precedence (`audio.dsp_jit.enabled` vs `audio.use_dsp_jit`) is
cataloged in `xemu-config-and-flags`; this skill only covers reading the
utilization number once you've picked an engine.

No script ships for this channel — the output is already one line per
second and is easy to `grep`/eyeball or pipe through `awk` for a mean.

## §3 XEMU_PFIFO_HEARTBEAT=1 — hang/freeze triage

Source: `hw/xbox/nv2a/pfifo.c:485-552`. A rate-limited liveness snapshot of
the PFIFO thread's main loop, printed every ~2 seconds
(`QEMU_CLOCK_REALTIME` — which in this codebase is the *monotonic* clock
despite the name; see xemu-architecture-contract Invariant 9 — it's a
liveness signal, not a precise interval timer). Only active when
`XEMU_PFIFO_HEARTBEAT=1`.

Verified exact format (`pfifo.c:538-551`):

```
xemu: pfifo heartbeat iters=48213 halt=0 flush=0 sync=0 waiting_flip=1 waiting_nop=0 waiting_ctxsw=0
```

Reading it — this is purpose-built to distinguish "the PFIFO thread is
alive but the game is legitimately waiting on X" from "the PFIFO thread
itself is stuck":

| Field | Meaning if stuck at 1 (or `iters` frozen) |
|---|---|
| `iters` not incrementing between prints | The PFIFO thread itself is hung — the real freeze is here, not downstream |
| `halt` | Renderer explicitly halted the pusher (surface operation in progress) |
| `flush` | `pgraph.flush_pending` — a flush was requested and hasn't completed; if stuck, the renderer's `vkWaitForFences`/`vkQueueSubmit` is hung |
| `sync` | `pgraph.sync_pending` — same category as `flush`, a synchronization request not completing |
| `waiting_flip` | The guest is waiting on `NV_PGRAPH_INCREMENT READ_3D` (flip acknowledgement) — pairs with nsprof's `flip_idle` counter; if this is stuck AND `flip_idle` is climbing without bound, vblank pacing itself is stuck (compositor/present, not PGRAPH) |
| `waiting_nop` | Waiting on a NOP fence |
| `waiting_ctxsw` | Waiting on a context switch |

Use this only for hang triage (a game reported "frozen") — it has no
purpose in a perf investigation, and its 2-second cadence is too coarse for
anything but "is the thread alive." Symptom-to-flag triage tables live in
`xemu-debugging-playbook`; this skill only documents what the fields mean.

## §4 DSP JIT diagnostics (`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`)

This is the fork's basic-block JIT for the two Motorola DSP56300 cores (GP
= Global Processor, EP = Effects Processor) inside the MCPX APU — distinct
from the "upstream" dsp56300-subproject engine (`dsp/dsp_jit.c`, a
different Rust-backed JIT) that `audio.use_dsp_jit` controls. All of the
env vars below only affect the fork's engine; precedence between the two
engines is owned by `xemu-config-and-flags` and `xemu-architecture-contract`.

All flags are parsed once (`parse_flags_once()`,
`dsp56k_jit_arm64.c:163-286`) at first use and cached — setting them after
the DSP core has started has no effect within that process.

### Quick reference

| Env var | Purpose | Cost when on |
|---|---|---|
| `XEMU_DSP_JIT=0\|1` | Force the inline JIT off/on *within the interpreter engine only* — it cannot select the engine (`audio.dsp_jit.enabled` does that; precedence: `xemu-config-and-flags`) | none |
| `XEMU_DSP_JIT_STATS=1` | Print a per-core counter summary at exit | negligible (counters only) |
| `XEMU_DSP_JIT_DIFF=N` | Validate JIT output against the interpreter (bit-exact); `1` = every unique translation, `N>=2` = sample every Nth block execution | 2-10x slowdown while validating; bounded by the per-translation gate (see below) |
| `XEMU_DSP_JIT_DIFF_SYNC=1` | Run the validator inline on the APU thread instead of the async worker | reintroduces APU-thread latency — bring-up only |
| `XEMU_DSP_JIT_DIFF_MAX=N` | Stop validating after N total enqueues | bounds a CI run's validation cost |
| `XEMU_DSP_JIT_DUMP=1` | Hex-dump every translated ARM64 block to stderr | very noisy — offline disassembly only |
| `XEMU_DSP_JIT_PIN_AUDIT=1` | Per-op runtime check that pinned accumulator registers (x26/x27) match packed memory | ~10 extra instructions per JIT'd op |
| `XEMU_DSP_JIT_NO_THROTTLE=1` | Disable the retranslation-churn auto-throttle | only matters on self-modifying overlay code |
| `XEMU_DSP_JIT_SENTINEL` / `XEMU_DSP_JIT_FORCE` | Bisect-only harnesses for a specific historical bug (see below) | debugging only, do not use day-to-day |

### `XEMU_DSP_JIT_STATS=1` — exit summary

Prints once per core (GP and EP separately) via an `atexit` handler
(registered because the normal quit paths — Cmd-Q, window close, SIGTERM —
never call the clean-shutdown `dsp_destroy`). Verified fields
(`dsp56k_jit_arm64.c:10002-10080`, struct `DspJitState`):

```
xemu: DSP JIT stats (GP core):
  blocks_translated = 4213
  blocks_executed   = 8841027
  cache_flushes     = 2
  fallbacks         = 19
  diff_ops_checked  = 0
  code_buf_used     = 612384 bytes / 8388608 bytes
  alu_inlined       = 91442 (98.7% of ALU ops)
  alu_fallback      = 1180
  alu_ccr_skipped   = 33012 (lazy-flag: dead ccr emit elided)
  alu_ccr_nz_skipped= 4108 (lazy-flag: N/Z-only half elided)
  alu_ccr_eu_skipped= 2244 (lazy-flag: E/U-only half elided)
  cf_inlined        = 60218 (94.2% of emitted ops)
  cf_fallback       = 3719
  cf_fallback buckets:
    movep_1      = 1802 (48.5% of cf_fallback)
    div          = 640 (17.2% of cf_fallback)
    ...
```

Field meanings: `blocks_translated`/`blocks_executed` — translation-cache
efficiency (executed >> translated is the goal: a block is compiled once
and run many times). `cache_flushes` — how many times the 8 MiB
per-core code buffer filled and had to be reset (frequent flushes on a
short session suggest a program that's constantly generating new code, or
the buffer is undersized for this title). `fallbacks` — whole-block BLR
fallback to the interpreter (as opposed to `alu_fallback`/`cf_fallback`,
which are per-*instruction* fallbacks inside an otherwise-JIT'd block).
`alu_inlined`/`alu_fallback` and `cf_inlined`/`cf_fallback` — translate-time
counts (not execute-time) of ALU / control-flow opcodes emitted as native
AArch64 vs. calling back into the C interpreter helper; the percentage is
"coverage on this workload," useful for deciding whether a new game exposes
an opcode class worth inlining. `alu_ccr_*_skipped` — the lazy
condition-code-flag elimination: when the *next* instruction in a block
unconditionally overwrites all of SR.E/U/N/Z (a "full ccr writer"), the
current instruction's own flag computation is dead code and gets elided;
these three counters show how often that firing (whole computation vs. just
the N/Z or E/U half).

The `cf_fallback buckets` block (only printed if any bucket is nonzero)
breaks per-instruction control-flow fallbacks down by opcode class —
verified bucket names (`dsp56k_jit_arm64.h`, `dsp_cpu.c:1815-1839`):
`other`, `movep_1`, `movep_23`, `movep_x_qq`, `div`, `norm`, `stop`, `wait`,
`reset`, `nop`, `illegal`, `undefined`, `bit_manip`, `short_imm_alu`,
`shift_imm`, `inc_dec`, `cmpu`, `mpyi`, `move_extended`. A large bucket
names exactly which opcode class to inline next for better coverage.

**Auto-throttle** (`DSP56K_JIT_THROTTLE_WINDOW` = 256 Ki block executions,
`DSP56K_JIT_THROTTLE_RATIO` = 16, i.e. threshold = 1 retranslation per 16
executions in the window): if retranslation churn — usually self-modifying
overlay code — exceeds that ratio, the JIT disables itself for that core
for the rest of the session and the interpreter takes over silently.
Verified log line (`dsp56k_jit_arm64.c:10827-10838`):

```
xemu: DSP JIT auto-throttled on EP core: 20734 retranslations in the last 262144 block executions (self-modifying overlay code); interpreter takes over. XEMU_DSP_JIT_NO_THROTTLE=1 disables this.
```

A dated reference point for "how different is healthy vs. pathological
churn here" (`21e9045344`): the EP core measured ~1 retranslation per 10
block executions (pathological — its program overlays P-space every pass,
so it trips the 1:16 throttle on the first window), while the GP core
measured ~1:10000+ (healthy).

### `XEMU_DSP_JIT_DIFF=N` — differential validator

Bit-exact validates JIT output against the interpreter. Because
translation is deterministic (same input block always produces the same
code), **one passing validation per unique translation is sufficient** —
a per-block `diff_checked` gate (`dsp56k_jit_arm64.c:10842-10858`) skips
re-validating a block once it has passed once, which is what keeps
validation cost bounded to roughly "number of unique block translations"
rather than "blocks executed per second." `XEMU_DSP_JIT_DIFF=1` validates
every unique translation once; `XEMU_DSP_JIT_DIFF=N` (N>=2) additionally
samples — only about 1-in-N not-yet-checked block *executions* even
attempt to enqueue — spreading validation bursts over wall time. `N` is
capped at 1,000,000. Recommended for day-to-day bisects: a sampled value
like `XEMU_DSP_JIT_DIFF=10`, not `=1` (bring-up only).

Two execution modes:
- **Async (default)**: a 16-slot SPSC ring feeds a dedicated worker thread
  named `mcpx.dsp_diff` (`dsp56k_jit_arm64.c:10623`). The APU thread only
  pays for a snapshot copy into the ring; validation runs off-thread.
- **Sync** (`XEMU_DSP_JIT_DIFF_SYNC=1`): the APU thread runs the
  interpreter replay and compare inline before continuing — reintroduces
  latency, useful only when you want the abort to fire the instant a
  divergence happens rather than whenever the async worker gets to it.

On any divergence, **both modes abort the process** (`abort()`, after
`fflush(stderr)` so the diagnostic survives an abrupt Xcode-build
termination) and print a one-line failure report identifying which state
field first differs (`diff_report_failure`, `dsp_cpu.c` /
`dsp56k_jit_arm64.c:10404-10420`):

```
xemu: DSP JIT DIFF FAILURE in block pc_start=0x0142 (jit_cycles=12)
  First differing byte at offsetof dsp_core_t = 1284 (registers[7]: interp=0x001234 jit=0x005678)
  (pc at entry = 0x0142)
  INTERP pc=0x014e sr=0x000040 A2:A1:A0=00:123456:000000 B2:B1:B0=ff:abcdef:000000
  JIT    pc=0x014e sr=0x000040 A2:A1:A0=00:567800:000000 B2:B1:B0=ff:abcdef:000000
  loop_rep interp=0 jit=0  interrupt_counter interp=3 jit=3
```

`XEMU_DSP_JIT_DIFF_MAX=N` caps total validations for CI runs that want
"validate the first N then run free" — usually unnecessary since the
per-translation gate already bounds the work; prefer sampling with plain
`XEMU_DSP_JIT_DIFF=N` for everyday use.

### `XEMU_DSP_JIT_DUMP=1` — block disassembly dump

Every translated block prints its address range and a raw 32-bit-word hex
dump to stderr (`dsp56k_jit_arm64.c:9883-9907`) for offline disassembly —
very high volume, only for inspecting a specific translation by hand
(pipe through `grep -A20 'pc_start=0x<addr>'`).

### `XEMU_DSP_JIT_PIN_AUDIT=1` — accumulator pin consistency

Emits a runtime check after most instructions that the "pinned" A/B
accumulator registers (ARM64 x26/x27, used to avoid reloading the 56-bit
accumulator from memory every instruction) still match the packed value in
`dsp->registers[]`. Deliberately does **not** assert/abort — a single
mismatch might self-heal on the next reload — it just prints and keeps a
counter, because the point is to see the *pattern* of divergence, not stop
at the first one. Verified failure format
(`dsp_cpu.c:1410-1450`):

```
DSP JIT pin audit FAIL: which=A pc=0x0142 inst=0x0140c1 pin=0x00ffabcdef123456 mem_packed=0x000000abcdef1234 (A2=0x00 A1=0xabcdef A0=0x123456)
```

`which` is `A`/`B` (56-bit signed accumulator, packed A2:A1:A0) or `X`/`Y`
(48-bit unsigned, packed hi:lo). Use this to find which emitter or BLR
fallback path is leaving a pin out of sync after a new inlined-opcode
change.

### `XEMU_DSP_JIT_SENTINEL` / `XEMU_DSP_JIT_FORCE` — historical bisect harnesses

Bit 0 of each controls a specific, already-settled bug investigation (the
"round-4 `cur_inst` skip" regression): `SENTINEL` poisons `dsp->cur_inst`
with `0x00adbeef` for parmove classes that shouldn't need it at runtime, to
catch a handler that reads it anyway; `FORCE` re-applies the literal skip
that caused the regression, for reproducing it on demand. These exist as
kept-in-tree harnesses for a fight that already happened — the full story
(what regressed, how it was caught, the fix) is in `xemu-failure-archaeology`.
Do not reach for these unless you are specifically re-investigating that
class of bug; for anything else they will just poison state you don't
expect.

### Decision table

| You want to... | Set |
|---|---|
| See opcode coverage / fallback rate on a new game | `XEMU_DSP_JIT_STATS=1`, quit normally, read the exit summary |
| Prove a JIT change didn't break correctness | `XEMU_DSP_JIT_DIFF=10` (sampled, async) for a normal play session |
| Catch a divergence the instant it happens (bring-up) | `XEMU_DSP_JIT_DIFF=1 XEMU_DSP_JIT_DIFF_SYNC=1` on a short repro |
| Find which opcode class most needs inlining next | `XEMU_DSP_JIT_STATS=1`, read the `cf_fallback buckets` list |
| Debug a self-modifying-code title running slower than expected | Check for the auto-throttle log line; `XEMU_DSP_JIT_NO_THROTTLE=1` to A/B against it |
| Track down an accumulator-pin bug after touching an emitter | `XEMU_DSP_JIT_PIN_AUDIT=1` |
| Inspect the actual generated code for one block | `XEMU_DSP_JIT_DUMP=1`, grep for the `pc_start` |

## §5 Host-side tooling

Standard macOS tooling, not fork-specific — pointers only, since these are
publicly documented elsewhere.

**CPU sampling over time**: `ps -o %cpu= -p <pid>` — this fork's own
`scripts/bench-savestate-ab.sh` samples exactly this way every 5 seconds
for the duration of a benchmark run, writing one `%cpu` value per line to a
`.cpu` file per run. Cheap, coarse, good for "did this change raise average
CPU."

**Busy-poll caveat (measured 2026-07-04): the vCPU (mttcg) thread's
utilization is NEVER evidence of guest-boundness on its own.** Azurik
busy-polls instead of HLTing — the vCPU thread reads ~95% even in a
scene pinned at a flat 60 fps vblank cap (F6), identical to a scene
missing frames (F5). The valid limiter signal is PFIFO *starvation*
time: PFIFO cond_wait share minus nsprof `flip_idle` = time the guest
computes while the renderer has nothing to do (≥5.8 ms/flip on the F5
heavy scene). Cross-check any "the guest is the bottleneck" claim
against a capped scene before believing thread %CPU.

**One-shot call-stack sampling**: `/usr/bin/sample <pid|partial-name>
[duration [interval_ms]] -file <path>` (verified usage banner: default
duration 10 s, default interval 1 ms). Good for "what function is actually
hot right now" without attaching a full profiler:

```
sample xemu 5 1 -file /tmp/xemu-sample.txt
```

**Instruments / xctrace** (deeper profiling — Metal System Trace for
GPU-side work, Time Profiler for CPU-side call trees; both templates are
present in a standard Xcode install, verified via
`xcrun xctrace list templates`). CLI form (verified against
`xctrace record`'s own usage text):

```
xcrun xctrace record --template 'Time Profiler' --attach xemu --time-limit 10s --output /tmp/xemu.trace
```

Open the resulting `.trace` in Instruments.app to inspect. This is
standard Xcode tooling guidance, not something this fork customizes.

**Visual artifact capture** (`screencapture -x -o -l<windowID>`, window
discovery via `CGWindowListCopyWindowInfo` matched on
`kCGWindowOwnerPID`): the capture *method*, including the Finder-vs-shell
launch owner-name distinction ("xemu" vs. "Python"), is owned by
`xemu-testing` — this skill covers scoring the resulting PNGs
(`scripts/score_frames.py`, §6), not capturing them.

## §6 Shipped scripts (`.claude/skills/xemu-diagnostics-and-tooling/scripts/`)

The four scripts live under this skill's own directory —
`.claude/skills/xemu-diagnostics-and-tooling/scripts/` relative to the
repo root — NOT the repo's top-level `scripts/` (that one holds
`bench-savestate-ab.sh` and other harness scripts). First invocation below
uses the full path; short names thereafter.

All four are executable, stdlib-only except `score_frames.py` (needs
Pillow), and take `--help`. Every script below was smoke-tested on
2026-07-04 against synthetic fixtures matching the exact formats
documented above (and, for the qcow2 parser, against a real fixture) —
see "Provenance and maintenance" for the re-run commands.

When an example below launches xemu, it launches a scratch APFS clone of
the bundle — never `dist/xemu.app` itself: a harness pointed at the real
bundle hijacks the user's session, config, and hdd write lock
(xemu-testing cardinal rule 2; xemu-change-control non-negotiable 3):

```sh
SCRATCH=$(mktemp -d)
cp -Rc dist/xemu.app "$SCRATCH/xemu-diag.app"
```

### `nsprof_summarize.py`

Parses one or two `XEMU_NV2A_NSPROF=1` stderr logs into a per-interval
table (fps, draws/flip, passes/flip, fence ms/flip, finish counts) with a
mean summary, or an A/B diff between two logs. Drops the first interval by
default (load transient, §1) and warns if draws/flip differs by >15%
between two logs being compared (§0's scene-identity rule, automated).

```
.claude/skills/xemu-diagnostics-and-tooling/scripts/nsprof_summarize.py xemu.log
nsprof_summarize.py before.log after.log        # A/B diff + scene-identity check
nsprof_summarize.py xemu.log --all              # every counter/event, not just headline
nsprof_summarize.py xemu.log --json             # machine-readable
```

Capture a log with:
`XEMU_NV2A_NSPROF=1 "$SCRATCH/xemu-diag.app/Contents/MacOS/xemu" 2> xemu.log`
(scratch clone per the §6 intro — never `dist/xemu.app` itself).

### `list_snapshots.py`

Lists QEMU savestates inside a qcow2 image by parsing the snapshot table
directly — no `qemu-img` needed (useful since a running xemu holds the
image's write lock, which `qemu-img snapshot -l` refuses to open around).
Read-only: opens the file `rb` and never writes.

Verified layout (confirmed against both a synthetic fixture built to the
documented spec and the real `xbox_hdd.qcow2` on this machine): header
offset 60 = `nb_snapshots` (u32 BE), offset 64 = `snapshots_offset` (u64
BE); each table entry is a 40-byte fixed struct (`>QIHHIIQII`:
`l1_table_offset, l1_size, id_str_size, name_size, date_sec, date_nsec,
vm_clock_nsec, vm_state_size32, extra_data_size`), 8-byte aligned, followed
by `extra_data` (if ≥8 bytes, its first u64 supersedes the legacy 32-bit
`vm_state_size`), then `id_str`, then `name`.

```
list_snapshots.py ~/Documents/Xemu/xbox_hdd.qcow2
list_snapshots.py image.qcow2 --json
```

On a live image, clone first so you never contend with a running xemu's
write lock: `cp -c /path/to/xbox_hdd.qcow2 /tmp/snap-list.qcow2` (APFS
instant copy-on-write clone, effectively free).

### `score_frames.py`

Scores captured PNGs for the "pink-tile" magenta corruption pattern:
`R>170 and B>140 and G<0.55*min(R,B)`. Downscales with a box filter to a
coarse grid (default 8px cells) first, tests the magenta condition on that
small grid, then 8-connected-flood-fills hot cells into clusters and
classifies each cluster's morphology by size: a cluster spanning ≥12% of
frame width or height is tagged `screen-rect` (this fork's pink-tile saga —
a MoltenVK immediate-command-buffer-encoding misconfiguration corrupting
whole streamed-texture-sized regions, i.e. compositor/present/driver
territory); a cluster ≤64px per side is tagged `morton-square` (texture /
unswizzle-path territory); anything else is `irregular`.

Requires Pillow (`pip3 install pillow`) — the only script in this set that
isn't stdlib-only.

```
score_frames.py capture001.png capture002.png
score_frames.py captures/*.png --cell-size 16
score_frames.py captures/*.png --json
```

Exit code 0 = nothing flagged, 1 = at least one file flagged (drop straight
into a test script's pass/fail), 2 = a file failed to decode.

### `inject_input.sh`

Shell helper for `XEMU_INPUT_PIPE`: create the FIFO, then send
named-key or raw-scancode `down`/`up`/`tap`/`clear` events. Works under
bash or zsh. Named keys (verified against
`macos-libs/*/opt/local/include/SDL3/SDL_scancode.h`): `W`=26 `A`=4 `S`=22
`D`=7, `UP`=82 `DOWN`=81 `LEFT`=80 `RIGHT`=79, `SPACE`=44, `KP1`=89 `KP2`=90
`KP3`=91 `KP5`=93, `LSHIFT`=225 `RSHIFT`=229 — or pass any raw scancode
integer (0-511) directly.

```
inject_input.sh create /tmp/xemu.fifo
# scratch clone per the §6 intro — never dist/xemu.app itself:
XEMU_INPUT_PIPE=/tmp/xemu.fifo "$SCRATCH/xemu-diag.app/Contents/MacOS/xemu" &
inject_input.sh tap /tmp/xemu.fifo W 200      # walk forward 200ms
inject_input.sh down /tmp/xemu.fifo DOWN      # hold a button
inject_input.sh up /tmp/xemu.fifo DOWN
inject_input.sh clear /tmp/xemu.fifo          # release everything
inject_input.sh keys                          # print the scancode table
```

Only writes to the fifo — it will block on the write if nothing has ever
opened the fifo for reading (i.e. start xemu with `XEMU_INPUT_PIPE` set
*before* running this). The full background-injection rationale (why
OS-level key injection can't reach an unfocused window) is owned by
`xemu-testing`; this script is the mechanical helper.

## When NOT to use this skill

- **Designing or running the actual A/B benchmark protocol** (interleaved
  pairs, monitor `loadvm`, boot timing, how many pairs to run) — see
  `xemu-testing`.
- **Deciding what evidence a change needs before it ships**, or what counts
  as noise vs. signal — see `xemu-validation-and-qa`.
- **Symptom-first triage** ("game frozen", "pink squares", "audio crackle")
  — start at `xemu-debugging-playbook`, which will point back here for the
  specific channel to measure with.
- **The full story of a past bug** (pink-tile saga, visibility-buffer
  crash, DSP JIT pin/PC battles) — see `xemu-failure-archaeology`; this
  skill only documents the diagnostic output format, not the incidents.
- **Looking up a config default, an XEMU_* flag's on/off semantics, or DSP
  engine selection precedence** — see `xemu-config-and-flags`.
- **Understanding *why* pass count matters on Apple GPUs, what a zpass
  report is, or DSP56300 register semantics** — domain theory lives in
  `xbox-hardware-reference`.

## Provenance and maintenance

Re-verify these before trusting them on a future date, especially after
any refactor touching the named files:

- nsprof counters/events/format: `sed -n '38,86p' hw/xbox/nv2a/nsprof.h`
  and `sed -n '25,177p' hw/xbox/nv2a/nsprof.c`
- nsprof is per-guest-flip, not per-displayed-frame:
  `grep -n nsprof_flip_tick hw/xbox/nv2a/pgraph/vk/renderer.c`
- APU utilization formula and frame period:
  `grep -n 'g_dbg.utilization\|EP_FRAME_US' hw/xbox/mcpx/apu/apu.c hw/xbox/mcpx/apu/apu_regs.h`
- PFIFO heartbeat fields: `sed -n '485,552p' hw/xbox/nv2a/pfifo.c`
- DSP JIT env vars and stats fields:
  `grep -n 'getenv("XEMU_DSP_JIT' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
  and `sed -n '10002,10080p' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- cf_fallback bucket names:
  `grep -n 'DSP56K_JIT_FB_' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h`
- DSP JIT engine identity (fork ARM64 JIT vs. upstream Rust JIT):
  `head -5 hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c hw/xbox/mcpx/apu/dsp/dsp_jit.c`
- occlusion/report rework fps figures (README prose; the receipt-backed
  38.57±0.80 primary figure is in the commit message):
  `grep -n '35.6\|376\|31.9' README.md; git show -s --format='%B' 6bfbc22863 | grep -i 'fps\|144'`
- byte-exact vertex refinement figures: `grep -n '5.4%\|93%\|7.6%' README.md`
- pipeline-cache cold/warm figures: `grep -n '164 ms\|≤ 4 ms' README.md`
- SDL scancode values: `grep -n 'SDL_SCANCODE_\(W\|A\|S\|D\|UP\|DOWN\|LEFT\|RIGHT\|SPACE\|KP_[1235]\|[LR]SHIFT\) =' macos-libs/arm64/opt/local/include/SDL3/SDL_scancode.h`
- qcow2 snapshot table layout: re-run
  `.claude/skills/xemu-diagnostics-and-tooling/scripts/list_snapshots.py`
  against a fresh `cp -c` clone of the live hdd and confirm the
  names/dates match the in-app F5-F8 shortcuts
- `nsprof_summarize.py` smoke test: regenerate a synthetic log
  matching `nsprof.c`'s format (see the format strings above) and confirm
  the parser's fps/draws-per-flip/fence-ms columns match what you encoded
- `score_frames.py` smoke test: `python3 -c "from PIL import Image, ImageDraw"`
  to confirm Pillow is still available, then run it against a synthetic
  clean image and a synthetic magenta-rectangle image and confirm CLEAN vs.
  FLAGGED/screen-rect
- `inject_input.sh` smoke test: `mkfifo` a scratch fifo, background
  a reader that opens it `O_RDONLY|O_NONBLOCK` and polls (mirroring
  `ui/xemu-input.c`'s `test_input_poll`), then run `down`/`tap`/`clear` and
  confirm the reader saw the expected `down <sc>`/`up <sc>`/`clear` lines
