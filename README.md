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

### Tier 1: High Impact (measurable FPS/latency improvement)

| Optimization | Speedup | Description |
|---|---|---|
| **ARM64 Hard FPU** | 3-6x faster x87 ops | Fast bit-level floatx80-to-double conversion replaces QEMU's softfloat integer-only emulation. Branchless normal path (~6 integer ops per conversion). Every x87 instruction (FADD, FMUL, FDIV, FSQRT, FRNDINT, FIST, etc.) benefits. Hardware `sqrt()`, `rint()`, and `fesetround()` for FSQRT/FRNDINT/rounding mode. |
| **MetalFX Temporal Upscaling** | Better quality at lower GPU cost | Apple's ML-based upscaling from internal resolution (e.g., 1280x960) to 1920x1440 with temporal accumulation and anti-aliasing via `MTLFXTemporalScaler`. Halton(2,3) sub-pixel jitter sequence enables true temporal super-resolution reconstruction. |
| **True Frame Interpolation** | 30fps -> 60fps or 120fps | `MTLFXFrameInterpolator` (macOS 26+) generates intermediate frames with correct deltaTime. 2x mode = 1 interpolated frame (dt=0.5), 4x mode = 3 interpolated frames (dt=0.25, 0.5, 0.75). Deferred generation across sync calls for true 120fps. |
| **Texture Upload Batching** | Eliminates GPU sync per texture | Staging buffer sub-allocation (bump allocator on `BUFFER_STAGING_SRC`) allows multiple texture uploads to batch on the main command buffer. Flush guards prevent corruption between users. |
| **Texture Decode Parallelization** | Up to 24x throughput on M2 Ultra | Persistent `GThreadPool` distributes S3TC decompression, unswizzle, and format conversion across all CPU cores. Pool is created once at init and reused across frames (no per-texture thread pool allocation). Atomic batch-completion tracking via `DecodeTaskBatch`. |

### Tier 2: Medium Impact (reduces per-frame overhead)

| Optimization | Description |
|---|---|
| **Display Command Buffer Merge** | PVIDEO overlay upload + display render pass combined into 1 aux GPU submission instead of 2. Eliminates 1 `vkQueueSubmit` + `vkWaitForFences` per display frame. |
| **Surface Init on Main CB** | New surface layout transitions use the main command buffer via `pgraph_vk_begin_nondraw_commands` instead of a separate aux submission. |
| **GPU Compute YUV-to-RGBA** | PVIDEO overlay YUV conversion runs as a Vulkan compute shader dispatch instead of CPU `convert_texture_data`. |
| **GPU Compute Z-Order Unswizzle** | Surface upload unswizzle for 4bpp and 2bpp surfaces runs as Vulkan compute shaders with Morton address decode. 2bpp shader packs/unpacks 16-bit elements in uint SSBOs. |
| **Pipeline Dirty Tracking** | `vertex_state_dirty` flag replaces per-draw `memcmp` of vertex descriptions. Only marks dirty when descriptions actually change (compares before/after in vertex bind). |
| **Vertex Staging Bulk Copy** | Single `memcpy` when source and destination strides match (common case), eliminating per-vertex loop overhead. |
| **Default Vulkan on macOS** | `get_default_renderer()` prefers Vulkan over OpenGL on `__APPLE__` because the Vulkan path provides IOSurface zero-copy display. |
| **Staging Buffer Scaling** | `BUFFER_STAGING_SRC/DST` increased from 64 MiB to 256 MiB with persistent VMA mapping. Eliminates per-upload `vmaMapMemory`/`vmaUnmapMemory` overhead and reduces forced `pgraph_vk_finish` stalls from staging buffer exhaustion. |
| **Narrower Pipeline Barriers** | `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` replaced with `COLOR_ATTACHMENT_OUTPUT_BIT` / `VERTEX_INPUT_BIT | TRANSFER_BIT` in image transitions and draw submission. Reduces MoltenVK Metal fence overhead. |
| **O(1) Surface Lookup** | `GHashTable` keyed on `vram_addr` accelerates `pgraph_vk_surface_get` from O(n) QTAILQ scan to O(1) hash lookup. NULL-guarded fallback to linear scan during early init. |
| **APU Attenuation/Pitch LUTs** | Pre-computed 4096-entry attenuation and 65536-entry pitch lookup tables replace per-voice `powf(10, ...)` and `powf(2, ...)` calls in the audio voice processor hot path. |
| **CoreAudio Priority-Safe Lock** | `pthread_mutex` replaced with `os_unfair_lock` in CoreAudio IOProc callback, eliminating priority inversion on the real-time audio thread. |

