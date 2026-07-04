---
name: xemu-gpu-frame-campaign
description: Executable, decision-gated campaign for cutting GPU work and per-draw CPU cost per frame in xemu-macos's NV2A Vulkan renderer, for when the fps plateau in heavy in-game scenes (post-occlusion-rework baseline ~38.6 fps, 408-485 draws/flip, ~14 render passes/flip) needs to move again. Load this when asked to reduce draw count / coalesce draws / batch vkCmdDraw calls, cut render-pass count further, profile the PFIFO thread for per-draw CPU cost, evaluate vkCmdDrawMultiEXT or VK_EXT_multi_draw, pick up the README "Future vectors" item on GPU-bound heavy scenes, or generally push fps in a draws/flip > 100 scene. Every phase is a measurement gate with a named branch on miss — this is not a place to eyeball a frame and guess. Do NOT load this for CPU-side wait/fence-policy tuning in isolation (settled dead end, see Fenced-off wrong paths below) or for a specific crash/artifact/regression symptom (xemu-debugging-playbook).
---

# GPU frame-cost reduction campaign

## Mission and standing conclusion

The occlusion/report rework (commit `6bfbc22863`, 2026-07-03) took the heavy
Azurik savestate scene from 15.78 to 38.57±0.80 fps (+144%) per that
commit's protocol receipt (`scripts/bench-savestate-ab.sh`, 3 interleaved
pairs; README prose rounds this change to 35.6 fps / 2.25x) by fixing two
CPU-side serialization bugs: per-query render-pass teardown (376 passes/flip
→ 14) and a full GPU sync per pending occlusion report (23+/flip → 0
stalled finishes). Two follow-on experiments then asked "is there more
CPU-side slack to remove?" and got a clean, twice-repeated **no**: byte-exact
vertex-conflict refinement and per-flight vertex-RAM mirrors each killed
most of the CPU-side `finish_vtx_dirty` stalls, and fps did not move either
time. The wait time was real GPU frame time (~22-30 ms/flip) being paid at
a different call site, not slack. **Heavy scenes are GPU-bound.** Full story
and numbers: `xemu-failure-archaeology`.

That makes the next win categorically different from every fps win so far
in this fork: it must reduce **actual GPU work per frame** (fewer/larger
draw submissions, fewer render passes, less redundant per-draw driver-side
setup) or **per-draw CPU dispatch cost** on the PFIFO thread (the renderer
thread — `hw/xbox/nv2a/nv2a.c:235`, thread name `nv2a.pfifo_thread`; nearly
every renderer cost in this fork accrues here). CPU-side wait elimination
is a **fenced-off wrong path** for this campaign, proven twice already —
see below. Every phase in this campaign ends in a number, not an
impression. If you cannot state the metric and the pass/fail threshold
before running something, you are not ready to run it (this discipline —
predict, then measure — is `xemu-research-methodology`'s subject; this
skill just enforces it phase by phase).

**PFIFO / PGRAPH / flip / render pass / TBDR / zpass**, defined once: PFIFO
is NV2A's command-FIFO engine, run on a dedicated xemu thread; PGRAPH is the
3D state machine it drives (methods named `NV097_*`); a "flip" is one guest
frame swap (flips/s ≈ in-game fps, guest-capped at 60); a Vulkan render pass
here maps 1:1 to a Metal render-command-encoder under MoltenVK, and on
Apple's tile-based deferred renderer (TBDR) every pass does a full tile
load/store — pass **count**, not just pass content, is a first-order cost;
a zpass/occlusion query counts pixels that passed the depth test for a
guest `NV097_GET_REPORT` poll.

Campaign map: Ground-truth table → **Phase 0** baseline recapture (gated)
→ **Phase 1** attribution instrumentation → **Phase 2** GPU-side ground
truth → **Solution menu**: Mechanism A (consecutive-draw coalescing,
top-ranked) · B (per-draw CPU cost, Phase-2-gated) · C (render-pass-count
reduction) · D (`vkCmdDrawMultiEXT`, candidate-blocked) → Fenced-off
wrong paths → Validation and promotion protocol → Campaign log template.

## When NOT to use this skill

- A visible artifact, crash, freeze, or "it got slower" regression with a
  known shape → `xemu-debugging-playbook` (symptom-to-subsystem triage).
- Wanting to retry CPU-side wait/fence/sync-policy tuning because "the
  fence-wait number still isn't zero" → read Fenced-off wrong paths below
  first; this has a name and a graveyard (`xemu-failure-archaeology`).
- The scene you're measuring has draws/flip in the single digits (menu,
  attract-reel title card) → wrong scene, not this campaign's target; see
  the scene-identity rule in `xemu-testing`.
- Adding a new `XEMU_*` flag, deciding what evidence a change needs, or
  cutting a release → `xemu-change-control`.
- DSP/CPU JIT, audio, input, build/packaging → unrelated subsystems; see
  the corresponding sibling skill.
- You just want to know what a register or hardware concept means →
  `xbox-hardware-reference`.

## Ground truth: what changes between two consecutive guest draws today

This table is the load-bearing fact this whole campaign is built on. It
was produced by reading `hw/xbox/nv2a/pgraph/vk/draw.c`,
`vertex.c`, `command.c`, `reports.c`, `shaders.c`, `renderer.h`, and
`hw/xbox/nv2a/pgraph/glsl/shaders.c` in full (as of `cf85e96597`,
v0.9, 2026-07-04). "Draw" here means one guest `NV097_SET_BEGIN_END`
block; see the last row for why that is *not* the same as one
`vkCmdDraw*` call.

