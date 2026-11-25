# MoltenVK optimization experiments

Research record from the 2026-07-21 audit of the bundled MoltenVK on an
M2 Ultra (24 CPU cores, 60-core GPU, `Apple 8` family, macOS 26). The
driver then was v1.4.2-rc1 (`98f35743`), built `-O3 -mcpu=apple-m2`,
stock `MVK_USE_METAL_PRIVATE_API=0`; releases now bundle the pin in
`build.sh` and local builds the one in `scripts/build-moltenvk.sh`.
Shipped MoltenVK settings are in the
[ledger](../optimizations.md), MoltenVK runtime config. The full
original write-up is in this file at `103c6b9187`.

| Item | State |
|---|---|
| A: build with `MVK_USE_METAL_PRIVATE_API=1` | Never built; a correctness lever, not speed |
| B (d): keep synchronous creation behind the two-tier cache | Shipped |
| B (a): pipeline prewarm | Shipped 2026-10-24 (`prewarm.c`, `d106c5ca25`; Apple-only, `6c37968b0c`; up to 4 workers, `d13fe7859c`). It rebuilds recorded keys at launch, destroys each pipeline and hands no handles to the LRU. The parallel compile of first-use miss bursts was closed by its scaling gate (Metal-cold 1.68x at 8 workers against 3x) |
| B (b): fail-fast creation that skips a draw (`pipeline_creation_cache_control` plus `FAIL_ON_COMPILE_REQUIRED`, supported by MoltenVK) | Rejected in 2026-07: a dropped draw can leave a render-to-texture surface stale. Adopting it also means replacing the fatal `VK_CHECK` in `create_pipeline`, which ends the process on the positive `VK_PIPELINE_COMPILE_REQUIRED`. `perf.async_pipeline_compile` (Better-xemu4macOS merge, `ccfdac39a5`, below) shares its cost, a skipped draw, not its mechanism |
| B (c): `VK_EXT_graphics_pipeline_library` | Absent from MoltenVK |
| `MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION` | Reverted: inert on macOS 26 (V1); untested with the prewarm workers |

## Audit framing

The GPU is far from busy (about 740-830 draws a flip); the fork is CPU
and driver bound. So only MoltenVK changes that cut CPU-side driver
overhead or compile stalls can matter. Most `MVK_CONFIG_*` knobs were
already optimal or the driver default: `USE_METAL_ARGUMENT_BUFFERS=1`
is the default (the fork sets it as pin-drift insurance), `USE_MTLHEAP`
defaults to `WHERE_SAFE` (always on Apple GPUs), and
`VK_SEMAPHORE_SUPPORT_STYLE` defaults to `MTLSharedEvent`. A higher
deployment target doesn't help: MoltenVK gates OS paths with runtime
`mvkOSVersionIsAtLeast()` (152 call sites), which doesn't constant-fold.

## Live validation (2026-07-21)

**V1, the concurrent-compilation hint is inert.** On macOS 26 the M2
Ultra reports `maximumConcurrentCompilationTaskCount = 24` (the CPU
count) before and after `setShouldMaximizeConcurrentCompilation:YES`.
It may still matter on macOS 14/15, and only with a producer of
concurrent compiles. Probe: `clang -fobjc-arc -framework Metal
-framework Foundation ccprobe.m` reading the count, setting the hint and
reading it again.

**V2, the private-API flag flips exactly the predicted features.** Two
dylibs from the same v1.4.2-rc1 source, the variant built to an
isolated `SYMROOT` so `/usr/local` and `dist/` stay untouched. Each was
`dlopen`ed and probed through a bare instance on the ICD (no
portability-enumeration extension, a loader concept) with
`vkGetPhysicalDeviceFeatures` and
`vkEnumerateDeviceExtensionProperties`:

| Capability | stock `=0` | `=1` |
|---|---|---|
| `wideLines`, `logicOp` | 0 | 1 |
| `VK_EXT_provoking_vertex`, `VK_EXT_primitive_topology_list_restart`, `VK_EXT_non_seamless_cube_map`, `VK_EXT_legacy_dithering` | absent | present |
| advertised device extensions | 130 | 134 |

The gating is real at runtime, so xemu can't opt in against a stock
dylib. (MoltenVK's startup dump lists 154 extensions in both builds;
that is the build-time set, not the advertised one.)

## Experiment A: `MVK_USE_METAL_PRIVATE_API=1`

The flag is build-time, default 0 (`MVKCommonEnvironment.h`); at runtime
`mvkSetConfig` ANDs the config bit with it, so building with 1 turns the
private API on by default and `MVK_CONFIG_USE_METAL_PRIVATE_API` can
only turn it off. It is a no-op against a stock dylib.

| Feature | Private Metal API | NV2A today | Value |
|---|---|---|---|
| `wideLines` | `setLineWidth:` | Requested optionally (`instance.c`); dynamic width only with it (`init_frame_dynamic_states`), else 1 px | Highest: correct wide and upscaled lines with no xemu change |
| `logicOp` | `isLogicOperationEnabled` / `logicOperation` (SPI) | `NV_PGRAPH_BLEND_LOGICOP*` decoded but ignored: `logicOpEnable = VK_FALSE`; `pgraph_blend_logicop_map` is commented out in `vk/constants.h` | High: fixes dropped XOR cursors and INVERT blends |
| list restart | `setOpenGLModeEnabled:`, `setPrimitiveRestartEnabled:index:` | Restart honored for strips and fans only | Moderate |
| provoking vertex | `setProvokingVertexMode:` | CPU index rotation | Marginal (below) |
| legacy dithering | `setDitherEnabled:` | Honored on GL, ignored on Vulkan | Low |
| non-seamless cube maps | `forceSeamsOnCubemapFiltering` (SPI) | No seamless control | Low |