### Tier 3: Low Impact / Quality of Life

| Optimization | Description |
|---|---|
| **STBI_NEON** | ARM NEON SIMD for stb_image JPEG/PNG decoding (IDCT, color conversion, resampling). |
| **fpng ARM64 CRC32** | Hardware CRC32 instructions (`__crc32d`/`__crc32w`/`__crc32b`) for PNG encoding. Processes 8 bytes per iteration. |
| **Display Uniform Caching** | 8 uniform locations (`display_size`, `line_offset`, PVIDEO params) resolved once at init via `resolve_display_uniform_locations()`, not per-frame string lookup. |
| **MoltenVK Environment Tuning** | `Info.plist` `LSEnvironment` sets: `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`, `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`, `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`, `MVK_CONFIG_FAST_MATH_ENABLED=1`, `MVK_CONFIG_RESUME_LOST_DEVICE=1`. |
| **Profile Counter Gating** | `nv2a_profile_inc_counter` stripped as no-op when `NV2A_VK_PERF_BUILD=1` (already enabled). Removes atomic increments from every draw/bind/upload/submit call. |
| **Conditional Surface Flush** | `invalidate_surface` only calls `pgraph_vk_finish` when `draw_time >= command_buffer_start_time` (surface was drawn in current CB), not unconditionally. |
| **Separate Compute Queue** | Probes for 2nd queue from same family at device creation. Falls back to single queue on MoltenVK (which only exposes `queueCount=1`). |
| **Debug Group Counter Gating** | `NV2A_VK_DGROUP_BEGIN/END` macros are complete no-ops when `DEBUG_VK=0`. Previously, the indent counter was incremented/decremented on every call (~55 hot-path uses) even with debug output disabled. |
| **Descriptor Set Pool Doubling** | Graphics and compute descriptor set arrays increased from 1024 to 2048, reducing forced `pgraph_vk_finish` flushes from descriptor pool exhaustion on complex scenes. |
| **Invalid Surface Pool Expansion** | `num_invalid_surfaces_to_keep` increased from 10 to 64, reducing `vmaCreateImage` calls for surface recycling on systems with ample memory. |
| **Scratch Image Skip (macOS)** | On `__APPLE__` at `surface_scale_factor == 1`, the per-surface scratch image allocation is skipped. Upload copies directly from staging buffer to the main image, bypassing the scratch->blit->main chain (AMD Windows driver workaround not needed on Apple Silicon). |
| **MetalFX Direct IOSurface Output** | On Apple Silicon unified memory, temporal upscaler and frame interpolator write directly to the IOSurface-backed shared texture, eliminating a redundant private->shared GPU blit per frame. |
| **MetalFX Thread Safety** | `os_unfair_lock` guards all MetalFX global mutable state against concurrent access between render and display threads. |
| **Flight Slot Infrastructure** | Command buffers, fences, semaphores, and framebuffers are organized into `NUM_FLIGHT_SLOTS` independent slots (currently N=1). Each slot has its own `main_cb`, `aux_cb`, `fence`, `semaphore`, and `framebuffers[50]`. Infrastructure is ready for N=2+ pipelined submission when descriptor set and staging buffer partitioning are implemented (see Failed Optimizations). |

