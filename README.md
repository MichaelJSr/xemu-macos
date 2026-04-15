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
| **ARM64 Hard FPU** | Bit-level floatx80-to-double replaces softfloat (~6 integer ops vs dozens). Hardware `sqrt()`, `rint()`, `fesetround()`. 3-6x faster x87 ops. |
| **MetalFX Temporal Upscaling** | ML-based temporal super-resolution via `MTLFXTemporalScaler`. Halton(2,3) jitter across 8 frames. Color-only accumulation (no depth/motion vectors). |
| **Frame Interpolation** | `MTLFXFrameInterpolator` (macOS 26+). 2x = 60fps, 4x = 120fps from 30fps source. Deferred generation during idle display syncs. |
| **Texture Upload Batching** | Bump allocator on `BUFFER_STAGING_SRC`. Multiple textures batch on main command buffer, eliminating per-texture GPU sync. |
| **Texture Decode Parallelization** | `GThreadPool` across all CPU cores for S3TC/unswizzle/conversion. `QemuEvent` for batch completion (no busy-wait). |
| **GPU Compute Shaders** | YUV-to-RGBA and Z-order unswizzle as Vulkan compute dispatches. Workgroup size 64 on Apple Silicon (SIMD-aligned). |

### Medium Impact

| Optimization | Description |
|---|---|
| **Display Path** | PVIDEO + display merged into 1 GPU submission. IOSurface rebind, descriptor set, and uniform caching skip redundant per-frame calls. |
| **Pipeline Dirty Tracking** | `vertex_state_dirty` flag replaces per-draw `memcmp`. |
| **Staging Buffers** | 256 MiB staging with persistent VMA mapping. Eliminates per-upload map/unmap and reduces forced GPU flushes. |
| **Narrower Barriers** | `ALL_COMMANDS_BIT` replaced with precise stage flags. Reduces MoltenVK Metal fence overhead. |
| **O(1) Surface Lookup** | `GHashTable` keyed on `vram_addr` replaces O(n) QTAILQ scan. |
| **Conditional Surface Flush** | `invalidate_surface` only flushes GPU when surface was drawn in current command buffer. |
| **APU LUTs + NEON** | Attenuation (4096) and pitch (65536) lookup tables. NEON `float_to_24b_bulk` and `vaddq_f32` for DSP/VP hot paths. |
| **CoreAudio `os_unfair_lock`** | Replaces `pthread_mutex` in IOProc, eliminating real-time thread priority inversion. |

### Low Impact / Quality of Life

| Optimization | Description |
|---|---|
| **MetalFX Direct IOSurface** | Upscaler/interpolator write directly to IOSurface on unified memory. `os_unfair_lock` guards all MetalFX state. |
| **MoltenVK Tuning** | Argument buffers, prefilled command buffers, async queue submits, fast-math, resume-lost-device. |
| **Pool Sizing** | Descriptor sets 2048, invalid surface pool 64. Reduces forced flushes. |
| **Scratch Image Skip** | At scale=1 on macOS, upload copies directly to main image (AMD workaround skipped). |
| **Counter/Debug Gating** | Profile counters and debug groups stripped as no-ops in release. |
| **O(1) Render Pass Lookup** | `GHashTable` with packed key replaces linear scan (~5 entries). |
| **VMA Budget Trimming** | Texture cache trimmed when allocation > 512 MiB and > 90% budget. Prevents long-session OOM. |
| **Build** | `-mcpu=native`, thin LTO, `-O3`. STBI_NEON, fpng CRC32. |
| **Flight Slot Infrastructure** | CB/fence/semaphore per-slot (N=1). Ready for N>1 pipelining. |

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

---

## Failed Optimizations (and Why)

| Attempt | Result | Root Cause |
|---|---|---|
| **Depth export (`vkExportMetalObjectsEXT`)** | Deadlock | MoltenVK's internal device mutex conflicts with PFIFO thread Vulkan state. Cannot safely call from any thread in xemu's architecture. |
| **BQL event batching** (x2) | Deadlock | Holding BQL around SDL event loop prevents QEMU cooperative scheduling. |
| **Deferred auxiliary fence** | Texture corruption | Staging buffer overwritten before GPU executed pending copy. |
| **Texture upload on main CB (v1)** | Texture corruption | Shared `BUFFER_STAGING_SRC` without sub-allocation; fixed with bump allocator. |
| **floatx80 union overlay (ARM64)** | Segfault | x87 80-bit and IEEE 64-bit have incompatible bit layouts on ARM64. |
| **TCG inline float ops (ARM64)** | Segfault | `tcg_gen_*_f64` crashes ARM64 TCG backend. Fixed by disabling `g_use_hard_fpu_inline` on ARM64. |
| **Separate compute queue** | No effect | MoltenVK only exposes `queueCount=1`. Infrastructure in place but inactive. |
| **Voice register cache** | Black screen | `__thread` cache served stale data; Xbox HW modifies registers via DMA/MMIO outside cached paths. |
| **Surface upload bump allocator** | Corruption | Staging shared between texture (main CB) and surface (aux CB) uploads; offsets conflicted. |
| **Flight slots N>1** | Corruption | 7 shared resources (uniform/index/vertex staging+device buffers, uploaded_bitmap) remain unpartitioned. ~20+ call sites need changes; minimal benefit on Apple Silicon. |
| **Async MetalFX (`dispatch_semaphore`)** | Tearing | IOSurface consumed by GL before Metal finished writing. Writer must complete before reader binds. |
| **Render pass hash table (mid-struct)** | Segfault | Inserting `GHashTable*` field mid-struct shifted member offsets; stale `.o` files read wrong memory. Fixed by adding field at end of struct instead. |
| **Dirty-range VRAM flush** | Segfault (then reverted) | `bitmap_clear` before `flush_memory_buffer` zeroed bitmap before read. Minimal benefit on Apple Silicon coherent memory anyway. |
| **Depth export via CPU readback** | Temporal slower than spatial | Copied zeta depth aspect via `vkCmdCopyImageToBuffer` + CPU `memcpy` to R32Float IOSurface every frame. Added 2 GPU sync points + 18+ MiB CPU copy per frame. Quality improvement was subtle; performance cost was 2-5ms/frame. Correct approach requires IOSurface-backed VkImage rendered directly in the NV2A pipeline (avoids CPU round-trip). |
| **Texture decode GMutex/GCond** | Crash + hang | Waiter destroyed mutex/condvar while last worker still inside `g_mutex_unlock`. Stack-allocated `DecodeTaskBatch` memory reused after `decode_batch_wait` returned. Caused `pthread_cond_signal: Invalid argument` and save-state hangs. Fixed by switching to `QemuEvent` (atomic flag, no lock held during signal). |

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
