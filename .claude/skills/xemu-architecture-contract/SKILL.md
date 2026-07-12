---
name: xemu-architecture-contract
description: >-
  The load-bearing design invariants of xemu-macos's NV2A Vulkan renderer,
  DSP56300 JIT, and presentation pipeline — what each invariant is, why it
  exists, and exactly what breaks if a change violates it. Load BEFORE touching
  hw/xbox/nv2a/pgraph/vk/* (surface eviction, command-buffer/flight-slot
  lifetime, occlusion queries/reports, vertex dirty-tracking),
  hw/xbox/mcpx/apu/dsp/* (engine selection, the ARM64 DSP JIT, register
  pinning), ui/xemu.c or ui/xemu-metal.m (present handoff, vblank timing,
  MVK_CONFIG setenv), or anything using QEMU_CLOCK_REALTIME/HOST, the BQL, or
  qemu_thread_jit_write/execute. Also load when reviewing a PR touching those
  files, when a change "should be safe but isn't", or when asked "does this
  violate an invariant", "is this safe to change", or why an ifdef __APPLE__
  branch exists. Keywords: queueCount=1, single-queue ordering, flight slot,
  NUM_FLIGHT_SLOTS, deferred eviction, visibility buffer, prefill,
  PREFILL_METAL_COMMAND_BUFFERS, page-granular dirty tracking, XEMU_VTX_EXACT,
  dsp_want_external_jit_engine, DSPOps, register pinning, PIN_AUDIT, W^X,
  pull-model present, nv2a_get_present_frame, BQL, pfifo_kick.
---

# xemu-macos architecture contract

This is the set of design decisions in this fork that later code silently
depends on. Each one was chosen for a stated reason, each one has a
concrete failure mode if violated, and most have an escape-hatch flag for
falling back to the safe/legacy behavior. This document exists so that a
future change — yours, or a model's — can be checked against it before
landing, not after a user reports a corrupted frame.

Audience: you are about to touch `hw/xbox/nv2a/pgraph/vk/`,
`hw/xbox/mcpx/apu/dsp/`, or `ui/xemu.c`/`ui/xemu-metal.m` and want to know
what you're allowed to assume. Every claim below was re-read from the
source on 2026-07-04; file:line pointers are given so you can re-verify
after the tree moves (see "Provenance and maintenance" at the end).

Glossary terms are defined at first use. For the full canonical glossary,
domain theory (NV2A registers, DSP56300 ISA, x87/floatx80 semantics,
Apple GPU/TBDR constraints), see `xbox-hardware-reference`. For the story
behind *how* an invariant was discovered (bisects, crash reports, measured
numbers), see `xemu-failure-archaeology`.

## Architecture at a glance

```
Xbox game (30fps target, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG, inline x87 → AArch64 FP (target/i386/tcg/,
       │  tcg/aarch64/, tcg/tcg-op-fp.c). g_use_hard_fpu picks IEEE
       │  double over floatx80 (invariant: none below governs this —
       │  see xbox-hardware-reference for the precision tradeoff).
       │
       ├─ GPU: NV2A (hw/xbox/nv2a/) — PFIFO thread pulls guest commands,
       │  PGRAPH renders via the Vulkan-on-MoltenVK backend
       │  (hw/xbox/nv2a/pgraph/vk/):
       │    command.c   — command-pool + N=2 flight slots      [INV 2]
       │    draw.c      — command-buffer recording, queries,
       │                  the visibility-buffer primer          [INV 2,3]
       │    surface.c   — render-target cache, eviction,
       │                  narrowed upload sync                  [INV 1]
       │    vertex.c    — guest dirty tracking, byte-exact
       │                  conflict refinement                   [INV 4]
       │    reports.c   — occlusion-query pool, deferred
       │                  zpass report delivery
       │    display.c / metalfx_upscale.m — MetalFX up-
       │                  scale/interpolate, present frame publish [INV 8]
       │        │
       │        └─ ui/xemu-metal.m: CAMetalLayer present, or
       │           ui/xemu.c gl_render_frame: IOSurface + CGL
       │
       └─ APU: hw/xbox/mcpx/apu/ — VP (per-voice pitch/volume) feeds
          two DSP56300 cores (GP/EP) behind a DSPOps vtable; this
          fork's ARM64 JIT lives inside the C-interpreter engine  [INV 5,6,7]
```

`NV2A` = the Xbox's GPU (NVIDIA NV2A). `PFIFO` = NV2A's command-FIFO
engine; in xemu it is a dedicated thread that pulls guest GPU commands
and feeds `PGRAPH` (the 3D engine state machine). Nearly everything in
`pgraph/vk/` runs on the PFIFO thread. `MCPX APU` = the Xbox audio
processing unit (voice processor + two DSP56300 cores).

## How to use this contract

1. Find the file you're touching in the diagram above, or grep this
   document for the function/struct you're changing.
2. Read the matching invariant's "What breaks" clause.
3. If your change doesn't touch the invariant's assumption, you're clear.
4. If it does, you have three options, worst to best: (a) don't — find a
   different approach; (b) update the invariant's guard/assert to keep
   holding under the new conditions; (c) if the invariant's premise is
   now false (e.g. MoltenVK ships multi-queue), remove the fast path
   entirely rather than leaving stale reasoning in a comment. Any of
   these is a change-control-tracked decision — see `xemu-change-control`
   for the evidence a change like this owes, and `xemu-config-and-flags`
   for how to add a new escape-hatch flag if you're keeping the old
   behavior reachable.
5. Never route around an invariant with a global that silently changes
   behavior for everyone — every existing escape hatch in this codebase
   is an explicit, documented, off-by-default `XEMU_*` env var or config
   key (see the table in `xemu-config-and-flags`).

## Invariant index

| # | Invariant |
|---|---|
| 1 | MoltenVK single-queue ordering: submission order = execution order |
| 2 | Whole-frame command buffers + N=2 flight slots: partition everything, guard everything |
| 3 | MoltenVK encoder-creation-time state: the visibility-buffer primer |
| 4 | Guest dirty tracking is page-granular; byte-exact is a refinement, never a replacement |
| 5 | DSP engine mediation: two JITs, one precedence rule |
| 6 | JIT W^X contract on Apple Silicon |
| 7 | Register-pinning write-through invariant (DSP JIT) |
| 8 | Present handoff is pull-model, off the PFIFO thread |
| 9 | Clock policy: `QEMU_CLOCK_REALTIME` is monotonic; `QEMU_CLOCK_HOST` is wall-clock — naming trap |
| 10 | BQL discipline: never block while holding it |
| 11 | Launch-path parity: `Info.plist` and `main()` must agree |
| 12 | Thread QoS: PFIFO and vblank run at `QOS_CLASS_USER_INTERACTIVE` |

---

## Invariant 1 — MoltenVK single-queue ordering: submission order = execution order

**Statement.** `MoltenVK` (the Vulkan-on-Metal translation layer this
fork's GPU renderer runs on, on macOS) exposes exactly one queue
(`queueCount=1` — `hw/xbox/nv2a/pgraph/vk/instance.c:515`, with a comment
at lines 504-509 noting a second compute queue was tried and abandoned
for this reason; also README "Failed experiments" row "Separate compute
queue"). Two Apple-only fast paths are sound *only* because of this.

**Evidence — two call sites lean on single-queue ordering:**

1. **Narrowed `pgraph_vk_upload_surface_data` sync**
   (`hw/xbox/nv2a/pgraph/vk/surface.c:1263-1318`). On Apple, it skips the
   full `pgraph_vk_finish` (submit + fence wait) when the upload target
   isn't the bound color/zeta `surface` (NV2A render target — color or
   depth/stencil — bound to a VRAM range) and no command buffer is
   recording:
   ```c
   #ifdef __APPLE__
       bool target_is_active = (surface == r->color_binding) ||
           (surface == r->zeta_binding) || r->in_command_buffer;
   #else
       bool target_is_active = true;   /* non-Apple: always finish */
   #endif
   ```
   The comment at lines 1275-1303 spells out why this is Apple-only:
   MoltenVK's single queue serializes the aux-CB upload behind any prior
   submitted main CB, but "Core Vulkan only orders same-queue submissions
   at execution *start*; a native driver may overlap an in-flight slot's
   reads/writes of this surface's image with the aux-CB upload (visible
   under WHPX)."

2. **Deferred zeta eviction** (zeta = depth/stencil surface).
   `invalidate_surface_full(d, surface, allow_deferred=true)`
   (`surface.c:784-826`), used from the zeta shape-switch path
   (`surface.c:1941`, e.g. Azurik ping-ponging one zeta allocation
   between a 1280×480 scene and a 256×256 swizzled RTT pass every
   frame), can quarantine a still-possibly-referenced image into
   `r->invalid_surfaces` without a submit+fence round trip. The helper
   `surface_image_in_recording_cb()` (`surface.c:769-782`) keeps
   `get_any_compatible_invalid_surface`/`prune_invalid_surfaces`
   (`surface.c:1169-1207`) from handing back or destroying an image the
   *recording* (unsubmitted) CB might still touch. Once a CB is
   *submitted*, no further check is needed — comment at
   `surface.c:774-775`: "Images referenced only by submitted work are
   safe to reuse — MoltenVK's single queue executes later submissions
   after earlier ones."

3. **Invalid-surface DESTRUCTION is gated on submission retirement
   (2026-07-04, `a045dfc780`) — and this one is deliberately
   platform-independent.** `pgraph_vk_finish` pipelines: it submits
   the current CB and waits only the previous slot's fence, so the
   just-submitted CB routinely still executes while the next records.
   "Not referenced by the recording CB" therefore never proves the
   GPU is done with an image. Every eviction stamps
   `surface->evict_submit_seq` (highest submission index that may
   reference the image); `r->retired_submit_count` is a watermark
   updated wherever a slot fence is observed signaled
   (`pgraph_vk_wait_slot_fence`, `pgraph_vk_wait_for_previous_flight`
   — the reports fence-poll routes through the former);
   `prune_invalid_surfaces` destroys only entries at or below the
   watermark, and `pgraph_vk_surface_flush` drains all slots before
   its keep=0 prune. REUSE (migrate) needs no watermark: a migrated
   image keeps its attachment layout and its first GPU touch is
   ordered against all earlier same-queue submissions by the draw
   render pass's explicit `VK_SUBPASS_EXTERNAL` dependency — a core
   Vulkan guarantee, not a MoltenVK one (proof comment at
   `get_any_compatible_invalid_surface`). **If you add a new
   destruction path for GPU resources with pipelined lifetimes, it
   needs the same watermark; if you add a new fence-observation
   site, it must update the watermark.**

**Why.** Both fast paths remove a submit+fence stall (one of the
largest nsprof cost buckets in heavy scenes) by trusting MoltenVK not
to reorder or run submissions concurrently — trust extended only on
`__APPLE__`. The destruction watermark is the flip side: the one
lifetime question where single-queue ordering does NOT save you on
any platform, because `vkDestroyImage` is a CPU-side act with no
place in the queue's ordering at all.

**What breaks if violated.** A second queue, or any change that submits
from two threads/queues, turns both into use-after-free-class hazards:
the narrowed sync could let a draw read a surface mid-overwrite
(flicker/corruption); deferred eviction could let a new allocation reuse
an image a still-executing submission is touching (corruption or a
GPU-side crash). **Do not remove either `#else` branch** — they are the
Windows/Linux correctness fallback (`xemu-change-control`'s "never
regress Windows/upstream paths" rule).

**Escape hatch.** None — the gate is compile-time (`#ifdef __APPLE__`);
the assumption is about the driver, not a preference.

---

## Invariant 2 — Whole-frame command buffers + N=2 flight slots: partition everything, guard everything

**Statement.** The renderer records one Vulkan command buffer per guest
frame (a "whole-frame command buffer"), double-buffered across
`NUM_FLIGHT_SLOTS = 2` ("flight slots" — CPU records slot *k+1* while the
GPU executes slot *k*). Every per-frame resource pool (descriptor sets,
staging buffers, the occlusion-query pool) is statically partitioned N
ways, one partition per slot. **Any new per-something allocation whose
cadence can change at runtime needs a capacity guard that degrades
gracefully (submit-and-continue), not just an assert** — because the
asserts in this file are compiled out in every build (see below).

**Evidence.**

- `#define NUM_FLIGHT_SLOTS 2` — `hw/xbox/nv2a/pgraph/vk/renderer.h:542`.
- Partitioning: `pgraph_vk_init_flight_partitions()`
  (`hw/xbox/nv2a/pgraph/vk/command.c:242-296`) divides descriptor sets,
  UBO/compute descriptor sets, and the index/vertex/uniform staging
  buffers into `NUM_FLIGHT_SLOTS` equal ranges.
- The occlusion-query pool is 4096 queries total
  (`hw/xbox/nv2a/pgraph/vk/reports.c:36`, overridable for guard-testing
  via `XEMU_MAX_QUERIES`, `reports.c:38-44`, range 8-65536), split evenly
  per slot via `pgraph_vk_queries_per_slot()` (`renderer.h:1075-1078`) —
  2048/slot by default.
- **The capacity guard that matters**, `begin_draw()`
  (`hw/xbox/nv2a/pgraph/vk/draw.c:2178-2193`): when zpass counting is on
  and `num_queries_in_flight >= pgraph_vk_queries_per_slot(r) - 1`, it
  forces `pgraph_vk_finish(pg, VK_FINISH_REASON_REPORTS_FULL)` +
  `pgraph_vk_ensure_command_buffer(pg)` — submit-and-continue — rather
  than let the next query index overrun the slot's partition. The
  in-code comment states this was "previously only a release-stripped
  assert" that "overran the pool and crashed inside MoltenVK's
  visibility-buffer encode."
- **Why an assert alone isn't enough**: every `nv2a_vk_assert()` in this
  renderer compiles to `((void)0)` — `hw/xbox/nv2a/pgraph/vk/debug.h:66-72`
  hardcodes `#define NV2A_VK_PERF_BUILD 1` unconditionally, with no
  `NDEBUG`/`--debug`/meson-option override anywhere in the tree (verified
  by grep). **These asserts are stripped in every build of this renderer,
  including `--debug` builds** — stricter than "release strips asserts."
  Only plain `assert()` (guest-boundary/DMA-bounds checks) follows normal
  NDEBUG rules.

**Why this design.** Deferred zpass-report delivery (occlusion results
now ride the flip and get harvested at slot reclaim instead of being
drained synchronously — see `reports.c`) let one command buffer span a
whole frame instead of tearing down per query rotation — the single
biggest win in this repo's history: 15.78 → 38.57±0.80 fps (+144%) per
commit `6bfbc22863`'s protocol receipt (README prose rounds this change
to 35.6 fps / 2.25x), passes 376 → 14/flip, 2026-07-03. But it also
means a partition that used to refill once
per short-lived CB can now be exhausted mid-frame in a report-heavy
scene; the guard exists to catch exactly that.