### MoltenVK Compatibility Layer

| Change | Description |
|---|---|
| **VK_KHR_portability_subset** | Required extension explicitly enabled for Apple Silicon physical device enumeration. Without it, MoltenVK hides the device. |
| **Geometry Shader Relaxation** | `geometryShader` feature set to not-required on `__APPLE__`. CPU-side primitive emulation handles quads, line loops, triangle fans, and provoking vertex rotation. |
| **Hardware Depth Fallback** | Fragment shader in `psh.c` uses `gl_FragCoord.z` with `dFdx`/`dFdy` slope approximation when geometry shaders unavailable (MoltenVK). `use_hw_depth` flag in `PshState`. |
| **VK_EXT_metal_objects** | Enabled for IOSurface import/export. Used to create IOSurface-backed `VkImage` for zero-copy display and MetalFX integration. |
| **VK_EXT_provoking_vertex** | Workarounds for MoltenVK's incomplete provoking vertex support. Hardware register state used directly for vertex rotation detection. |

### macOS 26 Build Fixes

| Fix | Description |
|---|---|
| **Stale pkg-config paths** | `download-macos-libs.py` replaced `os.path.realpath` with `os.path.abspath`. Added `repair_pc_prefixes()` to fix stale `.pc` prefix lines automatically. |
| **Duplicate LC_RPATH** | macOS 26 `dyld` rejects duplicate rpath entries. `build.sh` strips all rpaths after `dylibbundler` and adds the single correct one. |
| **IOSurface bytesPerRow** | macOS 26 creates BGRA8 IOSurfaces with incorrect `bytesPerRow` at widths > ~1920px. MetalFX output capped to 1920px wide; `texture_from_iosurface` validates before Metal texture creation. |
| **Non-portable exit code** | `exit -1` changed to `exit 1` in `build.sh`. |

---

## Failed Optimizations (and Why)

| Attempt | What Happened | Root Cause |
|---|---|---|
| **Depth export via `vkExportMetalObjectsEXT`** | Deadlock on launch, regardless of thread or timing | MoltenVK's internal device mutex conflicts with the PFIFO thread's Vulkan state. The function cannot be safely called from any thread context in xemu's architecture. |
| **BQL event batching** | Deadlock on launch | QEMU's Big QEMU Lock (BQL) is part of a cooperative scheduling model. Holding it around the entire SDL event loop prevents timer callbacks, I/O handlers, and the vCPU thread from running. |
| **Deferred auxiliary fence** | Graphics corruption (shuffled textures) | Callers of `pgraph_vk_end_single_time_commands` depend on transfers completing synchronously. The staging buffer was overwritten by subsequent uploads before the GPU executed the pending copy. |
| **Texture upload on main CB (v1, without sub-allocation)** | Texture corruption | `BUFFER_STAGING_SRC` is shared. Without sub-allocation, each texture upload wrote from offset 0, overwriting previous data before GPU copied it. Fixed with bump allocator approach. |
| **floatx80 union overlay on ARM64** | Segfault on launch | x87 80-bit and IEEE 64-bit double have incompatible bit layouts. The union `double fval` field doesn't correspond to `low`/`high` fields on ARM64 (unlike x86_64 where `long double` IS x87 80-bit). |
| **TCG inline float ops on ARM64** | Segfault during boot ROM translation | TCG float operations (`tcg_gen_add_f64` etc. from `ops_fpu.h`) crash on the ARM64 TCG backend. Fixed by splitting `g_use_hard_fpu` (helper selection) from `g_use_hard_fpu_inline` (TCG inlining, disabled on ARM64). |
| **Separate compute queue** | No effect on MoltenVK | MoltenVK only exposes 1 queue family with `queueCount=1`. The infrastructure is in place but inactive until a driver supports multiple queues. |
| **Voice register cache** | Black screen on game load | `__thread` cache of 128-byte voice register blocks served stale data. Xbox hardware/software modifies voice registers outside the `voice_get_mask`/`voice_set_mask` paths (DMA engine, guest CPU MMIO, linked-voice chains), so the cache had no way to detect external writes. |
| **Surface upload bump allocator** | Black screen / texture corruption | `BUFFER_STAGING_SRC` is shared between texture uploads (main CB) and surface uploads (aux CB with single-time commands). Sub-allocating with offsets between the two CB paths caused the main CB's pending texture copies to reference staging data overwritten by surface uploads. |
| **SDL event BQL batching** | Deadlock on launch | Holding BQL around the entire `SDL_PollEvent` loop prevents QEMU's cooperative scheduling (timer callbacks, I/O handlers, vCPU thread). Identical to the previously documented failure. |
| **Flight slots N>1 (ring-of-fences)** | Frame clipping/corruption at N=2, worse at N=3 | Descriptor sets and `BUFFER_STAGING_SRC` are shared globally, not partitioned per flight slot. When the CPU advances to the next slot and resets `descriptor_set_index=0` / `buffer_offset=0`, the GPU may still be reading from those same descriptor sets or staging buffer regions from the previous submission. The flight slot infrastructure is in place (per-slot CBs, fences, semaphores, framebuffers) and works at N=1. To enable N=2+, descriptor sets need per-slot index ranges (split 2048 into 1024 per slot) and the staging buffer needs per-slot offset regions (split 256 MiB into 128 MiB per slot). |

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
5. Configures with Meson: `-O3`, thin LTO, `-mcpu=apple-m2`, Vulkan enabled, Cocoa disabled
6. Builds `qemu-system-i386` with `make -j<cores>`
7. Packages into `dist/xemu.app` with `dylibbundler`, MoltenVK bundling, icon generation, and codesigning