| # | State component | Current dedup / cost shape | Where (file:function) |
|---|---|---|---|
| 1 | **Pipeline object** (shader stages + raster/blend/depth-stencil + vertex-input layout + render-pass compatibility) | LRU cache, hash+memcmp on `PipelineKey`. `check_pipeline_dirty` fast-rejects the expensive hash/lookup unless shader bindings, render-pass state, one of 5 registers (`BLEND`, `CONTROL_0/1/2`, `SETUPRASTER`), or vertex layout changed. | `draw.c:1053` `check_pipeline_dirty`, `draw.c:1131` `create_pipeline`, `renderer.h:71` `PipelineKey` |
| 2 | **Shader state** (`VshState`/`GeomState`/`PshState`) | LRU cache keyed on `ShaderState` hash+memcmp — but the *dirty check itself* runs **unconditionally every draw**: up to 16 register-dirty tests, a per-active-combiner-stage loop (4 checks × stage count), 6 field compares, a 4×3 texture-state loop. Only a `true` result triggers the cache lookup. | `glsl/shaders.c:39` `pgraph_glsl_check_shader_state_dirty`, `vk/shaders.c:604` `pgraph_vk_bind_shaders` (called unconditionally at the top of `create_pipeline`, *before* `check_pipeline_dirty` even runs) |
| 3 | **Descriptor set 0 (UBOs)** | Advance-index skip: rewritten only when `uniforms_changed \|\| shader_bindings_changed \|\|` the uniform staging buffer just wrapped; the `vkCmdBindDescriptorSets` call is separately skipped via a `last_bound_ubo_descriptor_set_index` compare. | `vk/shaders.c:187-288` `pgraph_vk_update_descriptor_sets`, `vk/draw.c:1553-1607` `bind_descriptor_sets` |
| 4 | **Descriptor set 1 (textures)** | Same advance-index pattern, keyed on `shader_bindings_changed \|\| texture_bindings_changed`; bind skipped via `last_bound_descriptor_set_index`. Both sets' multi-binding writes are coalesced into one `VkWriteDescriptorSet` each (spec §14.2.3 array overflow trick). | same functions, `vk/shaders.c:277-287,319-327` |
| 5 | **Vertex attribute layout** (binding/attribute descriptions) | A memcmp snapshot (`update_vertex_layout_dirty`) sets `vertex_state_dirty`, which feeds `check_pipeline_dirty` (row 1) — a real layout change forces a *new pipeline*, not just a rebind. But the O(16)-attribute-slot rebuild that *produces* the thing being compared (`pgraph_vk_bind_vertex_attributes`) runs unconditionally every guest block regardless of whether anything changed. | `vk/vertex.c:38` `update_vertex_layout_dirty`, `vk/vertex.c:321` `pgraph_vk_bind_vertex_attributes`, `vk/vertex.c:493` `_inline` variant |
| 6 | **Vertex buffer bindings** (`vkCmdBindVertexBuffers` args) | Exact memcmp of the would-be `buffers[]`/`offsets[]` against the last-issued args on this command buffer (CB); call skipped on a match. | `vk/draw.c:2627-2670` `bind_vertex_buffer` |
| 7 | **Index buffer binding** (`vkCmdBindIndexBuffer`) | Exact compare (buffer + offset + type); call skipped on a match. | `vk/draw.c:2682-2700` `bind_index_buffer` |
| 8 | **Push constants** — *only* the vertex shader's "uniform attrs" (attributes with stride 0, i.e. constant-valued streams; ≤16 attrs × 4 floats = 256 B, MoltenVK's `maxPushConstantsSize` is 4096 B so this path is always available on this device) | Memcmp of freshly-pulled values against the last-issued `(CB, pipeline layout)` payload — but `pgraph_get_inline_values` itself *always runs*; only the `vkCmdPushConstants` call is skipped on a match. General VSH/PSH uniforms (transforms, lighting, combiner constants, fog, `texScale`) are a **separate channel** — they go through UBOs (row 3), not push constants. | `vk/draw.c:1505-1551` `push_vertex_attr_values`; budget check `vk/shaders.c:653-655` |
| 9 | **Viewport / scissor / line width** (dynamic state) | Memcmp cache — but only computed/checked at all when `must_bind_pipeline` is true (an actual pipeline rebind or a fresh render pass). **Not evaluated on every draw.** | `vk/draw.c:2224-2281` (inside `begin_draw`, gated by `if (must_bind_pipeline)`) |
| 10 | **Blend constants / depth bias** (dynamic state) | Memcmp cache, but unlike row 9 these run on **every non-clearing draw** regardless of `must_bind_pipeline`. | `vk/draw.c:2283-2310` |
| 11 | **Occlusion query span** | A query rotates only when a report was requested since the last draw (`new_query_needed`) or `zpass_pixel_count_enable`/clearing toggled — otherwise the *same* query silently continues spanning multiple draws. Queries begin/end **inside** the render pass now (bulk pool reset moved to CB-begin; this is why passes no longer tear down per query rotation). | `vk/draw.c:2202-2222` (rotation logic in `begin_draw`); `vk/reports.c:96-139` (`alloc_report`/`clear_report_value` set `new_query_needed`) |
| 12 | **Render pass / framebuffer** | Stays open across draws until a surface rebind, a clear, or a non-draw command (texture upload needing the compute-unswizzle path, surface creation, render-to-texture) forces `pgraph_vk_ensure_not_in_render_pass`. | `vk/draw.c:2094-2098,2137-2146,2208-2210,2335-2337`; `vk/texture.c:959,1190,1395`; `vk/surface.c:1112,1841` |
| 13 | **Actual `vkCmdDraw*` call count vs. guest begin/end block count** | **Not 1:1 today.** The `NV097_DRAW_ARRAYS`-without-CPU-emulation path issues **one `vkCmdDraw` per accumulated non-contiguous start/count subrange within a single guest block** (`pg->draw_arrays_length` can exceed 1 — see `pgraph_expand_draw_arrays`/the "connect contiguous primitives" logic in `pgraph.c:2852-2862`). The CPU-emulated-primitive path (`LINE_LOOP`/`QUADS`/`QUAD_STRIP`/first-vertex `TRIANGLES`/line-mode `POLYGON`) *already* concatenates every subrange into one combined index buffer + one `vkCmdDrawIndexed`. The three inline paths (`inline_elements`/`inline_buffer`/`inline_array`) always issue exactly one draw call. | `vk/draw.c:2903-3193` `pgraph_vk_flush_draw`, specifically the emulated-merge loop at `2960-2985` vs. the per-subrange loop at `2987-2993` |

Row 13 is the campaign's single most concrete finding: xemu already knows
how to fuse N guest draws into one Vulkan draw call — it does it today for
CPU-emulated primitives — and simply doesn't apply the same technique to
the far more common non-emulated path. Mechanism A below is built directly
on this asymmetry.

## Phase 0 — Baseline recapture

Historical numbers in this file are dated; re-establish today's numbers
on this machine before touching anything. Every later phase compares
against *this* capture.