**What breaks if you violate this.** Any change that (a) increases the
per-rotation or per-draw consumption rate of a partitioned resource, or
(b) adds a new partitioned-by-slot resource, without an equivalent
runtime guard, will silently work in light scenes and overrun the
partition — writing into the *next* slot's region, or past the pool
entirely — in heavy scenes. Because `nv2a_vk_assert` is always compiled
out here, this manifests as silent corruption or a crash inside the
driver (this exact failure mode crashed inside MoltenVK's
visibility-buffer encode before the guard was added — see the
Visibility-buffer crash saga in `xemu-failure-archaeology`), not as a
clean assertion failure during development.

**Escape hatch / test knob.** `XEMU_MAX_QUERIES=<8..65536>` shrinks the
pool artificially so you can force the guard to fire and confirm it
holds (validated historically by forcing `MAX_QUERIES=64` and observing
the guard fire ~3312×/interval with no crash).

---

## Invariant 3 — MoltenVK encoder-creation-time state: the visibility-buffer primer

**Statement.** MoltenVK attaches Metal's visibility-result buffer to a
render encoder **only if the visibility flag was already set before the
encoder is created** — and only `vkCmdBeginQuery` sets that flag. Every
whole-frame command buffer in this renderer begins with an empty
"primer" occlusion query, issued outside any render pass, purely to set
that flag before the first pass's encoder exists.