### Build flags (automatic)

| Flag | Purpose |
|---|---|
| `-O3` | Maximum optimization |
| `-Db_lto=true -Db_lto_mode=thin` | Thin LTO with caching |
| `-mcpu=apple-m2` | ARM64 Apple Silicon tuning |
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
metalfx_mode = 'temporal'
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
renderer = 'VULKAN'            # 'NULL', 'OPENGL', 'VULKAN'
                                # Vulkan required for MetalFX and IOSurface zero-copy

metalfx_upscale = true          # Legacy boolean (redundant if metalfx_mode is set)

metalfx_mode = 'temporal'       # 'off'     - No MetalFX upscaling
                                # 'spatial' - Single-frame ML upscaling (fast, softer)
                                # 'temporal'- Multi-frame ML upscaling (best quality,
                                #             temporal accumulation reduces aliasing)

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
hard_fpu = true                 # ARM64: Use native double for x87 FPU emulation
                                # 3-6x faster than softfloat. Disable if a game
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
| 1x | 640x480 | Temporal -> 1920x1440 | 2x=60fps, 4x=120fps | Best perf, good quality |
| 2x | 1280x960 | Temporal -> 1920x1440 | 2x=60fps, 4x=120fps | **Recommended** |
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
  [Hard FPU: native double for x87]
  [Parallel texture decode: GThreadPool]
  [GPU compute: unswizzle + YUV]
       |
       v
  Display IOSurface (1280x960)
  [VK_EXT_metal_objects zero-copy]
       |
       v
  MetalFX Temporal Upscaler
  (1280x960 -> 1920x1440)
  [Halton jitter, depth if available]
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

If a game has floating-point precision issues (very rare), disable the hard FPU:
```toml
[perf]
hard_fpu = false
```

---

## Upstream

- Based on [xemu](https://github.com/xemu-project/xemu) - Original Xbox Emulator
- MoltenVK compatibility from [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)
- MetalFX, frame interpolation, hard FPU, and all performance optimizations are original work in this fork
