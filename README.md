# xemu-macos: Optimized Xbox Emulator for Apple Silicon

A personal fork of [xemu](https://github.com/xemu-project/xemu) with comprehensive performance optimizations for Apple Silicon Macs running macOS 26+.

> **Note:** This is for personal use. Optimizations are targeted at M2+ series Macs with macOS 26 (Tahoe) or later.

---

## Optimizations

### Tier 1: High Impact

| Optimization | Speedup | Description |
|---|---|---|
| **ARM64 Hard FPU** | 3-6x faster x87 ops | Fast bit-level floatx80-to-double conversion replaces QEMU's softfloat integer-only emulation. Branchless normal path (~6 integer ops per conversion). |
| **MetalFX Temporal Upscaling** | Better quality at lower render cost | ML-based upscaling from internal resolution (e.g. 1280x960) to 1920x1440 with temporal accumulation and anti-aliasing. |
| **True Frame Interpolation** | 30fps -> 60fps or 120fps | MTLFXFrameInterpolator (macOS 26+) generates intermediate frames. 2x mode = 1 interp frame, 4x mode = 3 interp frames with correct deltaTime values. |
| **Texture Upload Batching** | Eliminates GPU sync per texture | Staging buffer sub-allocation allows multiple texture uploads to batch on the main command buffer before a single flush. |
| **Texture Decode Parallelization** | Up to 24x throughput | GThreadPool distributes S3TC/format conversion across all CPU cores for cubemap and mipmapped textures. |

### Tier 2: Medium Impact

| Optimization | Description |
|---|---|
| **Display Command Buffer Merge** | PVIDEO upload + display render pass combined into 1 GPU submission instead of 2. |
| **Surface Init on Main CB** | Layout transitions for new surfaces use the main command buffer instead of a separate aux submission. |
| **GPU Compute YUV-to-RGBA** | PVIDEO overlay YUV conversion runs on GPU compute instead of CPU. |
| **GPU Compute Z-Order Unswizzle** | Surface upload unswizzle for 4bpp surfaces runs on GPU compute. |
| **Pipeline Dirty Tracking** | vertex_state_dirty flag replaces per-draw memcmp. Only triggers when vertex descriptions actually change. |
| **Vertex Staging Bulk Copy** | Single memcpy when source and destination strides match (common case). |
| **Default Vulkan on macOS** | IOSurface zero-copy display path is strictly superior to OpenGL on macOS. |

### Tier 3: Low Impact / Quality of Life

| Optimization | Description |
|---|---|
| **STBI_NEON** | ARM NEON SIMD for stb_image JPEG/PNG decoding. |
| **fpng ARM64 CRC32** | Hardware CRC32 instructions for PNG encoding. |
| **Display Uniform Caching** | 8 uniform locations resolved once at init, not per-frame string lookup. |
| **MoltenVK Environment Tuning** | Metal argument buffers, prefill, async submit via Info.plist. |
| **Profile Counter Gating** | nv2a_profile_inc_counter stripped in performance builds. |
| **Conditional Surface Flush** | Only flush GPU when surface was drawn in current command buffer. |
| **Separate Compute Queue** | Infrastructure for async compute (probes for 2nd queue from same family). |

### MoltenVK Compatibility

| Fix | Description |
|---|---|
| **VK_KHR_portability_subset** | Required extension for MoltenVK physical device enumeration. |
| **Geometry Shader Workarounds** | CPU-side primitive emulation for quads, provoking vertex rotation. |
| **Hardware Depth Fallback** | Fragment shader uses gl_FragCoord.z when geometry shaders unavailable. |

### macOS 26 Build Fixes

| Fix | Description |
|---|---|
| **Stale pkg-config paths** | download-macos-libs.py uses os.path.abspath instead of os.path.realpath. |
| **Duplicate LC_RPATH** | dyld on macOS 26 rejects binaries with duplicate rpath entries. |
| **IOSurface bytesPerRow** | macOS 26 IOSurface bug at widths >1920px; MetalFX output capped to safe limit. |

---

## Failed Optimizations (and Why)

| Attempt | Why It Failed |
|---|---|
| **Depth export via vkExportMetalObjectsEXT** | MoltenVK deadlocks on any thread context when calling this function. Internal device mutex conflicts with PFIFO thread state regardless of timing. |
| **BQL event batching** | Holding QEMU's Big QEMU Lock around the entire SDL event loop starves the main loop. QEMU's cooperative scheduling requires frequent BQL release. |
| **Deferred auxiliary fence** | Not waiting for aux command buffer completion caused graphics corruption -- callers depend on transfers completing synchronously. |
| **Texture upload on main CB (without sub-allocation)** | Shared staging buffer was overwritten by the next upload before GPU executed the copy. Fixed with bump allocator approach. |
| **floatx80 union overlay on ARM64** | x87 80-bit and IEEE 64-bit double have incompatible bit layouts. Union type punning gives garbage values. Fixed with explicit bit-level conversion functions. |
| **TCG inline float ops on ARM64** | TCG float operations (tcg_gen_add_f64 etc.) crash on ARM64 backend. Fixed by splitting g_use_hard_fpu into helper selection vs TCG inlining flags. |
| **Separate compute queue on MoltenVK** | MoltenVK only exposes 1 queue from 1 family with queueCount=1. Infrastructure is in place but inactive. |

---

## Configuration

### Recommended xemu.toml settings for Apple Silicon

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'temporal'
frame_interpolation = '2x'    # '2x' = 60fps, '4x' = 120fps, 'off' = native

[display.quality]
surface_scale = 2              # 1 = native 640x480, 2 = 1280x960, 4 = 2560x1920

[display.window]
fullscreen_on_startup = true
fullscreen_exclusive = true
startup_size = '1920x1080'
```

### Settings explained

- **metalfx_mode = 'temporal'**: Best quality upscaling with temporal accumulation
- **frame_interpolation = '2x'**: True 60fps (1 interpolated frame between each real frame)
- **frame_interpolation = '4x'**: True 120fps (3 interpolated frames at dt=0.25, 0.5, 0.75)
- **surface_scale = 2**: Good balance -- NV2A renders at 1280x960, MetalFX upscales to 1920x1440
- **surface_scale = 1**: Lowest GPU load, MetalFX does all upscaling (640x480 -> 1920x1440)
- **surface_scale = 4**: Sharpest textures but MetalFX skipped (display >1920px wide)

### Hard FPU

Enabled by default (`perf.hard_fpu = true`). To disable for a specific game:

```toml
[perf]
hard_fpu = false
```

---

## Build Instructions

### Prerequisites

- macOS 26.0 (Tahoe) or later
- Xcode Command Line Tools with macOS 26.4+ SDK
- Apple Silicon Mac (M2 or later recommended)
- Python 3.10+

### Build Steps

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git checkout macos-optimizations
git submodule update --init --recursive

# Build (release, optimized for Apple Silicon)
./build.sh

# The built app is at:
# dist/xemu.app

# Run
open dist/xemu.app
# or
./dist/xemu.app/Contents/MacOS/xemu
```

### Build flags (automatic via build.sh)

- `-O3` with thin LTO
- `-mcpu=apple-m2` (ARM64 tuning)
- `-mmacosx-version-min=14.0`
- Stack protector disabled, trace backends nop (release profile)
- MoltenVK bundled from MacPorts (downloaded automatically)

### Troubleshooting

**Build fails with "sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T":**
Delete `macos-libs/` and rebuild. The pkg-config paths are stale.

```bash
rm -rf macos-libs/ macos-pkgs/ build/
./build.sh
```

**Crash on launch with "duplicate LC_RPATH":**
This build already fixes the duplicate rpath issue. If you see this with an older build, rebuild from scratch.

**MetalFX not activating:**
Check that `metalfx_mode` is set to `'spatial'` or `'temporal'` in xemu.toml. MetalFX requires the Vulkan renderer and VK_EXT_metal_objects support.

**Game crashes when changing internal scaling:**
A safety guard prevents crashes but may show a black frame momentarily. This is a pre-existing race condition in xemu's hot-resize path.

---

## Architecture

```
Xbox 640x480 game
       |
       v
  NV2A Vulkan Renderer (surface_scale x)
       |
       v
  Display IOSurface (e.g. 1280x960 at 2x)
       |
       v
  MetalFX Temporal Upscaler -> 1920x1440
       |
       v
  Frame Interpolator (2x: dt=0.5, 4x: dt=0.25/0.5/0.75)
       |
       v
  GL TEXTURE_RECTANGLE -> SDL Window (fullscreen 4K)
```

---

## Upstream

Based on [xemu](https://github.com/xemu-project/xemu) - Original Xbox Emulator.
MoltenVK compatibility from [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu).