**Evidence** — `pgraph_vk_begin_command_buffer()`,
`hw/xbox/nv2a/pgraph/vk/draw.c:2046-2070`. Its comment states the
mechanism precisely: MoltenVK attaches the Metal visibility-result
buffer to a render encoder "only when
`MVKCommandBuffer::_needsVisibilityResultMTLBuffer` is already set at
pass-begin, and only `vkCmdBeginQuery` sets it — under immediate
prefill encoding (`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`) our
in-pass query begins come after the first pass's encoder exists, so a
frame whose FIRST pass contains the first zpass draw got an encoder with
a nil visibility buffer and crashed in AGX `setVisibilityResultMode`
(user-reported, real gameplay)." The fix, right after that comment:
```c
int primer_index = pgraph_vk_slot_query_base(r, r->current_flight) +
                    r->num_queries_in_flight;
vkCmdBeginQuery(r->command_buffer, r->query_pool, primer_index, 0);
vkCmdEndQuery(r->command_buffer, r->query_pool, primer_index);
r->num_queries_in_flight++;
```
issued outside any render pass, unconditionally for every command
buffer regardless of the current prefill setting — "correct by
construction" rather than relying on the config value being right.
(`zpass_pixel_count_enable` = guest has enabled an occlusion/"zpass"
query; a report write asks "how many pixels passed the depth test.")

**Why this design.** `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` controls
*when* MoltenVK translates a Vulkan command buffer's recorded commands
into a Metal encoder: immediately as each Vulkan call is issued
(prefill=2, "immediate encoding") vs. lazily when the command buffer is
submitted (prefill=0). Under immediate encoding, the first render pass's
Metal encoder can be created *before* the first in-pass
`vkCmdBeginQuery` runs (because occlusion queries in this renderer begin
inside the pass, per-draw-rotation) — so the encoder is created without
the visibility buffer attached, and the first zpass draw's
`setVisibilityResultMode` call crashes in Apple's AGX driver. The primer
forces the flag to be set at the very start of the command buffer,
before any pass exists, closing that race regardless of prefill timing.

**What breaks if you violate this.** Two independent ways to break this:
1. **Removing or reordering the primer** relative to
   `vkCmdResetQueryPool`/pass creation reopens the exact crash above —
   it is silent in scenes that begin with a clear (bench harnesses tend
   to) and only crashes in scenes whose *first* pass contains the first
   zpass draw, which is why this bug survived soak testing and only
   surfaced in real user gameplay (see `xemu-failure-archaeology`'s
   "Visibility-buffer crash, two acts" saga).
2. **Flipping `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` back to a
   nonzero value.** This is independently forbidden — see Invariant 11
   — but note that the primer defense and the prefill=0 policy are
   *layered*, not redundant: the primer fixes the visibility-buffer
   crash class regardless of prefill; prefill=0 additionally avoids
   texture corruption and a measured slowdown (46.33 vs 48.50 fps,
   dated session measurement) that have nothing to do with visibility
   buffers. Keep both.

**Escape hatch.** None — this is "correct by construction," not a
tunable. If you're adding a *new* code path that creates a render-pass
encoder, make sure it runs after the primer (i.e., after
`pgraph_vk_begin_command_buffer`), or repeat the pattern.

---

## Invariant 4 — Guest dirty tracking is page-granular; byte-exact is a refinement, never a replacement

**Statement.** The guest marks written memory dirty at page granularity
(`TARGET_PAGE_SIZE`). Vertex-stream conflict detection between the
recording command buffer and incoming guest writes is refined to
byte-exact using a per-page written-span table, but the refinement is
strictly additive: any page-granular conflict where the refinement can't
prove the bytes are identical still forces the conservative fallback (a
full `pgraph_vk_finish`).

**Evidence** — `hw/xbox/nv2a/pgraph/vk/vertex.c`. Per-flight-slot span
tracking fields `page_span_min`/`page_span_max`
(`hw/xbox/nv2a/pgraph/vk/renderer.h:589-590`), allocated per slot in
`hw/xbox/nv2a/pgraph/vk/buffer.c:128-130`. The refinement function,
`uploaded_span_content_differs()` (`vertex.c:111-149`), is gated by
`XEMU_VTX_EXACT` (`vertex.c:116-123`, `exact = !(e && e[0] == '0')`) —
`XEMU_VTX_EXACT=0` returns `true` unconditionally, i.e. restores the old
"every page-granular conflict finishes" behavior; default is
refinement-on. The refinement only *skips* the finish when `memcmp`
proves the conflicting bytes are byte-identical to what's already
uploaded (`vertex.c:143-146`); any actual difference falls through to
the same `pgraph_vk_finish(pg, VK_FINISH_REASON_VERTEX_BUFFER_DIRTY)`
the legacy path always took (`vertex.c:201`).

**Why this design.** Guest vertex-stream writes arrive padded to page
boundaries; two logically-independent writes that happen to share a
page boundary "false-share" it — the padded bytes are identical content
being re-copied, not a real conflict. Page-granular tracking alone can't
tell the difference and forces an unnecessary finish (measured: 4-5.6
forced finishes per flip down to ~0.9 in the Azurik attract demo).
Byte-exact refinement narrows this to genuine conflicts by comparing
only the actually-written sub-page span (`page_span_min`/`max`) against
a live mirror buffer.

**What breaks if you violate this.** If a change makes the refinement
*trust* a page as clean without the byte comparison actually running
(e.g., a fast-path that skips `uploaded_span_content_differs` under some
new condition), a real conflict — the recording command buffer's draws
reading stale/overwritten vertex data — would silently render wrong
geometry, and because this is a data race made deterministic-looking by
timing, it would reproduce inconsistently and be very hard to bisect.
The safe direction to err is always "finish more than necessary," never
"skip a finish the byte comparison didn't explicitly clear."

**Escape hatch.** `XEMU_VTX_EXACT=0` disables the refinement entirely
(legacy page-granular-always-finishes behavior) — use this to check
whether a suspected vertex-corruption bug is in the refinement logic
itself.

---

## Invariant 5 — DSP engine mediation: two JITs, one precedence rule

**Statement.** There are two independent JIT implementations for the
Xbox's DSP56300 audio cores (`GP`/`EP` — Global and Effects Processors,
two DSP56300-family cores in the MCPX APU), reached through a common
`DSPOps` vtable, and exactly one precedence rule decides which runs.

**Evidence.** The vtable and state split — `hw/xbox/mcpx/apu/dsp/dsp.h:36-108`:
`DSPOps` is a vtable of function pointers (`step`, `run`,
`sync_to_vm`/`sync_from_vm`, ...); `DSPState` holds `const DSPOps *ops`,
`DspCoreState core`, and `void *backend`. **`dsp->core` is explicitly a
save/restore snapshot**, kept in sync via `dsp_sync_to_vm`/
`dsp_sync_from_vm` — called from `hw/xbox/mcpx/apu/apu.c:384-411` on VM
stop/start (snapshot save/load, pause) — not the live execution state.
`dsp->backend` is the live state, engine-specific: for the C-interpreter
engine it is a `dsp_core_t*` (`hw/xbox/mcpx/apu/dsp/dsp_c.c:32-34`,
`c_core()`).

**The fork's ARM64 JIT is embedded inside the C-interpreter engine, not
a third engine.** `hw/xbox/mcpx/apu/dsp/dsp_c.c:94-105` calls
`dsp56k_jit_execute_block(core)` directly from the interpreter's step
function when `dsp56k_jit_enabled()`. Its translation unit is
`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`; symbols are namespaced
`dsp56k_jit_*` (not `dsp_jit_*` — the *other*, upstream-inherited engine
below). It compiles in only on Apple Silicon:
`#define DSP56K_JIT_SUPPORTED (defined(__APPLE__) && defined(__aarch64__))`
(`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h:16-20`).

**The precedence rule** — `dsp_want_external_jit_engine()`,
`hw/xbox/mcpx/apu/dsp/dsp.c:113-128`:
```c
static bool dsp_want_external_jit_engine(void)
{
#if DSP56K_JIT_SUPPORTED
    if (g_config.audio.dsp_jit.enabled) {
        return false;
    }
#endif
    return g_config.audio.use_dsp_jit;
}
```
Re-enforced on a live engine switch too: `dsp_set_engine()`
(`dsp.c:244-269`) silently forces the `use_jit` parameter (backing
`audio.use_dsp_jit`) to `false` when `audio.dsp_jit.enabled` is set —
**the UI's external-JIT toggle becomes a no-op while the fork engine is
enabled**, worth knowing before assuming a config change took effect.

