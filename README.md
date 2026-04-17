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
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` with progressive fallback to older chips clang recognizes |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use` for profile-guided optimization |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Opt into hardened-runtime codesign with `xemu.entitlements` (only for local notarization-style testing) |
| `XEMU_COREAUDIO_FRAMES` | `1024` | CoreAudio buffer size (≈21 ms @ 48 kHz) |
| `XEMU_PFIFO_HEARTBEAT` | `0` | 2-second pfifo-thread diagnostic snapshot (`halt` / `flush_pending` / `sync_pending` / `waiting_for_*`); used to categorize "frozen but UI responsive" reports |

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
  across translation blocks. `fucomi`/`fnstsw`/`frndint`/FPU constant
  loads and `fdecstp`/`fincstp` are also inline. ~30% faster than the
  helper-call path, 4–8× faster than softfloat.
- **FPCR caching.** `MSR FPCR` only re-emitted when guest RC bits change
  (invalidated on save/load and `cpu_set_fpuc`). FPCR `DN=0` so NaN
  propagation matches x87.
- **Rounding-mode-correct FIST.** `FIST`/`FISTP` honor the current
  control word via mode-specific fused TCG ops
  (`cvt_{rn,rm,rp}_i{32,64}_f{32,64}`) that lower to a single host
  `FCVT{N,M,P,Z}S` on AArch64.
- **JIT tightening.** `ld80f` NOP-copy removed, `gen_stn_ptr` uses
  shift-by-4, `insertion_sort_syncs` replaces `qsort` for N ≤ 16.
  `flcr` lowering is 5 insns via `RBIT`.

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces. CPU-side primitive
  emulation for quads, line loops, triangle fans, provoking vertex.
  Fragment-shader depth fallback via `gl_FragCoord.z + dFdx/dFdy`.
- **Split descriptor sets.** Set 0 UBOs, set 1 the `NV2A_MAX_TEXTURES`
  combined image samplers. Per-set pool/array/index with last-bound
  tracking — a draw that only changes textures doesn't rewrite UBOs
  and vice versa. Roughly halves `vkUpdateDescriptorSets` work.
- **Single-submit fast path + N=2 flight slots.** CPU records slot 1
  while GPU executes slot 0. Aux command buffer skipped when staging
  is empty and no VRAM page is dirty.
- **Texture upload batching + bump allocator** on `BUFFER_STAGING_SRC`.
  Multiple textures share one main-CB submission.
- **Parallel + GPU texture decode.** `GThreadPool` capped at
  `MIN(num_processors, 4)` for S3TC / unswizzle / conversion (mips
  ≤ 16×16 inline). Non-compressed 2bpp/4bpp textures bypass the CPU
  pool — the raw swizzled data stages to a buffer and a compute
  shader unswizzles it with CPU-computed `mask_x`/`mask_y` push
  constants for any power-of-two dimensions.
- **Dirty tracking.** UBO `uniform_copy` does `memcmp` before
  `memcpy` with per-layout dirty flag. Pipeline key re-hashes only
  when `check_pipeline_dirty` fires, hashing only the non-shader
  slice (`ShaderState` sub-hash cached). Vertex layout fingerprinted
  via `fast_hash`. Surface expiry throttled to ~33 ms host wall-clock
  (not guest `frame_time` — see failed experiments).
- **Incremental texture content hash.** Page-aligned textures
  ≥ 256 KiB keep a per-chunk XXH3 array (64 KiB chunks). Only chunks
  whose backing pages changed get re-hashed; aggregate is the XOR
  plus cached palette hash. Saves ~30–60% hash CPU for large atlases
  with sparse updates. Small / non-aligned textures fall back to the
  single-shot full-buffer hash.
- **Tight barriers.** `ALL_COMMANDS_BIT` replaced with precise stage
  flags; `VK_WHOLE_SIZE` with exact byte ranges.
  `vmaFlush`/`vmaInvalidate` skipped on coherent memory (Apple
  Silicon unified memory; per-buffer `is_coherent` flag). Vertex-RAM
  flush scoped to the tracked `[first, last]` dirty-page range.
