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
portable optimizations (the Vulkan renderer and APU work, the
pfifo / BQL fixes, the `XEMU_NV2A_NSPROF` profiler, the helper-based
hard FPU on x86_64) apply automatically. macOS-only pieces (MetalFX,
Metal / IOSurface presentation, CoreAudio, vDSP, the ARM64 DSP JIT,
the AArch64 inline x87 FPU) compile out, and the MoltenVK CPU
primitive-emulation paths don't activate on native Vulkan drivers.

- **Native (MSYS2/MINGW):** `./build.sh` from an MSYS2 shell.
  Release builds default to `-Dx86_version=3`; `XEMU_PGO=generate` /
  `XEMU_PGO=use` work like on macOS. This is the preferred route —
  a Windows VM (e.g. Parallels on Apple Silicon for a Windows/ARM
  guest, or any x86 Windows box) builds natively and can run-test
  the result, unlike the cross-compile below.
- **Cross (Docker):** `./build.sh -p win64-cross` from a Linux
  container with the `xemu-win64-toolchain` image and
  `CROSSPREFIX=x86_64-w64-mingw32.static-` set (see
  `.github/workflows/build-windows.yml` for the exact image tags and
  env). Verified from an Apple Silicon Mac via colima
  (`colima start --vm-type vz --vz-rosetta`, ~12 min for a clean
  static `xemu.exe`):

  ```bash
  docker run --rm --platform linux/amd64 -v "$PWD:/xemu" -w /xemu \
    -e CROSSPREFIX=x86_64-w64-mingw32.static- \
    -e CROSSAR=x86_64-w64-mingw32.static-gcc-ar \
    ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd \
    ./build.sh -p win64-cross -Dx86_version=3
  ```

  Use a copy of the tree (the cross build reuses `build/`/`dist/`),
  kept under `$HOME` (colima only shares the home directory).

### Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use`. A trained profile is committed at `pgo/default.profdata` and applied by CI to arm64 release builds (+9.4% fps — see Build + packaging). Retrain after large code churn: `generate` build → play the bench scenes → `use` build → commit the regenerated profdata. |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Hardened-runtime codesign via `xemu.entitlements` |
| `XEMU_MOLTENVK_VERSION` | `1.4.1` | MoltenVK release auto-vendored into `macos-libs` when no usable system copy exists |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for `scripts/build-moltenvk.sh` (the maintained optimized MoltenVK) |
| `XEMU_VIS` | `0` | `-fvisibility=hidden` (≈1.5 MiB smaller arm64 binary, cleaner LTO) |
| `XEMU_STRIP` | `0` | `-Wl,-dead_strip` (≈4 MiB smaller combined with `XEMU_VIS=1`; validate `type_init` constructors survive before shipping) |

### How Vulkan is provisioned (all platforms)

The NV2A Vulkan renderer loads Vulkan at runtime through **volk** —
nothing links against a Vulkan library at build time.

- **macOS:** volk dlopens `libMoltenVK.dylib`, which `build.sh`
  bundles into `xemu.app/Contents/Libraries/<arch>/` (the app's
  `LC_RPATH` points there). One resolution rule (`resolve_moltenvk`
  in `build.sh`) serves configure and packaging: `macos-libs`
  (vendored), then `/usr/local` (Vulkan SDK / manual install /
  `scripts/build-moltenvk.sh`), then Homebrew — a dylib counts only
  if it has the target arch slice. When none exists, `build.sh`
  vendors the pinned official release (`XEMU_MOLTENVK_VERSION`)
  automatically, so releases built by CI always ship that pin, while
  a dev machine's `/usr/local` custom build takes precedence locally
  — `build.sh` logs the bundled version + UUID so tested-vs-shipped
  driver drift is visible. A missing dylib at bundle time is a hard
  build error (the fork's default renderer, Metal presentation path,
  and MetalFX all require it). End users need nothing installed: the
  app is self-contained.
- **Windows:** volk loads the system `vulkan-1.dll` — the Khronos
  loader every GPU vendor ships with its driver. Nothing is (or
  should be) bundled; a machine with NVIDIA/AMD/Intel drivers
  installed works out of the box. If the loader or a Vulkan 1.1+
  device is missing (typical inside VMs — Parallels/UTM/Hyper-V
  guests have no Vulkan ICD), xemu falls back to the OpenGL
  renderer automatically and shows a notification. On Windows
  ARM64 VMs, Microsoft's "OpenCL, OpenGL, and Vulkan Compatibility
  Pack" can provide a D3D12-backed Vulkan ICD where the guest has
  DX12; performance is not representative of real hardware.
- **Linux:** the distro `libvulkan` loader + the GPU's ICD.

### Runtime debug / escape-hatch knobs

All default off / fast-path; set to `1` to enable.