So: `DSP56K_JIT_SUPPORTED && audio.dsp_jit.enabled` → fork's inline
ARM64 JIT (inside the interpreter engine, `c_dsp_ops`, `dsp_c.c:328`).
Otherwise `audio.use_dsp_jit` selects the upstream-inherited
`jit_dsp_ops` engine (`hw/xbox/mcpx/apu/dsp/dsp_jit.c:346`) vs. the
plain interpreter with no inline JIT.

**The mixbuffer handoff is engine-aware.** Per-frame VP (voice processor
— per-voice pitch/volume pipeline) output is bulk-copied into the GP
core's mixbuffer via `dsp_write_mixbuffer_bulk()`
(`hw/xbox/mcpx/apu/dsp/dsp.c:197-207`), called from
`mcpx_apu_dsp_frame()` (`hw/xbox/mcpx/apu/dsp/gp_ep.c:450-463`). It only
takes the fast bulk-memcpy path when `dsp->ops == &c_dsp_ops` (the
interpreter engine, JIT or not); the upstream JIT engine falls back to
per-sample `dsp_write_memory` calls (`gp_ep.c:464-473`).

**Why this design.** This fork's JIT had to land *inside* the existing
interpreter engine (not as a new top-level `DSPOps`) so that a July 2026
upstream merge bringing in a new `dsp56300`-subproject JIT engine
(`audio.use_dsp_jit`, `dsp_jit.c`) wouldn't collide symbol names or
config keys with the fork's own JIT. The `dsp56k_jit_*` vs `dsp_jit_*`
naming split and the "fork JIT wins when both are available" precedence
are both artifacts of that merge-mediation decision.

**What breaks if you violate this.** If a change makes `audio.use_dsp_jit`
bypass the `dsp_want_external_jit_engine()` gate (e.g. a new call site
that reads the config key directly), the fork's ARM64 JIT could end up
coexisting with — or getting silently overridden by — the upstream
engine in a way neither engine expects, and DSP JIT diagnostics
(`XEMU_DSP_JIT_STATS`, `PIN_AUDIT`, the diff validator) would report
against the wrong engine. If a future upstream merge introduces new
`DSPOps` fields without updating `dsp_c.c`'s `c_dsp_ops` table, the
build breaks loudly (missing initializer) — that's the easy case. The
harder case is reusing the `dsp_jit_*` (upstream) or `dsp56k_jit_*`
(fork) prefix for the wrong engine's symbols during a merge — see
`xemu-change-control` for the upstream-merge mediation checklist.

**Escape hatch.** `audio.dsp_jit.enabled` (config, default **false** —
this fork's JIT) and `audio.use_dsp_jit` (config, default **true** —
upstream engine, but only consulted when the fork JIT isn't
active/supported) are both persistent config keys, not env vars;
`XEMU_DSP_JIT` also exists as a runtime override — see
`xemu-config-and-flags` for the full precedence table including the
diagnostic knobs (`XEMU_DSP_JIT_STATS`, `_DIFF`, `_PIN_AUDIT`, etc).

---

## Invariant 6 — JIT W^X contract on Apple Silicon

**Statement.** Apple Silicon enforces W^X (a page is never both
writable and executable at once) at the OS level for JIT memory. Every
write into the DSP JIT's generated-code buffer must happen inside a
`qemu_thread_jit_write()`/`qemu_thread_jit_execute()` window — these are
QEMU's standard per-thread W^X toggle primitives (wrapping
`pthread_jit_write_protect_np` on Darwin). This fork does not use the
scoped-callback W^X API, and does not cache the W^X state in
thread-local storage — both were tried and reverted.

**Evidence.** Toggle sites in
`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`: `qemu_thread_jit_write()`
before emitting code (e.g. lines 9479, 9510), `qemu_thread_jit_execute()`
after (e.g. lines 9587, 9668, 9685, 9881, 10179), always paired within
the same function. The chain-patching comment at lines 9850-9856 makes
the ordering requirement explicit: a cross-block repatch "must happen
AFTER `block` has its generation/shared_exit set... and BEFORE
`qemu_thread_jit_execute()` — `chain_repatch_b` writes into the code
buffer." I.e. any function that patches a previously-emitted branch must
do so *before* the write-window closes, not after.

**Why this design — two failed alternatives, both in the README's
Failed/reverted-experiments table:**
- *`pthread_jit_write_with_callback_np` for `tb_phys_invalidate`*:
  "Scoped-W semantics force-drop W on callback return, incompatible
  with QEMU's nested JIT-write paths." The DSP JIT's write sites are not
  cleanly nestable inside a single callback scope (translation can
  recursively invalidate/re-translate), so the scoped API doesn't fit.
- *TLS-cached `pthread_jit_write_protect_np`*: "Per-thread cache drifts
  from kernel W^X on nested `tb_gen_code` / setjmp paths → SIGSEGV/SIGILL
  after ~1 min." Caching "am I currently in write mode" in
  thread-local storage went stale under QEMU's own nested
  translation-block-generation and `setjmp`/`longjmp` control flow,
  because the kernel's actual per-thread W^X state could change
  underneath the cached value.

**What breaks if you violate this.** Writing to the code buffer outside
a `qemu_thread_jit_write()` window either faults immediately (the page
is genuinely not writable — a debuggable SIGSEGV/SIGBUS) or, worse, if
you reintroduce a TLS-cache-style optimization, drifts from the real
kernel state and fails intermittently, often minutes into a session,
which is far harder to bisect than an immediate crash. Executing from a
buffer without having called `qemu_thread_jit_execute()` after your last
write can execute stale or partially-written instructions (silent wrong
behavior, or SIGILL on a torn instruction).

**Escape hatch.** None — this is a hard OS-level contract, not a
preference. If you need to add a new JIT-code-mutation path (e.g. a new
kind of chain patch), follow the existing pattern: write-window open →
all buffer writes (including any repatches of *other* blocks) → i-cache
flush (`jit_clear_icache`) → write-window close
(`qemu_thread_jit_execute()`).

---

## Invariant 7 — Register-pinning write-through invariant (DSP JIT)

**Statement.** The DSP JIT pins the DSP's A/B accumulators, X/Y data
registers, and the low 16 bits of the status register (SR) into
callee-saved ARM64 registers for the duration of a translated block
(x26/x27 for A/B, x20/x21 for packed X/Y, x28 for SR —
`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c:1448-1452`). The backing
memory (`dsp->registers[]`) remains the authoritative copy at all times
— every pinned write is write-through (updates both the register and
memory) — and every pin is reloaded from memory immediately after any
`BLR` fallback call that could have mutated that register's state.