**Fixtures** (re-verify before running): hdd `~/Documents/Xemu/xbox_hdd.qcow2`;
snapshot shortcuts read fresh from
`~/Library/Application Support/xemu/xemu/xemu.toml` under
`[general.snapshots.shortcuts]` — date-stamped, owner-volatile names
(current values live in the golden-fixture inventory,
`xemu-validation-and-qa` §3; **do not hardcode them across sessions**,
re-read the file). Never run this against `dist/xemu.app` directly and
never in parallel with a live user session — same hdd, same (unplanted,
shared) `xemu.toml`; see `xemu-testing` cardinal rule 2. Make a scratch
copy first:

```bash
SCRATCH=$(mktemp -d)
cp -Rc dist/xemu.app "$SCRATCH/xemu-campaign.app"   # APFS CoW clone, instant
pgrep -fl xemu || echo "no other xemu running — clear to proceed"
```

**Capture** (single arm — nothing to A/B yet at Phase 0):

```bash
SOCK=/tmp/xemu-campaign-mon.sock
rm -f "$SOCK"
# Heavy-scene snapshot: read the F6-bound name fresh from xemu.toml
SNAP=$(grep -m1 '^f6 = ' "$HOME/Library/Application Support/xemu/xemu/xemu.toml" | cut -d"'" -f2)
echo "using snapshot: $SNAP"   # confirm before proceeding
XEMU_NV2A_NSPROF=1 "$SCRATCH/xemu-campaign.app/Contents/MacOS/xemu" \
    -monitor unix:$SOCK,server,nowait \
    > "$SCRATCH/phase0.log" 2>&1 &
XEMU_PID=$!
sleep 25   # boot

python3 - "$SOCK" "$SNAP" <<'PYEOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.5); s.recv(4096)
s.sendall(("loadvm %s\n" % sys.argv[2]).encode())
time.sleep(3); s.recv(8192)
s.close()
PYEOF

sleep 60   # >=10 nsprof 5s intervals past the load transient
kill $XEMU_PID; sleep 2; kill -9 $XEMU_PID 2>/dev/null
grep "nsprof:" "$SCRATCH/phase0.log" | tail -120
```

**Read the output** (exact labels from `hw/xbox/nv2a/nsprof.c`): the
`%.1fs interval, N flips (F/s)` line gives fps; the `draws` event row
(`NSPROF_EV_DRAW`, one per guest begin/end block) gives draws/flip; the
`renderpass` row (`NSPROF_EV_RENDERPASS`, counted *only* on the main NV2A
command buffer — the once-per-flip Metal compositor/present pass is a
separate CB and is **not** included) gives passes/flip; the `fence_wait`
counter row gives `per_flip=` in µs (÷1000 for ms); the `pipeline_gen`
counter row (absent entirely if zero events — this is expected).
(`xemu-diagnostics-and-tooling` owns interpretation detail and any
log-parsing convenience for this output — check there before hand-parsing
a long capture.)

**Gate:**

| Metric | Pass envelope | On miss |
|---|---|---|
| draws/flip | 350-500 (historical 408-485; attract-reel demo runs 350-390 and is a legitimate but *different* in-game scene — see xemu-testing scene-identity rule) | ~3 draws/flip → wrong scene (menu/title card); reload via `xemu-testing`'s scene-identity check, not this campaign |
| render passes/flip | ≈14 (single digits to ~20 is fine) | ≫14 (dozens to hundreds) → a regression reintroduced per-draw pass teardown or a new non-draw-command interleave; **stop, this is `xemu-debugging-playbook` + `xemu-failure-archaeology`'s occlusion-rework saga, not a campaign target** |
| fps | ~38.6 (headline as of 2026-07-04; commit `6bfbc22863` (2026-07-03) measured 38.57±0.80 on this exact scene) | Materially lower with passes/draws in-range → something regressed CPU-side (fence_wait, a FINISH_* event spike); triage via `xemu-debugging-playbook` before starting this campaign |
| fence_wait ms/flip | ~2.3 | ≫2.3 with passes≈14 → new stall source; diagnose first (this campaign assumes the settled GPU-bound conclusion, which was proven on *this* fence-wait floor) |
| pipeline_gen events | ≈0 | Nonzero and climbing → pipeline cache is thrashing (state churn or a cache-persistence regression); unrelated prerequisite fix needed first |

Record all five numbers in the campaign log (template at the end) before
proceeding. If everything is in-range, the standing GPU-bound conclusion
still holds and this campaign is aimed correctly.

## Phase 1 — Attribution instrumentation (code change; additive diagnostics)