| Env var | Purpose |
|---|---|
| `XEMU_NV2A_NSPROF` | Wall-time frame profiler, 5 s summaries to stderr |
| `XEMU_PFIFO_HEARTBEAT` | 2-second pfifo diagnostic snapshot |
| `XEMU_ZETA_SHAPE_READBACK` | Restore GPU→CPU readback on zeta shape switches |
| `XEMU_TEX_BIND_RECHECK` | Restore per-bind texture dirty checks (vs once per frame) |
| `XEMU_VTX_EXACT` | `0` restores page-granular vertex-conflict finishes (vs byte-exact skip) |
| `XEMU_REPORTS_SYNC` | `1` restores synchronous zpass-report drains (vs flip-deferred + idle-budget fallback) |
| `XEMU_REPORTS_BUDGET_US` | Continuous-idle budget (µs) before the deferred-report safety-valve submit; default `300`, `5000` restores the pre-v0.10.1 value (clamped 0-100000) |
| `XEMU_MMIO_BQL` | `1` restores BQL-locked TCG dispatch for the audited lockless NV2A regions (PFB, USER, APU VP) — bisect hatch |
| `XEMU_MMIO_PROF` | `1` prints an exit histogram of guest MMIO traffic (region × page, loads/stores) + BQL acquire-wait totals |
| `XEMU_MAX_QUERIES` | Occlusion-query pool size, default `4096` (`begin_draw` guard submits before exhaustion) |
| `XEMU_INPUT_PIPE` | FIFO path; lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` inject input through normal bindings (works unfocused; test automation) |
| `XEMU_COREAUDIO_FRAMES` | CoreAudio buffer frames, default `1024` (≈21 ms @ 48 kHz) |
| `XEMU_MFX_REAL_DEPTH` | Feed real zeta depth to the temporal scaler (A/B) |
| `XEMU_GUEST_PROF` | One-run guest profiler: mach-thread sampler resolves vCPU samples to guest TBs vs host symbols; TB lookups classified by exit kind. Measurement-run only (not benchmark-neutral) |
| `XEMU_RAS` | `0` disables the near-return target memo (default on): 4096-entry eip→TB cache probed inline at ret sites (+0.77 fps, 6/6 pairs — see CPU / JIT changes). `-d exec` tracing won't log inline-hit rets — disable when tracing |
| `XEMU_TB_PROF` | Prints TB jump-cache totals at exit (lookups, hit%, htable walks, translations, tb_flush count) |
| `XEMU_INV_PROF` | Prints SMC / TB-invalidation-churn counters at exit: invalidations by source (notdirty vs explicit vs single-TB), the false-invalidation share a byte-range overlap check would skip, inv-htable recycle hit-rate (false-sharing vs true SMC), and translate-time FPU/exit census |
| `XEMU_TB_RANGE_INV` | `1` re-applies upstream's per-TB byte-range overlap filter inside the Xbox whole-page code-write invalidation (default off = invalidate every TB on a written code page). Correctness-safe (invalidates a correct subset — a TB whose bytes weren't written can't have changed); A/B knob for the SMC-false-sharing cure (`XEMU_INV_PROF` measured 100% false-invalidation, 0 true SMC) |
| `XEMU_CCOP_CENSUS` | `1` prints a runtime-weighted census of cc-flag liveness across TB boundaries at exit (predecessor tail class × successor head class). Forces every TB transition through the exec loop (no goto_tb / jump-cache / ret-memo) so pairs are exact — much slower, same executed instruction stream; sizing tool for the superblock roadmap item, never a perf mode |
| `XEMU_SSE_HOST` (alias `XEMU_SSE_NEON`) | NEON fast path for single-precision SSE arithmetic; default on for aarch64 with `perf.hard_fpu` (+1.90 fps — see CPU / JIT changes). `0` restores softfloat; `=2` runs both paths and aborts on divergence. x86_64 stays opt-in/dark: run `=2` clean on real silicon first |
| `XEMU_MFX_INTERP_ZERO_MOTION` | Old zero-motion interpolator binding (A/B) |
| `XEMU_PUSH_PRESENT` | Metal backend only: publish the present frame at flip so the UI reads it with no cross-thread round trip (removes the pull-model handshake wait; adds a `frame_seq` skip-when-unchanged dedup). Requires frame interpolation off; ignored on the GL backend |
| `XEMU_PUSH_PRESENT_REFUTE` | Debug: cross-check each pushed read against an immediate pull fetch (same `frame_seq` ⇒ identical texture/event/dims); prints `compared`/`mismatches` every ~5 s |
| `XEMU_DSP_JIT` | `0` disables the fork DSP JIT inside the interpreter engine (kill-switch; *enabling* is config-only — `audio.dsp_jit.enabled`) |
| `XEMU_DSP_JIT_STATS` / `XEMU_DSP_JIT_DIFF=N` | DSP JIT counters / bit-exact validation |
| `XEMU_DSP_JIT_NO_THROTTLE` | Disable the DSP JIT retranslation-churn auto-throttle |
| `XEMU_DSP_JIT_DIFF_SYNC` / `_DIFF_MAX` / `_DUMP` / `_PIN_AUDIT` / `_SENTINEL` / `_FORCE` | JIT bring-up harnesses (see `docs/dsp-jit-design.md`) |
| `XEMU_APU_PROF` | Per-second APU-thread utilization to stderr |

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

[audio.dsp_jit]
enabled = true                  # ARM64 inline basic-block DSP JIT
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
- **Sticky host-FPU bracket: the SSE fast path goes live on Apple
  Silicon (+1.9 fps).** The NEON SSE path had measured parity — the
  per-op serializing FPCR restore was worth exactly the SIMD win.
  The bracket is now sticky (host stays in guest-SSE mode between
  helpers; `=2` differential keeps strict brackets and re-confirmed
  bit-exactness). Measured **+1.90 fps, 3/3 pairs positive**;
  default on for aarch64 with `perf.hard_fpu`, `XEMU_SSE_HOST=0`
  restores softfloat. (x87 hard-FPU helpers save/restore around
  their own ops; softfloat/TCG carry no host FP-mode dependence.)
  Follow-up fix: the *inline* x87 path skips its `MSR FPCR` whenever
  the `cached_fpuc_rc` cache matches the TB's rounding bits, so the
  bracket now invalidates that cache when it rewrites FPCR —
  previously inline x87 could silently run under the sticky SSE
  FZ/RMode state (denormal flushing in x87 doubles). Zero cost in
  steady state (fires only on actual mode transitions); validated by
  a clean 90 s `XEMU_SSE_HOST=2` differential and fps parity.
- **TB-dispatch inline probes (+1.5 fps family, 2026-07-05).** The
  first guest-side profile (`XEMU_GUEST_PROF`: mach-thread sampler +
  per-exit-kind TB-lookup census) showed diffuse guest time (top TB
  1.8% — no hot-loop candidates) but near-returns at 46% of 15.8M/s
  TB lookups. Two shipped mechanisms: the near-return target memo — a
  4096-entry eip→TB cache probed inline at ret sites (`XEMU_RAS`,
  default on; **+0.77 fps, 6/6 pairs**; helper hits collapse
  362M→13k) — and the same inline-probe technique applied to the
  real jump cache at indirect/cross-page exits (emitter in
  `accel/tcg/xemu-inline-jc.c`, the sole owner of the cache layout;
  +0.70 fps with the enabled arm carrying ~+8% draw throughput). The
  APU VP doorbell block also joined the audited lockless-MMIO set.
  Two honest kills en route are in the failed-experiments table
  (JIT write-protect caching; the per-depth return-address ring).

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
- **`possibly_dirty` cleared after verified bind.** Upstream leaves
  the mark sticky once a page-sharing neighbor sets it, so every
  later bind re-hashed the full contents forever. A bind that
  verifies the cached hash (or re-uploads) now drops the mark;
  guest writes re-mark via the spatial index, and torn-snapshot
  binds keep it. Texture dirty-check + hash time fell ~2.1x
  (1.3-3.5 → 0.6-1.6 ms/flip in-game).
- **VkPipelineCache persisted across runs.** The cache was only
  saved in `pgraph_destroy()`, which a normal app quit never
  reaches — `pipeline_cache.bin` was never written, so every launch
  recompiled all MSL pipelines (measured 164 ms worst single
  `vkCreateGraphicsPipelines` on a cold cache). The PFIFO thread now
  flushes the cache at flip boundaries (≥ 30 s apart, only when new
  pipelines accumulated; write-then-rename keeps the file
  crash-consistent). Warm-cache worst case measured ≤ 4 ms — a ~40x
  hitch reduction on pipeline-heavy scene transitions.
- **`XEMU_NV2A_NSPROF=1` wall-time profiler.** `nv2a/nsprof.[ch]`:
  release-build-safe ns accumulators around every PFIFO-thread cost
  center (shader/pipeline gen, texture hash/snapshot/upload, fence
  waits, MetalFX drain, surface readbacks, the FLIP_STALL → vblank
  idle gap) plus event counters for finish reasons, draws/flip,
  draw-merge classification (`merge_*`), `vkCmdDraw*` call counts,
  and render-pass-end causes (`rpcause_*`); per-5 s summaries to
  stderr. This is what every optimization above was measured with.
  Observation-only (~3 flag stores per draw when off; fps parity
  46.83 ± 0.24 vs 46.59 ± 0.50).
- **Deferred-report idle budget cut 5 ms → 300 µs
  (`XEMU_REPORTS_BUDGET_US`).** Guests that consume a zpass report
  mid-frame spin-wait on it with an idle FIFO, and the safety-valve
  submit only fired after 5 ms of continuous idle — every such poll
  cost the guest's critical path up to the full budget. A too-small
  budget merely costs an extra small submit per idle episode
  (bounded by report count), so 300 µs is safe. Measured on a 770
  draws/flip scene: 24.04 ± 0.43 → 25.36 ± 0.22 fps (**+5.5%**, all
  pairs positive); artifact soak clean. `=5000` restores the old
  budget; `XEMU_REPORTS_SYNC=1` remains the full legacy hatch.
- **Vertex-mirror overwrite waits the just-submitted slot.** The
  `VERTEX_BUFFER_DIRTY` conflict path memcpy'd new guest data over
  the mirror right after finish — safe under upstream's synchronous
  finish, a CPU-vs-GPU race after flight-slot pipelining (the fork's
  finish waits only the *previous* slot). The conflict path now
  waits the submitted slot's fence and clears its upload tracking.
  Validated at fps parity vs v0.9 (24.45 ± 0.58 vs 24.48 ± 1.19),
  zero errors.
- **Invalid-surface destruction gated on submission retirement.**
  With pipelined finish, a quarantined surface image could be
  `vkDestroyImage`d while the just-submitted CB still referenced it
  (invalid usage on every driver; upstream's synchronous finish was
  immune). Evictions now carry the highest submission index that may
  reference them; a fence-derived retirement watermark gates
  destruction, and `pgraph_vk_surface_flush` drains all slots before
  its free-everything prune. Reuse of quarantined images needs no
  gate (ordered by the render pass's `VK_SUBPASS_EXTERNAL`
  dependency — proof comment at
  `get_any_compatible_invalid_surface`). Validated at fps parity
  (46.6-47.2 vs 46.83 ± 0.24), zero errors.
- **Texture/sampler eviction gated on submission retirement.** Third
  member of the same pipelined-finish race family (audit find, no
  observed crash): the LRU pre-evict guard only protected entries
  referenced by the *currently-recording* CB, but the just-submitted
  CB routinely still executes, and the memory-budget trim runs right
  after submit with that clause disabled — an evicted texture or
  sampler could be destroyed while the GPU still referenced it. Both
  caches now refuse eviction until `retired_submit_count` covers the
  entry's last-referencing submission (samplers gained the stamp
  field), with a slot-fence drain if a pool ever fills with pinned
  nodes and a drain in texture finalize so a live renderer switch
  can't leak refused nodes. Validated at fps parity (F5 +0.19 ± 0.14,
  3 interleaved pairs; F8 quiet-machine +0.12 ± 1.46, sign-mixed)
  with a 291-capture artifact soak clean.
- **Zeta shape-switch fast path.** A depth buffer ping-ponging
  between two shapes at one VRAM address every frame cost ~9
  `pgraph_vk_finish` fence cycles per flip (8-19 ms/flip): each
  switch did a full GPU→CPU readback, an eviction finish, and a
  multi-MB seed re-upload. Re-shaped zeta targets are cleared by the
  guest before use and the genuine RAM consumers measured zero, so
  the switch now skips the readback and seed upload and defers the
  eviction without a finish (old image quarantined until the
  recording CB rotates). Pending CPU-requested downloads are still
  honored; `XEMU_ZETA_SHAPE_READBACK=1` restores full fidelity.
- **Byte-exact vertex-conflict refinement.** Guest dirty bits are
  page-granular, so vertex-stream sync writes arrive page-padded and
  consecutive writes false-share their boundary page with the
  recording command buffer — each false conflict forced a finish
  (submit + rotation + mid-frame fence reclaim). A per-page
  written-span table + span-restricted memcmp proves most conflicts
  byte-identical and skips the finish (conservative: any differing
  byte keeps legacy behavior). Measured via the savestate A/B
  harness (`scripts/bench-savestate-ab.sh`, 3 interleaved pairs,
  baseline reproducibility ±0.02 fps) on a heavy in-game scene
  (408 draws/flip @ ~20 fps): forced finishes 5.65 → 0.37/flip
  (−93%), fence wait −11%, process CPU −7.6%, **fps +5.4%**.
  `XEMU_VTX_EXACT=0` restores page-granular conflicts.
- **Targeted vertex-RAM conflict wait.** Guest writes into VRAM
  pages uploaded by an *in-flight* slot forced a full finish
  (submit + slot rotate + fence) 3-6x per flip on streamed vertex
  data. When the conflict is with an already-submitted slot, only
  that slot's fence is waited (usually already signaled) and its
  upload tracking cleared; the full finish remains only for
  conflicts with the currently recording command buffer. Combined
  with the zeta fast path: medium scenes went 22-26 → 60 flips/s
  (guest vblank cap), heavy scenes ~22 → ~26-30, and per-flip
  fence waits fell from 8-19 ms to 1-3.5 ms.
- **Once-per-frame texture verification.** A binding verified (or
  uploaded) earlier in the same guest frame skips the per-bind
  dirty-bitmap scan + hash on re-binds unless re-marked
  possibly_dirty (~85% of the ~1100 binds/flip take the skip; the
  residual is genuine animated-content rehashing). A CPU write
  landing between two binds of the same texture within one frame
  takes effect one frame later; render-to-texture is unaffected
  (surfaces don't take this path). `XEMU_TEX_BIND_RECHECK=1`
  restores per-bind checks.
- **No hidden GL contexts under the Metal backend.** The 4 startup
  gloffscreen contexts (1 Vulkan-renderer + 3 GL-renderer) are
  skipped when presenting via Metal; live renderer switches away
  from Vulkan are deferred to the next launch (the settings UI
  already prompts for a restart), matching the window's fixed
  SDL_WINDOW_METAL type.
- **Texture VRAM spatial index.** Active `TextureBinding`s bucketed
  by 128 KiB VRAM ranges; `pgraph_vk_mark_textures_possibly_dirty`
  scans only touched buckets instead of all 65536 LRU bins. Lazy
  init on first dirty signal; seeded from active LRU; maintained
  via insert/remove on cache miss / eviction. Whole-VRAM signals
  (renderer flush) take a one-pass LRU walk instead of visiting
  every bucket (spanning textures appear once, not per-bucket).
- **MoltenVK configuration is launch-path independent (pink-tile
  fix).** The `MVK_CONFIG_*` tuning previously lived only in
  `Info.plist` `LSEnvironment`, which macOS applies to Finder
  launches exclusively — terminal launches ran library defaults, so
  automated testing could never see Finder-only bugs. The canonical
  values are now set in `main()` before MoltenVK loads (explicit
  env still overrides). Along the way,
  `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` moved from `2` to `0`:
  with whole-frame command buffers, immediate prefill encoding
  corrupted streamed textures (screen-wide magenta blocks, ~5% of
  frames in streaming-heavy areas), enabled an AGX visibility-buffer
  crash, and measured *slower* (46.3 vs 48.5 fps — encoding work
  landed on the PFIFO thread).
- **Maintained MoltenVK build** (`scripts/build-moltenvk.sh`):
  pinned upstream commit, `-O3`, per-machine `-mcpu`, arm64-only,
  installed to `/usr/local/lib` where `build.sh` prefers it and
  logs the bundled version + UUID (provenance). Validated at fps
  parity with 65 newer upstream fixes and half the binary size.
- **Crash fixes in the new query path**: bulk query-pool resets +
  in-pass rotation could exhaust the per-slot query partition
  mid-frame (release builds strip the old assert; MoltenVK then
  wrote through a nil visibility offset) — a capacity guard now
  submits before exhaustion and the pool grew 1024→4096. A
  per-command-buffer primer query keeps MoltenVK's visibility
  buffer attached regardless of which pass sees the frame's first
  zpass draw.
- **Texture snapshot copy-until-clean.** Guest writes landing
  mid-copy previously shipped one torn (Morton-tiled magenta)
  frame and repaired on the next bind; the copy now retries until
  no write tears it (bounded, self-consistent rehash).
- **Snapshot robustness.** Failed snapshot loads (controller USB
  topology drift — a pad asleep or re-enumerated since the save)
  no longer leave the VM stopped ("frozen"); the game resumes and
  the error is surfaced. Emulated pads with no host binding report
  neutral input instead of aborting, which also enables
  topology-matched snapshot loading in automation.
- **In-pass occlusion queries + deferred zpass reports (2.4x in
  report-heavy scenes).** Two coupled changes, measured together on
  a heavy in-game savestate (3 interleaved pairs): 15.78 →
  **38.57 ± 0.80 fps** (+144%), render passes **376 → 14 per
  flip**, fence waits 2.2 ms/flip, 0 stalled finishes. (1)
  Per-query `vkCmdResetQueryPool` (illegal inside a render pass)
  tore the pass down on every query rotation — ~one full tile
  load/store per draw on Apple GPUs; the slot's query partition is
  now bulk-reset at CB begin and queries begin/end *inside* the
  pass. (2) The FIFO-idle STALLED path did a full submit + GPU sync
  per pending report (23+/flip); engines consume last frame's
  counts, so reports now ride the next natural submission and
  deliver at slot reclaim, with a continuous-idle fallback submit
  for guests that truly spin-wait (see the idle-budget bullet).
  `XEMU_REPORTS_SYNC=1` restores legacy synchronous drains.
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
- **Monotonic sync clock.** `pgraph_vk_sync` uses
  `QEMU_CLOCK_REALTIME` (CLOCK_MONOTONIC — suspend / NTP slew don't
  stall the 8 ms gate; see "Monotonic present-path clocks" below).
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
- **Push-model present handoff** (behind `XEMU_PUSH_PRESENT=1`,
  default off; Metal backend, frame interpolation off — interpolation's
  sub-flip pacing needs the pull cadence, so interp-on falls back).
  The pull-model handoff above (`nv2a_get_present_frame`) costs a
  guaranteed cross-thread round trip per UI frame: the UI kicks the PFIFO
  thread and blocks on `qemu_event_wait` until it answers, and that
  handshake is also what *publishes* the frame — so the UI cannot skip an
  unchanged frame without first paying for the round trip. Under the flag
  the PFIFO thread composites and publishes the complete present tuple
  (texture/IOSurface + retain, shared event + value, `frame_seq`, dims)
  into a mutex-guarded slot at flip (`pgraph_vk_flip_stall`); the UI reads
  it with **no kick and no wait** and skips re-compositing when `frame_seq`
  is unchanged (the dedup the pull model structurally couldn't do). This
  is a latency/jitter change, **not** an fps change — guest flip/vblank
  timing is untouched. Measured on F5, quiet machine, interleaved 3-pair
  A/B (35 intervals/arm): the new `present_wait` nsprof counter drops
  from **81.2 ms/interval (sd 40, max 157 ms) across ~599 UI-thread
  blocks per interval to 0.000 across every push interval** (a one-time
  startup burst before the first publish, then the pull fallback never
  fires); **flips/s identical** (50.0 ± 6.6 vs 51.4 ± 5.6, the scene's
  own 38-60 bimodal range). The refuter (`XEMU_PUSH_PRESENT_REFUTE=1`)
  cross-checked **48,151** equal-`frame_seq` pushed-vs-pull reads over
  65 s with **0 mismatches** (plus 47,151/0 in the loaded-machine run).
  Startup / post-resize / GL / interpolation-on fall back to the pull
  path unchanged.
- **Async MetalFX via `MTLSharedEvent`** (Metal backend only). The
  three `waitUntilCompleted` stalls (spatial/temporal/interp,
  1-5 ms/frame on the PFIFO thread) are replaced by a monotonic
  shared-event signal; the UI present pass encodes a GPU-side wait
  on the frame's value before sampling. The GL backend keeps the
  synchronous waits (GL cannot wait on `MTLSharedEvent`).
- **Async `render_display`** (Metal backend only). The compositor
  pass no longer blocks the PFIFO thread in
  `vkWaitForFences(aux_fence)` — the last synchronous GPU wait on
  the present path. A timeline `VkSemaphore` (exported as an
  `MTLSharedEvent` via `VK_EXT_metal_objects`, one-time probe with
  sync fallback) is signaled per compositor submit; MetalFX command
  buffers and the UI present pass order GPU-side against it.
- **Dedicated compositor command-buffer ring.** The async compositor
  originally shared the per-slot aux CB + single aux fence, so the
  next aux use (texture/surface uploads, staging sync) reclaimed the
  compositor's fence on the PFIFO thread — measured 1.4-1.9 ms/flip
  in heavy scenes. The compositor now submits through its own 2-deep
  CB ring; an entry is reclaimed only when its slot recurs two sync
  intervals later (measured: 1.4-1.9 ms/flip → ~0.2 µs/flip), and
  upload paths never wait on compositor work at all.
- **IOSurface-free present chain** (Metal backend only). When
  MoltenVK can export the `MTLTexture` backing the compositor
  `VkImage` (probed at init), no IOSurface is created at all: MetalFX
  consumes the exported texture directly and the UI receives it
  through the present-frame handoff. This removes the macOS 26
  IOSurface constraint from the *input* side too, so **MetalFX
  engages at `surface_scale = 4`** (2560x1920 input, previously
  skipped). IOSurface remains the automatic fallback and the GL
  path.
- **Frame interpolation quality.** The interpolator is driven
  properly: presentation queue shows interpolated frames *before*
  the real frame they lead to, paced at `frame_period / mode`
  (forward-backward motion fixed); a zero-filled motion texture is
  no longer bound ("nothing moved" made moving objects ghost-blend —
  `nil` enables MetalFX internal motion estimation;
  `XEMU_MFX_INTERP_ZERO_MOTION` restores the old binding);
  `fieldOfView`/`aspectRatio` are set and synthetic luminance depth
  is bound at interpolation resolution; in temporal mode the
  interpolator is **linked to the temporal scaler**
  (`MTLFXFrameInterpolatableScaler`) and inherits its real
  motion/depth/history; 4x re-presents the cached midpoint instead
  of re-encoding the same pair (which violated the interpolator's
  history contract); a hitch guard skips interpolation and resets
  history on frame-gap spikes instead of blending across content
  jumps.
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
- **`MTLFXFrameInterpolator.deltaTime` in real seconds**
  (`QEMU_CLOCK_REALTIME` between input `CFRetain`s, clamped
  `[1/240, 1/10]` s) instead of a unitless ratio. Reduces ghosting
  on fast pans.
- **Exclusive fullscreen picks the highest refresh** among the
  native-resolution modes (120 Hz instead of a 60 Hz first entry);
  Metal vsync maps to `CAMetalLayer.displaySyncEnabled`.
- **GPU-paced interpolation steps.** Published frames carry a
  sequence number + intended hold duration; the UI presents new
  content with `presentDrawable:afterMinimumDuration:` and skips
  re-presenting unchanged frames during gameplay — exact
  `frame_period / mode` step timing instead of quantizing to the UI
  loop (±8 ms at 120 Hz). Menus keep full-rate rendering.
- **Monotonic present-path clocks.** Sync gate, interpolation
  timestamps, step pacing and surface expiry moved from
  `QEMU_CLOCK_HOST` (gettimeofday — reflects NTP changes) to
  `QEMU_CLOCK_REALTIME` (CLOCK_MONOTONIC); an earlier pass had the
  two semantics inverted. The guest-vblank IRQ timer was already a
  dedicated fixed-60 Hz monotonic-deadline thread, fully decoupled
  from the present path.
- **Opt-in real depth for temporal** (`XEMU_MFX_REAL_DEPTH=1`):
  zeta images are created exportable and the dims-matched zeta's
  MTLTexture feeds the temporal scaler (standard-Z) in place of
  synthetic luminance depth; automatic fallback when the format is
  rejected or no matching zeta exists. Ships dark for A/B: the
  single guest zeta typically holds the next in-progress frame's
  depth by present time, so quality impact is title-dependent.

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
- **DSP execution engines (post upstream merge).** Upstream added a
  DSPOps engine abstraction with two engines: the C interpreter
  (`dsp/interp/`) and a JIT wrapping the external `dsp56300` Rust
  subproject (`audio.use_dsp_jit`, default on). This fork's inline
  ARM64 JIT lives *inside* the interpreter engine
  (`interp/dsp56k_jit_arm64.c`, symbols `dsp56k_jit_*`); when
  supported and enabled it takes precedence and `use_dsp_jit` is
  ignored. Non-Apple hosts get upstream's dsp56300 engine by
  default.
- **Full inline DSP JIT (Apple Silicon).** ARM64
  basic-block JIT for both MCPX DSP56300 cores (GP + EP). Enable
  via `[audio.dsp_jit] enabled = true` (engine selection is
  config-only; `XEMU_DSP_JIT=0` is a runtime kill-switch within the
  selected engine).
- **Per-core retranslation auto-throttle.** The EP core overlays its
  P-space every pass (~1 retranslation per 10 block executions), so
  translating it costs more than interpreting it; a sustained-churn
  window (>1/16) permanently hands that core back to the
  interpreter. APU-thread utilization: JIT-both-cores 25%,
  interpreter 22%, JIT with EP throttled **20%** (upstream dsp56300
  engine 19%). GP never trips. `XEMU_DSP_JIT_NO_THROTTLE=1` for A/B.
- **JIT coverage + mechanics.** 100% ALU and 99.99% CF inlined
  (only `emu_undefined` and a few side-effect-register `bit_manip`
  variants stay on BLR); `pm_read_accu24` inlined for the scaling=0
  case universal on Xbox. A/B/X/Y/SR pinned in callee-saved ARM64
  regs. Static block chaining (with retro-chaining: untranslated
  targets get a patchable chain site, patched when the target is
  later translated) keeps hot loops inside JIT code; all chain
  repatching sits inside the JIT-write window (W^X), and
  invalidation-time chain *unpatching* now also flushes each
  repatched site's icache line (the write window covers permission,
  not coherency — the stale branch could otherwise keep executing
  from I-cache). Lazy-flag elimination elides ~20% of SR updates. Harnesses:
  `XEMU_DSP_JIT_DIFF=N` (off-thread bit-exact validation, bounded
  per unique translation), `XEMU_DSP_JIT_STATS=1` (counters). See
  [docs/dsp-jit-design.md](docs/dsp-jit-design.md).

### Input

- **In-app rebinding UI polish** (Input tab → Input Mapping; the
  remap table itself is upstream's). Esc cancels an in-progress
  capture (previously the only exits were pressing an input or
  unplugging the controller), right-click on a binding unbinds it
  (persisted; unbound entries read as never-pressed), capture rows
  say what they're waiting for, and a hint line documents the
  gestures. Controller buttons rebind to other controller buttons,
  stick/trigger axes to other axes, every Xbox control has a
  keyboard binding, and per-stick axis inversion toggles sit under
  the table — all per-controller (keyed by SDL GUID) and saved to
  `input.gamepad_mappings` / `input.keyboard_controller_scancode_map`
  in xemu.toml.

### Threads + runtime

- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE`.
- **BQL discipline.** `surface_access_callback` drops BQL before
  blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`; kick-check +
  release-wait under `d->pfifo.lock` guarantees no missed wake-up.
  `halt` via `qatomic_read` matches the writer contract.
- **BQL-free MMIO dispatch for the hottest guest register blocks.**
  `XEMU_MMIO_PROF` measured 78% of 8.2M guest MMIO ops/70 s hitting
  just PFB (`NV_PFB_WBC` polling) and USER doorbells, each paying
  the unconditional BQL in TCG's MMIO helpers — upstream's
  `lockless_io` flag is now honored on the TCG path too, and the
  audited PFB / USER / APU-VP regions opt in. Locked crossings drop
  ~80%. Measured honestly at **fps parity** (not claimed as a
  speedup) with a consistent steadiness win: per-run fps stdev
  1.21 → 0.89, 5/6 pairs — the 340 µs BQL-spike class no longer
  hits these ops. `XEMU_MMIO_BQL=1` restores locked dispatch.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`, `-mcpu`
  auto-detected. STBI_NEON + fpng CRC32 for ARM64.