**Evidence** — stated directly in the SR-pinning comment block,
`dsp56k_jit_arm64.c:1462-1480`: "The pin's invariant is `w28 ==
(registers[SR] & 0xFFFF)`. Memory stays authoritative (write-through);
BLR-fallback C handlers that write `dsp->registers[SR]` don't see the
pin, so we reload after every BLR that might touch SR." (`emit_store_sr_h`
writes both `STRH` to memory and `UBFX` into the pin register in the
same emit; `emit_reload_sr_pin` is the memory→pin reload, called at
prologue and after BLR fallbacks.) The A/B pin comment at line 2229
states the identical rule. Reload call sites
(`emit_reload_ab_pins`/`emit_reload_xy_pins`/`emit_reload_sr_pin`) appear
after BLR-fallback sites throughout the file — 18 call sites across the
three functions as of 2026-07-04 (`grep -c` on the three names returns
24; the extra 6 are their 3 definitions, 2 forward declarations, and 1
comment mention).

**Why this design.** Pinning avoids a load-store round trip to memory on
every register access inside a hot translated block (measured
motivation: eliminating a 7-instruction load chain / 6-instruction store
chain per accumulator access). But not every DSP operation is inlined
into ARM64 — rare paths (REP/DO hardware-loop state changes, certain
parallel-move edge cases) fall back to calling a C helper (`BLR`), and
that C helper reads/writes `dsp->registers[]` directly, with no idea a
pinned copy exists in a caller's register. Write-through plus
reload-after-BLR is what keeps the two copies from silently diverging.

**What breaks if you violate this.** If a new pinned register is added
without also write-through-updating memory, any BLR-fallback C helper
that reads that register sees stale data — silently wrong DSP execution
(audio corruption) with no crash to signal it, and the bug would only
appear in the rare fallback path, making it very hard to catch outside
of adversarial testing. If a reload is missing after a *new* BLR site
that can mutate a pinned register, the JIT's subsequent inlined reads of
that register see stale (pre-fallback) data — same silent-corruption
failure mode. `PIN_AUDIT` (below) exists precisely because this class of
bug is otherwise invisible.

**Escape hatch / checker.** `XEMU_DSP_JIT_PIN_AUDIT=1`
(`dsp56k_jit_arm64.c:258-266`) emits a per-op runtime check
(`emit_pin_audit_check`, lines 2481-2547) that compares the pinned
register's value against memory and calls
`dsp56k_jit_helper_pin_audit_fail` on mismatch. **Run this after adding
or moving any pin-touching code.** This exact checker caught a real pin
leak within 30 seconds during development (a direct-A1-slot pin write
that bypassed write-through) — see `xemu-failure-archaeology` for the
incident.

---

## Invariant 8 — Present handoff is pull-model, off the PFIFO thread

**Statement.** Pull is the default and the universal fallback: every UI
frame, `nv2a_get_present_frame()` performs one synchronous round trip to
the PFIFO thread (NV2A's command-processing thread — see the glossary
note in the architecture diagram) and blocks until PFIFO answers. Since
2026-07-11 there is a default-OFF push-model alternative
(`XEMU_PUSH_PRESENT=1`, Metal backend only): at flip PFIFO publishes this
flip's present *schedule* into a leaf-mutex-guarded **ring** of
`{texture/IOSurface, shared event + value, frame_seq, hold, generation}`
entries — one entry with interpolation off, or the paced sub-flip steps
(the interpolated midpoint step(s) then the real frame) with
interpolation on — and the UI consumes one step per frame with no kick
and no wait, in `frame_seq` order, pacing the steps GPU-side via
`presentDrawable:afterMinimumDuration:`. Consumption is a monotonic
cursor (a consumed step is never resurrected; when the UI falls behind
the ring drops its oldest entries, the same content the pull path drops
when a new flip lands before the UI reads). Startup-before-first-publish
and the GL backend fall back to pull. The GPU-side ordering contract is
unchanged in every model: the UI encodes its wait on the frame's
`MTLSharedEvent` value before sampling, and interpolated outputs the GPU
hasn't finished are gated by that same shared-event value.

**Evidence.** `nv2a_get_present_frame()`
(`hw/xbox/nv2a/pgraph/pgraph.c:443-460`) takes `pg->renderer_lock`,
asserts no other framebuffer consumer is active
(`pg->framebuffer_in_use`), and dispatches to the renderer's
`ops.get_present_frame`. The Vulkan implementation,
`pgraph_vk_get_present_frame()` (`hw/xbox/nv2a/pgraph/vk/renderer.c:283-323`),
does the actual round trip under `d->pfifo.lock`:
`qemu_event_reset` the completion event, `qatomic_set(&pg->sync_pending,
true)`, `pfifo_kick(d)`, unlock, then `qemu_event_wait(&d->pgraph.sync_complete)`
— blocking until the PFIFO thread services the request and fills in
`frame->iosurface`/`mtl_texture`/`event`. The caller,
`metal_render_frame()` (`ui/xemu.c:991-1008`), documents why the pull
happens first: "the frame's shared-event value must be known before the
drawable render pass is opened (the GPU-side wait is encoded ahead of
it)."

The producer side is the **vblank thread**, a fixed-cadence,
monotonic-deadline-scheduled thread wholly decoupled from host present
timing: `vblank_timer_thread()` (`ui/xemu.c:848-880`) computes an
absolute `next_vblank` deadline, advances it by a fixed
`vblank_interval_ns` (`= 16666666LL`, i.e. 60 Hz — `ui/xemu.c:78`) each
iteration regardless of how long the previous iteration took, and calls
`SDL_DelayPrecise` to the deadline. It runs at
`QOS_CLASS_USER_INTERACTIVE` (Invariant 12).

**Why this design.** In the pull model the round trip is also what
*publishes* a new present frame (the PFIFO-side sync handshake is shared
machinery, not present-specific), so pull cannot do a UI-side
"skip presenting if nothing changed" pre-check without first paying the
round trip. The push ring exists precisely to break that coupling —
publish at flip, read without blocking — and its equivalence to pull is
refuter-proven (`XEMU_PUSH_PRESENT_REFUTE=1`: the consumed `frame_seq`
stream must be monotonic with no skipped-then-resurrected step, and
equal-`frame_seq` pushed-vs-pull reads must describe identical content;
47k+ comparisons interp-off, 0 mismatches on record; interp-on adds the
schedule-stream check). Push deliberately uses a leaf mutex, not a
seqlock: the ring hands out reference-counted handles, and a lock-free
reader could CFRetain a pointer the writer concurrently CFReleases; each
entry owns a CFRetain until it is overwritten or invalidated, and a
generation counter (bumped on resize / teardown / hitch-guard reset)
fences off a stale schedule. Because the ring owns each flip's whole
schedule, `pgraph_vk_present_schedule_publish` leaves `present_*` /
`present_frame_seq` mirroring the final published step and clears the
interp stepping state, so the pull path stays self-consistent (and
non-perturbing) even while push is active.

Vblank cadence is intentionally NOT tied to host display refresh: an
earlier attempt to align `vblank_interval_ns` to the host's actual
refresh rate is in the README's Failed/reverted-experiments table
("Vblank cadence aligned to host display refresh") because
`vblank_timer_callback`/`vblank_timer_thread` → `process_vblank` →
`graphic_hw_update` → `nv2a_vga_gfx_update` → sets
`NV_PCRTC_INTR_0_VBLANK` (`hw/xbox/nv2a/nv2a.c:196-206`) — a *guest*
interrupt. Titles gate their own simulation on vblank count; retuning
the host-side timer retimes the guest's clock, not just presentation
smoothness.

**What breaks if you violate this.** Coupling vblank cadence to host
display refresh (again) would retime guest IRQs — this is a settled,
do-not-retry experiment (see `xemu-failure-archaeology`). Removing the
`renderer_lock`/`framebuffer_in_use` assert pairing, or calling
`nv2a_get_present_frame` from two threads concurrently, reopens the
exact hazard the assert exists to catch. Blocking longer than expected
inside the PFIFO-side handshake (e.g. adding synchronous work to the
`sync_pending` handler) directly adds latency to every UI frame, since
the UI thread is synchronously waiting on it.

**Escape hatch.** The pull model IS the default and the hatch:
`XEMU_PUSH_PRESENT=1` opts into the push ring (Metal, both interpolation
on and off), and unsetting it — or any fallback condition — restores
pull wholesale. `XEMU_PUSH_PRESENT_REFUTE=1` is the equivalence checker.
`vblank_interval_ns` and `use_vblank_timer_thread` are file-scope
globals in `ui/xemu.c` (not currently exposed as config/env), used for
PAL-mode consideration — see Known weak points.

---

## Invariant 9 — Clock policy: `QEMU_CLOCK_REALTIME` is monotonic; `QEMU_CLOCK_HOST` is wall-clock — naming trap

**Statement.** Despite the name, `QEMU_CLOCK_REALTIME` in this codebase
resolves to `CLOCK_MONOTONIC`, not wall-clock time. `QEMU_CLOCK_HOST` is
the one that reflects `gettimeofday`/wall-clock (NTP-adjustable,
jumpable). Every present-path and throttle-timing use in this fork uses
`QEMU_CLOCK_REALTIME`; **never gate rendering or frame-pacing on
`QEMU_CLOCK_HOST`.**

**Evidence.** Dispatch in `util/qemu-timer.c:639-648`:
```c
case QEMU_CLOCK_REALTIME:
    return get_clock();
...
case QEMU_CLOCK_HOST:
    return REPLAY_CLOCK(REPLAY_CLOCK_HOST, get_clock_realtime());