**Change class**: additive, `XEMU_NV2A_NSPROF`-gated diagnostics only — no
behavior change. Still goes through normal review; see
`xemu-change-control` for what evidence an additive-diagnostics change
needs (much less than a behavior change, but it's still a commit).

### 1.1 What question each new counter answers

Phase 0 tells you fps/draws/passes are in the expected envelope. It does
not tell you *which* mechanism (A/B/C/D below) has room to work. Add five
new `enum NsprofEvent` values to `hw/xbox/nv2a/nsprof.h` (append before
`NSPROF_EV__COUNT`, following the existing naming) and register their
labels in `hw/xbox/nv2a/nsprof.c`'s `event_names[]`, matching the existing
`nsprof_event(NSPROF_EV_...)` call-site pattern already used for
`NSPROF_EV_RENDERPASS`/`NSPROF_EV_PIPELINE_BIND`/`NSPROF_EV_DRAW`:

| New event | Fires where | Answers |
|---|---|---|
| `NSPROF_EV_VK_DRAW_CALL` | Every actual `vkCmdDraw`/`vkCmdDrawIndexed` call site in `pgraph_vk_flush_draw` (7 sites: `draw.c:2983,2991` [inside the per-subrange loop — one call per iteration] `,3055,3113,3116,3180,3184` — re-locate with `grep -n vkCmdDraw hw/xbox/nv2a/pgraph/vk/draw.c`, which matches both the plain and `Indexed` forms; one further match at `draw.c:2571` is the partial-color-clear helper triangle in `pgraph_vk_clear_surface`, a different function outside `flush_draw` that `NSPROF_EV_DRAW` also doesn't count — leave it out of this counter to keep the ratio below clean) | True Vulkan draw-call count per flip, vs. `NSPROF_EV_DRAW`'s guest-block count. The gap between them is exactly mechanism A's opportunity (row 13 above). |
| `NSPROF_EV_DRAW_ARRAYS_MULTI_SUBRANGE` | Once per `pgraph_vk_flush_draw` call where the non-emulated `draw_arrays` loop runs with `pg->draw_arrays_length > 1` (`draw.c:2987`, the `else` branch guarding the per-subrange `vkCmdDraw` loop) | How often mechanism A1 (same-block subrange coalescing) even applies. |
| `NSPROF_EV_DRAW_MERGE_IDENTICAL` | At the top of `begin_draw` (`draw.c:2172`, before line 2212): compare this draw's about-to-be-used pipeline pointer + `need_bind_ubo`/`need_bind_tex` outcome + vertex-buffer bind args against a small static "previous draw fingerprint" struct captured at the end of the *previous* `begin_draw` call (after line 2313). Bucket 1: pipeline same, descriptors same, vertex buffer+offset *also* identical. | Draws already fully deduped by existing caches (rows 3/4/6 above) — zero new opportunity here. |
| `NSPROF_EV_DRAW_MERGE_CANDIDATE` | Same comparison, bucket 2: pipeline same, descriptors same, vertex buffer *handle* same but **offset differs**. | This is the literal "pipeline == previous && descriptors unchanged && only vertex offsets differ" question mechanism A2 needs answered. |
| `NSPROF_EV_DRAW_STATE_CHANGED` | Same comparison, bucket 3: anything else (different pipeline, different descriptor advance, or a different vertex buffer entirely). | The floor mechanism A2 cannot touch. |

Also add four pass-cause tags (reuse the existing `NSPROF_EV_RENDERPASS`
event's *sites* — tag which caller triggered the pass end that preceded
each subsequent `NSPROF_EV_RENDERPASS`) at the four `end_render_pass` /
`pgraph_vk_ensure_not_in_render_pass` call groups: surface rebind
(`draw.c:2137-2146`), clear begin/end (`draw.c:2208-2210,2335-2337`),
texture-upload compute-unswizzle interleave (`texture.c:959-1097`), and
everything else routed through `pgraph_vk_begin_nondraw_commands`
(surface creation, render-to-texture — `surface.c:1112`,
`texture.c:1190,1395`). This feeds mechanism C.

### 1.2 Implementation sketch (small, mechanical)

```c
/* nsprof.h: append before NSPROF_EV__COUNT */
NSPROF_EV_VK_DRAW_CALL,
NSPROF_EV_DRAW_ARRAYS_MULTI_SUBRANGE,
NSPROF_EV_DRAW_MERGE_IDENTICAL,
NSPROF_EV_DRAW_MERGE_CANDIDATE,
NSPROF_EV_DRAW_STATE_CHANGED,
NSPROF_EV_RENDERPASS_CAUSE_SURFACE,
NSPROF_EV_RENDERPASS_CAUSE_CLEAR,
NSPROF_EV_RENDERPASS_CAUSE_TEXUPLOAD,
NSPROF_EV_RENDERPASS_CAUSE_OTHER,
```

Call `nsprof_event(NSPROF_EV_VK_DRAW_CALL)` immediately before/after each
of the 7 draw-call sites; call
`nsprof_event(NSPROF_EV_DRAW_ARRAYS_MULTI_SUBRANGE)` once when entering the
per-subrange loop with length > 1. For the merge-fingerprint comparison,
add a small file-static struct in `draw.c` next to the other per-CB
caches (it resets the same way `last_vertex_bind_valid` etc. do, in
`pgraph_vk_begin_command_buffer`) — this is diagnostics-only, it must not
change control flow, only observe the values `begin_draw` was already
going to compute.

### 1.3 Capture and the decision fork

Same capture procedure as Phase 0, rebuilt binary. Compute, per flip
(average over the same ≥10 post-load intervals):

```
merge_candidate_fraction = NSPROF_EV_DRAW_MERGE_CANDIDATE / NSPROF_EV_VK_DRAW_CALL
multi_subrange_fraction  = NSPROF_EV_DRAW_ARRAYS_MULTI_SUBRANGE / NSPROF_EV_DRAW
extra_calls_from_subrange = NSPROF_EV_VK_DRAW_CALL - NSPROF_EV_DRAW   (upper bound; inline paths always contribute exactly 1 each)
```

| Result | Branch |
|---|---|
| `merge_candidate_fraction` ≥ 30-50% | Proceed to Mechanism A2 (cross-block). High mechanical value, justifies its three theory-obligation proofs below. |
| `merge_candidate_fraction` < 10% | Do not implement A2. A1 (same-block) may still be worth it independently if `multi_subrange_fraction` is nonzero — it is far cheaper to prove correct (no cross-block theory obligations at all, see below). |
| Both fractions near zero AND `extra_calls_from_subrange` ≈ 0 | Draw count is already close to irreducible with this technique. Pivot to Mechanism B (per-draw CPU cost) or go straight to Phase 2 to find out whether CPU cost matters at all before writing any more code. |
| Render-pass-cause counters show one cause dominating (e.g. texture-upload interleave firing every flip) | Feeds Mechanism C — but only after this per-cause count exists; do not guess which cause matters. |

## Phase 2 — GPU-side ground truth (standard macOS tooling)

Standard Xcode/Instruments tooling — nothing xemu-specific to write. **The
engineer launches xemu for this phase**, not an automated harness; GPU
frame capture and Instruments both require attaching to a live, foreground
process.

- **Metal HUD**: launch the scratch copy with `MTL_HUD_ENABLED=1` in the
  environment (standard Apple env var). Gives a live GPU-time/frame-time
  overlay with zero setup — use it first as a sanity check before the
  heavier tools below.
- **Xcode GPU frame capture** (Debug → Capture GPU Frame, attached to the
  scratch-copy process during the same savestate scene): inspect the
  captured frame's encoder list. MoltenVK maps one `vkCmdBeginRenderPass`
  to one `MTLRenderCommandEncoder` — **the encoder count in the capture
  should match Phase 0/1's `renderpass` per-flip number**; if it doesn't,
  something about the capture's frame boundary differs from a flip and the
  comparison needs adjusting before trusting the rest. Per-encoder GPU
  duration and the Tile Memory / bandwidth pane are the load-bearing
  numbers for Mechanism C's win-ceiling estimate.
- **Instruments, Metal System Trace template**: attach to the scratch
  copy, record ~10 s during the same scene. The GPU track gives
  per-command-buffer/per-encoder duration and draw-time distribution
  within an encoder (feeds Mechanism A's win estimate: how much GPU time
  is genuinely proportional to draw *count* vs. fill/shading work that
  coalescing can't touch). The CPU track, filtered to the
  `nv2a.pfifo_thread`, gives time-in-driver around `vkQueueSubmit`/
  `vkCmdDraw*` calls — this is Mechanism B's actual evidence base (do not
  guess hot sites from code reading alone; rank Phase 2's measured
  self-time, not the candidate list in Mechanism B).

**Cross-check against nsprof, then decide:**

| Observation | Branch |
|---|---|
| GPU track shows total encoder time ≈ the frame's full time budget (≈1000/fps ms), CPU/PFIFO time is a small slice | Confirms GPU-bound. Prioritize Mechanism A (fewer, larger draws reduce actual GPU dispatch overhead even when execution is GPU-bound) and Mechanism C (pass count) over B. |
| CPU/PFIFO per-draw dispatch time is a substantial, measured fraction of frame time | Mechanism B has real headroom — proceed to it with the *specific* hot site(s) Instruments ranked, not the full candidate list. |
| Neither — GPU time is well under the frame budget and PFIFO is mostly idle | The bottleneck is somewhere this campaign doesn't cover (audio thread, vCPU, present pacing) — stop, this campaign's premise doesn't apply to what you're actually measuring; re-check Phase 0's scene identity. |

## Solution menu (ranked)

Every mechanism below needs its expected win **derived from Phase 1/2
numbers**, not asserted. None of them get an fps figure here — that would
be exactly the mistake the streamed-vertex campaign's initial "-30%"
misread made (an invalid baseline, no prediction, no kill threshold; see
Fenced-off wrong paths).

### Mechanism A — consecutive-draw coalescing (top-ranked)

**Mechanism**: reduce `vkCmdDraw*` call count by extending the
index-buffer-concatenation trick the CPU-emulated-primitive path already
uses (row 13 in the ground-truth table) to cases it currently doesn't
cover.

#### A1 — same-block multi-subrange (do this first: cheapest, lowest risk)

**Scope**: the non-emulated `draw_arrays` branch's per-subrange
`vkCmdDraw` loop (`vk/draw.c:2987-2993`), active whenever
`pg->draw_arrays_length > 1` and `draw_needs_primitive_emulation()`
(`draw.c:122`) is false — i.e. `POINTS`/`LINES`/`LINE_STRIP`/
`TRIANGLE_STRIP`/`TRIANGLE_FAN`, or `TRIANGLES` with provoking-vertex =
*last*.

**Win derivation**: `extra_calls_from_subrange` (Phase 1) is the exact
number of `vkCmdDraw` calls A1 removes per flip (replaces N calls with 1
per multi-subrange block). Whether that translates into fps depends on
the per-call CPU dispatch cost Phase 2 measures on *this* machine — do not
assume a number.

**Theory obligations** (why this is low-risk): all subranges inside one
guest begin/end block already share **one** `create_pipeline` /
`pgraph_vk_bind_vertex_attributes` / descriptor-set call — `flush_draw`
applies a single end-of-block state snapshot to every subrange today,
whether it issues 1 or N draw calls. So merging changes *only* the call
count, not which state gets applied to which vertices — no new
"state changed mid-merge" risk exists for A1. The one real correctness
axis is **topology-aware index construction**:
- `POINT_LIST`/`LINE_LIST`/`TRIANGLE_LIST` (and last-vertex `TRIANGLES`,
  the common case here) have no connectivity between primitives — plain
  concatenation of per-subrange identity indices (`start_i, start_i+1,
  ...`) into one combined buffer + one `vkCmdDrawIndexed` is safe, exactly
  mirroring what `build_emulated_indices_from_array` already does for the
  emulated path.
- `LINE_STRIP`/`TRIANGLE_STRIP`/`TRIANGLE_FAN` **do** have implicit
  connectivity between consecutive indices — naive concatenation would
  draw a phantom primitive bridging the end of one subrange to the start
  of the next. These require inserting Vulkan's primitive-restart index
  (`0xFFFFFFFF`) between subranges, and only work because `create_pipeline`
  already sets `primitiveRestartEnable = true` for exactly these three
  topologies (verified: `draw.c:1251-1260`). `LINE_LOOP` is irrelevant
  here — it is unconditionally CPU-emulated (`draw_needs_primitive_emulation`
  returns `true` for it), never reaches this branch.

**Implementation sketch**: in the `else` branch at `draw.c:2987`, branch on
`get_primitive_topology(pg)` (`draw.c:55`): for list topologies, build one
combined identity-index buffer and one `vkCmdDrawIndexed` (mirroring
`build_emulated_indices_from_array`'s loop-and-concatenate shape at
`draw.c:2960-2985`, minus the emulation math); for strip/fan topologies,
build the same but insert a restart index between subranges.

**Escape hatch**: `XEMU_DRAW_MERGE=0` disables A1 and restores the
per-subrange loop (polarity matches the existing `XEMU_VTX_EXACT`
precedent: default is the new merged behavior, `=0` reverts to legacy —
not the "set to 1 to enable" polarity used by `XEMU_ZETA_SHAPE_READBACK`/
`XEMU_TEX_BIND_RECHECK`; pick one and say so in the README knob table).

**Kill criterion**: `extra_calls_from_subrange` is negligible (near zero
per flip) in Phase 1 → don't implement, note it as a measured non-issue in
the campaign log (not a "failed experiment" — it was never tried, just
ruled out by measurement first, which is the point of Phase 1). If
implemented and any visual diff appears in the validation soak (below),
revert and add the README failed-experiments row.

#### A2 — cross-block draw coalescing (harder; gate on Phase 1's fraction)

**Scope**: fusing two *consecutive guest begin/end blocks* into one
`begin_draw`/`vkCmdDraw*` pair when Phase 1's
`NSPROF_EV_DRAW_MERGE_CANDIDATE` fires (pipeline unchanged, descriptors
unchanged, same vertex buffer, different offset — e.g. the same mesh
instanced at a different position with no state change between
instances).

**Win derivation**: ceiling is `NSPROF_EV_DRAW_MERGE_CANDIDATE`/flip calls
removed; same caveat as A1 on translating that into fps — get Phase 2's
per-call dispatch cost first.

**Theory obligations** (three, all required — this is why A2 ranks below
A1 despite potentially larger scope):

1. **Pipeline/descriptor/vertex-layout identity**, the obvious one: no
   PGRAPH state write between the two blocks may flip any input to
   `check_pipeline_dirty` (row 1), `shader_bindings_changed`,
   `texture_bindings_changed`, or `vertex_state_dirty` (row 5) — exactly
   the flags Phase 1's fingerprint comparison already keys on.
2. **The inline-uniform-value channel is a separate, untracked-by-those-flags
   state axis.** `SET_VERTEX_DATA{2S,4F_M,2F_M,...}`-style methods
   (`pgraph.c`, e.g. the `SET_VERTEX_DATA4F_M` handler) write
   `attribute->inline_value` directly with **no associated dirty flag**
   comparable to `shader_bindings_changed`. `push_vertex_attr_values`
   (row 8) re-pulls fresh values every `begin_draw` call and only skips
   the *Vulkan call* on a memcmp match — today that is safe because each
   block gets its own `begin_draw`/comparison. A2 collapses two blocks
   into one `begin_draw` call, i.e. **one** decision point instead of two —
   if the guest changed an inline vertex-attribute value between the two
   blocks without touching anything in obligation 1, a merge would silently
   drop that update. A2 must explicitly compare the actual inline-attribute
   values (or the `uniform_attrs` payload) between the two candidate blocks,
   not just reuse the pipeline/descriptor dirty flags.
3. **A candidate pair must not straddle an occlusion-report or
   zpass-enable boundary.** `alloc_report`/`clear_report_value`
   (`reports.c:96,110`) set `new_query_needed`, which `begin_draw`'s query
   rotation logic (`draw.c:2202-2222`) consumes **once per `begin_draw`
   call** to decide whether to end the current occlusion query and start a
   new one. If the guest issued `NV097_GET_REPORT` between block 1 and
   block 2, the unmerged code correctly rotates the query between them, so
   block 1's pixels land in report N and block 2's in report N+1; a merge
   collapses both blocks' pixels into a single query span, corrupting
   report N. Gate on `r->new_query_needed` being false and
   `pg->zpass_pixel_count_enable` being unchanged across the pair.

**Implementation sketch**: defer issuing block 1's draw call until block
2's `NV097_SET_BEGIN_END_OP_END` arrives (or a non-mergeable event forces
a flush); if all three obligations hold, apply block 2's vertex range as
an additional subrange through the *same* combined-index-buffer mechanism
A1 builds, rather than inventing a second fusion path. This makes A2
close to "A1's mechanism, triggered by a cross-block gate" rather than
independent machinery.

**Escape hatch**: `XEMU_DRAW_MERGE_CROSS=0` (separate from A1's flag —
these must be independently toggleable so a regression can be bisected to
one or the other).

**Kill criterion**: `merge_candidate_fraction` < 10% (Phase 1) → don't
implement. If implemented: any occlusion/zpass visual diff in the
zpass-consumer soak (below) → revert immediately, this is exactly the
class of bug obligation 3 exists to prevent; add the README row.

### Mechanism B — per-draw CPU cost reduction (Phase-2-gated)

**Mechanism**: cut redundant unconditional-per-draw CPU work identified in
the ground-truth table. **Do not implement any of these from code reading
alone** — Phase 2's Instruments profile on `nv2a.pfifo_thread` must rank
them first; code reading only tells you these run every draw, not that
they cost anything relative to a GPU-bound frame.

**Candidate hot sites** (all verified to run unconditionally per draw;
none independently profiled — rank by Phase 2's measured self-time):

- **B1**: `pgraph_glsl_check_shader_state_dirty` (`glsl/shaders.c:39`) —
  up to 16 register-dirty checks + a per-combiner-stage loop + 6 field
  compares + a 4×3 texture-state loop, every non-clear draw, called from
  `pgraph_vk_bind_shaders` (`vk/shaders.c:613`) unconditionally at the top
  of `create_pipeline` — before the cheap `check_pipeline_dirty` fast-path
  (row 1) even runs. The single most expensive-*looking* always-on check
  in the draw path.
- **B2**: `pgraph_vk_bind_vertex_attributes` (`vk/vertex.c:321`, and the
  `_inline` variant at `493`) — unconditional O(16) loop over every
  vertex-shader attribute slot every guest block, regardless of how many
  attributes actually changed.
- **B3**: `begin_draw`'s always-run block (`vk/draw.c:2283-2313`) — blend-
  constants recompute (register read + ARGB-unpack + 16-byte memcmp),
  depth-bias flag check, `bind_descriptor_sets`, `push_vertex_attr_values`
  — all run every non-clearing draw. (Viewport/scissor/line-width, row 9,
  are *not* in this bucket — they're gated behind `must_bind_pipeline` and
  do not run on every draw; don't conflate the two.)
- **B4**: `push_vertex_attr_values`'s unconditional `pgraph_get_inline_values`
  call (`vk/draw.c:1518`) before its own memcmp-skip decision.
- **B5**: `pgraph_vk_bind_textures` (`vk/texture.c`) — called unconditionally
  every non-clear draw from `create_pipeline`, **not read in this pass —
  cost unverified**, candidate pending both a code read and Instruments
  confirmation before prioritizing.

**Win derivation**: `Σ (measured self-time of chosen site) × draws/flip ×
fps`, compared against the GPU-bound frame budget from Phase 2. If PFIFO
total busy time is already a small fraction of the frame period, B's
ceiling is invisible to fps regardless of which site you pick — confirm
this is *not* the case before writing code.

**Theory obligations**: none of B1-B5 currently skip correctness-relevant
work — they're pure redundant-computation candidates (the *check* runs
every draw, not the *update* it guards). The risk shows up only if you
add a *new* short-circuit (e.g. "skip the register-dirty scan when we
believe nothing touched these registers since last draw") — that is
structurally identical to the DSP JIT's `EPI_NO_PC` and round-4 `cur_inst`
preset-skip attempts, both of which *looked* safe by inspection and were
wrong 9.75% of the time and on `op=0x001000` respectively (see
`xemu-failure-archaeology`). **Any new "skip when we assume unchanged"
optimization here needs the same discipline that saved those: a sentinel/
audit harness that can prove the skip condition never fires wrong, not
code review alone.**

**Escape hatch**: whichever specific fast-path ships gets its own
`XEMU_<SITE>_FASTPATH=0`-style flag (the `XEMU_TEX_BIND_RECHECK`/
`XEMU_VTX_EXACT` pattern) — do not ship one shared kill-switch for
multiple unrelated fast-paths; that prevents isolating which one caused a
regression.

**Kill criterion**: Phase 2 shows PFIFO CPU time is a small fraction
(rule of thumb, confirm against your own frame-budget headroom rather than
trusting a fixed percentage) of the GPU-bound frame time → deprioritize
entirely in favor of A/C.

### Mechanism C — further render-pass-count reduction

**Mechanism**: 14 passes/flip is already a 96% reduction from the
pre-rework 376. The requirement here is explicit: **measure per-cause
counts before attempting anything** — the remaining causes are a short,
enumerable list, not a diffuse problem like the one the occlusion rework
solved.

**Causes enumerated from code** (each needs its own Phase-1-style
per-flip counter before ranking — see the four
`NSPROF_EV_RENDERPASS_CAUSE_*` events above):

- **C1 — surface switches**: a color/zeta rebind sets `framebuffer_dirty`/
  `render_pass_dirty` in `begin_pre_draw`, forcing
  `pgraph_vk_ensure_not_in_render_pass` (`draw.c:2137-2146`). The zeta
  shape-switch fast path (README "Vulkan renderer" changes) removed the
  *readback* cost of a shape ping-pong but a genuinely different surface
  still needs a different framebuffer attachment — Vulkan requires ending
  the pass regardless. This cause has a hard floor; don't expect to
  remove it, only to confirm it isn't firing *more* than the scene's
  actual surface count requires.
- **C2 — clears**: `pg->clearing` unconditionally calls `end_render_pass`
  on **both** sides of a clear operation (`draw.c:2208-2210` and
  `2335-2337`) — every `NV097_CLEAR_SURFACE` costs a pass boundary.
- **C3 — compute-unswizzle interleave**: texture uploads needing the
  compute-shader unswizzle path call `pgraph_vk_begin_nondraw_commands`
  (which ends the current pass) mid-stream (`texture.c:959-1097`,
  confirmed: the dispatch calls at lines 1086/1092 execute inside the
  same `cmd` opened by the `begin_nondraw_commands` call at line 959).
  Streaming-heavy scene transitions will show this spike; steady-state
  in-game should show it near zero.
- **C4 — other non-draw commands**: surface creation
  (`surface.c:1112`) and render-to-texture (`texture.c:1190,1395`), both
  routed through the same `pgraph_vk_begin_nondraw_commands` helper.

**Win derivation**: `(removable pass count) × (per-pass tile load/store
cost from Phase 2's Xcode GPU-capture Tile Memory pane)` — measured, not
assumed. Given the pass count is already down 96% from its pre-rework
value, expect the *absolute* removable count here to be small (single
digits), so rank this mechanism's ceiling accordingly against A and B.

**Escape hatch / kill criterion**: provisional — this mechanism doesn't
have a settled implementation shape yet (that's the point of "measure
per-cause counts first"). If every C1-C4 counter comes back firing at
most 1-2 times per flip, the 14-pass floor is close to irreducible with
this scene's content — document that finding in the campaign log and
stop; don't invent a fix for a cause that measurement shows is already
minimal.

### Mechanism D — `vkCmdDrawMultiEXT` / `VK_EXT_multi_draw` (candidate-blocked)

**Status: verified blocked.** `VK_EXT_multi_draw` is defined in the
Vulkan headers vendored alongside the sibling MoltenVK checkout
(`Vulkan-Headers/include/vulkan/vulkan_core.h`) but has **zero** matches
in MoltenVK's own extension registry
(`MoltenVK/MoltenVK/Layers/MVKExtensions.def`, pinned commit
`096714a2954fc8e9db9daae97c426d7dd7f8a838` — the same pin
`scripts/build-moltenvk.sh` builds). It is not implemented. It is also
absent from xemu's own `required_device_extensions`/
`add_optional_device_extension_names` lists in `vk/instance.c` — nobody
has even asked MoltenVK for it. Re-verify:
`grep -n MULTI_DRAW <MoltenVK checkout>/MoltenVK/MoltenVK/Layers/MVKExtensions.def`
(no output = still blocked).

**Why this is ranked last regardless**: even if MoltenVK ships it later,
Mechanism A's index-buffer-concatenation approach already achieves the
same CPU-side call-count reduction *today* without the extension — a
multi-draw entry point would mostly save manual index-buffer bookkeeping,
not unlock a new capability. Revisit only after A ships and only if A's
implementation complexity (topology branching, restart-index insertion)
turns out to be a real maintenance burden.

**Kill criterion**: trivially met — blocked until MoltenVK implements the
extension. Re-run the grep above on any future MoltenVK pin bump
(`xemu-testing`'s MoltenVK maintenance note already says to re-run the
artifact hunt + fps A/B on a pin bump; add this grep to that checklist).

## Fenced-off wrong paths

Every one of these has already been tried, measured, and closed. Retrying
without new evidence that the *conditions* changed is not this campaign's
job — full stories in `xemu-failure-archaeology`.

- **CPU-side wait/fence elimination for GPU-bound scenes** — settled
  *twice* (byte-exact vertex-conflict refinement, then + per-flight
  vertex-RAM mirrors on top): both killed most `finish_vtx_dirty` CPU
  stalls, fps did not move either time, because the wait was real GPU
  frame time surfacing at a different call site. This is the standing
  conclusion this whole campaign exists downstream of — do not propose a
  variant of "remove another CPU-side wait" without first re-running
  Phase 2 and finding *new* evidence the frame is no longer GPU-bound.
- **Per-flight vertex-RAM mirrors** — README failed-experiments row: one
  128 MiB host mirror per flight slot, structurally removes cross-slot
  conflict waits, measured neutral on the attract reel (an earlier "-30%"
  read was traced to an invalid baseline parked on a 3-draws/flip static
  screen — the exact kind of unmeasured-scene-identity mistake Phase 0's
  gate exists to catch).
- **Sync-policy changes that bypass the deferred-report design** — the
  occlusion rework's own intermediate designs regressed hard before
  landing: in-pass queries alone with the old synchronous STALLED drain
  policy measured **6.86 ± 0.71 fps** (commit `6bfbc22863`'s own A/B, this
  is the git-verified number — session notes recorded it rounded as
  "6.6 fps"); an
  earlier submit-per-report design was recorded in session notes at
  ~11.2 fps with ~144 tiny per-flip submissions (session record, not
  independently git-verified — treat as directionally correct, not exact).
  `XEMU_REPORTS_SYNC=1` still exists specifically to reproduce the
  synchronous-drain regression class for comparison; don't build a new
  sync policy without reading `pgraph_vk_process_pending_reports`
  (`reports.c:259`) first.
- **Anything reintroducing per-draw render-pass teardown** — the
  pre-rework baseline was 376 passes/flip, "about one full tile
  load/store cycle per draw on Apple GPUs" (commit `6bfbc22863`'s own
  description) at 15.78 fps. Any change that makes `end_render_pass` fire
  once per draw again (e.g. a naive per-query reset, a naive per-draw
  surface flush) is this regression, not a new idea.
- **`EPI`-style check-skipping without a sentinel harness proof** — the
  DSP JIT's `EPI_NO_PC` (9.75% legitimate PC divergence missed) and
  round-4 `cur_inst` preset-skip (regressed on `op=0x001000`, caught by
  `XEMU_DSP_JIT_SENTINEL`) are the canonical example of "looked safe by
  inspection, wasn't." Mechanism B's candidates are today pure redundant
  *checks*, not skips — the moment anyone turns one into a skip
  ("don't re-scan dirty registers, we're sure nothing changed"), it needs
  the same sentinel-harness treatment before shipping, not code review
  alone.

## Validation and promotion protocol

1. **A/B measurement**: once a mechanism has a real `XEMU_*` flag,
   re-run `scripts/bench-savestate-ab.sh <snapshot> <FLAG_NAME> 3 75` (3
   interleaved pairs, 75 s each) per `xemu-testing`'s protocol — same
   binary, only the flag toggles.
2. **Acceptance thresholds**: apply `xemu-validation-and-qa`'s bar (static
   savestate scenes reproduce to ±0.02 fps; treat sub-0.5% deltas as
   noise-suspect and re-run; live-route comparisons need ≥3 pairs and a
   mean comparison given ±5 fps route variance).
3. **zpass-consumer visual soak** — specific to this campaign because
   Mechanism A operates immediately adjacent to occlusion-query
   boundaries (ground-truth row 11) and Mechanism C touches pass
   boundaries the query-rotation logic depends on. Run the visual
   artifact oracle (`xemu-testing`) on the same report-heavy Azurik scene
   for an extended soak, specifically watching for occlusion-driven
   effects (anything gated on zpass pixel counts) rendering wrong —
   this is the failure mode A2's obligation 3 exists to prevent, and a
   silent wrong-report-value bug will not show up as a crash.
4. **Cross-title smoke**: at minimum boot + a few minutes of normal play
   on Battlefield 2 Modern Combat, Conker Live & Reloaded, KOTOR, and Vexx
   (the corpus in `xemu-validation-and-qa`) — draw-batching changes are
   exactly the class of change that can be invisible on the one title
   used for A/B and wrong on another with different primitive-mode/report
   usage patterns.
5. **Documentation on promotion** (`xemu-change-control`,
   `xemu-docs-and-writing`): a README "Changes" bullet with the A/B
   numbers, the escape hatch name in the knob table, and — if the
   mechanism is later reverted — a failed-experiments row with the
   reason, per the same discipline every other shipped optimization in
   this fork has followed.

## Campaign log template

Copy this per session; append, don't overwrite.

```
### YYYY-MM-DD — Phase N

Command: <exact command run>
Scene / snapshot: <name, re-verified from xemu.toml>
Numbers: fps=___  draws/flip=___  passes/flip=___  fence_wait_ms=___  <new counters>=___
Comparison basis: <previous log entry / Phase 0 capture date>
Decision: <proceed to X / branch to Y / kill mechanism Z — cite the exact gate/threshold that fired>
Notes: <anything that didn't fit the expected envelope, even if not blocking>
```

## Provenance and maintenance

Every fact above was verified against `cf85e96597` (v0.9, 2026-07-04, tag
`v0.9`). Re-verify before trusting this skill on a later commit:

- draws/flip, passes/flip, fps, fence_wait envelope → re-run Phase 0; do
  not trust the numbers in this file past a few weeks of further
  optimization work.
- nsprof event/counter names → `grep -n "NSPROF_EV_\|NSPROF_" hw/xbox/nv2a/nsprof.h`
- the 7 `vkCmdDraw*`/`vkCmdDrawIndexed` call sites in `flush_draw` →
  `grep -n "vkCmdDraw" hw/xbox/nv2a/pgraph/vk/draw.c`
- `PipelineKey` shape / which registers gate it →
  `grep -n "regs\[5\]\|NV_PGRAPH_CONTROL_3 is intentionally omitted" hw/xbox/nv2a/pgraph/vk/renderer.h hw/xbox/nv2a/pgraph/vk/draw.c`
- `NUM_FLIGHT_SLOTS` / query pool sizing →
  `grep -n "define NUM_FLIGHT_SLOTS\|max_queries_in_flight = " hw/xbox/nv2a/pgraph/vk/renderer.h hw/xbox/nv2a/pgraph/vk/reports.c`
- push-constant budget still ≤ MoltenVK's limit →
  `grep -n MAX_UNIFORM_ATTR_VALUES_SIZE hw/xbox/nv2a/pgraph/vk/shaders.c` and
  `grep -n maxPushConstantsSize /Users/michaelsrouji/Documents/Xemu/tools/MoltenVK/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm`
  (adjust if your MoltenVK checkout lives elsewhere)
- `VK_EXT_multi_draw` still unimplemented in MoltenVK →
  `grep -n MULTI_DRAW /Users/michaelsrouji/Documents/Xemu/tools/MoltenVK/MoltenVK/MoltenVK/Layers/MVKExtensions.def`
  (same checkout note)
- Apple-path feature relaxations (`geometryShader`/`occlusionQueryPrecise`
  forced off) still in effect →
  `grep -n "F(geometryShader\|F(occlusionQueryPrecise" hw/xbox/nv2a/pgraph/vk/instance.c`
- snapshot shortcuts (F5/F6 names change over time) →
  `grep -A2 "\[general.snapshots.shortcuts\]" "$HOME/Library/Application Support/xemu/xemu/xemu.toml"`
- occlusion-rework baseline numbers cited in Fenced-off wrong paths →
  `git show --format='%H %s' -s 6bfbc22863` (immutable history; won't
  drift, but confirm the hash still resolves on this checkout)