- **PGO shipped for arm64 release builds.** A trained profile
  (`pgo/default.profdata`, Azurik savestate corpus: the four bench
  scenes, ~75 s each) is committed and applied by CI to macOS arm64
  release legs. Measured on the 770 draws/flip heavy savestate scene
  (interleaved cross-binary A/B, 3 pairs, identical source both
  arms): 25.42 ± 0.34 → 27.81 ± 0.62 fps (**+9.4%**, deltas
  +2.71/+1.85/+2.61, scene identity 740-746 draws/flip). x86_64 legs
  stay plain (profiles are arch-specific); a stale profile degrades
  to partial coverage, and `build.sh` fails loudly if the profile
  file disappears. Retrain per the build-knob table.
- macOS 26 build fixes: `download-macos-libs.py` uses
  `os.path.abspath` and repairs stale `prefix=` lines in vendored
  `.pc` files; `build.sh` strips all `LC_RPATH` before
  `dylibbundler` and adds the single correct one.
- Ad-hoc codesign by default (MAP_JIT works);
  `XEMU_CODESIGN_ENTITLEMENTS=1` opts into hardened runtime +
  `xemu.entitlements`. For notarized distribution use
  `scripts/sign-macos-release.sh`.
- Optional PGO (`XEMU_PGO=generate` → run → `XEMU_PGO=use`), wired
  into the macOS, Linux, and native Windows (MSYS2) branches via one
  shared `setup_pgo` — the consolidation also fixed the MSYS2
  branch's stale-profile trap (it only re-merged `.profraw` files
  when `default.profdata` was absent, so a retrain silently lost to
  the committed profile).