```
`get_clock()` (`include/qemu/timer.h`) branches on `use_rt_clock`, and
`util/qemu-timer-common.c:51-62` sets `use_rt_clock = 1` at process-init
time whenever `clock_gettime(CLOCK_MONOTONIC, &ts) == 0` — true on every
POSIX host this fork targets (macOS, Linux). So `QEMU_CLOCK_REALTIME` →
`clock_gettime(CLOCK_MONOTONIC)`. `get_clock_realtime()`
(`include/qemu/timer.h:809-816`) is a direct `gettimeofday` wrapper,
used only by `QEMU_CLOCK_HOST`.

Consumers, all using `QEMU_CLOCK_REALTIME` (i.e. monotonic) deliberately:
- `update_fps()` / vblank pacing — `ui/xemu.c:811, 843, 856, 863`.
- Surface-expiry scan throttle — `hw/xbox/nv2a/pgraph/vk/surface.c:2115-2121`
  (`surface_expire_interval_ns = 33 * 1000 * 1000`, ~33 ms, "~2 frames at
  60 Hz"), explicitly commented as host-wall-clock-*style* throttling
  but implemented on the monotonic clock — see next paragraph for why
  that distinction still matters.
- PFIFO heartbeat diagnostic — `hw/xbox/nv2a/pfifo.c:521-522`, with an
  explicit inline comment: `/* Monotonic; QEMU_CLOCK_HOST is wall clock
  (NTP-jumpable) */`.
- Reports idle-budget timer — `hw/xbox/nv2a/pgraph/vk/reports.c:301-307`
  (5 ms budget before a deferred-report safety-valve submit).

**Why this design.** A wall-clock (`QEMU_CLOCK_HOST`) source can jump
backward or forward (NTP sync, manual clock change, DST), which would
make an elapsed-time throttle either stall (clock jumped back — appears
no time has passed) or fire spuriously (clock jumped forward). Anything
gating rendering, frame pacing, or a cache-eviction scan must use a
clock that only moves forward at a steady rate — `QEMU_CLOCK_REALTIME`
(really `CLOCK_MONOTONIC`) is that clock here, confusingly named.

**What breaks if you violate this.** Using `QEMU_CLOCK_HOST` for a
frame-pacing or eviction-throttle deadline would make that logic
vulnerable to system clock changes — an eviction scan could stop running
for the rest of a session (clock stepped backward) letting invalid
surfaces accumulate unbounded (this exact failure mode, with a
different trigger — gating on `pg->frame_time` instead of wall-clock at
all — caused a reproducible black-screen freeze on rapid double-level-
load, fixed by switching to host wall-clock-*style but monotonic*
timing; see the "Surface-expiry throttle keyed on `pg->frame_time`" row
in the README's Failed experiments table). The fix keeps the "throttle
regardless of frame_time" property while using the *monotonic* clock,
not `QEMU_CLOCK_HOST`.

**Escape hatch.** None — this is a correctness rule about clock choice,
not a tunable.

---

## Invariant 10 — BQL discipline: never block while holding it

**Statement.** QEMU's Big (QEMU) Lock — `BQL`, the single global lock
protecting most device-emulation state — must never be held across a
blocking wait. Two concrete contracts in this fork depend on that:
CPU-thread surface access drops BQL before taking the renderer lock, and
the PFIFO kick/wake handshake requires every kick site to hold
`d->pfifo.lock` across the kick-flag-set *and* the condvar broadcast.

**Evidence.**

- `surface_access_callback()` (`hw/xbox/nv2a/pgraph/vk/surface.c:656-666`)
  — invoked when the guest CPU touches VRAM backing a live render
  target — explicitly calls `bql_unlock()` (if held) before taking
  `d->pgraph.lock`, which can itself block on renderer work; restored
  afterward (read the full function before assuming symmetry holds if
  you edit it).
- `pfifo_kick()` (`hw/xbox/nv2a/pfifo.c:97-112`) documents its own
  locking precondition inline: "Callers must hold `d->pfifo.lock` across
  the kick-set + broadcast pair. All 15 call sites in the tree satisfy
  this... With the invariant held we can use an untimed
  `qemu_cond_wait` in the `pfifo_thread` idle path... because the
  writer's broadcast can no longer slip between the reader's kick-check
  and its atomic release-wait." (Count drift, re-derived 2026-07-11 at
  `7e2e6e7256`: there are now **20** `pfifo_kick(d)` call sites — 15
  core/VK + 5 GL, `grep -rn 'pfifo_kick(d)' hw/ | wc -l`. The comment's
  "15" enumeration is stale in the code — a code-pass item. The VK-side
  sites were re-checked to hold `d->pfifo.lock` across the kick; the
  precondition, not the count, is the load-bearing invariant — re-audit
  any new site against it.) The PFIFO thread's idle wait
  (`pfifo.c:554-565`) relies on exactly this: it checks `fifo_kick` and
  calls `qemu_cond_wait` while still holding `d->pfifo.lock`.
- Halt-flag access follows a matching atomics contract, stated at
  `pfifo.c:506-512`: all writers use `qatomic_set`; the heartbeat's
  reader uses `qatomic_read` — kept consistent "so TSAN is quiet and
  future refactors that drop the lock don't silently tear," even though
  the read currently happens under `d->pfifo.lock` too.

**Why this design.** BQL guards vCPU and timer-thread progress across
the whole of QEMU; holding it while waiting on a GPU fence, a renderer
mutex, or any other potentially-slow operation would stall every vCPU
and timer in the system for the duration of that wait — this is a
general QEMU rule, not fork-specific, but this fork's renderer callbacks
(surface access from CPU writes) are exactly the kind of code that
*could* violate it if written carelessly. The PFIFO kick/wake contract
is a fork-specific optimization: holding the lock across kick-set +
broadcast is what allows the idle-wait to be untimed (previously an
~1 kHz polling safety-net wakeup was needed without this guarantee).
A `pfifo_kick` call site that *doesn't* hold `d->pfifo.lock` reopens the
polling requirement silently (no crash, just a missed wakeup that
another kick or the removed poll would have masked) — this was a
real historical bug: hoisting `can_fifo_access` out of the pusher's word
loop broke the "lock held throughout" assumption because
`pfifo_run_puller` drops `pfifo.lock` when taking `pgraph.lock`
(README Failed-experiments row, "Hoist can_fifo_access...").

**What breaks if you violate this.** Blocking while holding BQL freezes
the whole VM's vCPU/timer progress for the wait's duration — perceived
by the user as a hitch or freeze proportional to the wait length. A new
`pfifo_kick` call site that doesn't hold `d->pfifo.lock` across the
kick-set+broadcast can produce a missed wakeup: the PFIFO thread's next
untimed `qemu_cond_wait` never gets kicked, appearing as a stuck/frozen
renderer (heartbeat diagnostic, `XEMU_PFIFO_HEARTBEAT=1`, is the
prescribed way to distinguish "PFIFO thread genuinely stuck" from
"PFIFO thread never woke up").

**Escape hatch.** None — audit every new BQL-adjacent or
`pfifo_kick`-adjacent call site by hand against these two contracts;
there is no flag that makes an unsafe pattern safe.

---

## Invariant 11 — Launch-path parity: `Info.plist` and `main()` must agree

**Statement.** `Info.plist`'s `LSEnvironment` dictionary is applied
**only** when the app is launched via Finder/LaunchServices (`open`,
double-click) — not when the same binary is launched from a shell. This
fork sets the same `MVK_CONFIG_*` values in `main()` via
non-overwriting `setenv` calls, so both launch paths converge on the
same MoltenVK configuration regardless of how the user (or a test
harness) starts the app. **Any new plist environment variable must be
mirrored in `main()`, or the split reopens.**

**Evidence** — `ui/xemu.c:1589-1615`, inside `main()`, `#ifdef __APPLE__`,
before MoltenVK is dlopen'd: five `setenv("MVK_CONFIG_…", …, 0)` calls
covering the canonical MVK tuple — the five values are cataloged in
`xemu-config-and-flags` Axis 3 (the value owner; prefill=0 is the
load-bearing one). The `setenv(..., 0)` third argument means "don't
overwrite" — an explicit env var set by the user or a harness before
launch still wins; this only fills in the default when nothing else set
it. The inline comment states the intent directly: "Keep in sync with
Info.plist. Prefill MUST stay 0: immediate encoding corrupts streamed
textures now that command buffers span whole frames, enabled an AGX
visibility crash, and measures slower (encoding lands on the PFIFO
thread)."

`Info.plist` (repo root, lines 35-52) carries the matching values,
verified equal to the `main()` block as of 2026-07-04. Re-verify the two
lists agree: `grep -n "MVK_CONFIG" Info.plist ui/xemu.c`.

