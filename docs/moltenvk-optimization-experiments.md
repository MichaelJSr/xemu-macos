# MoltenVK optimization experiments (M2 Ultra audit — Tier 3 research)

Research record for two MoltenVK optimizations that came out of the
2026-07-21 deep audit of the fork's bundled MoltenVK on Apple Silicon
(M2 Ultra, macOS 26 Tahoe). Both are **deferred as research, not
landed** — each is parked behind an explicit bar below. Shipped MoltenVK
config and the audit's low-risk wins live in the
[optimization ledger](optimizations.md#moltenvk-runtime-config); this
document is the deep record for the two items that need code changes and
validation before they could graduate.

State: **2026-07-21.** Bundled driver = MoltenVK **v1.4.2-rc1**
(`98f35743`, reports 1.4.2), built `-O3 -mcpu=apple-m2`, arm64-only,
`MVK_USE_METAL_PRIVATE_API=0` (stock). Host = Apple M2 Ultra, 24 CPU
cores (16P+8E), 60-core GPU, Metal 4 / MSL 4.0, `Apple 8` GPU family.

---

## Audit framing (why only these two)

The bundled MoltenVK is already heavily tuned. The decisive workload
fact: the 60-core GPU is **massively underutilized** (~740–830
draws/flip is trivial for it) and the fork is CPU/driver-bound —
`roadmap.md` confirms the wall is guest-TCG throughput. So the only
MoltenVK changes that can matter reduce **CPU-side driver overhead** or
**shader-compilation stalls**, never GPU throughput.

Cross-referencing every `MVK_CONFIG_*` knob against
`MVKEnvironment.h` defaults found most already-optimal or already the
driver default:

- `USE_METAL_ARGUMENT_BUFFERS=1` — already the driver default
  (`MVKEnvironment.h:299`); the fork's explicit set is pin-drift
  insurance, not a delta.
- `USE_MTLHEAP` (unset) — defaults to `WHERE_SAFE`, which is `ALWAYS`
  on Apple GPUs; placement heaps already on.
- `VK_SEMAPHORE_SUPPORT_STYLE` (unset) — defaults to
  `METAL_EVENTS_WHERE_SAFE` → `MTLSharedEvent`, already used.
- A raised deployment target does **not** help: MoltenVK gates OS paths
  with runtime `mvkOSVersionIsAtLeast()` (`Common/MVKOSExtensions.h:50`,
  152 call sites, zero `@available`), which does not constant-fold.

Two low-risk wins shipped in the audit (see the ledger): the pin bump to
v1.4.2-rc1 and `LOG_LEVEL=1`. A third, `SHOULD_MAXIMIZE_CONCURRENT_-
COMPILATION=1`, was shipped, then **measured inert on this box** (see
§Live validation V1) and **reverted**. Its real value is gated behind
Experiment B — it should return only alongside a concurrent-compile
producer (prewarm).

Everything else worth doing needs code + validation. That is these two
experiments.

---

## Live validation (measured on this box, 2026-07-21)

Two probes were run directly against the hardware/driver to move these
claims from "source says" to "measured here." Reproduction steps are in
the appendix.

### V1 — the concurrent-compilation knob is inert on macOS 26 / M2 Ultra

`MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION` maps to
`-[MTLDevice setShouldMaximizeConcurrentCompilation:]`
(`MVKDevice.mm:2396`). A direct Metal probe:

```
device: Apple M2 Ultra
respondsTo maximumConcurrentCompilationTaskCount   = 1
respondsTo setShouldMaximizeConcurrentCompilation: = 1
BEFORE: maximumConcurrentCompilationTaskCount = 24
set shouldMaximizeConcurrentCompilation = YES
AFTER : maximumConcurrentCompilationTaskCount = 24
```

On macOS 26 the reported concurrency ceiling is **already 24 (= CPU
count) by default**, and the hint does not raise it. Combined with the
fact that xemu creates pipelines **strictly serially** on one thread
(Experiment B §1), the knob does nothing measurable here today. It may
still matter on macOS 14/15 (Metal historically throttled the default
lower) and becomes meaningful only once a concurrent-compile producer
exists (Experiment B, option a). It was therefore **reverted** from the
shipped config; it should return only alongside a prewarm producer.

### V2 — the private-API flag flips exactly the predicted capabilities on

Two MoltenVK dylibs were built from the **same** v1.4.2-rc1 source,
differing only in `MVK_USE_METAL_PRIVATE_API` (stock UUID
`4477256B…`, variant `3BC561D0…`), then probed via a bare instance
talking directly to each ICD:

| Capability (device-level) | stock `=0` | variant `=1` |
|---|---|---|
| `VkPhysicalDeviceFeatures.wideLines` | 0 | **1** |
| `VkPhysicalDeviceFeatures.logicOp` | 0 | **1** |
| `VK_EXT_provoking_vertex` | absent | **present** |
| `VK_EXT_primitive_topology_list_restart` | absent | **present** |
| `VK_EXT_non_seamless_cube_map` | absent | **present** |
| `VK_EXT_legacy_dithering` | absent | **present** |
| total advertised device extensions | 130 | **134** |

This confirms the runtime gating is real (not just a build-time
`#if`): without the flag these are genuinely absent from
`vkGetPhysicalDeviceFeatures` / `vkEnumerateDeviceExtensionProperties`,
so xemu could never opt into them against a stock dylib. The `+4`
extensions are exactly the four rows above. (MoltenVK's own startup
info-dump lists 154 "supported" extensions in both builds — that is the
build-time known set, *not* the advertised set; the probe's 130/134 is
authoritative.)

---

## Experiment A — build MoltenVK with `MVK_USE_METAL_PRIVATE_API=1`

### Mechanism

`MVK_USE_METAL_PRIVATE_API` is a **build-time** flag, default `0`
(`Common/MVKCommonEnvironment.h:112`). At runtime `mvkSetConfig`
force-ANDs the config bit with the compile flag
(`MVKEnvironment.cpp:80-81`), and the config member's default *is* the
build flag (`MVKEnvironment.h:328`). Consequences:

- Building with `=1` turns the private API **on by default at
  runtime**. The env var `MVK_CONFIG_USE_METAL_PRIVATE_API` can only
  force it *off* (or redundantly on) — **it is a no-op against a stock
  dylib.** The fork builds its own MoltenVK, so this is a knob it owns.
- This is primarily a **correctness/quality** lever, not a speed one.

### What it unlocks, and the underlying private Metal call

| Vulkan feature | Private Metal API used | Gate |
|---|---|---|
| `VK_EXT_provoking_vertex` (`provokingVertexLast`, per-pipeline, EDS3 mode) | `-[MTLRenderCommandEncoder setProvokingVertexMode:]` | `MVKDevice.mm:763/1292/716`, `MVKCommandEncoderState.mm:1204` |
| core `logicOp` | `MTLRenderPipelineDescriptor.isLogicOperationEnabled/logicOperation` (SPI) | `MVKDevice.mm:2765`, `MTLRenderPipelineDescriptor+MoltenVK.m:32-35` |
| core `wideLines` | `-[MTLRenderCommandEncoder setLineWidth:]` | `MVKDevice.mm:2769`, `MVKCommandEncoderState.mm:1198` |
| `VK_EXT_primitive_topology_list_restart` | `setOpenGLModeEnabled:` + `setPrimitiveRestartEnabled:index:` | `MVKRenderPass.mm:289`, `MVKCommandEncoderState.mm:1363` |
| `VK_EXT_non_seamless_cube_map` | `MTLSamplerDescriptor.forceSeamsOnCubemapFiltering` (SPI) | `MVKImage.mm:2536`, `MTLSamplerDescriptor+MoltenVK.m:28` |
| `VK_EXT_legacy_dithering` | `-[MTLRenderPassDescriptor setDitherEnabled:]` | `MVKRenderPass.mm:284` |
| fixed-function `sampleMask`/coverage | `MTLRenderPipelineDescriptor.sampleMask/sampleCoverage` (SPI) | `MVKPipeline.mm:2020` (already emulated in-shader without it) |

Not gated by this flag (so no benefit from it): depth clip/clamp
(`setDepthClipMode:` is public), sample locations, custom border color
(already used via `respondsToSelector`).

### Per-feature benefit to the NV2A renderer

| Feature | NV2A status today | Payoff | Priority |
|---|---|---|---|
| **wideLines** | Requested optionally (`instance.c:549`); dynamic line width used only if present (`draw.c:1425-1426`), else clamped to 1px (`draw.c:1278`) | **Correct wide/upscaled lines** — and the *build flag alone* enables it, xemu already probes for it. Zero xemu code change. | **Highest / cleanest** |
| **logicOp** | Registers decoded (`NV_PGRAPH_BLEND_LOGICOP*`) but **ignored**: `logicOpEnable` hard-`VK_FALSE` (`draw.c:985,1409`); GL→VK map is dead commented code (`vk/constants.h`) | Fixes silently-dropped **XOR cursors / INVERT** blends. Needs a small `draw.c` + register-wiring change | **High** |
| primitiveTopologyListRestart | Restart honored for strip/fan only; NV2A maps `TRIANGLES/LINES/QUADS`→list topologies where it can't (`draw.c:99,92,105`) | Targeted correctness for restart markers in list/quad draws | Moderate |
| provokingVertex | CPU index-rotation fallback (see §quantified) | Removes per-draw CPU work for flat first-vertex **triangle-lists** only | Marginal (see below) |
| legacyDithering | Honored on GL backend, **ignored on VK** | Parity only; barely perceptible | Low |
| nonSeamlessCubeMap | Cube maps work; no seamless control anywhere | Accuracy only; no waiting consumer | Low |
| sampleMask / sampleLocations / depthClip | Single-sample, supersampled AA, in-shader depth clip (`psh.c:1069-1078`) | **None** as architected | None |

### Provoking vertex, quantified

`draw_needs_primitive_emulation()` (`draw.c:122-157`) forces CPU
primitive emulation **unconditionally** for `LINE_LOOP`, `QUADS`,
`QUAD_STRIP` and for line-mode `POLYGON` — because Metal has no such
primitives. For `PRIM_TYPE_TRIANGLES` the **only** trigger is
provoking-vertex == FIRST (`draw.c:143-151`). So native
`VK_EXT_provoking_vertex` removes emulation for **exactly the
triangle-list-first-vertex case and nothing else**; quads/line-loops/
polygons still need CPU index expansion regardless.

Cost of the case it would remove: the rotation
(`build_emulated_indices_from_array/_from_elements`, `draw.c:294-310,
399-411`) emits each triangle `(v0,v1,v2)` as `(v1,v2,v0)`. There is
**no index-count inflation** for triangles (`draw.c:192-193`), but it
converts a non-indexed `vkCmdDraw` into a CPU-built index array +
`vkCmdDrawIndexed` (~`count` uint32 writes + an index-buffer upload per
affected draw), and it triggers on the register alone — **even for
Gouraud-shaded** lists where provoking vertex is visually irrelevant.
Hardware default is provoking=LAST (`pgraph.c:360-361`), so the cost
appears only while a title has selected FIRST; exact prevalence needs
profiling. On desktop this is solved in a geometry shader
(`glsl/geom.c`), which is why the Vulkan extension is not requested on
any platform today — the CPU path is the MoltenVK-only fallback.

Net: a few-percent CPU/bandwidth reclaim in affected scenes, not a
headline speedup, and the highest-effort + highest-risk item behind the
flag.

### Concrete change to adopt (if pursued)

1. **Build flag** — `scripts/build-moltenvk.sh`, append to the existing
   override: `OTHER_CFLAGS="-mcpu=${MCPU} -DMVK_USE_METAL_PRIVATE_API=1"`.
   (Verified sufficient: the V2 variant was built exactly this way.)
2. **Runtime (optional, for explicitness)** — `MVK_CONFIG_USE_METAL_-
   PRIVATE_API=1` in `ui/xemu.c` + `Info.plist` (kept in sync).
3. **wideLines** — nothing; already wired. Validate visually first.
4. **logicOp** — consume `NV_PGRAPH_BLEND_LOGICOP*`; set
   `logicOpEnable/logicOp` at `draw.c:1409` from the register; revive
   the GL→VK map in `vk/constants.h`.
5. **provokingVertex (separate, fallback-preserving)** — request
   `VK_EXT_provoking_vertex` + chain
   `VkPhysicalDeviceProvokingVertexFeaturesEXT{provokingVertexLast=TRUE}`
   in `instance.c` (mirror `custom_border_features`); make
   `draw_needs_primitive_emulation` return false for the TRIANGLES case
   when the extension is active; chain
   `VkPipelineRasterizationProvokingVertexStateCreateInfoEXT` in
   `create_pipeline`; **and add provoking mode to the pipeline-cache
   key** (`draw.c:1075-1087` deliberately excludes it today) or two
   draws differing only in provoking mode collide on a stale pipeline.
   **Keep the CPU rotation as a fallback — do not delete it.**

### Risk surface

Guard discipline is uniform: every private selector is behind
`-respondsToSelector:` (no `@available`, no unguarded dispatch). So the
failure mode on a future OS that drops an AGX SPI is a **silent no-op**,
not a crash — which for a renderer means **silent wrong output** (wrong
flat-shade colors, 1px lines, restart not applied) with no error and no
log. macOS 26 is in scope but the private selectors are validated only
empirically. `setLineWidth:` / `setProvokingVertexMode:` /
`setPrimitiveRestartEnabled:` are long-standing AGX SPI (lower risk);
`setOpenGLModeEnabled:` / `setDitherEnabled:` are the least documented
(higher risk). App Store disqualification is moot (GPL, self-signed).
Enabling the flag **immediately flips wideLines on** (xemu already
requests it) — a real line-rendering behavior change that must be in the
same validation pass. Re-validate on every MoltenVK pin bump and macOS
update.

### Verdict + bar

**Favorable, staged, fallback-preserving.** Land the flag purely for
**wideLines** first (free, visually verifiable in minutes), then wire
**logicOp**. Treat provoking vertex as a separate guarded follow-up that
layers on top of — never replaces — the CPU rotation. **Bar:** visual
A/B on the target OS confirming wideLines/logicOp correctness with zero
regression on a boot+game run; provoking vertex additionally gated on
profiling showing FIRST-mode triangle-list draws are a material CPU
cost. These are accuracy wins, not the speed the audit targeted — so
they clear on *correctness value*, not fps.

---

## Experiment B — async / non-blocking graphics-pipeline creation

### Current flow

Pipeline creation is **fully synchronous on the single PFIFO emulation
thread**, on the hot draw path. Chain:
`pfifo_thread` → `pgraph_method` → `pgraph_vk_flush_draw/draw_end`
(`draw.c:2988/2412`) → `begin_pre_draw` → `create_pipeline`
(`draw.c:1131`) → `vkCreateGraphicsPipelines` (`draw.c:1489`, blocking).
The invariant is explicit (`draw.c:530-532`: *"All pipeline creation
happens on this thread, so no external synchronization of the
VkPipelineCache is needed."*).

Two-tier cache makes steady state compile-free: an in-memory LRU
(`r->pipeline_cache`, 4096 entries) keyed by `PipelineKey`
(`renderer.h:71-87`), backed by a driver `VkPipelineCache` persisted to
disk (`pipeline_cache.bin`, load `draw.c:559-574`, flush at flips
`draw.c:534-551`). On an LRU hit the handle is reused with **zero
compile** (`draw.c:1186-1192`).

### Hitch cost

On a miss, `vkCreateGraphicsPipelines` runs the full
SPIR-V→MSL→MTLLibrary→MTLRenderPipelineState lowering on the PFIFO
thread. SPIRV-Cross runs single-threaded there; the Metal steps
`dispatch_async` then **block on a condition variable** until done
(`MVKSync.mm:455`, timeout `metalCompileTimeout` default `INT64_MAX`).
This does not corrupt an in-flight present (present is on the UI thread)
but **stalls production of the new frame and the flip ack**
(`renderer.c:320-347`), i.e. a frame-time spike / dropped frame. Misses
**serialize on one thread**, so entering a scene with N first-encounter
pipelines is an N× multi-frame hitch. xemu already instruments this:
`NSPROF_PIPELINE_GEN` / `NV2A_PROF_PIPELINE_GEN`.

Crucial nuance: the on-disk cache stores **compressed MSL, not Metal
binaries** (`MVKPipeline.mm:2631`). So a warm boot skips SPIR-V→MSL but
still pays a `newLibraryWithSource` + `newRenderPipelineState` on first
use each session (OS-function-cache-accelerated) — exactly the
streaming pain.

### Interaction with the concurrent-compile knob (and why it is inert today)

xemu issues exactly **one** `vkCreateGraphicsPipelines` at a time, so
the knob's inter-compile parallelism is never exercised, and V1 shows it
does not raise the ceiling on macOS 26 anyway. Any async design that
issues **concurrent** compiles is precisely what would make both the
knob and the idle cores earn their keep. They are complements, not
substitutes.

### Options

| Option | MoltenVK support (this pin) | Verdict |
|---|---|---|
| (a) Parallel prewarm at startup/idle | thread-safe: cache guarded by `_shaderCacheLock`, compilers `@synchronized(getMTLDevice())` | **Best if anything.** Persist the `PipelineKey` set from prior sessions; a worker recreates them concurrently into the shared `VkPipelineCache`; hand finished handles to the PFIFO thread for LRU insertion at flip boundaries. xemu-side caches must stay single-threaded. ~200–400 LOC. Moderate risk. |
| (b) `pipeline_creation_cache_control` + `FAIL_ON_COMPILE_REQUIRED` | **supported** (`MVKExtensions.def:164`); fail-fast confirmed (`MVKShaderModule.mm:326`, `MVKPipeline.mm:2385`) | **Rejected for live draws.** Dropping an NV2A draw for a frame can leave a render-to-texture surface stale and cascade into corruption — unacceptable for frame-accurate emulation. Keep only as a cheap "is-cached?" probe building block for (a). Also requires replacing `VK_CHECK` at `draw.c:1489` (it `__builtin_trap()`s on the positive `VK_PIPELINE_COMPILE_REQUIRED`). |
| (c) `VK_EXT_graphics_pipeline_library` | **absent** from `MVKExtensions.def` | Off the table without patching MoltenVK; poor fit for NV2A's monolithic dynamic-state pipelines anyway. |
| (d) Do nothing more | — | **Honest default.** Two-tier + on-disk cache already makes steady state and warm boots cheap. |

### Verdict + bar

**Ship (d) now.** Pursue **(a) parallel prewarm** only if
`NSPROF_PIPELINE_GEN` / `NSPROF_PRESENT_WAIT` profiling on target titles
shows first-encounter compile bursts are a material, recurring hitch
(not a one-time cold-boot cost the on-disk cache already amortizes).
**Bar:** a measured hitch profile that (a) would demonstrably flatten,
justifying ~200–400 LOC and the single-threaded-cache-mutation
discipline. This is the only *performance* follow-up of the two
experiments, and the one design that finally uses the concurrent-compile
knob.

---

## Appendix — reproducing the validation

Both probes are self-contained and need no Vulkan SDK.

**V1 (concurrent compilation)** — `clang -fobjc-arc -framework Metal
-framework Foundation ccprobe.m`: create `MTLCreateSystemDefaultDevice`,
read `maximumConcurrentCompilationTaskCount`, set
`shouldMaximizeConcurrentCompilation = YES`, read it again.

**V2 (private-API feature diff)** — build two dylibs from the same
MoltenVK checkout, one with `OTHER_CFLAGS="… -DMVK_USE_METAL_PRIVATE_-
API=1"` to an isolated `SYMROOT` (leaves `/usr/local` + `dist/`
untouched). Probe each by `dlopen`ing it, bootstrapping
`vkGetInstanceProcAddr`, creating a **bare** instance (no
portability-enumeration extension — that is a loader concept, absent
when talking to the ICD directly), then reading
`vkGetPhysicalDeviceFeatures` (wideLines, logicOp) and
`vkEnumerateDeviceExtensionProperties` (the four `VK_EXT_*`). The stock
vs. variant diff is the table in §Live validation V2.