- **Cross-platform parity batch (2026-07-05).** The DSP56K JIT gate
  widened to all POSIX aarch64 hosts, so Linux arm64 compiles the
  fork's biggest CPU win — and since 2026-07-11 CI runs the `xbox`
  suite (interpreter/JIT differential + swizzle) on the
  `ubuntu-22.04-arm` leg: the first automated runtime coverage of
  the DSP JIT on non-Apple aarch64, all three differential arms
  green with the engagement assert (blocks actually JIT-executed).
  (The chain-unpatch-under-SMC icache path is corpus-invisible, so
  an in-game `XEMU_DSP_JIT_DIFF` run there is still owed —
  `docs/windows-gating-audit.md`.)
  build.sh grew Linux PGO wiring, an arch-clean guard (a stale
  `build/` for another arch was silently reused, so `-a x86_64`
  could "succeed" with an arm64 binary), and arch-aware MoltenVK
  resolution (a single-arch system dylib silently disabled the whole
  Vulkan renderer for cross builds; the UI's MetalFX references are
  CONFIG_VULKAN-guarded now). MoltenVK resolution + vendoring now
  live in single functions (`resolve_moltenvk` / `vendor_moltenvk`)
  shared by configure and package time.
- Native Windows release builds default to `-Dx86_version=3`
  (AVX2 / BMI2 / FMA — matches CI release config; override by
  passing your own `-Dx86_version=`).
