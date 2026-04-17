# xemu-macos: Optimized Xbox Emulator for Apple Silicon

A personal fork of [xemu](https://github.com/xemu-project/xemu) with performance
work targeted at Apple Silicon (M1/M2+) running macOS 14+ (best on macOS 26 Tahoe).
Adds MoltenVK compatibility — which upstream does not officially support — plus
an inline ARM64 FPU, MetalFX upscaling, frame interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK compat seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

---

## Quick start

Prerequisites: macOS 14.0+ (macOS 26 for MetalFX and frame interpolation),
Apple Silicon, Xcode Command Line Tools, Homebrew `pkg-config`.

```bash
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git checkout macos-optimizations
git submodule update --init --recursive
./build.sh
open dist/xemu.app
```

Clean rebuild: `rm -rf macos-libs macos-pkgs build dist && ./build.sh`.

### Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto-detect | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string`, walks down to the nearest older chip clang recognizes |
| `XEMU_PGO` | unset | `generate` then `use` for profile-guided optimization |
| `XEMU_PGO_DIR` | `./pgo` | Where `.profraw` files land |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Set to `1` to opt into hardened-runtime codesign with `xemu.entitlements` (only needed to exercise notarization-style behavior locally) |
| `XEMU_COREAUDIO_FRAMES` | `1024` | CoreAudio buffer size (≈21 ms @ 48 kHz) |
| `XEMU_HLT_BSOD_RECOVERY` | `1` | Force `IF=1` on `CLI; HLT` with pending IRQ (kernel-bugcheck escape) |

### Recommended `xemu.toml`

`~/Library/Application Support/xemu/xemu/xemu.toml`:

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'        # off | spatial | temporal
frame_interpolation = '4x'      # off | 2x | 4x

[display.quality]
surface_scale = 2               # 1 = 640x480 ... 4 = 2560x1920 (MetalFX skipped)

[display.window]
fullscreen_on_startup = true
fullscreen_exclusive = true
startup_size = '1920x1080'

[display.ui]
fit = 'stretch'

[perf]
hard_fpu = true                 # ARM64 inline x87; disable only for rare precision bugs
cache_shaders = true
```

The in-app Settings UI exposes the main MetalFX / interpolation / scale /
hard-FPU knobs; TOML is only needed for fine tuning.

---

## What this fork changes

### CPU / JIT (ARM64)

- **Inline hard FPU.** x87 ops emit native AArch64 FP instructions
  (`FADD`/`FMUL`/`FDIV`/`FSQRT` etc.). `floatx80`↔`double` conversion is
  inlined (~15 host insns vs ~40 in the helper). Values stay in D-regs
  across translation blocks. `fucomi`/`fnstsw`/`frndint`/FPU constant loads
  and the `fdecstp`/`fincstp` stack pointer updates are also inline.
  ~30% faster than the helper-call path, 4–8× faster than softfloat.
- **FPCR caching across TBs.** `MSR FPCR` only re-emitted when guest RC
  bits change (invalidated automatically on save/load and `cpu_set_fpuc`).
  FPCR `DN=0` so NaN propagation matches x87.
- **Rounding-mode-correct FIST.** `FIST`/`FISTP` use `FRINTI` + `FCVTZS`,
  honoring the current control word (regression from bare `FCVTZS`
  broke Azurik's D-pad — see failed-opts below).
- **JIT tightening.** `ld80f` NOP-copy removed, `gen_stn_ptr` uses shift-
  by-4, `insertion_sort_syncs` replaces `qsort` for N ≤ 16. `flcr`
  lowering is 5 insns via `RBIT`.
- **`HLT` BSOD recovery.** On XBOX targets only, `HLT` with `IF=0` and
  a hardware IRQ already pending gets `IF=1` forced so the NV2A vblank
  ISR can wake the CPU (prevents permanent freezes after kernel
  `CLI; HLT` bugchecks). One-shot log; disable with
  `XEMU_HLT_BSOD_RECOVERY=0` for strict-semantics testing.

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces, `VK_KHR_dynamic_rendering`
  probed at device create. CPU-side primitive emulation for quads, line
  loops, triangle fans, and provoking vertex. Fragment-shader depth
  fallback via `gl_FragCoord.z + dFdx/dFdy`.
- **Single-submit fast path + N=2 flight slots.** CPU records slot 1
  while GPU executes slot 0. Aux command buffer skipped when all
  staging is empty and no VRAM page is dirty.
- **Texture upload batching + bump allocator** on `BUFFER_STAGING_SRC`.
  Multiple textures share one main-CB submission.
- **Parallel + GPU texture decode.** `GThreadPool` capped at
  `MIN(num_processors, 4)` for S3TC/unswizzle/conversion (mips ≤ 16×16
  decoded inline to skip dispatch overhead). Non-compressed 2bpp/4bpp
  textures bypass the CPU pool: raw data copies to a staging buffer and
  is unswizzled by a compute shader that accepts CPU-computed
  `mask_x`/`mask_y` push constants (matches
  `generate_swizzle_masks()` so any power-of-two dimensions work).
- **Dirty tracking everywhere.** UBO `uniform_copy` does `memcmp` before
  `memcpy` with a per-layout dirty flag. Pipeline key re-hashes only
  when `check_pipeline_dirty` fires, and then hashes only the non-
  shader-state slice (ShaderState sub-hash is cached). Vertex layout
  fingerprinted via `fast_hash`. Per-draw surface expiry runs every 8
  frame ticks instead of every draw.
- **Tight barriers.** `ALL_COMMANDS_BIT` replaced with precise stage
  flags. `VK_WHOLE_SIZE` replaced with exact byte ranges.
  `vmaFlush`/`vmaInvalidate` skipped on Apple Silicon coherent memory
  (per-buffer `is_coherent` flag). Vertex-RAM flush is scoped to the
  tracked `[first, last]` dirty-page range.
- **Primitive restart.** `primitiveRestartEnable` is now `VK_TRUE` for
  strip / fan topologies so NV2A restart-index draws render correctly.
- **Display path.** PVIDEO + display merged into one GPU submit. GL
  rebind cache keyed on `IOSurfaceGetID`, not the (reusable) pointer.
  Pending reports / compute descriptor reset consolidated in
  `pgraph_vk_finish`.
- **Dynamic blend / depth bias.** `BLENDCOLOR` / `ZOFFSETBIAS` /
  `ZOFFSETFACTOR` removed from the pipeline key and set as Vulkan
  dynamic state per draw (skipped on clear pipelines).

### MetalFX + presentation

- **Spatial / temporal upscaling** via `MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`. Temporal uses an 8-frame Halton(2,3) jitter
  and optional compute-kernel synthetic depth. Shared `MTLDevice` +
  `MTLCommandQueue` refcounted across spatial/temporal/interpolation.
  File compiles without ARC; every `new*` / `alloc+init` has an
  explicit `release` in the destroy path or before rebinding cached
  textures.
- **Frame interpolation** via `MTLFXFrameInterpolator` (macOS 26+).
  2×=60fps, 4×=120fps from 30fps source, with deferred generation on
  idle display syncs. On-failure slot is skipped (no busy retry).
- **Teardown safety.** Every encode path attaches a completion handler
  that decrements a per-subsystem in-flight counter. Destroy/reinit
  paths drain the counter before releasing scalers/textures, closing
  the race where `waitUntilCompleted` runs after the lock was released.
- **IOSurface lifetime.** `IOSurfaceGetID` is the cache key everywhere
  (Metal caches and GL rebind), with `CFRetain`/`CFRelease` balanced.
  Output width capped at 1920 to work around a macOS 26 BGRA
  `bytesPerRow` bug.

### MCPX APU

- **VP voice processing.** Voice struct (128 B) `memcpy`'d to a stack
  buffer at the top of `voice_process`; all `voice_get/set_mask` calls
  hit the local copy. Attenuation (4096) and pitch (65536) LUTs replace
  transcendentals on the hot path; envelope decay uses `expf` with a
  precomputed log base.
- **Resampler.** `SRC_LINEAR` replaces `SRC_SINC_FASTEST` (matches Xbox
  hardware). Voices at rate == 1.0 bypass libsamplerate entirely,
  reading straight into the output buffer.
- **Accelerate / vDSP.** `vDSP_vsma` for 8-bin × 32-sample mix
  accumulation; `vDSP_vadd` for `float_accumulate`. `float_to_24b` uses
  `vcvtnq_s32_f32` in bulk on ARM64.
- **Atomic consistency.** `d->regs`, `pause_requested`, and the
  `NV_PAPU_FEMEMADDR` load in `fe_method` all go through
  `qatomic_read`/`qatomic_set` so the VP frame thread, worker threads,
  and guest MMIO dispatcher agree under weak memory ordering.
- **CoreAudio.** `os_unfair_lock` with trylock; buffer defaults to
  1024 frames (~21 ms @ 48 kHz, override via `XEMU_COREAUDIO_FRAMES`).
  Silence on contention; underrun returns `BadDevice`/`Unknown` errors
  correctly instead of mis-reporting success.

### Threads + runtime

- **P-core QoS.** Both the PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE` on Apple Silicon.
- **`pgraph.lock` / BQL discipline.** `surface_access_callback`
  conditionally drops BQL before blocking so the vblank thread can
  still fire interrupts.
- **PFIFO wait.** `qemu_cond_timedwait(1 ms)` bounds worst-case
  wake latency from the (unsynchronized) `pfifo_kick` writer.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`.
  `-mcpu` auto-detected from the host chip (M1 → M4+), with a
  progressive fallback to the nearest older chip clang recognizes.
- macOS 26 build fixes: `download-macos-libs.py` uses `os.path.abspath`
  and repairs stale `prefix=` lines in vendored `.pc` files;
  `build.sh` strips all `LC_RPATH` before `dylibbundler` runs and
  adds the single correct one. Local codesign stays ad-hoc without
  hardened runtime (MAP_JIT works); set `XEMU_CODESIGN_ENTITLEMENTS=1`
  to opt into hardened runtime + `xemu.entitlements` (rarely needed;
  use `scripts/sign-macos-release.sh` for notarized distribution).
- Optional `XEMU_PGO=generate` then `XEMU_PGO=use` build loop using
  `llvm-profdata` merge.
- STBI_NEON + fpng CRC32 for ARM64.

### MoltenVK runtime config (`Info.plist` `LSEnvironment`)

| Key | Value | Purpose |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor binding overhead |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | `2` | Prefill at CB end |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async submits |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Ignore transient GPU errors |

---

## Failed / reverted experiments

Lessons worth preserving so they aren't re-attempted.

| Attempt | Reason |
|---|---|
| `floatx80` union overlay on ARM64 | Layout incompatible with IEEE 64-bit — segfaults |
| Voice register `__thread` cache | Stale data; Xbox HW mutates voice regs via DMA |
| Async MetalFX (double-buffered IOSurface) | First-frame segfault; keep synchronous until the display path is Metal-native |
| Async MetalFX `dispatch_semaphore` | IOSurface consumed by GL before Metal finished writing |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export via CPU readback | Added 2 ms/frame and ~18 MiB of copies; quality barely improved |
| Depth export via `vkExportMetalObjectsEXT` | Deadlock with MoltenVK's internal device mutex |
| Dirty-range VRAM flush | Bitmap was cleared before `flush_memory_buffer` read it — reverted before the fix-in-place landed; tracked per-flight range does the safe equivalent now |
| Surface-range binary search on `end` | `end` isn't monotonic in a `start`-sorted array → skipped valid overlaps; linear scan is O(n) and n is small |
| Two-level quick texture hash | 192-byte sampling missed content changes (YUV / FMV) |
| Hash skip for textures > 64 KiB | Dirty-bitmap false positives caused massive GPU re-upload churn |
| HRTF hand NEON | Gather-then-FMA on a circular buffer defeated OoO overlap; scalar + `-O3 -mcpu=native` autovectorizes better |
| Always-on bounds checks in perf builds | ~5–10% GPU-pipeline regression vs. `__builtin_unreachable()` |
| BQL event batching (x2) | BQL around the SDL event loop breaks QEMU cooperative scheduling |
| Partial descriptor set writes | Each draw consumes a fresh descriptor set — would require `VK_EXT_descriptor_indexing` refactor |

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Crash on launch with "duplicate LC_RPATH".**
Older builds only. Rebuild from scratch; `build.sh` now strips rpaths.

**MetalFX not activating.**

- Needs `renderer = 'VULKAN'` and `metalfx_mode = 'spatial'` or `'temporal'`.
- `surface_scale = 4` produces a 2560×1920 input which exceeds the
  1920px BGRA-IOSurface safe cap, so MetalFX is skipped.
- Look for `MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.**

- Requires macOS 26.0+ and MetalFX upscaling active (width ≤ 1920).

**FPU precision bug in a game.**
Disable the inline path: `[perf] hard_fpu = false`. The inline FPU
uses IEEE double (52-bit mantissa) vs x87 extended precision
(64-bit); extremely rare in practice.

**Audio glitches / too much latency.**
`XEMU_COREAUDIO_FRAMES=2048 ./build.sh` for a larger buffer. Or file
an issue — default is 1024 frames.

---

## Architecture at a glance

```
Xbox game (30fps, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG with inline x87 → AArch64 FP
       │  floatx80↔double inline JIT · FPCR cache · D-regs across TBs
       │
       ├─ GPU: NV2A Vulkan renderer (MoltenVK on macOS)
       │  flight slots · bump-alloc staging · dirty-tracked pipelines
       │  compute unswizzle + YUV · IOSurface zero-copy
       │        │
       │        └─ MetalFX Spatial / Temporal → 1280×960→1920×1440
       │             │
       │             └─ MTLFXFrameInterpolator (30→60 or 30→120)
       │                  │
       │                  └─ CGLTexImageIOSurface2D → SDL3 window
       │
       └─ APU: VP (per-voice pitch/vol) + DSP (scripted M56001)
           linear resampler · vDSP_vsma mixbin · CoreAudio IOProc
```
