# xemu-macos: Optimized Xbox Emulator for Apple Silicon

Fork of [xemu](https://github.com/xemu-project/xemu) tuned for Apple Silicon
(M1/M2+) on macOS 14+ (best on macOS 26 Tahoe). Adds MoltenVK support
(unsupported upstream), an inline ARM64 x87 FPU, full DSP JIT, MetalFX
upscaling + frame interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

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

### Building for Windows

The upstream Windows build paths are preserved and CI-tested. All
portable optimizations apply automatically: the Vulkan renderer work
(flight slots, dirty hashing, spatial index, draw-path dedup, tight
barriers, query drain at slot reclaim), the APU work (LUTs, batched
register reads, SVF cache, mono paths, SSE2 mix kernels), the pfifo /
BQL fixes, and the helper-based hard FPU on x86_64. macOS-only:
MetalFX, IOSurface presentation, CoreAudio, vDSP, the ARM64 DSP JIT,
and the AArch64 inline x87 FPU (Windows uses the bit-equivalent
helper-based hard FPU instead). The MoltenVK CPU primitive-emulation
paths don't activate on native Vulkan drivers.

- **Native (MSYS2/MINGW):** `./build.sh` from an MSYS2 shell.
  Release builds default to `-Dx86_version=3`; `XEMU_PGO=generate` /
  `XEMU_PGO=use` work like on macOS.
- **Cross (Docker):** `./build.sh -p win64-cross` with the
  `ubuntu-win64-cross` toolchain image (see
  `.github/workflows/build-windows.yml`).

### Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use` for PGO |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Hardened-runtime codesign via `xemu.entitlements` |
| `XEMU_COREAUDIO_FRAMES` | `1024` | CoreAudio buffer (≈21 ms @ 48 kHz) |
| `XEMU_PFIFO_HEARTBEAT` | `0` | 2-second pfifo diagnostic snapshot |

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
presentation_backend = 'auto'   # auto | opengl | metal (auto = Metal w/ Vulkan renderer)
fullscreen_on_startup = true
fullscreen_exclusive = true
startup_size = '1920x1080'

[display.ui]
fit = 'stretch'

[perf]
hard_fpu = true                 # ARM64 inline x87
cache_shaders = true
audio.dsp_jit.enabled = true    # ARM64 basic-block DSP JIT
```

In-app Settings covers the main toggles.

---

## Changes

### CPU / JIT (ARM64)

- **Inline hard FPU.** x87 ops emit native AArch64 FP
  (`FADD`/`FMUL`/`FDIV`/`FSQRT`); `floatx80`↔`double` inlined
  (~15 vs ~40 host insns); values stay in D-regs across TBs.
  Inlined: `fucomi`, `fnstsw`, `frndint`, FPU constant loads,
  `fdecstp`/`fincstp`, `ffree`/`ffreep`, 16-bit `FIST`, and all
  `FISTTP` widths. (FISTTP inline is AArch64-only: `tcg_gen_cvt_f*_i*`
  lowers to `FCVTZS` on AArch64 but to MXCSR-rounded `CVTSS2SI` on
  x86-64, so non-AArch64 hosts fall back to the floatx80 helper.)
  ~30% faster than the helper-call path, 4–8× faster than softfloat.
- **FPCR caching + rounding-correct FIST.** `MSR FPCR` re-emitted only
  when guest RC bits change. `FIST`/`FISTP` honor the control word via
  fused `cvt_{rn,rm,rp}_i{32,64}_f{32,64}` TCG ops (single host
  `FCVT{N,M,P,Z}S` on AArch64).
- **Signaling vs quiet compares.** `coms_f32`/`coms_f64` (FCMPE /
  COMISS) for FCOMI; `com_f32`/`com_f64` for FUCOMI. Matches x87 NaN
  semantics.
- **Control-word resync.** Every path that writes `env->fpuc`
  (`fldcw`, `fldenv`, `frstor`, `fxrstor`, `fninit`, xsave helpers,
  `cpu_post_load`, gdbstub, SEV, reset) routes through
  `cpu_set_fpuc` so `HF_FPU_RC` / `HF_FPU_PC` in `hflags` are
  rebuilt — otherwise fused FIST would round with the stale mode.
- **Misc.** `gen_flcr` materializes rc-bits from `tb->flags`;
  `fnstcw` is one load; `insertion_sort_syncs` replaces `qsort`
  for N ≤ 16; `flcr` is 5 insns via `RBIT`.
- **3-MOVK constant materialization.** `tcg_out_movi` now emits
  MOVZ/MOVN + up to 3 MOVK for 3- and 4-chunk constants instead of
  dumping them into the literal pool (LDR + D-cache pressure).
  48-bit host pointers — materialized constantly in TB prologues —
  stay in the instruction stream on Apple cores.

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces, CPU-side quads /
  line loops / triangle fans / provoking-vertex emulation,
  fragment-shader depth fallback.
- **Descriptor / pipeline.** Split sets (UBOs vs textures) with
  last-bound tracking; coalesced multi-binding writes. Dynamic
  blend / depth bias / viewport / scissor / line width / blend
  constants cached per-CB, skipped on match. Pipeline key excludes
  dynamic state and `NV_PGRAPH_CONTROL_3` (routed via `ShaderState` /
  `PROVOKING_VERTEX`). `check_pipeline_dirty` ignores
  `texture_bindings_changed`; set-1 refresh via
  `pgraph_vk_update_descriptor_sets` handles rebinds.
- **Flight slots (N=2) + single-submit fast path.** CPU records
  slot 1 while GPU runs slot 0; aux CB skipped when staging is empty.
- **Texture upload.** Batched + bump-allocator staging; parallel CPU
  decode pool capped at `MIN(nproc, 4)`. Non-compressed 2/4-bpp
  textures use a compute-shader unswizzler. `VkBufferImageCopy`
  regions stack-allocated (bounded by 6 layers × 16 mips).
- **Dirty tracking.** Per-chunk XXH3 on page-aligned textures
  ≥ 256 KiB (saves 30-60% hash CPU on sparse atlas updates). UBO
  `memcmp` before `memcpy`. Surface expiry throttled to host
  wall-clock (~33 ms).
- **Texture VRAM spatial index.** Active `TextureBinding`s bucketed
  by 128 KiB VRAM ranges; `pgraph_vk_mark_textures_possibly_dirty`
  scans only touched buckets instead of all 65536 LRU bins. Lazy
  init on first dirty signal; seeded from active LRU; maintained
  via insert/remove on cache miss / eviction. Whole-VRAM signals
  (renderer flush) take a one-pass LRU walk instead of visiting
  every bucket (spanning textures appear once, not per-bucket).
- **Query-pool drain at slot reclaim.** Occlusion queries are
  partitioned per flight slot; each submission's queries + pending
  guest reports are handed to the slot at submit and drained when
  the slot fence is reaped — `pgraph_vk_finish` no longer blocks on
  `vkGetQueryPoolResults(WAIT_BIT)` for the work it *just*
  submitted (which defeated the 2-slot pipeline on every finish in
  zpass-report titles). Guest-facing paths (`STALLED`,
  `REPORTS_FULL`, `FLUSH`) still drain synchronously so report
  polling can't deadlock and savestate/renderer-switch stay
  consistent.
- **LRU free-list + true-LRU eviction.** `lru.h` keeps free nodes on
  a dedicated list (O(1) allocation) and `global` holds only in-use
  nodes, so eviction starts at the true LRU entry instead of
  scanning a mixed list from the tail — O(cache size) per eviction
  before, relevant for the texture cache and the 50k-entry shader
  module cache.
- **Texture-upload CPU trims.** Post-snapshot full re-hash skipped
  when the dirty re-check proves no guest write landed during the
  snapshot memcpy (the snapshot is then bit-identical to what the
  incremental chunk hash covered — also keeps the stored hash
  comparable with the chunked composition); `TextureLayout` (~5 KiB)
  reuses a single scratch instead of g_malloc0/g_free per upload.
- **Tight barriers.** Precise stage masks replace
  `ALL_COMMANDS_BIT`; exact byte ranges replace `VK_WHOLE_SIZE`;
  coherent-memory flushes skipped via per-buffer `is_coherent`.
- **Renderer-switch hardening.** AB-BA-safe lock ordering on
  GL↔VK toggle; atomic `flush_pending`; `pgraph.lock` dropped across
  `framebuffer_released` wait.
- **Monotonic sync clock.** `pgraph_vk_sync` uses `QEMU_CLOCK_HOST`
  (suspend / NTP slew don't stall the 8 ms gate).
- **Fence-wait diagnostics.** 5 s timeout wrapper on single-time CBs
  aborts with the named call site instead of hanging silently on
  MoltenVK internal-mutex deadlocks.
- **Narrowed GPU syncs.** `pgraph_vk_upload_surface_data`'s full
  sync only fires when the target is the active color/zeta binding
  or a CB is recording; otherwise MoltenVK's single-queue order +
  aux-CB fence wait suffice. `invalidate_overlapping_surfaces`
  collects into a 64-entry stack array in one pass (heap fallback
  for >64).
- **Narrowed remapped-attribute staging.**
  `pgraph_vk_bind_vertex_attributes` shifts
  `vertex_attribute_offsets[i]` by `min_element * stride`;
  `remap_unaligned_attributes` copies only
  `[min_element..max_element]`. Draws rebase via `firstVertex -
  min_element` / `vertexOffset = -min_element`.
- **Draw-path dedup caches** (memcmp against last CB-scoped value,
  skip on match, reset on CB begin):
  - `vkCmdBindVertexBuffers` (buffers + offsets)
  - `vkCmdBindIndexBuffer` (buffer + offset + type)
  - `vkCmdPushConstants` (inline uniform attrs, keyed on layout)
  - Vertex layout snapshot (attribute + binding descriptions;
    replaces two `fast_hash` passes per draw)
  - Scaled `surface_binding_dim`
- **`surface_ranges` insert / remove.** `lower_bound` insert
  (O(log n)) and cached `surface_range_slot` remove (O(1)) replace
  linear scans. One-shot `fprintf` on invariant break for release
  observability.
- **Shader-uniform pull gated on `shader_uniform_inputs_dirty`.**
  Set by `pgraph_reg_w`, every `ltctxa/b/c1/vsh_constants` writer,
  every `vertex_attributes[].inline_value` writer, GL texture
  rebinds, renderer switch. `bind_shaders` skips
  `update_shader_uniforms` when clean.
  `pgraph_update_inline_value` memcmp's before write.
- **Correctness.** Frame-interpolation state (saved prev/cur
  IOSurface pair + remaining count) is dropped on display resize so
  deferred interpolation can't present stale-resolution frames.
  Screenshots/thumbnails query the framebuffer through
  `GL_TEXTURE_RECTANGLE` on the IOSurface path (previously
  `GL_TEXTURE_2D` returned zero dimensions → broken captures).
  Partial-channel clear updates cached scissor +
  blend constants so subsequent same-CB draws on the pipeline
  fast-out path don't inherit the overrides. PVIDEO non-coherent
  flush uses `row_bytes * in_height` (exact CPU-written range)
  instead of `yuv_size` (short for odd `in_width`). GPU-unswizzle
  upload emits WAR fences on `BUFFER_COMPUTE_DST` /
  `BUFFER_COMPUTE_SRC` between iterations and once up-front.
  `create_texture` snapshots guest VRAM once into a grow-only
  scratch on `PGRAPHVkState`, hashes + uploads from the snapshot,
  and re-checks the dirty bitmap afterwards; closes the CPU-CPU
  tear race between guest writes and the renderer memcpy that
  otherwise showed as single-frame Morton-tiled magenta flashes
  on 2/4-bpp textures under heavy contention. GPU-unswizzle levels
  point `TextureLevel.decoded_data` directly into the snapshot,
  dropping the per-level intermediate copy.

### MetalFX + presentation

- **Metal-native presentation backend** (macOS). New
  `display.window.presentation_backend = auto | opengl | metal`;
  `auto` selects Metal when the Vulkan renderer is configured. The
  main window becomes `SDL_WINDOW_METAL` + `CAMetalLayer`
  (`ui/xemu-metal.m`): the NV2A present frame is handed off as an
  IOSurface/`MTLTexture` (`nv2a_get_present_frame`, new
  `get_present_frame` renderer op) and composited with an MSL gamma
  blit; ImGui renders through `imgui_impl_metal`; the entire
  GL blit hop + `glFlush` + CGL rebind disappears from the present
  path. The complete UI (mask decals, SDF logo, render-to-texture
  panels, thumbnails, screenshots via GPU readback) has a Metal twin
  in `ui/xui/metal-helpers.mm`; the GL path remains fully
  selectable (`presentation_backend = 'opengl'`) and is the only
  path for the OpenGL NV2A renderer (switching renderer away from
  Vulkan under Metal prompts for a restart).
- **Async MetalFX via `MTLSharedEvent`** (Metal backend only). The
  three `waitUntilCompleted` stalls (spatial/temporal/interp,
  1-5 ms/frame on the PFIFO thread) are replaced by a monotonic
  shared-event signal; the UI present pass encodes a GPU-side wait
  on the frame's value before sampling. The GL backend keeps the
  synchronous waits (GL cannot wait on `MTLSharedEvent`).
- **1920px output cap lifted** (Metal backend only). MetalFX outputs
  rotate through rings of 3 private `MTLTexture`s instead of one
  shared IOSurface — no IOSurface means the macOS 26 BGRA
  `bytesPerRow` bug doesn't apply, so upscaling targets the
  CAMetalLayer's pixel size (aspect-fit), e.g. 1280×960 → 2880×2160
  on a 4K panel instead of 1920×1440. The ring also makes frame
  interpolation receive genuinely distinct prev/cur frames (the
  single shared output surface aliased them).
- **Spatial + temporal upscaling** (`MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`). Temporal uses 8-frame Halton(2,3) jitter
  + optional compute-kernel synthetic depth. Shared `MTLDevice` +
  `MTLCommandQueue` refcounted with MRC discipline.
- **Frame interpolation** (`MTLFXFrameInterpolator`, macOS 26+):
  2× = 60 fps, 4× = 120 fps from 30 fps; deferred generation on
  idle syncs.
- **Teardown safety.** Per-subsystem in-flight counter drains before
  releasing. IOSurface cache keyed on `IOSurfaceGetID`; output width
  capped at 1920 under the GL backend (macOS 26 BGRA `bytesPerRow`
  bug).
- **`MTLFXFrameInterpolator.deltaTime` in wall-clock seconds**
  (`QEMU_CLOCK_HOST` between input `CFRetain`s, clamped
  `[1/240, 1/10]` s) instead of a unitless ratio. Reduces ghosting
  on fast pans.
- **Exclusive fullscreen picks the highest refresh** among the
  native-resolution modes (120 Hz instead of a 60 Hz first entry);
  Metal vsync maps to `CAMetalLayer.displaySyncEnabled`.

### MCPX APU

- **Voice processing.** 128-byte voice struct memcpy'd to stack;
  `voice_get/set_mask` hits the local copy. Paused voices skip the
  memcpy entirely (peek PAUSED / STEREO from guest RAM first).
  LUTs replace transcendentals: attenuation (4096), pitch (65536),
  LPF cutoff (65536), envelope decay / release (1024 each, linear
  interp, branchless clamp).
- **Batched voice register reads.** VBIN / FMT / VOLA / VOLB / VOLC:
  one word-load per register + constant-mask bit extracts; replaces
  a ~14-call `voice_get_mask` fanout per active voice per frame.
  3D voices skip VBIN V0..V3 (overwritten from `hrtf_submix[]`).
  Same batching in `voice_get_samples` (runs inside the resampler
  callback: 9 CFG_FMT fields from one word-load) and
  `get_voice_bin_src_dst` (10 reads → 2 word-loads per queued
  voice). The paused-voice peek loads FMT/STATE directly off the
  precomputed `voice_addr` instead of two `voice_get_mask` round
  trips.
- **Mono LPF skip.** Mono voices carry identical data in both
  channels and both use the FCA register; the DLS2 SVF now filters
  ch 0 only and mirrors the result, halving SVF cost on the common
  mono-voice case (ch0==ch1 invariant kept for HRTF / monitor).
- **HRTF FIR linearization.** `hrtf_filter_process` linearizes the
  circular history once per frame (two-span memcpy + input append)
  so the 31-tap convolution runs over contiguous memory with no
  per-tap modulo — the `% HRTF_BUFLEN` indexing defeated the
  autovectorizer and dominated 3D-voice CPU time. (Hand-NEON was
  tried and reverted; removing the modulo lets `-O3` autovectorize
  profitably instead.)
- **SSE2 mix kernels (non-Apple x86_64).** The vDSP mixbin
  accumulate, `float_accumulate`, and `float_to_24b_bulk` now have
  SSE2 equivalents so Windows / Linux x86_64 builds get the same
  vectorized audio mix path (float-domain clamp before
  `cvtps2dq` matches NEON/lrint saturation semantics).
- **SVF coefficient cache.** `setup_svf` short-circuits when
  `(fc, q, filter-type)` is unchanged — skips `sqrtf` + stores on
  the common voice-per-channel-per-frame path.
- **Mono libsamplerate.** 1-channel `src_callback_new` for mono
  voices (was always 2-channel with duplicated output); format
  changes trigger `src_delete` + recreate. Halves the resampler's
  inner loop for the common mono case.
- **Accelerate / vDSP.** `vDSP_vsma` for 8-bin × 32-sample mix
  accumulation, `vDSP_vadd` for `float_accumulate`; `float_to_24b`
  uses `vcvtnq_s32_f32` in bulk on ARM64. Single-precision SVF/LPF.
  `SRC_LINEAR` resampler matches Xbox hardware.
- **Atomic consistency.** `d->regs[]` writers, `voice_locked[]`,
  `pause_requested`, `NV_PAPU_FEMEMADDR` load, and `VPVADDR` /
  `FENADDR` reads in lock-free VP paths all use `qatomic_*` so the
  VP frame thread, workers, and lock-free MMIO dispatcher agree
  under weak ordering. `fe_method`'s register reads (FECV / FEAV /
  list tops / VPSGEADDR / VPSSLADDR / FETFORCE1) follow the same
  contract.
- **CoreAudio.** `os_unfair_lock` with trylock. Underruns now
  partial-fill (drain what's pending, zero only the tail) instead
  of zeroing the whole IOProc buffer on any shortfall.
- **Full DSP JIT (opt-in, default on for new configs).** ARM64
  basic-block JIT for both MCPX DSP56300 cores (GP + EP). Enable
  via `[perf] audio.dsp_jit.enabled = true` or `XEMU_DSP_JIT=1`.
  100% ALU inlined, 99.99% CF inlined; only `emu_undefined` stays
  on BLR — the 16-variant `bit_manip` tail (bset/bclr/bchg/btst ×
  aa/ea/pp/reg) is now emitted inline (REG variants targeting
  side-effect registers SR/OMR/SP/SSH/SSL keep the BLR fallback),
  and `pm_read_accu24` — the limited A/B read on every accumulator
  parmove — is inlined for the scaling=0 case universal on Xbox
  (2-insn fits-in-24-bit check on the sign-extended pin; SR.L
  semantics preserved; non-zero scaling BLRs the bit-exact helper).
  A/B/X/Y/SR accumulators pinned in callee-saved ARM64 regs.
  Static block chaining for unconditional + conditional-taken
  terminators keeps hot inner loops inside JIT code, with
  retro-chaining: a terminator whose target isn't translated yet
  emits a patchable chain site (initially routed to the shared
  exit) that gets patched to the target's `chain_entry` when it is
  later translated; incoming-chain cap raised 8 → 16. Chain-site
  repatching at block install was also moved inside the JIT-write
  window (previously ran after `qemu_thread_jit_execute()` — a
  latent W^X fault on the list-full path). Lazy-flag
  elimination (full-dead + per-flag N/Z + E/U halves) elides ~20%
  of SR updates on Azurik. Bit-exact harness:
  `XEMU_DSP_JIT_DIFF=N` validates every Nth unique block against
  the interpreter on an off-thread worker (bounded via
  `diff_checked`). `XEMU_DSP_JIT_STATS=1` dumps per-run
  inline/fallback counters, lazy-flag skips, and
  `g_cf_fallback_buckets`. See
  [docs/dsp-jit-design.md](docs/dsp-jit-design.md).

### Threads + runtime

- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE`.
- **BQL discipline.** `surface_access_callback` drops BQL before
  blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`; kick-check +
  release-wait under `d->pfifo.lock` guarantees no missed wake-up.
  `halt` via `qatomic_read` matches the writer contract.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`, `-mcpu`
  auto-detected. STBI_NEON + fpng CRC32 for ARM64.
- macOS 26 build fixes: `download-macos-libs.py` uses
  `os.path.abspath` and repairs stale `prefix=` lines in vendored
  `.pc` files; `build.sh` strips all `LC_RPATH` before
  `dylibbundler` and adds the single correct one.
- Ad-hoc codesign by default (MAP_JIT works);
  `XEMU_CODESIGN_ENTITLEMENTS=1` opts into hardened runtime +
  `xemu.entitlements`. For notarized distribution use
  `scripts/sign-macos-release.sh`.
- Optional PGO (`XEMU_PGO=generate` → run → `XEMU_PGO=use`) — now
  also wired into the native Windows (MSYS2) branch.
- Native Windows release builds default to `-Dx86_version=3`
  (AVX2 / BMI2 / FMA — matches CI release config; override by
  passing your own `-Dx86_version=`).

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
| Async MetalFX under *GL presentation* | GL↔Metal cross-API sync can't be expressed with `SDL_GL_SwapWindow` + vsync alone. Landed later for the Metal presentation backend, where both sides speak `MTLSharedEvent` |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export (CPU readback or `vkExportMetalObjectsEXT`) | CPU: 2 ms/frame + 18 MiB of copies for minimal quality. Metal export deadlocks MoltenVK's internal mutex |
| Direct-VRAM compute unswizzle via `VK_EXT_external_memory_host` | `HOST_WRITE → SHADER_READ` barriers enforce visibility up to *submission*, not GPU execution; on HOST_COHERENT memory CPU can tear reads mid-frame → particle/HUD flicker |
| Host-imported `BUFFER_VERTEX_RAM` | Same torn-read class for vertex data; GPU fetches see two CPU frames mid-primitive on dynamic meshes |
| `pthread_jit_write_with_callback_np` for `tb_phys_invalidate` | Scoped-W semantics force-drop W on callback return, incompatible with QEMU's nested JIT-write paths |
| `VK_KHR_dynamic_rendering` full lowering | Missing `VK_SUBPASS_EXTERNAL → first subpass` dep; back-to-back passes stalled Metal's tile renderer |
| APU voice resampler `rate == 1.0` fast path | Skipped libsamplerate but didn't replicate silence-padding on voice drain → `voice_process` spun on draining voices during scene transitions |
| `HLT` BSOD recovery (IF=1 force at-HLT) | Unblocks `CLI;HLT` deadlocks, but the IRQ-loop pattern is a *symptom* of a stuck thread elsewhere |
| Surface-expiry throttle keyed on `pg->frame_time` | `frame_time` advances only on `NV097_FLIP_INCREMENT_WRITE`; rapid level loads starved the prune → black-screen freeze. Switched to host wall-clock |
| Surface-range binary search on `end` | `end` isn't monotonic in a start-sorted array → skipped valid overlaps |
| Two-level quick texture hash / hash-skip > 64 KiB | 192-byte sampling missed content changes (YUV / FMV); false positives caused GPU re-upload churn |
| Dirty-range VRAM flush (early) | Bitmap cleared before `flush_memory_buffer` read it. Tracked per-flight `[first, last]` range does the safe equivalent |
| Incremental texture hash on misaligned textures | Host-page boundaries straddle chunks → `test_and_clear_dirty` steals another chunk's signal. Gated to page-aligned textures only |
| HRTF hand-NEON | Gather-then-FMA on a circular buffer defeated OoO overlap; `-O3 -mcpu=native` autovectorizes better |
| BQL event batching | Breaks QEMU cooperative scheduling |
| TLS-cached `pthread_jit_write_protect_np` | Per-thread cache drifts from kernel W^X on nested `tb_gen_code` / setjmp paths → SIGSEGV/SIGILL after ~1 min |
| Hoist `can_fifo_access` out of pfifo pusher word loop | `pfifo_run_puller` drops `pfifo.lock` when taking `pgraph.lock`, so "lock held throughout" is false → `ERROR_CALL` on boot |
| DSP JIT round-4 `cur_inst` preset skip | Regresses on startup with `op=0x001000` — some inlined path reads `cur_inst` at runtime that the static classifier doesn't see. Harness (`XEMU_DSP_JIT_SENTINEL` / `_FORCE`) kept in-tree |
| DSP JIT round-4 EPI_NO_PC | 9.75% mismatch per pcskip sentinel — calc_ea mode 6 lengthens parmoves and REP/DO loops rewind pc; both legitimately diverge |
| Vblank cadence aligned to host display refresh | Host `vblank_interval_ns` drives `process_vblank` → `graphic_hw_update` → `nv2a_vga_gfx_update` → `NV_PCRTC_INTR_0_VBLANK`. Retuning retimes the *guest* IRQ (titles gate sim on vblank count). Needs NV2A-internal timer (fixed 60 / 50 Hz) decoupled from host present cadence |
| AArch64 `fsin` / `fcos` libm-helper inline | Swapping `gen_helper_fsin` / `_fcos` (floatx80 round-trip) for a thin `helper_sin_fast_f{32,64}` / `cos_fast_*` (f-bits → `memcpy` → libm → bits) made audio crackly on at least one title. Plausible causes: (a) `gen_flush_fp` was also the de-facto flush for *other* live x87 temps; (b) `TCG_CALL_NO_RWG_SE` let TCG reorder across boundaries the floatx80 helper implicitly fenced |
| Voice-list `SE2FE_IDLE_VOICE` re-notify skip | No guest-write hook on `NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` transitions → can't reliably clear a "notified this activation" flag. Risk of missing an FE-observed idle transition outweighs the per-frame save |
| Cross-TB FPCR elision via `cached_fpuc_rc` as TCG global | TCG globals are memory-backed and reloaded per TB — a global doesn't eliminate the per-TB `ld16u` in `gen_flcr`, it just renames it. Eliminating the load needs either TB-chain metadata (RC equality across chain edges) or relaxing `gen_flush_fp`'s `flcr_set` reset — the same implicit-fence class that made the fsin/fcos inline regress. No win available at acceptable risk |

---

## Future vectors

Not attempted, or scope/risk too high for a one-shot change.

- **GL NV2A renderer under the Metal window.** Currently a
  renderer switch away from Vulkan under the Metal backend needs a
  restart; full unification would render the GL display buffer into
  an IOSurface-backed FBO and feed the same present-frame handoff.
- **Aux-submit pipelining for `render_display`.**
  `pgraph_vk_end_single_time_commands` waits its fence
  synchronously; MetalFX's command queue is distinct from
  MoltenVK's so deferring races. Needs `MTLSharedEvent` cross-queue
  sync.
- **Texture-upload barrier batching.** Current per-mip
  `pre_compute` / `post_compute` pair is load-bearing on reused
  `COMPUTE_DST` / `COMPUTE_SRC`; batching requires disjoint offsets
  + dispatch-offset args.
- **`VK_EXT_external_memory_host` with snapshot scheme.** Per-flight
  COW or `MTLSharedEvent`-keyed boundary to sidestep the
  host-coherent tear.
- **`VK_KHR_dynamic_rendering` with explicit barriers.** Emit
  `vkCmdPipelineBarrier` around every `BeginRendering` /
  `EndRendering`.
- **`MTLResidencySet` (macOS 15+).** Pin frequently-used Metal
  buffers/textures as resident.
- **BINK video via VideoToolbox.** Xbox BINK decoder is CPU-bound;
  offload YUV→RGBA (or full transcode).
- **Shader specialization constants.** Burn alpha-test / fog-enable
  into compile-time constants; eliminates per-draw uniform
  bandwidth + fragment branches.
- **GPU S3TC decode.** Compute-shader decoder offloads the CPU
  thread pool.
- **Decoupled guest-vblank IRQ timer.** Dedicated NV2A-model 60 /
  50 Hz timer lets the host present path retune for ProMotion
  without changing guest sim speed.
- **Voice-register writeback batching.** Defer `ram_stl` in
  `voice_set_mask` to end-of-frame memcpy (risk: mid-frame MMIO
  reads see stale data).
- **`qemu_cpu_kick` via `dispatch_semaphore_t`.**
  `pthread_kill(SIGUSR1)` has ~50 µs P99 jitter on macOS 26.
- **DSP JIT parmove+ALU fusion.** Redundant `save_reg` → mem → reg
  round-trips when the parmove's own destination provides the same
  value. (The other half of that design note — inlining
  `pm_read_accu24` for scaling=0 — has landed.)

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Crash on launch with "duplicate LC_RPATH".** Older builds — rebuild;
`build.sh` now strips rpaths.

**MetalFX not activating.** Requires `renderer = 'VULKAN'` and
`metalfx_mode = 'spatial'` or `'temporal'`. Under the GL presentation
backend, `surface_scale = 4` produces 2560×1920 input and is skipped
(1920 px BGRA-IOSurface safe cap). Check for
`MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.** Requires macOS 26.0+ and MetalFX
upscaling active.

**Black/garbled window or Metal init failure on launch.** Set
`[display.window] presentation_backend = 'opengl'` in `xemu.toml` to
fall back to the GL presentation path, and report the issue. The
`auto` default uses Metal only with the Vulkan renderer.

**FPU precision bug.** `[perf] hard_fpu = false`. Inline FPU uses IEEE
double (52-bit mantissa) vs x87 extended (64-bit); extremely rare.

**Audio glitches.** `XEMU_COREAUDIO_FRAMES=2048` for a larger buffer.
For DSP-heavy titles, confirm DSP JIT is enabled.

---

## Architecture at a glance

```
Xbox game (30fps, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG, inline x87 → AArch64 FP
       │  floatx80↔double inline · FPCR cache · D-regs across TBs
       │
       ├─ GPU: NV2A Vulkan (MoltenVK)
       │  flight slots · bump-alloc staging · VRAM spatial dirty index
       │  compute unswizzle + YUV · IOSurface zero-copy
       │        │
       │        └─ MetalFX Spatial / Temporal (async, MTLSharedEvent)
       │             │   Metal backend: private-texture ring → panel-fit
       │             │   (e.g. 1280×960→2880×2160) · GL: 1920 cap
       │             └─ MTLFXFrameInterpolator (30→60 or 30→120)
       │                  │
       │                  ├─ Metal backend: CAMetalLayer drawable +
       │                  │  MSL gamma blit + ImGui Metal (default)
       │                  └─ GL backend: CGLTexImageIOSurface2D → SDL3 GL
       │
       └─ APU: VP (per-voice pitch / vol) + DSP JIT (ARM64)
           linear resampler (1ch mono / 2ch stereo) · vDSP_vsma mixbin
           CoreAudio IOProc w/ partial-fill on underrun
```