- Windows cross-compile fixes (found by building win64-cross from
  this fork): the x86_64 hard-FPU `__hard` set was missing
  `sqrt` / `round_to_int` / `to_int32[_round_to_zero]` / `to_int64`
  (implicit-decl compile error in `fpu_helper_hard.c`); the SPIR-V
  shader cache used 2-arg POSIX `mkdir` (→ `g_mkdir_with_parents`);
  the pipeline cache used `rename()` onto an existing file, which
  Windows rejects (→ `g_rename`); the `HAVE_EXTERNAL_MEMORY` display
  path lost its `gl_internal_format` declaration.

### MoltenVK runtime config

Set in `main()` before MoltenVK loads (launch-path independent;
explicit env overrides win) and mirrored in `Info.plist`
`LSEnvironment`. The two homes must stay in sync.

| Key | Value | Purpose |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor binding overhead |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | `0` | Deferred encoding. Must stay 0: with whole-frame CBs, prefill corrupted streamed textures, enabled the AGX visibility-buffer crash, and measured slower (46.3 vs 48.5 fps) |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async submits |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Ignore transient GPU errors |

---

## Failed / reverted experiments

Lessons worth preserving so they aren't re-attempted.

| Attempt | Reason |
|---|---|
| TB byte-range invalidation filter (`XEMU_TB_RANGE_INV`, 2026-07-11) | `XEMU_INV_PROF` measured 100% of TB invalidations as data-write false sharing (0 true SMC, 100% recycle-hit) — so re-applying upstream's overlap filter looked free. Counter A/B: invalidations 338k → 675 (works) but notdirty traps **25.2×** (198k → 4.98M) + 46M range scans — whole-page invalidation is what empties the page and fires `tlb_unprotect_code`, making subsequent data writes free; base-xemu removed the check (`6ea11938b2e`) deliberately. Instrument-killed pre-fps-A/B; knob ships dark. Real lever: sub-page dirty tracking (design parked pending correctness review) |
| L2 victim jump-cache (2026-07-05) | 64k-entry victim tier probed on L1 miss looked capacity-shaped (410k TBs vs 4096 entries) — but live hit rate measured **11.2%**: residual misses are one-shot/cold pcs, not a recurring set. Ceiling ~0.1 fps; instrument-killed without an fps A/B. Measure the miss stream's *shape* (recurrence), not its volume, before building any cache tier |
| JIT write-protect flip caching (2026-07-05) | Sampling attributed 9.7% of the vCPU thread to per-TB-entry `pthread_jit_write_protect_np`; a thread-local skip of redundant flips measured **parity** (6 pairs, −0.10 mean). The 9.7% was `thread_suspend`-sampling skid onto barrier instructions — discount barrier-heavy symbols in suspend-based profiles |
| Per-depth return-address ring (2026-07-05) | Classic shadow stack: eip prediction paired at 99.6%, but per-depth slots are shared by every same-depth call site, so TB fills ran 2.6× hits; a 512-deep ring changed nothing (a key problem, not a depth problem). Superseded same day by the eip-keyed ret-target memo (`XEMU_RAS`): the ret target is already in `env->eip` at dispatch, so depth-shaped state adds nothing a target-keyed cache doesn't |
| Vertex copy-on-conflict transient remap (`XEMU_VTX_TRANSIENT`, 2026-07-05) | Replace the ~6/flip conflict finishes with per-slot transient copies + remapped draw bindings. Faithfully implemented; **−2.09 fps, 6/6 pairs negative**: the title rewrites broad vertex ranges every frame, so the 64-entry/2 MiB table overflowed every frame (each overflow an all-slot drain, heavier than the waits it replaced) while the remap search ran 227 times/flip. The targeted submitted-slot wait remains the shipped design |
| TB jump-cache enlargement (12→16 bits, 2026-07-05) | 93.4% hit at 15.8M lookups/s made capacity misses look like free money; measured **−1.10 fps, 4/6 pairs negative** — the 4096-entry (64 KiB) cache is L1-resident, and a 1 MiB cache pays a few ns on each of 14.8M *hits*/s to avoid ~500k walks. Target lookup *volume*, not cache geometry |
| Eager report submit (`XEMU_REPORTS_EAGER=N`, 2026-07-04) | Submit the recording CB when its Nth zpass report is *requested*, front-running the guest's poll stall. Mean −0.08 fps (6 pairs, sign-inconsistent) with a +4% draws/flip composition shift. With the 300 µs idle budget the residual wait is GPU catch-up time — eager submission moves the submit without shrinking the wait |
| Occlusion-rework intermediates: in-pass queries with synchronous drains; submit-on-idle per pending report | Both **regressed** vs the 15.8 fps baseline the rework started from (6.9 fps; ~11.2 fps at ~144 tiny submissions/flip). Query placement × report delivery is a policy *pair* — never evaluate piecewise (the shipped pair: 2.4x, +144%) |
| Per-flight vertex-RAM mirrors | One 128 MiB host mirror per flight slot so in-flight slots read frozen data (cross-slot conflict waits structurally impossible). Measured ~**neutral** (an initial "-30%" read traced to an invalid baseline parked on a menu): the dominant cost was the *recording-CB* boundary-page conflict cascade, which mirrors can't address. Reverted — +128 MiB and delta complexity for no win. The salvage, byte-exact conflict refinement, shipped separately (+5.4% fps) |
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

