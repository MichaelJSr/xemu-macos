# xemu-macos: Optimized Xbox Emulator for Apple Silicon

A personal fork of [xemu](https://github.com/xemu-project/xemu) with comprehensive performance optimizations for Apple Silicon Macs running macOS 26+.

> **Note:** This is for my personal use. Optimizations are targeted at M2+ series Macs with macOS 26 (Tahoe) or later. Upstream xemu does not officially support macOS Vulkan -- this fork adds MoltenVK compatibility and Apple Silicon-specific performance work.

---

## Table of Contents

- [Optimizations](#optimizations)
- [Failed Optimizations](#failed-optimizations-and-why)
- [MoltenVK Setup](#moltenvk-setup)
- [Build Instructions](#build-instructions)
- [Configuration](#configuration)
- [Architecture](#architecture)
- [Troubleshooting](#troubleshooting)

---

## Optimizations

### High Impact

| Optimization | Description |
|---|---|
| **ARM64 Inline FPU** | Full inline TCG FPU: x87 ops emit native AArch64 FP instructions (FADD/FMUL/FDIV/FSQRT as single insns). floatx80↔double conversion as inline JIT code (~15 insns) instead of helper calls (~35-45 insns). Values cached in D-registers across TBs. Inline fucom/fucomi/fcomi comparisons, fnstsw, frndint, FPU constant loads (fldl2t/fldl2e/fldpi/fldlg2/fldln2), and fdecstp/fincstp (direct `fpstt` manipulation instead of helper calls). ~30% faster than the helper-call hard FPU path, ~4-8x faster than softfloat. |
| **MetalFX Temporal Upscaling** | ML-based temporal super-resolution via `MTLFXTemporalScaler`. Halton(2,3) jitter across 8 frames. Color-only accumulation (no depth/motion vectors). |
| **Frame Interpolation** | `MTLFXFrameInterpolator` (macOS 26+). 2x = 60fps, 4x = 120fps from 30fps source. Deferred generation during idle display syncs. |
| **Texture Upload Batching** | Bump allocator on `BUFFER_STAGING_SRC`. Multiple textures batch on main command buffer, eliminating per-texture GPU sync. |
| **Texture Decode Parallelization** | `GThreadPool` capped at `MIN(num_processors, 4)` for S3TC/unswizzle/conversion. `QemuEvent` for batch completion (no busy-wait). Mip levels ≤16×16 decoded inline (dispatch overhead exceeds decode cost at tiny sizes). |
| **GPU Compute Shaders** | YUV-to-RGBA and Z-order unswizzle as Vulkan compute dispatches. Workgroup size 64 on Apple Silicon (SIMD-aligned). 2bpp unswizzle shader uses bitwise ops (`& (width-1)`, `>> findMSB(width)`) instead of integer division for power-of-two widths. |
| **GPU Texture Unswizzle** | Non-compressed 2bpp/4bpp textures bypass the CPU thread pool entirely — raw swizzled data is copied to a staging buffer and unswizzled on the GPU via compute shader. Expected 20-40% reduction in texture upload latency for affected formats. |

### Medium Impact

| Optimization | Description |
|---|---|
| **Display Path** | PVIDEO + display merged into 1 GPU submission. IOSurface rebind, descriptor set, and uniform caching skip redundant per-frame calls. Single `memcpy` when PVIDEO row pitch is contiguous. |
| **Incremental UBO Dirty Tracking** | `uniform_copy` does `memcmp` before `memcpy` with per-layout `dirty` flag. Replaces full-buffer `fast_hash` that ran on every shader bind. |
| **Scoped Vertex RAM Barrier** | `flush_memory_buffer` computes actual dirty page range via `find_first_bit`/`find_last_bit`. Barrier scoped to that subregion instead of `VK_WHOLE_SIZE`. |
| **Pipeline Dirty Tracking** | `vertex_state_dirty` flag replaces per-draw `memcmp`. `render_pass_state_dirty` flag set only when surface bindings change, replacing per-draw struct rebuild + `memcmp`. |
| **Vertex Layout Fingerprinting** | `fast_hash` of active vertex descriptions replaces paired `memcpy` + `memcmp` of full arrays (~512 bytes of stack copies eliminated per draw). |
| **Surface Expiry Throttling** | `expire_old_surfaces` + `prune_invalid_surfaces` run every 8 frame ticks instead of every draw begin. Eliminates O(surfaces) scan per draw. |
| **Emulated Index Batching** | Primitive emulation (quads, line loops, etc.) batches all draw_arrays index runs into one staging allocation + one `vkCmdDrawIndexed`. Single-pass sizing (duplicate loop eliminated). |
| **Staging Buffers** | 512 MiB persistent VMA mapping. No per-upload map/unmap. |
| **Narrower Barriers + Coherent Elision** | `ALL_COMMANDS_BIT` replaced with precise stage flags. `vmaFlush`/`vmaInvalidate` skipped on Apple Silicon coherent memory (per-buffer `is_coherent` flag). All barriers scoped to exact byte ranges. `VK_WHOLE_SIZE` replaced with precise sizes in buffer memory barriers — reduces Metal barrier scope from 512 MiB to actual transfer size on MoltenVK. |
| **O(1) Surface Lookup** | `GHashTable` for exact match. Sorted range array with binary search for containment queries — O(log n) instead of O(n). |
| **Texture/Sampler Cache Split** | `TextureKey` split into image data key and `SamplerKey` for sampler state. Separate LRU caches for textures and samplers. Effectively doubles texture cache capacity for games reusing textures with different sampler states. |
| **Dynamic Blend/Depth Bias** | `NV_PGRAPH_BLENDCOLOR`, `NV_PGRAPH_ZOFFSETBIAS`, `NV_PGRAPH_ZOFFSETFACTOR` removed from pipeline key. Blend constants and depth bias set as Vulkan dynamic state every non-clear draw, eliminating pipeline cache misses from frequently-changing registers. Clear pipelines skip dynamic state (no blend/depth bias needed). |
| **APU VP Batch-Read** | Voice struct (128 bytes) `memcpy`'d to stack-local buffer at start of `voice_process`. All `voice_get_mask`/`voice_set_mask` calls read from the local buffer, eliminating per-field scatter-gather overhead. 20-30% VP frame time reduction. |
| **Flight Slot Pipelining (N=2)** | Two command buffer/fence/semaphore slots with full resource partitioning. CPU records slot 1 while GPU executes slot 0. |
| **Conditional Surface Flush** | `invalidate_surface` only flushes GPU when surface was drawn in current command buffer. |
| **APU Linear Resampler + RAM Fast Path** | `SRC_LINEAR` replaces `SRC_SINC_FASTEST` for voice pitch shifting (matches Xbox hardware, order-of-magnitude faster). All VP/DSP memory access (`voice_get_mask`/`voice_set_mask`, PCM fetch, SGE lookup, SSL reads, notifiers, scatter-gather DMA) uses shared inline `ram_ldl` with bounds-checked `ram_ptr` direct access and `ldl_le_phys` fallback. Envelope `powf` replaced with `expf` using precomputed log base. |
| **APU LUTs + NEON** | Attenuation (4096) and pitch (65536) lookup tables. NEON `float_to_24b_bulk` and `vaddq_f32` for DSP/VP hot paths. |
| **CoreAudio `os_unfair_lock` + trylock** | Replaces `pthread_mutex` in IOProc. Trylock outputs silence on contention. Buffer at 4096 samples (~85ms at 48kHz). |
| **Single-Submit Fast Path** | `pgraph_vk_finish` skips aux command buffer + semaphore when all staging buffers are empty and VRAM has no dirty pages. Single `VkSubmitInfo` instead of two. |
| **Descriptor Set Bind Skip** | `vkCmdBindDescriptorSets` skipped when the same descriptor set is already bound (`last_bound_descriptor_set_index` tracking). |
| **FPCR Caching Across TBs** | `gen_flcr` only emits `MSR FPCR` when guest rounding mode actually changes. Eliminates ~10-20 cycle pipeline stall per translation block. |
| **Frame Interpolation Sync Bypass** | Deferred interpolation generation decoupled from 8ms display sync gate. Produces smoother 4x (120fps) output. |

### Low Impact / Quality of Life

| Optimization | Description |
|---|---|
| **MetalFX Direct IOSurface** | Upscaler/interpolator write directly to IOSurface on unified memory. Single shared `MTLDevice` + `MTLCommandQueue` across spatial, temporal, and interpolation (refcounted). `os_unfair_lock` guards MetalFX state; released before `waitUntilCompleted` so GPU wait doesn't block other threads. Feature-support queries cached in static vars. |
| **MoltenVK Tuning** | Argument buffers, prefilled command buffers, async queue submits, fast-math, resume-lost-device. |
| **Pool Sizing** | Descriptor sets 8192, pipeline cache 4096, texture cache 8192, invalid surface pool 128. Sized for high-memory Apple Silicon systems. |
| **Scratch Image Skip** | At scale=1 on macOS, upload copies directly to main image (AMD workaround skipped). |
| **Counter/Debug Gating** | Profile counters and debug groups stripped as no-ops in release. Critical array bounds checks use `__builtin_unreachable()` for zero-cost optimization hints in perf builds (full diagnostic + `abort()` in debug). MetalFX/IOSurface `fprintf(stderr)` messages gated behind `METALFX_DPRINTF`/`DISPLAY_DPRINTF` macros (no-ops in release). |
| **O(1) Render Pass Lookup** | `GHashTable` with packed key replaces linear scan (~5 entries). |
| **VMA Budget Trimming** | Texture cache LRU eviction when allocation > 2 GiB and > 95% budget. Thresholds tuned for high-memory unified memory systems (e.g. 192 GB M2 Ultra). |
| **JIT Tightening** | `ld80f` NOP-copy removed (4 bytes saved per x87 reload). `flcr` lowering reduced from 6 to 5 instructions via RBIT bit-swap. Insertion sort replaces `qsort` for vertex sync arrays (N <= 16). `gen_stn_ptr` uses shift-left-4 instead of multiply-by-16 (`sizeof(FPReg)`). |
| **PFIFO/Vblank QoS** | `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)` pins both the PFIFO and vblank timer threads to Apple Silicon P-cores. |
| **PFIFO Timeout Safety** | `qemu_cond_wait` replaced with `qemu_cond_timedwait(1ms)` to bound worst-case wakeup latency from lost kick signals. |
| **GLSL Source Freeing** | `ShaderModuleInfo.glsl` freed immediately after SPIRV compilation. Prevents hundreds of MB waste with large shader caches (~50K entries). |
| **Surface Expiry State** | `last_expire_frame_time` moved from process-global `static` to `PGRAPHVkState` field, fixing stale state across save/restore. |
| **Build** | `-mcpu=native`, `-ffp-contract=fast` (enables FMA contraction on Apple Silicon), thin LTO, `-O3`. STBI_NEON, fpng CRC32. |
| **Dead Code Removal** | ~180 lines of `#if 0` blocks and commented-out code removed across 12 files: dead surface migration logic, disabled `vkCmdCopyImage` path, `usb_xid_handle_destroy` stub, alternate LPC implementation, unused `ilu_opcode_params`, dead `snorm_tex` assignment, stale `DriveInfo` path, commented shader cache lines, commented `memory_region_init_alias`, and `possibly_dirty_checked` dead flag. |

### MoltenVK Compatibility Layer

| Change | Description |
|---|---|
| **Portability Extensions** | `VK_KHR_portability_subset`, `VK_EXT_metal_objects` (IOSurface zero-copy), `VK_EXT_provoking_vertex` workarounds. |
| **No Geometry Shaders** | `geometryShader` not required on `__APPLE__`. CPU-side primitive emulation for quads, line loops, triangle fans, provoking vertex. Fragment shader depth fallback via `gl_FragCoord.z` + `dFdx`/`dFdy`. GS winding probe skipped when unavailable. |

### macOS 26 Build Fixes

| Fix | Description |
|---|---|
| **Stale pkg-config paths** | `download-macos-libs.py` replaced `os.path.realpath` with `os.path.abspath`. Added `repair_pc_prefixes()` to fix stale `.pc` prefix lines automatically. |
| **Duplicate LC_RPATH** | macOS 26 `dyld` rejects duplicate rpath entries. `build.sh` strips all rpaths after `dylibbundler` and adds the single correct one. |
| **IOSurface bytesPerRow** | macOS 26 creates BGRA8 IOSurfaces with incorrect `bytesPerRow` at widths > ~1920px. MetalFX output capped to 1920px wide; `texture_from_iosurface` validates before Metal texture creation. |
| **Non-portable exit code** | `exit -1` changed to `exit 1` in `build.sh`. |

### Bug Fixes

| Fix | Description |
|---|---|
| **MetalFX Spatial Deadlock** | Lock not released on nil texture early return in `metalfx_upscale()`. |
| **CoreAudio `va_list` UB** | `coreaudio_logerr` passed `va_list` to variadic `AUD_log`; changed to `AUD_vlog`. Dead lock wrappers removed. |
| **Image Layout Transition UB** | `assert` in `image.c` compiled out in release; uninitialized barrier stages. Replaced with `abort()`. |
| **Shader Uniform Leak** | `layout->allocation` not freed in `finalize_uniform_layout` (`glsl.c`). |
| **GMatchInfo Heap Corruption** | `g_free(match)` on `GMatchInfo*` in `main-menu.cc`; corrected to `g_match_info_free`. |
| **Texture Decode Condvar Race** | `GMutex`/`GCond` destroyed by waiter while last worker still inside `g_mutex_unlock` after signaling. Caused `pthread_cond_signal: Invalid argument` crash on level loads and infinite hang on save state. Replaced with `QemuEvent` (no lock held during signal). |
| **MetalFX Spatial Dead Code** | `g_spatial.pendingCB` never assigned in `metalfx_upscale()`, so the `waitUntilCompleted` guard in `metalfx_destroy_locked()` was dead code. Removed field and dead wait path. |
| **Redundant `vmaMapMemory`** | `upload_pvideo_to_cmd` (display.c) and `create_dummy_texture` (texture.c) called `vmaMapMemory`/`vmaUnmapMemory` on `BUFFER_STAGING_SRC` which was already persistently mapped at init. Replaced with direct use of `.mapped` pointer. |
| **APU VP/FE MMIO Data Race** | `vp_write` → `fe_method` wrote `d->regs[]`, `d->vp.*` (HRTF, SSL, submix headroom, filters) without holding `d->lock`, racing with the APU frame thread. Wrapped `fe_method` in `d->lock`; `voice_lock` refactored to `voice_lock_locked` (assumes lock held) to avoid recursive mutex deadlock. |
| **FCMOV Uninitialized FP Temp** | `FCMOVB`/`FCMOVNBE` etc. called `get_st0`/`get_stn` inside a conditional TCG block. When the preceding `FUCOMI` flushed all inline FP temps, the `ld80f` reload only executed on the taken branch, leaving the TCG temp undefined on the not-taken path. Subsequent instructions read garbage. Fixed by pre-loading both operands before the conditional branch. |
| **FIST/FISTP Rounding Mode** | Inline FPU's `FIST`/`FISTP` used AArch64 `FCVTZS` (truncate toward zero), but x87 `FIST`/`FISTP` round using the current control word rounding mode (default: round-to-nearest-even). Broke Azurik's pause menu D-pad navigation: the menu computes `atan2(vertical, horizontal)` of the combined input vector (sticks + D-pad), scales by `(180/π) * (1/90)` to map angles to direction indices (0=right, 1=up, 2=left, 3=down), then uses `FISTP` to quantize. Due to float imprecision in the atan2/multiply chain, the pre-quantization value for pure D-pad UP is `0.999…` (not exactly 1.0). Round-to-nearest correctly gives 1 (UP); truncation gives 0 (RIGHT). Fixed by adding `rint_f32`/`rint_f64` TCG ops backed by AArch64 `FRINTI` (round using FPCR) and x86 `FRNDINT`, inserted before `FCVTZS`/`FCVTZS` in the inline FIST path. |
| **MetalFX Interpolation Stale Depth** | `metalfx_interpolation_generate` cached depth IOSurface textures but never cleared them when depth inputs transitioned to NULL. Interpolator bound stale depth from a previous frame. Added `else if (!depthB/A)` branches to nil cached textures. |
| **MetalFX Spatial Init Leak** | If `create_iosurface_bgra` or `texture_from_iosurface` failed after the `MTLFXSpatialScaler` and Metal device/queue were created, the early return leaked those objects. `metalfx_destroy_locked` only ran when `initialized == true`, which was never set. All failure paths now call `metalfx_destroy_locked`. |
| **DecodeTaskBatch Mixed Sync Model** | `remaining` field declared `volatile int` but updated via `qatomic_dec_fetch` and initialized with plain store. Removed `volatile`; initialization uses `qatomic_set`. |
| **MetalFX `is_supported()` Device Leak** | `metalfx_is_supported()`, `metalfx_temporal_is_supported()`, and `metalfx_interpolation_is_supported()` each called `MTLCreateSystemDefaultDevice()` (retained) on every call without releasing. Cached result in static var; device now properly released after each probe. |
| **MetalFX Interpolation Init Leak** | If `MTLFXFrameInterpolator` creation failed after `MTLDevice` and `MTLCommandQueue` were allocated, the failure path returned without cleanup. Added `metalfx_interpolation_destroy_locked()` on failure, matching the spatial init pattern. |
| **MetalFX Temporal Init Leak** | If `newTemporalScalerWithDevice:` returned nil after `shared_metal_acquire`, the failure path leaked the shared device refcount and left `g_temporal.device` set. Added `metalfx_temporal_destroy_locked()` on failure. |
| **PVIDEO Flight-Slot Offset** | `upload_pvideo_to_cmd` wrote YUV data at staging buffer offset 0 regardless of `current_flight`. Flush, barrier, and copy also used offset 0. When `current_flight == 1`, GPU read stale data from slot 0's range. All offsets now based on `staging_buffer_base`. |
| **Bump Allocator Overflow** | `pgraph_vk_buffer_has_space_for` accounted for one alignment round-up but `append_to_buffer` applied per-element alignment. For `count > 1` with `alignment > 1`, the actual space consumed could exceed `buffer_limit`. Added `count` parameter with worst-case inter-element padding `(count-1)*(alignment-1)`. |
| **MetalFX Texture Cache No-Retry** | `texture_from_iosurface` failure still updated cached IOSurface pointer, preventing retry on subsequent frames with the same surface. All 8 cache sites (spatial, temporal, interpolation) now only update the cached pointer on success. |
| **Stale CGL Surface Pointer** | `destroy_current_display_image` released the IOSurface but did not clear `last_cgl_surface`/`last_cgl_width`/`last_cgl_height`. If a new IOSurface was allocated at the same address, the rebind check was skipped, showing stale content. |
| **APU GP/EP MMIO Read Race** | `gp_read`/`ep_read` read DSP memory without `d->lock`. Reads are intentionally lock-free: all values are 32-bit aligned and the BQL serializes guest MMIO dispatch. Locking reads caused severe audio dropouts by contending with the APU frame thread during voice processing. |
| **CoreAudio BadObjectError as Success** | `init_out_device` returned `0` (success) on `kAudioHardwareBadObjectError`/`kAudioHardwareBadDeviceError`, leaving the voice half-initialized. Now returns the error status and marks device as `kAudioDeviceUnknown`. |
| **VK_CHECK Undefined Behavior** | `__builtin_unreachable()` after `fprintf(stderr)` on Vulkan failure allowed the compiler to optimize away the error path entirely. Replaced with `__builtin_trap()` for deterministic crash. |
| **MetalFX Config Migration** | Legacy `metalfx_upscale` (bool) and `metalfx_mode` (enum) both existed. Users setting `metalfx_upscale = true` in TOML while `metalfx_mode` was "off" got no upscaling. On config load, `metalfx_upscale = true` now migrates to `metalfx_mode = spatial`. |
| **Snapshot Search Regex Injection** | User text embedded directly into regex pattern `(.*)%s(.*)` without escaping. Metacharacters in the search box broke the regex or caused expensive backtracking. Now escaped with `g_regex_escape_string`. |
| **Build LDFLAGS Missing Min Version** | `CFLAGS` included `-mmacosx-version-min` but `LDFLAGS` did not, causing the linker to not embed the correct `LC_BUILD_VERSION`. Added to `LDFLAGS`. |
| **VkFramebuffer Leak** | Per-flight `framebuffer_index` was never written during creation, so `destroy_flight_framebuffers` always iterated 0 times. Framebuffers leaked every flight cycle. Now saved into `flight[slot].framebuffer_index` before advancing. |
| **Scatter-Gather Bounds Off-by-One** | `assert(paddr + bytes_to_copy < ram_size)` used `<` instead of `<=`, rejecting valid DMA touching the final byte of RAM. |
| **VP `voice_buf` Use-After-Free** | `voice_should_mute()` triggered bare `return` in `voice_process`, skipping the `cleanup:` label that nulls the stack-local `voice_buf` pointer. Subsequent `voice_get_mask`/`voice_set_mask` calls for that voice dereferenced deallocated stack memory. Changed to `goto cleanup`. |
| **Stale Blend Constants / Depth Bias** | `vkCmdSetBlendConstants` and `vkCmdSetDepthBias` only called inside `if (must_bind_pipeline)` block, but the corresponding registers were removed from the pipeline dirty check. Blend/depth changes alone never triggered refresh. Moved to unconditional per-draw dynamic state, guarded by `!pg->clearing` (clear pipelines don't declare dynamic blend/depth). |
| **IOSurface Cache Key** | MetalFX texture caches compared `IOSurfaceRef` pointer values, which can be reused after release. Replaced with `IOSurfaceGetID()` for stable cache keys. Added `CFRetain`/`CFRelease` for proper lifetime management. |
| **Report Pool Overflow** | Missing bounds check on `r->report_pool_next` before indexing `r->report_pool`. Added overflow guard that triggers `pgraph_vk_finish(REPORTS_FULL)`. |
| **APU IRQ Register Race** | `update_irq` read `NV_PAPU_FECTL`, `NV_PAPU_IEN`, `NV_PAPU_ISTS` non-atomically while other threads wrote them. Changed to `qatomic_read` for all reads. |
| **Halton Y Jitter Duplicate** | MetalFX temporal upscaler's Halton Y jitter table had `-0.333f` at both index 1 and index 7, reducing effective temporal sampling quality. Fixed index 7. |
| **Double `NV2A_PROF_TEX_UPLOAD`** | Texture upload counter incremented at both function entry and inside the command block. Removed duplicate. |
| **`vk_mag_filter` Lookup** | Magnification filter incorrectly used the minification filter map. Changed to `pgraph_texture_mag_filter_vk_map`. |
| **EP FIFO Divide-by-Zero** | `cur % (end - base)` with `end == base` was UB. Added guard to clamp and return early. |
| **EP Silence Buffer Overread** | `assert(len <= sizeof(ep_silence))` compiled out in release. Replaced with runtime clamp. |
| **Query Pool Error Handling** | `vkGetQueryPoolResults` errors other than `VK_NOT_READY` silently ignored. Now logs error and zeroes results. |
| **CoreAudio Unknown Device** | `init_out_device` returned success when `outputDeviceID == kAudioDeviceUnknown`. Now returns `-1`. |
| **PVIDEO State Ordering** | `get_pvideo_state` called after `update_uniforms`, so push constants used previous frame's PVIDEO state. Reordered. |
| **`memcpy_image` Int Overflow** | `dst_stride * height` computed as `int` before widening to `size_t`. Cast to `size_t` prevents overflow at high scale factors. |
| **IOSurface Per-Frame Leak** | Upscaled and interpolated IOSurfaces created each frame were never released after GL texture bind. `CFRelease` added for both paths in the display loop. |
| **Clear Pipeline Dynamic State** | Clear pipelines created without `VK_DYNAMIC_STATE_DEPTH_BIAS` / `VK_DYNAMIC_STATE_LINE_WIDTH` but `vkCmdSetBlendConstants`/`vkCmdSetDepthBias` were called unconditionally, violating Vulkan spec. Dynamic state now skipped for clear draws; `has_dynamic_*` flags initialized in pipeline cache. |
| **ADPCM Bounds Check** | `memcpy` of ADPCM stream data from guest RAM without `ram_size` bounds check. Added guard + `memset` fallback. Stack buffer overflow guard added for `block_size > sizeof(adpcm_block)`. |
| **VP Multipass Voice List Cap** | Multipass voice linked-list walk had no iteration limit. Corrupted `next_voice` pointer could cause infinite loop. Capped at `MCPX_HW_MAX_VOICES`. |
| **VP Resample Failure Cleanup** | Resample failure in `voice_process` used `break` instead of `goto cleanup`, leaving partially-filled buffer and skipping `voice_buf` null-out. |
| **Vblank Timer Leak** | `display_finalize` leaked `vblank_timer` on the non-thread path. Added `timer_free` (which internally calls `timer_del`). |
| **Atomic `waiting_for_*` Flags** | `waiting_for_flip`, `waiting_for_nop`, `waiting_for_context_switch` read/written with plain loads/stores across threads. Changed to `qatomic_set`/`qatomic_read` for ARM memory ordering correctness. |
| **Depth-Stencil Upload Buffer** | After depth-stencil unpack, `upload_src_buffer` still pointed to the original staging buffer instead of the unpack buffer. GPU copied from wrong data. Barrier size also used `uploaded_image_size` instead of `unpacked_size`. Both fixed. |
| **USB HID `GET_REPORT` Overrun** | `memcpy(data, &s->in_state, s->in_state.bLength)` wrote `bLength` bytes into a `length`-byte buffer. When `bLength > length`, overran the host buffer. Changed to `memcpy(..., length)`. |
| **Pipeline Binding Stale Flag** | `pipeline_binding_changed` not cleared on pipeline cache hits, causing redundant `vkCmdBindPipeline` calls on subsequent draws. |

---

## Failed Optimizations (and Why)

| Attempt | Result | Root Cause |
|---|---|---|
| **Depth export (`vkExportMetalObjectsEXT`)** | Deadlock | MoltenVK's internal device mutex conflicts with PFIFO thread Vulkan state. Cannot safely call from any thread in xemu's architecture. |
| **BQL event batching** (x2) | Deadlock | Holding BQL around SDL event loop prevents QEMU cooperative scheduling. |
| **Deferred auxiliary fence** | Texture corruption | Staging buffer overwritten before GPU executed pending copy. |
| **Texture upload on main CB (v1)** | Texture corruption | Shared `BUFFER_STAGING_SRC` without sub-allocation; fixed with bump allocator. |
| **floatx80 union overlay (ARM64)** | Segfault | x87 80-bit and IEEE 64-bit have incompatible bit layouts on ARM64. |
| **Separate compute queue** | No effect | MoltenVK only exposes `queueCount=1`. Infrastructure in place but inactive. |
| **Voice register cache** | Black screen | `__thread` cache served stale data; Xbox HW modifies registers via DMA/MMIO outside cached paths. |
| **Surface upload bump allocator** | Corruption | Staging shared between texture (main CB) and surface (aux CB) uploads; offsets conflicted. |
| **Async MetalFX (`dispatch_semaphore`)** | Tearing | IOSurface consumed by GL before Metal finished writing. Writer must complete before reader binds. |
| **Render pass hash table (mid-struct)** | Segfault | Inserting `GHashTable*` field mid-struct shifted member offsets; stale `.o` files read wrong memory. Fixed by adding field at end of struct instead. |
| **Dirty-range VRAM flush** | Segfault (then reverted) | `bitmap_clear` before `flush_memory_buffer` zeroed bitmap before read. Minimal benefit on Apple Silicon coherent memory anyway. |
| **Depth export via CPU readback** | Temporal slower than spatial | Copied zeta depth aspect via `vkCmdCopyImageToBuffer` + CPU `memcpy` to R32Float IOSurface every frame. Added 2 GPU sync points + 18+ MiB CPU copy per frame. Quality improvement was subtle; performance cost was 2-5ms/frame. Correct approach requires IOSurface-backed VkImage rendered directly in the NV2A pipeline (avoids CPU round-trip). |
| **Texture decode GMutex/GCond** | Crash + hang | Waiter destroyed mutex/condvar while last worker still inside `g_mutex_unlock`. Stack-allocated `DecodeTaskBatch` memory reused after `decode_batch_wait` returned. Caused `pthread_cond_signal: Invalid argument` and save-state hangs. Fixed by switching to `QemuEvent` (atomic flag, no lock held during signal). |
| **Texture hash skip for large textures** | Massive GPU re-upload churn | Skipping `fast_hash` for textures >64 KiB and always re-uploading when the dirty bitmap fired. The VRAM dirty bitmap triggers frequently for unchanged textures (DMA/MMIO writes to nearby pages). The hash comparison was the critical guard preventing redundant multi-MB texture re-uploads every frame. CPU hash cost (~µs) is far cheaper than a GPU staging+copy+upload cycle. |
| **HRTF NEON vectorization** | Slower than scalar | Manual NEON intrinsics for 31-tap FIR convolution, coefficient smoothing, and normalization. The gather-then-FMA pattern for convolution (pre-loading circular buffer into linear stack array) prevented the M2's out-of-order engine from overlapping load latency with multiply-accumulate. Coefficient smoothing was called per-sample (32x/frame/voice) — NEON setup overhead didn't amortize at 31 elements. Clang `-O3 -mcpu=native` auto-vectorizes these loops more effectively. |
| **Always-on bounds checks in perf builds** | ~5-10% GPU pipeline regression | Replacing `nv2a_vk_assert` (compiled to `((void)0)`) with `G_UNLIKELY(!(x)) abort()` on ~25 hot-path array bounds checks (pipeline creation, texture binding, surface ops). Even with cold-path hints, the branches added instruction cache pressure in tight per-draw-call loops. Fixed by using `__builtin_unreachable()` instead (zero instructions emitted, compiler optimization hint only). |
| **Async MetalFX (double-buffered IOSurface)** | Segfault (first frame) | Two output IOSurfaces: write to back while presenting front, swap next frame. First `metalfx_get_output_surface` returned the uninitialized back buffer (never rendered to). Needs full display pipeline lifecycle integration to handle the bootstrapping frame. |
| **Surface upload skip finish** | Black screen / stale state | Removed the unconditional `pgraph_vk_finish` in surface upload when `!in_command_buffer`. Although the GPU drain is a no-op when idle, `pgraph_vk_finish` also runs essential cleanup (pending reports processing, compute descriptor reset, staging offset reset). Skipping it caused stale compute descriptor indices and missing report processing. |
| **Partial descriptor set writes** | N/A (architecturally infeasible) | Each draw consumes a fresh descriptor set that starts empty, so all bindings (UBOs + textures) must always be written. Partial writes only work with update-after-bind or separate descriptor sets for UBOs vs textures — a larger architectural change. |
| **Two-level texture quick hash** | Black screen during FMV/logo | Quick hash sampled 192 bytes (head/mid/tail via XOR of three XXH3) to filter false dirty-bitmap triggers before full hash. For video textures with structured layouts (YUV, black borders, swizzled data), the sampled regions matched across frames while actual content changed, causing texture uploads to be silently skipped. Full-content hash is the only reliable guard. |

---

## MoltenVK Setup

MoltenVK is the Vulkan-to-Metal translation layer that enables xemu's Vulkan renderer on macOS. This fork handles MoltenVK automatically.

### How it works

1. **`build.sh`** calls `scripts/download-macos-libs.py` which downloads pre-built MacPorts packages including MoltenVK (`libMoltenVK.dylib`)
2. The library is downloaded to `macos-libs/<arch>/opt/local/lib/`
3. During packaging (`package_macos`), `dylibbundler` copies all dylib dependencies into the app bundle
4. MoltenVK is separately bundled at `dist/xemu.app/Contents/Libraries/<arch>/libMoltenVK.dylib`
5. At runtime, Volk (the Vulkan loader used by xemu) loads MoltenVK via `dlopen`

### MoltenVK configuration

This fork sets optimal MoltenVK runtime configuration via `Info.plist` `LSEnvironment`:

```xml
<key>LSEnvironment</key>
<dict>
    <key>MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS</key>
    <string>1</string>
    <key>MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS</key>
    <string>2</string>
    <key>MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS</key>
    <string>0</string>
    <key>MVK_CONFIG_FAST_MATH_ENABLED</key>
    <string>1</string>
    <key>MVK_CONFIG_RESUME_LOST_DEVICE</key>
    <string>1</string>
</dict>
```

| Variable | Value | Effect |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | 1 | Reduces descriptor binding overhead by using Metal argument buffers |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | 2 | Prefills Metal command buffers at queue submit time for lower latency |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | 0 | Enables asynchronous queue submission for better GPU pipelining |
| `MVK_CONFIG_FAST_MATH_ENABLED` | 1 | Enables Metal shader fast-math for improved GPU shader throughput |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | 1 | Prevents device-lost crashes from transient GPU errors |

### MoltenVK compatibility changes in the codebase

The Vulkan renderer includes several MoltenVK-specific workarounds:

- **`instance.c`**: `VK_KHR_portability_subset` and `VK_KHR_portability_enumeration` extensions
- **`draw.c`**: CPU-side primitive emulation when geometry shaders unavailable (`draw_needs_primitive_emulation`, `build_emulated_indices_*`)
- **`shaders.c`**: Geometry shader module skipped when `!r->supports_geometry_shaders`
- **`psh.c`**: Hardware depth fallback using `gl_FragCoord.z` instead of geometry-shader-computed depth
- **`display.c`**: IOSurface sharing via `VK_EXT_metal_objects` for zero-copy display

### No manual MoltenVK installation needed

You do **not** need to install MoltenVK separately. The build system downloads it automatically. If you want to use a custom MoltenVK build, place `libMoltenVK.dylib` in `macos-libs/<arch>/opt/local/lib/` before building.

---

## Build Instructions

### Prerequisites

- **macOS 26.0** (Tahoe) or later
- **Xcode Command Line Tools** with macOS 26.4+ SDK
  ```bash
  xcode-select --install
  ```
- **Apple Silicon Mac** (M2 or later recommended; M1 should also work)
- **Python 3.10+** (included with Xcode CLT)
- **pkg-config** (via Homebrew):
  ```bash
  brew install pkg-config
  ```

### Build Steps

```bash
# 1. Clone with submodules
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos

# 2. Switch to the optimizations branch
git checkout macos-optimizations

# 3. Initialize all submodules (including nested ones)
git submodule update --init --recursive

# 4. Build (release, optimized for Apple Silicon)
./build.sh

# 5. The built app bundle is at:
ls -la dist/xemu.app

# 6. Run
open dist/xemu.app
# or directly:
./dist/xemu.app/Contents/MacOS/xemu
```

### What `build.sh` does

1. Detects macOS/ARM64, selects macOS 14.0 minimum deployment target
2. Finds the newest macOS SDK (requires >= 14.0)
3. Downloads MacPorts dependencies (SDL3, glib2, libsamplerate, pixman, epoxy, pcap, slirp, libusb, MoltenVK) via `scripts/download-macos-libs.py`
4. Sets `PKG_CONFIG_LIBDIR` to the downloaded libraries
5. Configures with Meson: `-O3`, thin LTO, `-mcpu=native`, Vulkan enabled, Cocoa disabled
6. Builds `qemu-system-i386` with `make -j<cores>`
7. Packages into `dist/xemu.app` with `dylibbundler`, MoltenVK bundling, icon generation, and codesigning

### Build flags (automatic)

| Flag | Purpose |
|---|---|
| `-O3` | Maximum optimization |
| `-Db_lto=true -Db_lto_mode=thin` | Thin LTO with caching |
| `-mcpu=native -ffp-contract=fast` | ARM64 Apple Silicon tuning + FMA contraction |
| `-DXBOX=1` | Xbox emulation mode |
| `-DVK_USE_PLATFORM_METAL_EXT` | MoltenVK/Metal Vulkan platform |
| `-mmacosx-version-min=14.0` | Minimum macOS version |

### Clean build

If you encounter issues, do a clean build:

```bash
rm -rf macos-libs/ macos-pkgs/ build/ dist/
./build.sh
```

---

## Configuration

### In-App Settings

Most settings added by this fork are accessible directly in the xemu UI:

**Display tab** (Settings > Display):
- **MetalFX Mode** dropdown: Off / Spatial / Temporal
- **Frame Interpolation** dropdown: Off / 2x (60fps) / 4x (120fps)
- **Internal resolution scale**: 1x through 10x
- **Backend**: Null / OpenGL / Vulkan

**General tab** (Settings > General):
- **Hard FPU emulation** toggle: Enable/disable ARM64 hardware FPU (requires restart)

**View menu** (quick access when menu bar is visible):
- **MetalFX** dropdown
- **Interpolation** dropdown
- Plus existing Backend, Display Mode, Filter Method, Aspect Ratio

### xemu.toml Reference

The config file is at `~/Library/Application Support/xemu/xemu/xemu.toml`. Most settings can be changed in-app, but the toml allows manual fine-tuning. Changes take effect on next launch unless noted.

#### Recommended config for Apple Silicon

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'
frame_interpolation = '4x'

[display.quality]
surface_scale = 2

[display.window]
fullscreen_on_startup = true
fullscreen_exclusive = true
startup_size = '1920x1080'

[display.ui]
fit = 'stretch'
```

#### Complete xemu.toml reference (our additions)

```toml
# ============================================================
# Display settings
# ============================================================
[display]
renderer = 'VULKAN'            # 'NULL', 'OPENGL', 'VULKAN' (default: VULKAN)
                                # Vulkan required for MetalFX and IOSurface zero-copy

metalfx_upscale = true          # Legacy boolean (redundant if metalfx_mode is set)

metalfx_mode = 'spatial'        # 'off'     - No MetalFX upscaling
                                # 'spatial' - Single-frame ML upscaling (recommended,
                                #             fast and sharp with minimal artifacts)
                                # 'temporal'- Multi-frame ML upscaling (higher quality
                                #             but can introduce ghosting artifacts)

frame_interpolation = '4x'      # 'off' - No interpolation, native game framerate
                                # '2x'  - True 60fps: 1 interpolated frame (dt=0.5)
                                # '4x'  - True 120fps: 3 interpolated frames
                                #         (dt=0.25, 0.5, 0.75) matching 120Hz panels

# ============================================================
# Display quality
# ============================================================
[display.quality]
surface_scale = 2               # Internal rendering resolution multiplier
                                # 1 = 640x480 (native Xbox, lowest GPU load)
                                # 2 = 1280x960 (recommended: good balance)
                                # 3 = 1920x1440 (high quality)
                                # 4 = 2560x1920 (sharpest, but MetalFX skipped)

# ============================================================
# Vulkan-specific settings
# ============================================================
[display.vulkan]
preferred_physical_device = 'Apple M2 Ultra'  # GPU selection (auto-detected)

# ============================================================
# Window settings
# ============================================================
[display.window]
fullscreen_on_startup = true    # Launch directly into fullscreen
fullscreen_exclusive = true     # Use exclusive fullscreen mode (better latency)
startup_size = '1920x1080'      # Initial window size before fullscreen
                                # Options: '640x480', '1280x720', '1920x1080',
                                #          '2560x1440', '3840x2160', etc.
vsync = true                    # Sync to display refresh (reduces tearing)

# ============================================================
# UI settings
# ============================================================
[display.ui]
show_menubar = false            # Show menu bar (press F10 to toggle)
fit = 'stretch'                 # 'center', 'scale', 'stretch'
                                # stretch fills the window (breaks aspect ratio)
                                # scale preserves aspect ratio with black bars
scale = 2                       # UI element scale (1 or 2)

# ============================================================
# Performance settings
# ============================================================
[perf]
hard_fpu = true                 # ARM64: Inline FPU — x87 ops as native AArch64 FP
                                # instructions with floatx80 conversion as JIT code.
                                # ~4-8x faster than softfloat. Disable if a game
                                # has floating-point precision issues (very rare).
                                # Requires restart to take effect.

cache_shaders = true            # Cache compiled shaders to disk to reduce stutter

# ============================================================
# Audio settings
# ============================================================
[audio]
use_dsp = true                  # Enable Xbox audio DSP emulation
volume_limit = 0.75             # Master volume (0.0 to 1.0)

# ============================================================
# System settings
# ============================================================
[sys]
mem_limit = '128'               # Xbox RAM size (always '128' for standard Xbox)

[sys.files]
bootrom_path = '/path/to/mcpx_1.0.bin'           # MCPX boot ROM
flashrom_path = '/path/to/bios.bin'               # Xbox BIOS
eeprom_path = '~/Library/.../eeprom.bin'          # EEPROM (auto-created)
hdd_path = '/path/to/xbox_hdd.qcow2'             # Xbox HDD image
dvd_path = '/path/to/game.iso'                    # Game disc image
```

### Settings interaction table

| Scale | Display Size | MetalFX | Interpolation | Effective Output |
|---|---|---|---|---|
| 1x | 640x480 | Spatial -> 1920x1440 | 2x=60fps, 4x=120fps | Best perf, good quality |
| 2x | 1280x960 | Spatial -> 1920x1440 | 2x=60fps, 4x=120fps | **Recommended** |
| 3x | 1920x1440 | No upscale needed | 2x=60fps, 4x=120fps | High quality, no upscale benefit |
| 4x | 2560x1920 | Skipped (>1920 wide) | Skipped | Sharpest textures, native framerate only |

---

## Architecture

```
Xbox Game (30fps)
       |
       v
  NV2A Vulkan Renderer
  (surface_scale = 2x -> 1280x960)
  [Inline FPU: x87 as native AArch64 FADD/FMUL/etc]
  [Parallel texture decode: GThreadPool]
  [GPU compute: unswizzle + YUV]
       |
       v
  Display IOSurface (1280x960)
  [VK_EXT_metal_objects zero-copy]
       |
       v
  MetalFX Spatial Upscaler
  (1280x960 -> 1920x1440)
  [Single-frame ML upscaling]
       |
       v
  Frame Interpolator (macOS 26+)
  [4x mode: dt=0.25, 0.5, 0.75]
  [Deferred generation per sync call]
       |
       v
  GL TEXTURE_RECTANGLE
  -> SDL Window (fullscreen 4K 120Hz)
```

---

## Troubleshooting

### Build fails with "sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T"

The pkg-config paths are stale. Delete cached libraries and rebuild:
```bash
rm -rf macos-libs/ macos-pkgs/ build/
./build.sh
```

### Crash on launch with "duplicate LC_RPATH"

This build already fixes the duplicate rpath issue. If using an older build, rebuild from scratch.

### MetalFX not activating

- Ensure `metalfx_mode = 'temporal'` or `'spatial'` in xemu.toml
- MetalFX requires the Vulkan renderer (`renderer = 'VULKAN'`) and `VK_EXT_metal_objects`
- At `surface_scale = 4`, the display is 2560x1920 which exceeds the 1920px safe limit, so MetalFX is skipped
- Check the xemu output log for `MetalFX: Temporal upscaler initialized` or `MetalFX: Skipping IOSurface`

### Game crashes when changing internal scaling

A safety guard prevents most crashes but may show a momentary black frame. This is a pre-existing race condition in xemu's hot-resize path where the surface pointer can be stale during scale factor changes.

### Frame interpolation not working

- Requires macOS 26.0+ (`MTLFXFrameInterpolator` API)
- Requires `frame_interpolation = '2x'` or `'4x'`
- Only works when MetalFX upscaling is active (display width <= 1920px)
- Check log for `MetalFX: Frame interpolator initialized`

### FPU-related game issues

The inline FPU uses IEEE double (52-bit mantissa) instead of x87 extended precision (64-bit mantissa). Most games are unaffected, but rare precision-dependent code paths can behave differently.

If a game has floating-point precision issues (very rare), disable the hard FPU:
```toml
[perf]
hard_fpu = false
```

---

## Upstream

- Based on [xemu](https://github.com/xemu-project/xemu) - Original Xbox Emulator
- MoltenVK compatibility from [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)
- MetalFX, frame interpolation, inline FPU, and all performance optimizations are original work in this fork
