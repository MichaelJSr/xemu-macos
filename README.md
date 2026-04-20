# xemu-macos: Optimized Xbox Emulator for Apple Silicon

A personal fork of [xemu](https://github.com/xemu-project/xemu) with performance
work targeted at Apple Silicon (M1/M2+) running macOS 14+ (best on macOS 26
Tahoe). Adds MoltenVK compatibility — which upstream does not officially
support — plus an inline ARM64 FPU, full DSP JIT, MetalFX upscaling, frame
interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK compat seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

---

## Quick start

Prerequisites: macOS 14.0+ (macOS 26 for MetalFX + frame interpolation),
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
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` with fallback to older chips |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use` for profile-guided optimization |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Opt into hardened-runtime codesign with `xemu.entitlements` |
| `XEMU_COREAUDIO_FRAMES` | `1024` | CoreAudio buffer size (≈21 ms @ 48 kHz) |
| `XEMU_PFIFO_HEARTBEAT` | `0` | 2-second pfifo-thread diagnostic snapshot |

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
hard_fpu = true                 # ARM64 inline x87
cache_shaders = true
audio.dsp_jit.enabled = true    # full DSP JIT (see below)
```

The in-app Settings UI exposes the main toggles; TOML is only needed for
fine tuning.

---

## What this fork changes

### CPU / JIT (ARM64)

- **Inline hard FPU.** x87 ops emit native AArch64 FP (`FADD`/`FMUL`/`FDIV`/
  `FSQRT`); `floatx80`↔`double` conversion is inlined (~15 host insns vs
  ~40 in the helper); values stay in D-regs across TBs. `fucomi` /
  `fnstsw` / `frndint` / FPU constant loads / `fdecstp`-`fincstp` also
  inline. ~30% faster than the helper-call path, 4–8× faster than
  softfloat.
- **FPCR caching + rounding-correct FIST.** `MSR FPCR` only re-emitted
  when guest RC bits change. `FIST`/`FISTP` honor the control word via
  fused `cvt_{rn,rm,rp}_i{32,64}_f{32,64}` TCG ops that lower to one
  host `FCVT{N,M,P,Z}S`.
- **Signaling vs quiet compares.** `coms_f32`/`coms_f64` (FCMPE / COMISS)
  for FCOMI; `com_f32`/`com_f64` (FCMP / UCOMISS) for FUCOMI. Matches
  x87 NaN semantics.
- **Snapshot resync.** `xsave_helper.c` and `cpu_post_load` route through
  `cpu_set_fpuc` so `HF_FPU_RC` in `hflags` is rebuilt after vmstate
  restore — otherwise fused FIST ops would round with the pre-restore
  mode.
- **Misc JIT tightening.** `gen_flcr` materializes rc-bits from `tb->flags`;
  `fnstcw` inlines to one load; `insertion_sort_syncs` replaces `qsort`
  for N ≤ 16; `flcr` is 5 insns via `RBIT`.

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces, CPU-side quads / line
  loops / triangle fans / provoking vertex emulation, fragment-shader
  depth fallback.
- **Descriptor / pipeline plumbing.** Split sets (UBOs vs textures) with
  last-bound tracking. Coalesced multi-binding writes. Dynamic blend /
  depth bias / viewport / scissor / line-width / blend-constants cached
  per-CB so they're only re-emitted on change.
- **Pipeline key narrowed.** Dynamic state off the key;
  `NV_PGRAPH_CONTROL_3` off the key (its bits route through
  `ShaderState` / `PROVOKING_VERTEX`).
- **Flight slots (N=2) + single-submit fast path.** CPU records slot 1
  while GPU executes slot 0; aux CB skipped when staging is empty.
- **Texture upload.** Batching + bump-allocator staging, parallel CPU
  decode pool (S3TC / swizzle / conversion) capped at `MIN(nproc, 4)`.
  Non-compressed 2/4-bpp textures use a compute-shader unswizzler.
- **Dirty tracking.** Per-chunk XXH3 on page-aligned textures ≥ 256 KiB
  (saves 30-60% hash CPU on sparse atlas updates). UBO `memcmp` before
  `memcpy`. Surface expiry throttled to host wall-clock (~33 ms).
- **Tight barriers.** Precise stage masks replace `ALL_COMMANDS_BIT`;
  exact byte ranges replace `VK_WHOLE_SIZE`; coherent-memory flushes
  skipped via per-buffer `is_coherent`.
- **Renderer-switch hardening.** AB-BA-safe lock ordering on
  GL↔VK toggle; atomic `flush_pending`; `pgraph.lock` dropped across
  the `framebuffer_released` wait.
- **Monotonic sync clock.** `pgraph_vk_sync` uses `QEMU_CLOCK_HOST`
  (suspend / NTP slew no longer stall the 8 ms gate).
- **Fence-wait diagnostics.** 5 s timeout wrapper on single-time CBs
  that aborts with the named call site instead of hanging silently on
  MoltenVK internal-mutex deadlocks.
- **Narrowed GPU-side synchronization.** `pgraph_vk_upload_surface_-
  data`'s full GPU sync only fires when the target surface is the
  active color/zeta binding or a CB is recording; otherwise
  MoltenVK's single-queue serialization + aux-CB fence wait suffice.
  `invalidate_overlapping_surfaces` collects into a 64-entry stack
  array in a single pass instead of two passes via `g_newa`.
- **Draw-path dedup caches.** `vkCmdBindVertexBuffers`,
  `vkCmdPushConstants` (inline uniform attrs, keyed on layout),
  and a scaled `surface_binding_dim` cache all memcmp against the
  last CB-scoped value and skip on match. Reset alongside
  `dynstate_cache_valid` on CB begin.
- **Pipeline-rebuild decoupled from texture rebind.**
  `check_pipeline_dirty` no longer treats `texture_bindings_changed`
  as pipeline-dirty; `PipelineKey` contains no texture identity
  so set-1 refresh via `pgraph_vk_update_descriptor_sets` is
  sufficient. Fast-out hits the common texture-change-per-draw case.
- **Narrowed remapped-attribute staging.** `pgraph_vk_bind_vertex_-
  attributes` shifts `vertex_attribute_offsets[i]` by
  `min_element * stride`; `remap_unaligned_attributes` +
  `copy_remapped_attributes_to_inline_buffer` only size and copy
  `[min_element..max_element]`. Draws rebase via
  `firstVertex - min_element` / `vertexOffset = -min_element`.
- **`surface_ranges` insert / remove.** `lower_bound` insert
  (O(log n)) and cached `surface_range_slot` remove (O(1)) replace
  the prior linear scans; `memmove` cost unchanged.
- **Shader-uniform pull gated on a dirty flag (both renderers).**
  `pg->shader_uniform_inputs_dirty` is set by `pgraph_reg_w`, every
  `ltctxa/ltctxb/ltc1/vsh_constants` writer, every
  `vertex_attributes[].inline_value` writer (via
  `pgraph_allocate_inline_buffer_vertices`), GL texture rebinds, and
  renderer switch. `pgraph_vk_bind_shaders` /
  `pgraph_gl_bind_shaders` skip `update_shader_uniforms` when the
  flag plus shader/texture rebind bits are clean.
  `pgraph_update_inline_value` memcmp's before write so identical
  VRAM data doesn't defeat the gate.

### MetalFX + presentation

- **Spatial + temporal upscaling** (`MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`). Temporal uses 8-frame Halton(2,3) jitter +
  optional compute-kernel synthetic depth. Shared `MTLDevice` +
  `MTLCommandQueue` refcounted with MRC discipline.
- **Frame interpolation** (`MTLFXFrameInterpolator`, macOS 26+): 2× =
  60 fps, 4× = 120 fps from a 30 fps source; deferred generation on
  idle syncs; failure-slot skipped.
- **Teardown safety.** Completion handlers decrement a per-subsystem
  in-flight counter; destroy/reinit drains before releasing.
- **IOSurface lifetime.** `IOSurfaceGetID` is the cache key; output
  width capped at 1920 to work around a macOS 26 BGRA
  `bytesPerRow` bug.
- **Correct `MTLFXFrameInterpolator.deltaTime`.** Wall-clock seconds
  between the two input-frame `CFRetain`s (`QEMU_CLOCK_HOST`,
  clamped `[1/240, 1/10]` s) instead of a unitless ratio. Reduces
  ghosting on fast pans.

### MCPX APU

- **Voice processing.** 128-byte voice struct memcpy'd to stack; all
  `voice_get/set_mask` calls hit the local copy. LUTs replace
  transcendentals on the hot path: attenuation (4096), pitch (65536),
  LPF cutoff (65536), envelope decay/release (1024 each, linear
  interp). 3D voices skip four dead `voice_get_mask` reads for
  `V0BIN..V3BIN` (those values are immediately overwritten from
  `hrtf_submix[]`).
- **Accelerate / vDSP.** `vDSP_vsma` for 8-bin × 32-sample mix
  accumulation, `vDSP_vadd` for `float_accumulate`; `float_to_24b`
  uses `vcvtnq_s32_f32` in bulk on ARM64. Single-precision SVF/LPF
  eliminates float→double→float round-trips.
- **Resampler.** `SRC_LINEAR` (matches Xbox hardware).
- **Atomic consistency.** All `d->regs[]` writers, `voice_locked[]`,
  `pause_requested`, and the `NV_PAPU_FEMEMADDR` load go through
  `qatomic_*` so VP frame thread, workers, and lock-free MMIO
  dispatcher agree under weak ordering.
- **CoreAudio.** `os_unfair_lock` with trylock; underrun reports
  `BadDevice` / `Unknown` correctly instead of silently succeeding.
- **Full DSP JIT (opt-in, default on for new configs).** ARM64
  basic-block JIT for both MCPX DSP56300 cores (GP + EP). Enable
  with `[perf] audio.dsp_jit.enabled = true` in `xemu.toml` or
  `XEMU_DSP_JIT=1`. 100% ALU inlined, 99.99% CF inlined; only
  `emu_undefined` + the 16-variant bit_manip tail (~180 ops/run)
  stay on the BLR path. A/B/X/Y/SR accumulators pinned in
  callee-saved ARM64 regs. Static block chaining for unconditional
  + conditional-taken terminators keeps hot inner loops inside JIT
  code across iterations. Lazy-flag elimination (full-dead + per-
  flag N/Z + E/U halves) elides ~20% of SR.E/U/N/Z updates on
  Azurik. Bit-exact correctness harness: `XEMU_DSP_JIT_DIFF=N`
  validates every Nth unique block against the interpreter on an
  off-thread worker (per-translation gated via `diff_checked`, so
  validator work is bounded to unique-block count). Stats:
  `XEMU_DSP_JIT_STATS=1` on emulator exit dumps per-run inline /
  fallback counters, lazy-flag skip counts, and
  `g_cf_fallback_buckets` breakdown. See
  [docs/dsp-jit-design.md](docs/dsp-jit-design.md) for the phased
  roadmap and architecture.

### Threads + runtime

- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE`.
- **BQL discipline.** `surface_access_callback` drops BQL before
  blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`; kick-check + release-
  wait under `d->pfifo.lock` guarantees no missed wake-up. `halt`
  read via `qatomic_read` to match the writer contract.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`, `-mcpu` auto-
  detected.
- macOS 26 build fixes: `download-macos-libs.py` uses `os.path.abspath`
  and repairs stale `prefix=` lines in vendored `.pc` files;
  `build.sh` strips all `LC_RPATH` before `dylibbundler` runs and
  adds the single correct one.
- Ad-hoc codesign by default (MAP_JIT works);
  `XEMU_CODESIGN_ENTITLEMENTS=1` opts into hardened runtime +
  `xemu.entitlements`. For notarized distribution use
  `scripts/sign-macos-release.sh`.
- Optional PGO (`XEMU_PGO=generate` → run → `XEMU_PGO=use`).
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
| Async MetalFX (all variants) | GL↔Metal cross-API sync can't be expressed with `SDL_GL_SwapWindow` + vsync alone; needs `MTLSharedEvent` + `glWaitSync` or triple-buffering |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export (CPU readback or `vkExportMetalObjectsEXT`) | CPU: 2 ms/frame + 18 MiB of copies for minimal quality. Metal export: deadlocks MoltenVK's internal device mutex |
| Direct-VRAM compute unswizzle via `VK_EXT_external_memory_host` | `HOST_WRITE → SHADER_READ` barriers enforce visibility up to *submission*, not GPU execution; on HOST_COHERENT memory the CPU can tear the read mid-frame → particle/HUD flicker |
| Host-imported `BUFFER_VERTEX_RAM` | Same torn-read class for vertex data; GPU fetches can see two CPU frames mid-primitive on dynamic meshes |
| `pthread_jit_write_with_callback_np` for `tb_phys_invalidate` | Scoped-W semantics force-drop W on callback return, incompatible with QEMU's nested JIT-write paths |
| `VK_KHR_dynamic_rendering` full lowering | Missing the render pass's `VK_SUBPASS_EXTERNAL → first subpass` dependency; back-to-back passes stalled Metal's tile renderer |
| APU voice resampler `rate == 1.0` fast path | Skipped libsamplerate but didn't replicate its silence-padding on voice drain → `voice_process`'s outer loop spun on draining voices during scene transitions |
| `HLT` BSOD recovery (IF=1 force at-HLT) | Unblocks Xbox-kernel `CLI;HLT` deadlocks, but the IRQ-loop pattern is a *symptom* of a stuck thread elsewhere, not the cause |
| Surface-expiry throttle keyed on `pg->frame_time` | `frame_time` only advances in `NV097_FLIP_INCREMENT_WRITE`; rapid level loads with no flips starved the prune and `r->invalid_surfaces` grew unbounded → black-screen freeze. Switched to host wall-clock |
| Surface-range binary search on `end` | `end` isn't monotonic in a start-sorted array → skipped valid overlaps |
| Two-level quick texture hash / hash-skip for > 64 KiB | 192-byte sampling missed content changes (YUV / FMV); dirty-bitmap false positives caused massive GPU re-upload churn |
| Dirty-range VRAM flush (early) | Bitmap cleared before `flush_memory_buffer` read it. Tracked per-flight `[first, last]` range does the safe equivalent |
| Incremental texture hash on misaligned textures | Host-page boundaries straddle chunks → `test_and_clear_dirty` by one chunk steals another's dirty signal. Gated to page-aligned textures only |
| HRTF hand-NEON | Gather-then-FMA on a circular buffer defeated OoO overlap; `-O3 -mcpu=native` autovectorizes better |
| BQL event batching | Breaks QEMU cooperative scheduling |
| TLS-cached `pthread_jit_write_protect_np` | Per-thread cache drifts from kernel W^X state on nested `tb_gen_code` / setjmp paths → SIGSEGV/SIGILL after ~1 min |
| Hoist `can_fifo_access` out of pfifo pusher word loop | `pfifo_run_puller` drops `pfifo.lock` when taking `pgraph.lock`, so the "lock held throughout" invariant is false. `ERROR_CALL` on game boot |
| DSP JIT round-4 `cur_inst` preset skip | Regresses on startup with `op=0x001000` — some inlined path reads `cur_inst` at runtime that the static classifier doesn't see. Investigation harness (`XEMU_DSP_JIT_SENTINEL` / `_FORCE`) kept in-tree |
| DSP JIT round-4 EPI_NO_PC (skip PC-mismatch check) | 9.75% mismatch rate per the pcskip sentinel — calc_ea mode 6 lengthens parmoves and REP/DO loops rewind pc; both legitimately diverge and need the check |
| Vblank cadence aligned to host display refresh | `ui/xemu.c`'s `vblank_interval_ns` drives `process_vblank` → `graphic_hw_update` → `hw_ops->gfx_update`, which `hw/xbox/nv2a/nv2a.c:250` registers as `nv2a_vga_gfx_update` — that handler fires `NV_PCRTC_INTR_0_VBLANK`. Retuning the host-side timer retimes the guest vblank IRQ too, so 120 Hz host = 2× guest sim speed, 240 Hz = 4× (titles gate simulation on vblank count). A correct fix requires decoupling the guest vblank IRQ timer (fixed 60 Hz NTSC / 50 Hz PAL, inside the NV2A model) from the host display-present cadence — substantial NV2A-model rework, not a one-liner |

---

## Future vectors

Not attempted or punted on risk. Each either cites the reverted-experiments
entry it would need to sidestep, or describes the blocking work.

- **Metal-native presentation.** Replace SDL3 + GL +
  `CGLTexImageIOSurface2D` with `CAMetalLayer` direct drawable acquisition.
  Unblocks async MetalFX + dynamic-rendering re-enable.
- **Async MetalFX via `MTLSharedEvent` + `glWaitSync`.** Revisit after
  Metal-native presentation lands (or use a triple-buffered IOSurface
  ring with CPU-side fences).
- **`VK_EXT_external_memory_host` with snapshot scheme.** Per-flight COW
  or `MTLSharedEvent`-keyed boundary to sidestep the host-coherent tear.
- **`VK_KHR_dynamic_rendering` with explicit barriers.** Emit
  `vkCmdPipelineBarrier` around every `BeginRendering` / `EndRendering`.
- **`MTLResidencySet` (macOS 15+).** Pin frequently-used Metal
  buffers/textures as resident so the OS doesn't page them out.
- **BINK video via VideoToolbox.** Xbox BINK decoder is CPU-bound;
  VideoToolbox-offload YUV→RGBA (or full BINK→H.264/HEVC transcode).
- **Shader specialization constants.** Burn alpha-test / fog-enable into
  pipeline-compile-time constants; eliminates per-draw uniform bandwidth
  + fragment branches.
- **GPU-based S3TC decode.** Compute-shader decoder offloads the CPU
  thread pool.
- **Aux-submit pipelining.** `pgraph_vk_end_single_time_commands`
  currently waits fully; chain ordered commits via semaphores.
- **Query-pool drain at slot reclaim.** `vkGetQueryPoolResults` with
  `WAIT_BIT` stalls per-submit; move the drain into the flight-slot
  fence wait (careful: the current `wait_for_previous_flight` waits a
  different slot than the one carrying the queries).
- **LRU eviction fast path.** `lru_try_evict_one` walks the tail
  linearly; aux evictable queue or per-slot bitmask.
- **Decoupled guest-vblank IRQ timer.** The reverted "vblank cadence
  to host refresh" experiment died because the host timer also
  drives the guest `NV_PCRTC_INTR_0_VBLANK` IRQ. A dedicated
  NV2A-model timer for the guest IRQ (fixed 60 Hz NTSC / 50 Hz
  PAL) would let the host present path retune for ProMotion
  smoothness without touching guest simulation speed.
- **Voice-register writeback batching.** Defer `ram_stl` in
  `voice_set_mask` to a single `memcpy` at end of `voice_process`
  (behavior risk: mid-frame MMIO reads see stale data).
- **`qemu_cpu_kick` via `dispatch_semaphore_t`.** `pthread_kill(SIGUSR1)`
  has ~50 µs P99 jitter on macOS 26.
- **TCG AArch64 3-MOVK constant materialization.** Beats the literal-pool
  LDR latency on Apple chips.
- **Cross-TB FPCR elision.** `cached_fpuc_rc` as a TCG global register
  across chained TBs → pure register compare instead of memory reload.
- **DSP JIT parmove+ALU fusion.** Shape-specific redundant
  `save_reg` → mem → reg round-trips when the parmove's own
  destination provides the same value.

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Crash on launch with "duplicate LC_RPATH".** Older builds only — rebuild;
`build.sh` now strips rpaths.

**MetalFX not activating.** Needs `renderer = 'VULKAN'` and
`metalfx_mode = 'spatial'` or `'temporal'`. `surface_scale = 4` produces
2560×1920 input which exceeds the 1920 px BGRA-IOSurface safe cap, so
MetalFX is skipped. Look for `MetalFX: Spatial upscaler initialized …`
in the log.

**Frame interpolation not working.** Requires macOS 26.0+ and MetalFX
upscaling active (width ≤ 1920).

**FPU precision bug.** `[perf] hard_fpu = false`. Inline FPU uses IEEE
double (52-bit mantissa) vs x87 extended (64-bit); extremely rare.

**Audio glitches / too much latency.** `XEMU_COREAUDIO_FRAMES=2048` for
a larger buffer. For DSP-heavy titles that glitch, confirm DSP JIT is
enabled (`audio.dsp_jit.enabled = true`).

---

## Architecture at a glance

```
Xbox game (30fps, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG with inline x87 → AArch64 FP
       │  floatx80↔double inline · FPCR cache · D-regs across TBs
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
       └─ APU: VP (per-voice pitch/vol) + DSP JIT (ARM64 basic-block)
           linear resampler · vDSP_vsma mixbin · CoreAudio IOProc
```