- **Primitive restart.** `primitiveRestartEnable = VK_TRUE` for
  strip / fan topologies so NV2A restart-index draws render correctly.
- **Display path.** PVIDEO + display merged into one GPU submit.
  GL rebind cache keyed on `IOSurfaceGetID`. Pending reports /
  compute descriptor reset consolidated in `pgraph_vk_finish`.
- **Dynamic blend / depth bias.** `BLENDCOLOR` / `ZOFFSETBIAS` /
  `ZOFFSETFACTOR` removed from the pipeline key and set as Vulkan
  dynamic state per draw (skipped on clear pipelines).

### MetalFX + presentation

- **Spatial / temporal upscaling** via `MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`. Temporal uses 8-frame Halton(2,3) jitter
  and optional compute-kernel synthetic depth. Shared `MTLDevice` +
  `MTLCommandQueue` refcounted. MRC lifetime: every `new*` /
  `alloc+init` has an explicit `release`.
- **Frame interpolation** via `MTLFXFrameInterpolator` (macOS 26+).
  2× = 60 fps, 4× = 120 fps from a 30 fps source, with deferred
  generation on idle display syncs. On-failure slot is skipped.
- **Teardown safety.** Every encode path attaches a completion handler
  that decrements a per-subsystem in-flight counter; destroy/reinit
  drains the counter before releasing scalers/textures.
- **IOSurface lifetime.** `IOSurfaceGetID` is the cache key everywhere
  (Metal + GL), `CFRetain`/`CFRelease` balanced. Output width capped
  at 1920 to work around a macOS 26 BGRA `bytesPerRow` bug.

### MCPX APU

- **VP voice processing.** Voice struct (128 B) memcpy'd to stack at
  the top of `voice_process`; all `voice_get/set_mask` calls hit the
  local copy. Attenuation (4096) and pitch (65536) LUTs replace
  transcendentals on the hot path; envelope decay uses `expf` with
  a precomputed log base.
- **Resampler.** `SRC_LINEAR` replaces `SRC_SINC_FASTEST` (matches
  Xbox hardware).
- **Accelerate / vDSP.** `vDSP_vsma` for 8-bin × 32-sample mix
  accumulation; `vDSP_vadd` for `float_accumulate`; `float_to_24b`
  uses `vcvtnq_s32_f32` in bulk on ARM64.
- **Atomic consistency.** `d->regs`, `pause_requested`, and the
  `NV_PAPU_FEMEMADDR` load in `fe_method` all go through
  `qatomic_read`/`qatomic_set` so the VP frame thread, workers, and
  guest MMIO dispatcher agree under weak ordering.
- **CoreAudio.** `os_unfair_lock` with trylock; default buffer 1024
  frames (~21 ms @ 48 kHz, override via `XEMU_COREAUDIO_FRAMES`).
  Silence on contention; underrun returns `BadDevice` / `Unknown`
  correctly instead of mis-reporting success.

### Threads + runtime

- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE` on Apple Silicon.
- **BQL discipline.** `surface_access_callback` conditionally drops
  BQL before blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`. All `pfifo_kick` sites
  hold `d->pfifo.lock` across the kick-set + broadcast, so the
  reader's kick-check + atomic release-wait can't miss a concurrent
  kick. Removes the ~1 kHz idle wake-up of the previous 1 ms
  `timedwait`.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`. `-mcpu`
  auto-detected with progressive fallback.
- macOS 26 build fixes: `download-macos-libs.py` uses `os.path.abspath`
  and repairs stale `prefix=` lines in vendored `.pc` files;
  `build.sh` strips all `LC_RPATH` before `dylibbundler` runs and
  adds the single correct one. Local codesign stays ad-hoc without
  hardened runtime (MAP_JIT works); set `XEMU_CODESIGN_ENTITLEMENTS=1`
  to opt into hardened runtime + `xemu.entitlements` (for notarized
  distribution use `scripts/sign-macos-release.sh`).
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
| Async MetalFX (all variants: sem-primed / double-buffered IOSurface / `dispatch_semaphore`) | GL ↔ Metal cross-API sync can't be expressed with `SDL_GL_SwapWindow` + vsync alone; needs `MTLSharedEvent` + `glWaitSync` or triple-buffering. Sync MetalFX is the baseline until the display path goes Metal-native |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export (CPU readback or `vkExportMetalObjectsEXT`) | CPU: 2 ms/frame + 18 MiB of copies for minimal quality. Metal export: deadlocks MoltenVK's internal device mutex |
| Direct-VRAM compute unswizzle via `VK_EXT_external_memory_host` | `HOST_WRITE → SHADER_READ` barriers enforce visibility up to *submission*, not GPU execution; on HOST_COHERENT memory the CPU can tear the read mid-frame → particle/HUD flicker. Staging-copy path is kept |
| Host-imported `BUFFER_VERTEX_RAM` (α2) | Same torn-read class for vertex data: GPU fetches can see two different CPU frames mid-primitive on dynamic meshes. Needs snapshot-COW or `MTLSharedEvent` to re-enable |
| `pthread_jit_write_with_callback_np` for `tb_phys_invalidate` (A3) | Scoped-W semantics force-drop W on callback return, incompatible with QEMU's nested JIT-write paths. Restored the manual `qemu_thread_jit_write` / `execute` pair |
| `VK_KHR_dynamic_rendering` full lowering (B2) | Missing the render pass's `VK_SUBPASS_EXTERNAL → first subpass` dependency; back-to-back passes stalled Metal's tile renderer. Re-enable needs explicit `vkCmdPipelineBarrier` around every Begin/End |
| APU voice resampler `rate == 1.0` fast path | Skipped libsamplerate but didn't replicate its silence-padding on voice drain → `voice_process`'s outer loop spun on draining voices during scene transitions |
| `HLT` BSOD recovery (IF=1 force at-HLT and post-halt) | Unblocks Xbox-kernel `CLI; HLT` deadlocks, but the IRQ-loop pattern is a *symptom* of a stuck thread elsewhere, not the cause. Removed |
| Surface-expiry throttle keyed on `pg->frame_time` | `frame_time` only advances in `NV097_FLIP_INCREMENT_WRITE`, so rapid level-load bursts with no flips starved the prune and `r->invalid_surfaces` grew unbounded → black-screen freeze on rapid double-level-load + die. Switched to host wall-clock (~33 ms) |
| Surface-range binary search on `end` | `end` isn't monotonic in a start-sorted array → skipped valid overlaps |
| Two-level quick texture hash / hash-skip for > 64 KiB | 192-byte sampling missed content changes (YUV / FMV); dirty-bitmap false positives caused massive GPU re-upload churn |
| Dirty-range VRAM flush (early) | Bitmap cleared before `flush_memory_buffer` read it. Tracked per-flight `[first, last]` range does the safe equivalent |
| Incremental texture hash on misaligned textures | Host-page boundaries straddle chunks → `test_and_clear_dirty` by one chunk steals another's dirty signal. Gated to page-aligned textures only |
| HRTF hand-NEON / always-on bounds checks | Gather-then-FMA on a circular buffer defeated OoO overlap; `-O3 -mcpu=native` autovectorizes better. Always-on bounds: ~5–10% GPU-pipeline regression |
| BQL event batching | BQL around the SDL event loop breaks QEMU cooperative scheduling |

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Crash on launch with "duplicate LC_RPATH".**
Older builds only. Rebuild from scratch; `build.sh` now strips rpaths.

**MetalFX not activating.**

- Needs `renderer = 'VULKAN'` and `metalfx_mode = 'spatial'` or `'temporal'`.
- `surface_scale = 4` produces 2560×1920 input which exceeds the 1920 px
  BGRA-IOSurface safe cap, so MetalFX is skipped.
- Look for `MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.**
Requires macOS 26.0+ and MetalFX upscaling active (width ≤ 1920).

**FPU precision bug in a game.**
Disable the inline path: `[perf] hard_fpu = false`. The inline FPU
uses IEEE double (52-bit mantissa) vs x87 extended precision (64-bit);
extremely rare in practice.

**Audio glitches / too much latency.**
`XEMU_COREAUDIO_FRAMES=2048 ./build.sh` for a larger buffer.

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