Not gated by the flag: depth clip and clamp, sample locations, custom
border color. Sample mask and coverage are already emulated in-shader.

**Provoking vertex.** `draw_needs_primitive_emulation()` forces CPU
emulation for `LINE_LOOP`, `QUADS`, `QUAD_STRIP` and line-mode `POLYGON`
regardless (Metal lacks them); for `TRIANGLES` its only trigger is
provoking vertex FIRST. The rotation (`build_emulated_indices_from_*`)
emits `(v1,v2,v0)` with no index inflation, but turns a `vkCmdDraw` into
a CPU-built index array plus `vkCmdDrawIndexed`, even for Gouraud lists
where the provoking vertex doesn't matter. Hardware default is LAST
(`pgraph.c`), so the cost appears only while a title selects FIRST. Net:
a few percent of CPU in affected scenes at most, for the riskiest change.

**Adopting it:**

1. `scripts/build-moltenvk.sh`: `OTHER_CFLAGS="-mcpu=${MCPU}
   -DMVK_USE_METAL_PRIVATE_API=1"` (how V2's variant was built).
   Optionally `MVK_CONFIG_USE_METAL_PRIVATE_API=1` in both `ui/xemu.c`
   and `Info.plist`.
2. `wideLines`: nothing to wire; validate visually.
3. `logicOp`: set `logicOpEnable` / `logicOp` from the register and
   revive the map.
4. Provoking vertex, separately: request `VK_EXT_provoking_vertex` with
   `provokingVertexLast`, skip emulation for `TRIANGLES` when active,
   chain `VkPipelineRasterizationProvokingVertexStateCreateInfoEXT`, and
   add provoking mode to the pipeline key (it is excluded today, so two
   draws differing only in it would share a stale pipeline). Keep the CPU
   rotation as the fallback.

**Risk.** Every private selector is behind `-respondsToSelector:`, so an
OS that drops one gives silent wrong output (flat colors, 1 px lines, no
restart) rather than a crash. `setLineWidth:`, `setProvokingVertexMode:`
and `setPrimitiveRestartEnabled:` are long-standing AGX SPI;
`setOpenGLModeEnabled:` and `setDitherEnabled:` are the least known. The
flag turns `wideLines` on at once, a real behaviour change. Re-validate
on every MoltenVK pin bump and macOS update.

**Bar:** a visual A/B on the target OS confirming `wideLines` and
`logicOp` with no regression on a boot and game run; provoking vertex
additionally needs a profile showing FIRST-mode triangle lists are a
material CPU cost. These clear on correctness, not fps.

## Experiment B: pipeline creation

**Flow.** `pfifo_thread` -> `pgraph_method` -> `pgraph_vk_flush_draw` /
`pgraph_vk_draw_end` -> `begin_pre_draw` -> `create_pipeline` ->
`vkCreateGraphicsPipelines`, serialized by `pgraph.lock` (on the PFIFO
thread, or under icount on the inline pusher's thread). An in-memory LRU
(`r->pipeline_cache`, 4096 entries, keyed by `PipelineKey`) sits on a
driver `VkPipelineCache` persisted as `pipeline_cache.bin` and saved
off-thread at flips (`pipeline_cache_save_thread`, v0.15.1,
`42900e676d`). So the cache stays internally synchronized (never
`VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`), and a creation
can wait on a running save's serialization. A failed
`vkCreateGraphicsPipelines` is fatal (`VK_CHECK`, `pgraph_vk_fatal_error`;
v0.15, `3147640b9b`).

**Hitch.** A miss runs SPIR-V -> MSL -> MTLLibrary ->
MTLRenderPipelineState on the PFIFO thread; MoltenVK blocks on a
condition variable until Metal finishes (`metalCompileTimeout`, default
`INT64_MAX`). That stalls the new frame and the flip ack
(`pgraph_vk_flip_stall`), and N first-encounter pipelines cost N serial
compiles (`NSPROF_PIPELINE_GEN`). The disk cache stores compressed MSL,
not Metal binaries, so a warm boot still pays `newLibraryWithSource` and
`newRenderPipelineState` on first use each session.

**Async first-use compile** (`perf.async_pipeline_compile`, Settings >
Performance; `vk/async.c`; MoltenVK and KosmicKrisp only). A
first-encounter miss is built on one worker thread through
`pgraph_vk_build_draw_pipeline`, and the draw is skipped until the build
lands; the PFIFO thread installs the handle on drain. A key is built
synchronously 200 ms after submission, a failed submit falls back at
once, and no worker starts under icount. The worker never touches PGRAPH
state or the renderer's single-threaded caches. The cost is the one (b)
was rejected for: a skipped draw that wrote a render-to-texture surface
leaves it a frame stale until the retry.