**Parity holds in all three homes** (re-verified 2026-07-11 at
`7e2e6e7256`): the README "MoltenVK runtime config" table, `Info.plist`,
and `ui/xemu.c:1602-1606` all read prefill `0` — the README row was
fixed 2026-07-05. If any of the three ever disagrees again, that is
drift; record it in `xemu-docs-and-writing` §4, the stale-doc list home.

**Why this design.** The pink-tile saga (`xemu-failure-archaeology`) is
the origin story: for weeks, every test harness (shell-launched) ran
MoltenVK's *default* config while every real user session
(Finder-launched, or launched via `open`) ran the `Info.plist`
config — and the two disagreed on `PREFILL_METAL_COMMAND_BUFFERS`. Every
harness run was clean; every user session periodically flashed magenta
tiles. The bug was invisible until someone thought to check whether
Finder-launched and terminal-launched sessions were actually running the
same configuration.

**What breaks if you violate this.** Any *new* `MVK_CONFIG_*` (or other
launch-sensitive) variable added only to `Info.plist` without a matching
`setenv` in `main()` reopens exactly this asymmetry: shell-launched
sessions (including every automated test harness — see `xemu-testing`)
silently run a different configuration than what real users experience,
and any bug specific to the missing variable's default will be
invisible in testing and present for every real user.

**Escape hatch.** None needed — the fix *is* the escape hatch (explicit
user-set env vars still override, via the `overwrite=0` argument).

---

## Invariant 12 — Thread QoS: PFIFO and vblank run at `QOS_CLASS_USER_INTERACTIVE`

**Statement.** The two threads on the frame-critical path — the PFIFO
thread (NV2A command processing / rendering) and the vblank timer
thread (frame-pacing) — both explicitly raise their QoS (macOS's
thread-scheduling priority/latency hint mechanism) to
`QOS_CLASS_USER_INTERACTIVE` on macOS.

**Evidence.**
- PFIFO thread — `hw/xbox/nv2a/pfifo.c:473-479`:
  ```c
  void *pfifo_thread(void *arg)
  {
      NV2AState *d = (NV2AState *)arg;
  #if defined(__APPLE__)
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  #endif
      pgraph_init_thread(d);
      ...
  ```
- Vblank timer thread — `ui/xemu.c:848-854`:
  ```c
  static void *vblank_timer_thread(void *opaque)
  {
      struct xemu_console *scon = (struct xemu_console *)opaque;
  #ifdef __APPLE__
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  #endif
      ...
  ```
These are the only two `pthread_set_qos_class_self_np` call sites in the
tree (verified by grep across `hw/` and `ui/`).

**Why this design.** Both threads have a hard real-time-ish obligation:
PFIFO is the renderer's single-threaded critical path (nearly all
per-frame GPU work funnels through it), and the vblank thread's whole
purpose is a fixed 60 Hz monotonic deadline (Invariant 8). Left at
default QoS, macOS's scheduler can deprioritize either thread under
system load or thermal pressure, directly causing frame-pacing jitter or
stalls that have nothing to do with actual GPU/CPU work.

**What breaks if you violate this.** Removing either QoS bump, or adding
a new frame-critical thread without the same treatment, reintroduces
scheduler-induced jitter that will look exactly like a performance
regression in profiling (nsprof, APU_PROF) but isn't caused by any
change to the actual work being measured — a classic false-signal trap
for savestate A/B testing (see `xemu-testing`'s environment-parity
rules; this is the kind of confound that protocol is designed to keep
out of a benchmark).

**Escape hatch.** None — if you're adding a new thread that sits on the
frame-critical path, give it the same QoS bump; there's no reason to
make this configurable.

---

## Known weak points

Stated plainly, not softened. These are accepted tradeoffs or open
problems, not secretly-broken code — but a change that interacts with
one of these should know it's stepping onto contested ground.

- **MoltenVK internal-mutex deadlocks.** The depth-export experiment
  (`vkExportMetalObjectsEXT`) deadlocked MoltenVK's internal mutex under
  certain conditions (README Failed-experiments table) — a
  MoltenVK-internal failure mode this fork's code can't fix directly.
  The mitigation is defensive: `vk_wait_for_fence_or_die()`
  (`hw/xbox/nv2a/pgraph/vk/command.c:34-54`), used by single-time
  command buffers and other fence waits, aborts with a diagnostic after
  a 5-second timeout instead of hanging forever ("anything past the
  seconds range is a real hang... previously `UINT64_MAX` masked these
  as silent freezes"). This turns an unrecoverable hang into a visible
  crash-with-reason; it does not prevent the underlying deadlock class.
- **GL presentation backend: 1920px cap, no async MetalFX.** Under
  `display.window.presentation_backend = 'opengl'`, MetalFX output is
  capped at 1920px (`METALFX_SAFE_MAX_OUTPUT`,
  `hw/xbox/nv2a/pgraph/vk/display.c:1836`; `METALFX_MAX_IOSURFACE_WIDTH`,
  `metalfx_upscale.m:218`) because a macOS 26 bug gives BGRA8 IOSurfaces
  wider than ~1920px the wrong `bytesPerRow`. At `surface_scale=4`
  (2560×1920), MetalFX is skipped outright under GL. The Metal
  presentation backend has no such cap (private `MTLTexture`s, no
  IOSurface, `display.c:1828-1834`) — a reason to prefer it, not a bug
  to fix in GL. Async (non-blocking, `MTLSharedEvent`-paced) MetalFX
  submission is likewise Metal-only; GL keeps the synchronous
  `waitUntilCompleted` path (`metalfx_upscale.m:127-132`) because "GL
  cannot wait on MTLSharedEvent."
- **`hard_fpu` double-vs-extended precision delta.** `[perf] hard_fpu =
  true` (default) maps x87's 80-bit extended float (`floatx80`, 64-bit
  mantissa) to native ARM64 IEEE double (52-bit mantissa) instead of
  QEMU's software `floatx80`. README calls this "extremely rare" as a
  visible bug source, but the delta is real and unconditional when
  `hard_fpu` is on — a deliberate tradeoff (the performance win is the
  point of the inline-FPU work), not a bug. `hard_fpu = false` is the
  escape hatch for a title needing exact x87 semantics.
- **STALLED report drains are still finish-heavy when they fire — the
  default path just avoids firing them.** `VK_FINISH_REASON_STALLED`
  (full submit + fence wait + drain-every-pending-report,
  `hw/xbox/nv2a/pgraph/vk/draw.c:2045-2050`) is reachable from exactly
  one caller as of 2026-07-11: the *legacy*
  `XEMU_REPORTS_SYNC=1` branch (`reports.c:321-323`). The default path
  instead waits up to a 300 µs budget (`XEMU_REPORTS_BUDGET_US`,
  default 300 — `reports.c:313`; was 5 ms until 2026-07-04) and, if
  still pending, does a plain
  submit (`REPORTS_SUBMIT`, not `STALLED`) that does *not* trigger the
  full drain-all. The expensive path still exists and is still exactly
  as expensive when it runs — it's a legacy fallback now, not
  steady-state behavior. Re-verify with `grep -rn
  VK_FINISH_REASON_STALLED hw/xbox/nv2a/pgraph/vk/*.c` (expect 3
  matches: the profile-map entry and the finish-reason comparison in
  `draw.c`, plus exactly one *caller* — `reports.c:322`,
  `sync_mode`-gated) before relying on this description.
- **Pull-model present round trip** (Invariant 8) is a known,
  unresolved cost: the PFIFO-side handshake is what *publishes* a
  frame, so a UI-side "don't re-present unchanged content" pre-check
  can't run before paying for the round trip that would tell it the
  content is unchanged. RESOLVED 2026-07-11 for the Metal backend: the
  push-model redesign landed dark behind `XEMU_PUSH_PRESENT=1` (see the
  Statement above — measured: 601 present-handoff blocks/interval with
  up-to-79 ms single-wait tails under pull → 0 under push, flips/s
  identical). The pull weak point still fully applies to the default
  config (flag off) and always under frame interpolation / GL.
