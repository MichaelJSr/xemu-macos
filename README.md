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
- **Narrowed surface-upload finish.** `pgraph_vk_upload_surface_data`
  now only forces a full GPU sync when the target is the active
  color/zeta binding or a command buffer is recording. The common
  `render_display` path (which already ran a `PRESENTING` finish and
  uploads an unrelated source surface) and texture-streaming bursts
  skip the finish; MoltenVK's single queue serializes the aux upload
  behind any prior-submitted render work, and the aux-CB fence wait
  preserves host-visible ordering.
- **Narrowed remapped-attribute staging.** `pgraph_vk_bind_vertex_-
  attributes` shifts `vertex_attribute_offsets[i]` by
  `min_element * stride`, so `remap_unaligned_attributes` +
  `copy_remapped_attributes_to_inline_buffer` only size and copy
  `[min_element..max_element]` instead of `[0..max]`. Draws rebase
  via `vkCmdDraw(firstVertex - min_element)` /
  `vkCmdDrawIndexed(vertexOffset = -min_element)`. Saves
  `min_element * stride` bytes per remapped attribute per draw on
  indexed meshes / glyph / sprite batches where the first referenced
  vertex is well above zero.
- **Faster `surface_ranges` insert / remove.** Insert uses a
  `lower_bound` binary search instead of the linear `while` probe;
  remove consults a cached `surface_range_slot` index stored on
  `SurfaceBinding` instead of scanning the table for pointer
  equality. `memmove` cost is unchanged (array stays sorted by
  `start`); asymptotic find goes from O(n) to O(1) for remove and
  O(log n) for insert. Defensive assert on the remove path catches
  any missed `surface_range_slot` init.
- **Pipeline-rebuild decoupled from texture rebind.**
  `check_pipeline_dirty` no longer treats `texture_bindings_changed`
  as pipeline-dirty. `PipelineKey` contains no texture identity
  (render-pass state, shader state, 5 raster/blend regs, vertex
  layout); a rebind only affects descriptor set 1, which already
  refreshes independently via `pgraph_vk_update_descriptor_sets`.
  The common texture-change-per-draw case now hits the fast-out,
  avoiding `init_pipeline_key` + sub-hashes + LRU lookup.
- **`vkCmdBindVertexBuffers` dedup.** `bind_vertex_buffer` memcmp's
  the would-be `buffers[]` / `offsets[]` against the last issued
  bind on the same CB; matches skip the Vulkan call. Reset on CB
  begin alongside `dynstate_cache_valid`. Kills the per-flush
  vertex-bind call when back-to-back flushes hit the same mesh
  (common during material swaps / instanced-style draws).
- **`vkCmdPushConstants` dedup for inline uniform attrs.**
  `push_vertex_attr_values` caches the last payload + pipeline
  layout + attr count on the CB; skips the push when all three
  match. Layout handle is part of the fingerprint, so a pipeline
  rebind to a different layout implicitly busts the cache without
  a separate invalidation hook. Big win on fixed-function-transform
  titles that keep inline uniform attrs constant across many draws.

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
- **Correct `deltaTime` units for `MTLFXFrameInterpolator`.** Host-
  monotonic timestamps (`QEMU_CLOCK_HOST`) are captured at each
  input-frame `CFRetain`; `interp.deltaTime` now gets the wall-clock
  seconds between the two inputs (clamped to `[1/240, 1/10]` s), per
  Apple's API contract, instead of a unitless `(index+1)/(total+1)`
  ratio. Reduces ghosting / motion-vector lag on fast pans.
- **Host-refresh-aligned vblank cadence.** `ui/xemu.c`'s vblank timer
  interval is no longer a 60 Hz hardcode — it's recomputed per tick
  as `1 / min(SDL_GetCurrentDisplayMode.refresh_rate, 60 * interp_factor)`
  (floor 30 Hz, cap 240 Hz). On a 120 Hz ProMotion panel with 2x /
  4x MetalFX frame interpolation, interpolated frames now actually
  reach the panel, removing the every-other-refresh judder. 60 Hz
  panels and interp-off keep their existing 16.67 ms interval.
  Guest-side NV2A vblank IRQs are unaffected (driven by a separate
  NV2A-model timer).

### MCPX APU

- **Voice processing.** 128-byte voice struct memcpy'd to stack; all
  `voice_get/set_mask` calls hit the local copy. LUTs replace
  transcendentals on the hot path: attenuation (4096), pitch (65536),
  LPF cutoff (65536), envelope decay/release (1024 each, linear
  interp).
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
  fence wait.
- **`pgraph_vk_upload_surface_data` flush narrowing.** Skip the
  unconditional `pgraph_vk_finish` when the target surface isn't bound
  to the open render pass.
- **LRU eviction fast path.** `lru_try_evict_one` walks the tail
  linearly; aux evictable queue or per-slot bitmask.
- **`MTLFXFrameInterpolator` `deltaTime` in seconds.** Currently fed a
  frame-sequence ratio; feed wall-clock seconds for better temporal IQ.
- **Voice-register writeback batching.** Defer `ram_stl` in
  `voice_set_mask` to a single `memcpy` at end of `voice_process` (behavior
  risk: mid-frame MMIO reads see stale data).
- **`qemu_cpu_kick` via `dispatch_semaphore_t`.** `pthread_kill(SIGUSR1)`
  has ~50 µs P99 jitter on macOS 26.
- **Vblank cadence aligned to host refresh.** `ui/xemu.c` hardcodes 60 Hz;
  align to `SDL_GetDisplayMode` for 120 Hz ProMotion smoothness.
- **TCG AArch64 3-MOVK constant materialization.** Beats the literal-pool
  LDR latency on Apple chips.
- **Cross-TB FPCR elision.** `cached_fpuc_rc` as a TCG global register
  across chained TBs → pure register compare instead of memory reload.
- **DSP JIT parmove+ALU fusion.** With pinning in place the save/restore
  dance is already 1-insn UBFX; remaining fold candidates are shape-
  specific (skip redundant `save_reg` → mem → reg round-trips when the
  parmove's own destination provides the same value).

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