### The remaining performance roadmap (2026-07-05, post-dispatch-campaign)

State when this was written: F8 heavy anchor 31.4 fps @ 819 draws/flip
(v0.10 shipped it at 23.9), F6 at the 60 cap, guest-TCG execution is
53.7% of the vCPU thread, softfloat is out of the profile top, and the
cheap-lever list is measured empty — everything below is ranked by
expected value against real effort/risk, with the receipts that aimed
it. Instruments to re-run before starting any of these:
`XEMU_GUEST_PROF=1` (sampler + exit-kind census) and `XEMU_TB_PROF=1`.

1. **SMC / TB-invalidation churn** (INSTRUMENTED 2026-07-11; cure dark
   pending A/B). `XEMU_INV_PROF` (default-off exit counters) settled the
   classification: on both F8 and F5 savestate scenes **100% of TB
   invalidations are guest data writes to code pages (notdirty),
   100% hit TBs whose bytes do not overlap the write, and 0 are genuine
   SMC** — inv-htable recycle hit-rate is 100% (true_smc=0). So the
   `do_tb_phys_invalidate` churn (top-25) is pure false-sharing: the
   Xbox whole-page invalidation (`6ea11938b2e`, no byte-range check)
   nukes every TB on a written code page, which then recycle unchanged.
   Cure implemented behind `XEMU_TB_RANGE_INV=1` (default off): re-apply
   upstream's exact per-TB byte-range overlap filter (correctness-safe —
   invalidates a correct subset, never misses). But the counter A/B
   (`XEMU_INV_PROF` with the filter on vs off, F8) shows why it is likely
   the WRONG cure: the filter cuts invalidations 500× (338k→675) and kills
   the recycle round-trip, but keeps the written page write-protected, so
   notdirty traps **explode 25×** (198k→4.98M — each a
   `physical_memory_test_and_clear_dirty` TLB walk) and the per-page scan
   grows unbounded (TBs never leave the page). This recovers *why*
   base-xemu removed the check (`6ea11938b2e`): whole-page invalidation's
   `tlb_unprotect_code` side effect makes the ~25 subsequent data
   writes/epoch free — it was removed FOR performance on the Xbox
   data-shares-a-code-page pattern. So `XEMU_TB_RANGE_INV` is a
   correctness-verified dark A/B knob predicted **neutral-to-negative**;
   the real lever is **sub-page dirty tracking** (stop the non-code write
   from trapping at all), sketched but parked pending a correctness review
   (shared dirty-bitmap race class). Interleaved A/B to confirm the kill:
   `scripts/bench-savestate-ab.sh <snap> XEMU_TB_RANGE_INV`.