- **PAL titles run a 60 Hz vblank.** `vblank_interval_ns` is a single
  fixed value (`16666666LL`, 60 Hz — `ui/xemu.c:78`) with no PAL
  (50 Hz) variant anywhere in the tree (verified by grep). Whether/how
  this affects a PAL-region title expecting 50 Hz has not been
  characterized in this repo's history as of 2026-07-04 — treat as an
  open question, not a confirmed-benign fact.

---

## Checking a proposed change against the contract

Before landing a change to any file in the architecture diagram, scan
for a "does it touch" match and apply the rule:

| # | Does your change... | Rule |
|---|---|---|
| 1 | touch surface eviction/upload sync, or anything `#ifdef __APPLE__` in `pgraph/vk/surface.c`? | The non-Apple `#else` branch must keep the conservative (full-finish/immediate-evict) behavior unchanged. |
| 2 | add/change a per-flight-slot resource, or change how often one is consumed per frame? | Add a runtime capacity guard that submits-and-continues. Never rely on `nv2a_vk_assert` — compiled out unconditionally in this file. |
| 3 | create a new render-pass encoder path, or touch `vkCmdBeginQuery`/`vkCmdResetQueryPool` sequencing? | Must run after the per-CB primer query; never reintroduce per-query pool resets inside a pass. |
| 4 | change vertex/dirty tracking, or add a guest-write-triggered invalidation? | New refinements must be strictly additive over the page-granular fallback — never skip a conflict the coarse check would catch without independently proving safety. |
| 5 | touch DSP engine selection, add a `DSPOps` field, or touch `dsp->core` vs `dsp->backend`? | Preserve the `dsp56k_jit_*` (fork) / `dsp_jit_*` (upstream) naming split and "fork JIT wins when supported+enabled" precedence; update both engines' vtables if you add an op. |
| 6 | write to the DSP JIT code buffer, or patch/repatch an emitted branch? | Every write inside a `qemu_thread_jit_write()`/`_execute()` window; cross-block repatches before the window closes. No TLS-cached W^X state, no scoped-callback API — both failed before. |
| 7 | add/move a pinned-register access in the DSP JIT? | Preserve write-through; reload after any new BLR fallback that can mutate a pinned register. Run `XEMU_DSP_JIT_PIN_AUDIT=1` before calling it done. |
| 8 | touch present timing, vblank cadence, or `nv2a_get_present_frame`? | Never couple vblank interval to host display refresh (settled, do-not-retry). Remember this is pull-model before assuming a "skip if unchanged" check is easy. |
| 9 | add a new timing/throttle gate? | Use `QEMU_CLOCK_REALTIME`, never `QEMU_CLOCK_HOST` — REALTIME is the monotonic one here despite the name. |
| 10 | add a `pfifo_kick` call site, or run code with BQL held? | New kick sites must hold `d->pfifo.lock` across the kick-set + broadcast pair. Never block while BQL is held — drop it first, as `surface_access_callback` does. |
| 11 | add a new `MVK_CONFIG_*` var, or anything else in `Info.plist` `LSEnvironment`? | Mirror it in `main()` with `setenv(..., 0)`, same `#ifdef __APPLE__` block, before MoltenVK loads. |
| 12 | add a new thread on the frame-critical path? | Give it `QOS_CLASS_USER_INTERACTIVE` on `__APPLE__`, matching PFIFO and vblank. |

If your change requires *violating* one of these outright (not just
touching the area, but genuinely changing the assumption — e.g.
upgrading to a MoltenVK version that supports multiple queues), that is
a deliberate architecture change, not a bug fix: it needs the evidence
bar in `xemu-validation-and-qa`, the change-class handling in
`xemu-change-control`, and a README update per `xemu-docs-and-writing`
— and probably a new escape hatch (`xemu-config-and-flags`) so the old
behavior stays reachable during the transition.

## When NOT to use this skill

- You need Xbox/NV2A/DSP56300 **domain theory** (register semantics,
  ISA details, floatx80 math, Apple GPU/TBDR concepts) rather than
  *this fork's* design decisions built on top of that theory — use
  `xbox-hardware-reference`.
- You need the **story** behind an invariant — the bisect, the crash
  report, the measured before/after numbers, the dead ends tried first
  — rather than the invariant itself. This document gives you the
  "what" and a compressed "why"; `xemu-failure-archaeology` has the
  full narrative.
- You're deciding **whether a change is safe to land** procedurally
  (what evidence it owes, what gate it must pass, commit conventions) —
  that's `xemu-change-control`.
- You're **debugging a live symptom** (magenta tiles, a hang, a crash
  signature) and want a triage table, not a design-contract check —
  start at `xemu-debugging-playbook`, which will point back here for
  the "why is this the fix" once the fix is identified.
- You need the **catalog** of every config key and `XEMU_*` env var
  (names, defaults, precedence) rather than the invariants they protect
  — that's `xemu-config-and-flags`.
- You need **proof standards** for a performance or correctness claim
  (what counts as evidence, acceptance thresholds) — that's
  `xemu-validation-and-qa`.

## Provenance and maintenance

Every fact above was verified against the repository on 2026-07-04 at
commit `cf85e96597` (tag `v0.9`), branch `macos-optimizations`. On
2026-07-11 at `7e2e6e7256` (latest tag `v0.10.2`) the following were
re-verified: MVK three-home prefill parity (all `0`), the STALLED
reachability facts (one caller, `reports.c:322`; 300 µs default budget),
and the `pfifo_kick(d)` call-site count (20). Re-run
these before trusting a claim above on a later tree:

```sh
# Invariant 1 — single queue
grep -n "queueCount = 1" hw/xbox/nv2a/pgraph/vk/instance.c

# Invariant 1 (item 3) — destruction watermark still wired
grep -n "evict_submit_seq\|retired_submit_count" hw/xbox/nv2a/pgraph/vk/surface.c hw/xbox/nv2a/pgraph/vk/command.c hw/xbox/nv2a/pgraph/vk/renderer.h

# Invariant 2 — flight slots, query pool size, perf-build assert stripping
grep -n "NUM_FLIGHT_SLOTS" hw/xbox/nv2a/pgraph/vk/renderer.h
grep -n "max_queries_in_flight = 4096" hw/xbox/nv2a/pgraph/vk/reports.c
grep -n "NV2A_VK_PERF_BUILD" hw/xbox/nv2a/pgraph/vk/debug.h

# Invariant 3 — primer query still present, prefill still 0
grep -n "needsVisibilityResultMTLBuffer\|primer_index" hw/xbox/nv2a/pgraph/vk/draw.c

# Invariant 4 — byte-exact refinement flag
grep -n "XEMU_VTX_EXACT" hw/xbox/nv2a/pgraph/vk/vertex.c

# Invariant 5 — DSP engine precedence
grep -n "dsp_want_external_jit_engine\|DSP56K_JIT_SUPPORTED" hw/xbox/mcpx/apu/dsp/dsp.c

# Invariant 6 — JIT W^X windows
grep -n "qemu_thread_jit_write\|qemu_thread_jit_execute" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c | wc -l

# Invariant 7 — pin audit still wired
grep -n "XEMU_DSP_JIT_PIN_AUDIT" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c

# Invariant 8 — pull-model present, fixed vblank interval
grep -n "vblank_interval_ns = " ui/xemu.c

# Invariant 9 — clock mapping
grep -n "case QEMU_CLOCK_REALTIME\|case QEMU_CLOCK_HOST" util/qemu-timer.c

# Invariant 10 — pfifo kick contract
grep -n "Callers must hold d->pfifo.lock" hw/xbox/nv2a/pfifo.c

# Invariant 11 — launch-path parity (both must show PREFILL=0)
grep -n "PREFILL_METAL_COMMAND_BUFFERS" Info.plist ui/xemu.c

# Invariant 12 — QoS bumps
grep -rn "QOS_CLASS_USER_INTERACTIVE" hw/ ui/

# README's MVK table (should read 0 — fixed 2026-07-05; disagreement = new drift)
grep -n "PREFILL_METAL_COMMAND_BUFFERS" README.md

# Known weak point: STALLED reachability (expect 3 matches: draw.c profile-map
# entry + draw.c finish-reason compare + exactly one caller, reports.c:322)
grep -rn "VK_FINISH_REASON_STALLED" hw/xbox/nv2a/pgraph/vk/*.c
```

If any of these come back empty, differently-worded, or with a changed
value, the corresponding invariant section above needs a re-read of the
current code before being trusted.