2. **Cross-page direct chaining, Xbox-relaxed** (a week; risky).
   The "other" 43% of the exit census is dominated by cross-page
   direct jumps that pay the ~20-op inline probe today. Upstream
   forbids cross-page `goto_tb` because mappings can change; the
   Xbox's effectively static flat mapping makes a fork-specific
   relaxation plausible (chain + invalidation hooks for the SMC edge
   cases). Worth +1-2 fps. Archaeology-1.x-grade design required —
   the failure mode is a rare wrong-code hang, the worst class.
3. **PGRAPH MMIO lockless audit** (days; jitter-class expectation).
   Remaining vCPU lock waits ~5.6%. Unlike PFB/USER/vp, PGRAPH
   handlers genuinely interleave with the PFIFO thread — the audit is
   the work; expect steadier pacing more than fps.
4. **Ret-memo fill path** (hours, but sub-noise-bar). The memo's cold
   fills pay `tcg_tb_lookup` g_tree walks (~3% sample share at 6.4%
   fill rate). Candidates: export `tb_lookup` to the ret helper
   (skip the tc.ptr→tb reversal), or grow `XEMU_RETC_BITS` 12→13/14
   (compile-time; predicted +0.3-0.5 — AT the honest measurement
   floor on this scene, hence parked rather than shipped blind).
5. **Trace / superblock formation in TCG** (weeks; transformative or
   bust). With dispatch spent and the guest profile diffuse (top TB
   2.5%), the remaining wall IS generated-code quality. Multi-TB
   traces with cross-block optimization are the only identified lever
   plausibly worth +15-30% — and the only realistic path to real-60
   on F7/F8. Upstream-divergent compiler work; run it as a time-boxed
   research campaign with falsifiable early milestones, not as a
   task. (2026-07-11 censuses: `XEMU_INV_PROF` exit mix is 72%
   direct-chain goto_tb / 16% inline-jc / 12% ret-memo and flcr
   already compile-skips 92-94% — the *dispatch* half of the
   superblock motivation is weak. The `XEMU_CCOP_CENSUS`
   runtime-weighted pair census then sized the codegen half, and it
   is REAL: 69-83% of TB transitions end with lazy flags pending
   (F8/F5), and 46-49% of those are dead at the successor — i.e.
   **~34-38% of all block boundaries carry a flag materialization
   (cc_op + operand spills) the next block provably kills unread**.
   The consumed share is also large (30-45%), so naive skip-the-spill
   tricks are DOA — only genuine cross-block liveness (superblock)
   collects this. That is the campaign's quantified upside basis,
   alongside cross-block register allocation.)
6. **Real-hardware validation debt** (not fps; the largest open
   correctness item). Windows/Linux runtime proof for the shipped
   cross-platform wins: the gating-audit needs-real-HW list, the DSP
   JIT `XEMU_DSP_JIT_DIFF` run on non-Apple aarch64, `XEMU_SSE_HOST=2`
   on real x86_64 silicon (one un-root-caused ±0-sign divergence
   under Rosetta keeps the x86_64 arm dark), and per-platform
   savestate baselines before any perf claim there.

Presentation note: `display.frame_interpolation` (MetalFX) already
delivers displayed-60 from a solid real-30 today; it does not change
simulation rate or input latency and benches must keep reading real
flips.

- **Streamed-vertex stall reduction: attempted twice, both ~neutral —
  those scenes were GPU-bound.** (a) Byte-exact conflict refinement
  eliminated 75-85% of the boundary-page `finish_vtx_dirty` cascade;
  flips/s did not move — the finishes reappeared as cross-slot
  waits. (b) Adding per-flight mirrors (cross-slot waits
  structurally impossible): wait *events* ballooned while total wait
  stayed ~2.5-3.5 s per 5 s. The ~22-30 ms/flip of waits was the
  GPU's actual frame time; CPU-side wait elimination only relocates
  which call site absorbs it. (True of the 2026-07 attract-reel
  fixtures; the 2026-07-04 savestate scenes measure CPU-bound — see
  the campaign entry below.) Both implementations preserved in
  history.
- **GPU frame-cost campaign: draw/pass-reduction menu measured
  exhausted on the current fixtures (2026-07-04).** Attribution
  counters (`vk_draw_call`, `merge_*`, `rpcause_*`) + a 10 s
  `sample` profile on the heavy savestate scene killed every ranked
  mechanism: subrange coalescing has zero opportunity, cross-block
  merge candidates are 10.6% of draws with ~0 per-call PFIFO
  recording cost (prefill=0 defers Metal encoding to MoltenVK's
  queue thread), per-draw CPU sites all <1 ms/flip, pass-count cuts
  save GPU time that isn't the constraint (~14 ms/flip of GPU
  slack), and `VK_EXT_multi_draw` is unimplemented in MoltenVK. The
  frame limiter on these fixtures is CPU-side (guest TCG owns the
  largest share; ≥5.8 ms/flip of PFIFO starvation). Re-run the
  campaign's Phase 0/2 gates before reviving the menu on a future
  GPU-bound scene.
- **Dirty-clear TLB-walk coalescing (measured, parked).**
  `physical_memory_test_and_clear_dirty` triggers a full-TLB walk
  per dirty detection; the per-draw vertex sync and per-bind texture
  checks pay ~1.8 ms/flip on the PFIFO thread plus a vCPU-side echo
  (~6%). The hot paths already pay exactly one walk per detection,
  so the remaining design is cross-call deferral of the TLB re-arm —
  which widens the bitmap-cleared/TLB-armed race window (the class
  that already shipped one cleared-before-consumed bug) and needs a
  dedicated correctness review. Ceiling ~1-1.5 fps at the ~31 fps
  floor; parked until it ranks again.
- **Windows real-hardware Vulkan validation (gating audit committed
  2026-07-04, `docs/windows-gating-audit.md`).** Every fork divergence
  touching Windows-compiled code is now statically audited: Apple-only
  fast paths verified to keep upstream semantics behind their `#else`
  branches, shared behavior changes classified with written
  equivalence arguments, and one real cross-platform defect found and
  fixed (invalid-surface destruction racing pending submissions —
  `a045dfc780`). What still **needs real Windows hardware** (no
  Vulkan ICD exists in VMs): (1) occlusion-rework report values under
  native drivers/WHPX (`XEMU_REPORTS_SYNC=1` is the bisect hatch);
  (2) primer-query validation-layer cleanliness; (3) a zeta
  quarantine/reuse soak (equivalence proven on paper); (4) byte-exact
  vertex refinement under WHPX write timing (`XEMU_VTX_EXACT=0`);
  (5) the 5 s fence-or-die margin on slow systems; (6) a
  Windows-native savestate A/B baseline before any perf claim.
- **Push-model present × frame interpolation (schedule publish).**
  The landed push handoff (`XEMU_PUSH_PRESENT`, Changes above) is
  gated off under frame interpolation because it publishes one frame
  per flip while interpolation needs a paced sub-flip sequence — and
  measured 2026-07-11 under the default 2x config, the pull path it
  falls back to blocks the UI thread **280-400 ms per 5 s interval
  (~660 waits/s, single waits up to 8.8 ms)** — tails at the exact
  ±8 ms step-deadline scale of 120 Hz pacing. Design: generalize the
  published slot to a small ring of {handle, event value, seq, hold}
  entries written at flip (the GPU-paced-steps machinery already
  presents by seq + hold via `afterMinimumDuration`; the shared-event
  contract already gates not-yet-finished interpolated outputs).
  Hard parts: multi-entry retain lifetime (compositor-CB-ring
  precedent), schedule invalidation on hitch-guard reset / resize /
  teardown (generation counter), interpolator history contract
  across skipped entries. Extend `XEMU_PUSH_PRESENT_REFUTE` to
  schedule-vs-pull equivalence before trusting it.
- **MetalFX *input* ring (drop `metalfx_drain_inflight`).** Ring the
  compositor *output* texture that MetalFX consumes (distinct from
  the landed compositor *command-buffer* ring). Measured: the drain
  costs ~0.1 µs/flip steady-state (max ~4 ms on rare hitches) — not
  worth the complexity at current frame rates.
- **GL NV2A renderer under the Metal window.** Currently a
  renderer switch away from Vulkan under the Metal backend needs a
  restart; full unification would render the GL display buffer into
  an IOSurface-backed FBO and feed the same present-frame handoff.
- **Flip-time zeta snapshot for real depth.** The opt-in real-depth
  path (`XEMU_MFX_REAL_DEPTH`) feeds the live zeta texture, which by
  present time holds the next in-progress frame. Capturing a GPU
  copy of zeta at flip-stall would give temporally-correct depth at
  the cost of one blit per frame.
- **Texture-upload barrier batching.** Current per-mip
  `pre_compute` / `post_compute` pair is load-bearing on reused
  `COMPUTE_DST` / `COMPUTE_SRC`; batching requires disjoint offsets
  + dispatch-offset args. Measured (`XEMU_NV2A_NSPROF`): total
  upload time is ≤ 15 ms per 5 s even during streaming-heavy scene
  transitions, ~0 in steady state — not worth the risk.
- **`VK_EXT_external_memory_host` with snapshot scheme.** Per-flight
  COW or `MTLSharedEvent`-keyed boundary to sidestep the
  host-coherent tear. Measured: the snapshot memcpy it would
  eliminate totals < 0.5 ms per 5 s in-game — far below the
  ≥ 1 ms/frame bar for attempting this.
- **`VK_KHR_dynamic_rendering` with explicit barriers.** Emit
  `vkCmdPipelineBarrier` around every `BeginRendering` /
  `EndRendering`.
- **`MTLResidencySet` (macOS 15+).** Pin frequently-used Metal
  buffers/textures as resident.
- **BINK video via VideoToolbox.** Xbox BINK decoder is CPU-bound;
  offload YUV→RGBA (or full transcode).
- ~~Shader specialization constants~~ — stale: alpha-test and
  fog-enable are *already* compile-time GLSL variants (baked into the
  generated shader via `PshState`/`VshState`; the LRU shader cache
  keys on them). Only value uniforms (alphaRef / fogParam / fogColor)
  are per-draw, and those must remain uniforms. A useful reframing
  would target shader-compile stutter (variant count) rather than
  per-draw branching.
- **GPU S3TC decode.** Compute-shader decoder offloads the CPU
  thread pool. Measured: total texture-upload CPU (including S3TC
  decode) is ~0 in steady state and ≤ 15 ms per 5 s during
  streaming — no longer a meaningful target.
- **PAL 50 Hz guest-vblank cadence.** The dedicated vblank thread is
  fixed at 60 Hz; deriving 50 Hz from the guest video mode would
  serve PAL titles. (Host-present decoupling itself is done: the
  Metal present path never references the vblank timer.)
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
(1920 px BGRA-IOSurface safe cap); the Metal backend's
IOSurface-free chain has no such cap. Check for
`MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.** Requires macOS 26.0+ and MetalFX
upscaling active.

**Interpolation ghosting on moving objects.** Should be fixed (motion
estimation + paced presentation order); `XEMU_MFX_INTERP_ZERO_MOTION=1`
restores the old zero-motion binding for A/B comparison. Temporal
mode gives the interpolator the best data (scaler-linked).

**Black/garbled window or Metal init failure on launch.** Set
`[display.window] presentation_backend = 'opengl'` in `xemu.toml` to
fall back to the GL presentation path, and report the issue. The
`auto` default uses Metal only with the Vulkan renderer.

**FPU precision bug.** `[perf] hard_fpu = false`. Inline FPU uses IEEE
double (52-bit mantissa) vs x87 extended (64-bit); extremely rare.

**Depth artifacts after a render-target switch.** Set
`XEMU_ZETA_SHAPE_READBACK=1` to restore the full GPU→CPU round trip
on zeta shape switches (costs ~2 finishes + multi-MB copies per
frame on affected titles) and report the title.

**Stale/late-updating textures.** Set `XEMU_TEX_BIND_RECHECK=1` to
restore per-bind content checks (a CPU write between two binds of
the same texture within one frame otherwise lands a frame late).

**Audio glitches.** `XEMU_COREAUDIO_FRAMES=2048` for a larger buffer.
For DSP-heavy titles, confirm DSP JIT is enabled.

**Windows: "Failed to initialize Vulkan renderer" / falls back to
OpenGL.** The Vulkan loader (`vulkan-1.dll`) comes from the GPU
driver; xemu bundles nothing. Update the vendor driver on real
hardware. Inside VMs (Parallels, UTM, Hyper-V) there is usually no
Vulkan ICD at all — the OpenGL fallback is expected; test Vulkan on
real hardware. `vulkaninfo` (from the Vulkan SDK) shows what the
loader sees.

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
       │  compute unswizzle + YUV · zero-copy display
       │  (Metal: exported MTLTexture, async timeline submit ·
       │   GL: IOSurface + CGL)
       │        │
       │        └─ MetalFX Spatial / Temporal (async, MTLSharedEvent)
       │             │   Metal backend: private-texture ring → panel-fit
       │             │   (1280×960→2880×2160; works at scale 4) · GL: 1920 cap
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
