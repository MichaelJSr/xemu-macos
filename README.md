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
(flight slots, dirty hashing + spatial index + once-per-frame
verification, the zeta shape-switch fast path, targeted
vertex-conflict waits, VkPipelineCache persistence, draw-path dedup,
tight barriers, query drain at slot reclaim), the APU work (LUTs,
batched register reads, SVF cache, mono paths, SSE2 mix kernels),
the pfifo / BQL fixes, the `XEMU_NV2A_NSPROF` profiler, and the
helper-based hard FPU on x86_64. macOS-only: MetalFX, the Metal /
IOSurface presentation backends and their async present chain,
CoreAudio, vDSP, the ARM64 DSP JIT, and the AArch64 inline x87 FPU
(Windows uses the bit-equivalent helper-based hard FPU instead). The
MoltenVK CPU primitive-emulation paths don't activate on native
Vulkan drivers.

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
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use` for PGO. A trained profile is committed at `pgo/default.profdata` (Azurik savestate corpus, 2026-07-04) and CI applies it to arm64 release builds — measured +9.4% fps on the heavy savestate scene (25.42 ± 0.34 → 27.81 ± 0.62, 3 interleaved cross-binary pairs). Retrain after large code churn: `XEMU_PGO=generate ./build.sh`, play the bench scenes, `XEMU_PGO=use ./build.sh`, commit the regenerated profdata. |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Hardened-runtime codesign via `xemu.entitlements` |
| `XEMU_COREAUDIO_FRAMES` | `1024` | CoreAudio buffer (≈21 ms @ 48 kHz) |
| `XEMU_MOLTENVK_VERSION` | `1.4.1` | MoltenVK release auto-vendored into `macos-libs` when no system copy exists |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for `scripts/build-moltenvk.sh` (the maintained optimized MoltenVK) |

### How Vulkan is provisioned (all platforms)

The NV2A Vulkan renderer loads Vulkan at runtime through **volk** —
nothing links against a Vulkan library at build time.

- **macOS:** volk dlopens `libMoltenVK.dylib`, which `build.sh`
  bundles into `xemu.app/Contents/Libraries/<arch>/` (the app's
  `LC_RPATH` points there). At build time the dylib + headers are
  found in `macos-libs` (vendored), `/usr/local` (Vulkan SDK /
  manual install), or Homebrew — in that order — and when none
  exists, `build.sh` downloads the pinned official MoltenVK release
  into `macos-libs` automatically. A missing dylib at bundle time is
  a hard build error (the fork's default renderer, Metal
  presentation path, and MetalFX all require it). End users need
  nothing installed: the app is self-contained.
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
| `XEMU_MMIO_BQL` | `1` restores BQL-locked TCG dispatch for the audited lockless NV2A regions (PFB, USER) — bisect hatch |
| `XEMU_MMIO_PROF` | `1` prints an exit histogram of guest MMIO traffic (region × page, loads/stores) + BQL acquire-wait totals |
| `XEMU_MAX_QUERIES` | `4096` | Occlusion-query pool size (begin_draw guard submits before exhaustion) |
| `XEMU_INPUT_PIPE` | unset | FIFO path; lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` inject input through normal bindings (works unfocused; test automation) |
| `XEMU_MFX_REAL_DEPTH` | Feed real zeta depth to the temporal scaler (A/B) |
| `XEMU_TB_PROF` | `1` prints TB jump-cache totals at exit (lookups, hit%, htable walks, translations, tb_flush count). The heavy scene measures 15.8M lookups/s at 93.4% hit — the profile that killed the cache-sizing experiment (see failed-experiments) and aims future TCG work at lookup volume instead |
| `XEMU_SSE_HOST` (alias `XEMU_SSE_NEON`) | `1` enables the host-SIMD fast path for packed/scalar single-precision SSE arithmetic — NEON on Apple Silicon, host SSE on x86_64 hosts. Dark A/B knob — parity on the arm64 bench scene post-PGO; `=2` differential mode runs both paths and aborts on divergence. arm64: 60 s zero-divergence receipt. x86_64: **no bit-exactness claim yet** — the first Rosetta `=2` run caught a real leaked-rounding-mode bug (fixed: both brackets now force RN), and a second unresolved ±0-sign divergence under Rosetta remains; run `=2` clean on real x86_64 silicon before enabling `=1` |
| `XEMU_MFX_INTERP_ZERO_MOTION` | Old zero-motion interpolator binding (A/B) |
| `XEMU_DSP_JIT_STATS` / `XEMU_DSP_JIT_DIFF=N` | DSP JIT counters / bit-exact validation |
| `XEMU_DSP_JIT_NO_THROTTLE` | Disable the DSP JIT retranslation-churn auto-throttle |
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
- **`possibly_dirty` cleared after verified bind.** Inherited
  upstream behavior left a texture binding's `possibly_dirty` mark
  sticky once set by a neighboring texture sharing a host page —
  every later bind re-hashed the full contents forever. After a bind
  verifies the cached hash (or re-uploads), the mark is dropped; any
  later guest write re-sets page dirty bits and re-marks via the
  spatial index. Measured with `XEMU_NV2A_NSPROF=1` in-game: total
  texture dirty-check + hash time fell ~2.1x (~290 ms → ~135 ms per
  5 s interval; 1.3-3.5 ms/flip → 0.6-1.6 ms/flip on the PFIFO
  thread). Torn-snapshot binds keep the mark, preserving the
  next-bind re-hash contract.
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
  release-build-safe ns accumulators around shader gen, pipeline
  gen, texture hash / snapshot / upload, geometry buffer copies,
  flight-slot fence waits, aux-CB fence waits, MetalFX drain,
  surface readbacks, and the FLIP_STALL → vblank idle gap, plus
  event counters for finish reasons, draws/flip, and surface
  download/upload trigger sites; per-5 s summaries (total /
  per-flip / max event) to stderr. The `NV2A_PROF_*` counters are
  compiled out in release builds and count events, not time; this
  is what the optimization passes above were measured with.
  2026-07-04 additions (GPU frame-cost campaign Phase 1): actual
  `vkCmdDraw*` call counts vs guest blocks (`vk_draw_call`,
  `da_multi_subrange`), consecutive-block draw-merge classification
  (`merge_identical` / `merge_candidate` / `merge_cand_udiff` /
  `merge_state_changed`), and render-pass-end cause tags
  (`rpcause_surface` / `_clear` / `_texupload` / `_other`).
  Observation-only; ~three flag stores per draw when the profiler is
  off. Validated at fps parity (46.83 ± 0.24 vs 46.59 ± 0.50) on the
  heavy savestate scene.
- **Cross-platform parity batch (2026-07-05).** The DSP56K JIT gate
  widened to all POSIX aarch64 hosts (`__aarch64__ && !_WIN32`) — the
  fork's biggest CPU win now compiles for Linux arm64 (CI-covered by
  the ubuntu-22.04-arm leg; runtime acceptance there still gated on a
  clean `XEMU_DSP_JIT_DIFF` run, see docs/windows-gating-audit.md).
  build.sh: clang PGO wiring for the Linux branch (mechanism only);
  arch-clean guard (a stale `build/` configured for another arch was
  silently reused — an `-a x86_64` run could "succeed" with an arm64
  binary); MoltenVK resolution is now arch-aware at both configure and
  bundle time (a single-arch system dylib — e.g. the custom arm64
  /usr/local build — silently disabled the entire Vulkan renderer for
  cross builds and broke the link; the UI's MetalFX references are
  also CONFIG_VULKAN-guarded now). Windows PGO is recorded as
  needs-real-HW (win64-cross x86_64 is GCC — incompatible profile
  format; arm64 llvm-mingw lacks a Windows-trained profile).
- **BQL-free MMIO dispatch for the hottest guest register blocks.**
  `XEMU_MMIO_PROF` measured 8.2M guest MMIO ops in 70 s on the heavy
  savestate scene — PFB alone 4.85M (dominated by `NV_PFB_WBC`
  write-combine flush polling) and USER doorbells 1.58M, 78% of all
  traffic — each paying the unconditional BQL in TCG's MMIO helpers
  (upstream's `lockless_io` flag was only honored on the
  address-space path; the fork's cputlb now honors it too). Both
  regions audited: PFB reads return constants or plain `regs[]`
  (its one cross-thread reader was never BQL-protected); USER holds
  `pfifo.lock` for its entire handler and kicks under it. Result:
  locked MMIO crossings drop ~80%. Honest measurement (6 interleaved
  pairs, F8 scene): **fps parity** (mean +0.54 driven by one outlier
  pair — not claimed as a speedup), with a consistent intra-run
  steadiness improvement (per-run fps stdev 1.21 → 0.89, 5/6 pairs
  steadier — the 340 µs BQL-spike class no longer hits these ops).
  Shipped for the jitter benefit and for weaker/busier hosts where
  main-loop BQL pressure is higher; `XEMU_MMIO_BQL=1` restores
  locked dispatch.
- **Deferred-report idle budget cut 5 ms → 300 µs
  (`XEMU_REPORTS_BUDGET_US`).** Guests that consume a zpass report
  value mid-frame spin-wait on it with an idle FIFO; the deferred
  design's safety-valve submit only fired after 5 ms of continuous
  idle, so every such poll cost the guest's critical path up to the
  full budget (the per-scene `finish_reports_submit` rate tracks each
  scene's fps deficit: 0.18/flip at 60 fps → 2.23/flip at 24 fps).
  The budget is now 300 µs and tunable: a too-small budget merely
  costs an extra small submit per mid-frame idle episode (bounded by
  report count — not the per-report submit storm the deferred design
  replaced). Measured (interleaved same-binary A/B, 3 pairs, 770
  draws/flip savestate scene): 24.04 ± 0.43 → 25.36 ± 0.22 fps
  (+5.5%, all pairs positive; +1.48 mean on the pre-death early
  window). The original ~10 ms/flip stall hypothesis was killed
  honestly — measured reclaim is ~2 ms/flip. Artifact soak at the new
  default: same content-flag signature as legacy, zero
  corruption-class clusters. `XEMU_REPORTS_BUDGET_US=5000` restores
  the previous behavior; `XEMU_REPORTS_SYNC=1` remains the full
  legacy hatch.
- **Vertex-mirror overwrite waits the just-submitted slot.** Second
  member of the pipelined-finish family: the `VERTEX_BUFFER_DIRTY`
  conflict path submitted the recording CB and immediately memcpy'd
  new guest data over the conflicting `BUFFER_VERTEX_RAM` range —
  upstream's single-slot finish really did drain first, but the
  fork's flight-slot finish waits only the previous slot, so the
  copy raced the submitted frame's vertex fetches (torn-geometry
  class on any driver; masked on macOS by MoltenVK's deferred
  encode). The conflict path now waits the submitted slot's fence
  and clears its upload tracking (later writes that frame skip the
  signaled fence). Validated: cross-binary interleaved A/B vs v0.9,
  24.45 ± 0.58 vs 24.48 ± 1.19 fps (parity), 765-794 draws/flip
  scene identity, zero errors.
- **Invalid-surface destruction gated on submission retirement.**
  `pgraph_vk_finish` pipelines (submits the current CB, waits only
  the previous slot's fence), so a quarantined surface image could be
  `vkDestroyImage`d while the just-submitted CB still referenced it —
  invalid usage on every driver, introduced with flight-slot
  pipelining (upstream's synchronous finish was immune). Evictions
  are now stamped with the highest submission index that may
  reference them; a retirement watermark, updated wherever a slot
  fence is observed signaled, gates destruction, and
  `pgraph_vk_surface_flush` drains all slots before its
  free-everything prune. Reuse of quarantined images needs no gate —
  ordered by the draw render pass's explicit `VK_SUBPASS_EXTERNAL`
  dependency (proof comment at `get_any_compatible_invalid_surface`).
  Validated at fps parity (46.6-47.2 vs 46.83 ± 0.24, identical
  finish mix, zero errors) on the heavy savestate scene.
- **Zeta shape-switch fast path.** Whole-frame attribution showed
  the dominant cost in-game was ~9 `pgraph_vk_finish` fence cycles
  per flip (8-19 ms/flip), driven by a depth buffer ping-ponging
  between two shapes at one VRAM address every frame (Azurik: a
  1280x480 linear scene zeta and a 256x256 swizzled RTT zeta) —
  each switch did a full GPU→CPU readback (finish + fence + D24S8
  compute conversion + multi-MB memcpy), an eviction finish, and a
  multi-MB seed re-upload. Re-shaped zeta targets are cleared by
  the guest before use and the genuine RAM consumers (CPU access
  callbacks, texture binds over the range) measured zero, so the
  switch now: skips the readback, skips the RAM seed upload, and
  defers the eviction without a finish (the old image is
  quarantined in the invalid pool until the recording command
  buffer rotates; reuse is safe afterwards by single-queue
  submission order). Pending CPU-requested downloads are still
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
- **In-pass occlusion queries + deferred zpass reports (2.25x in
  report-heavy scenes).** Two coupled changes, measured together on
  a heavy in-game savestate (Azurik, 408-485 draws/flip):
  15.8 → **35.6 fps**, render passes **376 → 14 per flip**, fence
  waits 31.9 → 2.3 ms/flip. (1) The per-query
  `vkCmdResetQueryPool` (illegal inside a render pass) forced query
  rotation to tear the pass down — about one full tile load/store
  cycle per draw on Apple GPUs. The slot's whole query partition is
  now bulk-reset once at command-buffer begin, and queries begin/end
  *inside* the pass (Metal visibility-buffer path; a query begun in
  a subpass ends in it — `end_render_pass` guarantees this, and
  report sums already span query ranges). (2) The FIFO-idle STALLED
  path did a full submit + GPU sync per pending report — 23+ per
  flip. Engines consume last frame's occlusion counts, so reports
  now ride the next natural submission (flip) and deliver at slot
  reclaim; a guest that truly spin-waits with an idle FIFO is
  caught by a 5 ms continuous-idle fallback that submits once and
  delivers via non-blocking fence polling. `XEMU_REPORTS_SYNC=1`
  restores the legacy synchronous drains.
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
  via `[audio.dsp_jit] enabled = true` or `XEMU_DSP_JIT=1`.
- **Per-core retranslation auto-throttle.** The EP runs per-pass
  code overlays (measured ~1 retranslation per 10 block executions
  on Azurik — 26k retranslations per 262k-execution window), so
  translating it costs more than interpreting it; a sustained-churn
  window (>1/16) permanently hands that core back to the
  interpreter. Measured APU-thread utilization (Azurik attract,
  `XEMU_APU_PROF`): JIT-both-cores 25%, interpreter 22%, JIT with
  EP auto-throttled **20%** — statistically tied with upstream's
  dsp56300 engine (19%) in this DSP-light scene. GP never trips
  (~1:10000). `XEMU_DSP_JIT_NO_THROTTLE=1` for A/B.
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
- Optional PGO (`XEMU_PGO=generate` → run → `XEMU_PGO=use`) — now
  also wired into the native Windows (MSYS2) branch.
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
| Vertex copy-on-conflict transient remap (`XEMU_VTX_TRANSIENT`, 2026-07-05) | Replace the mid-frame conflict finish (submit + wait the just-submitted slot, ~6/flip on the heavy scene) with per-slot transient copies + remapped draw bindings (chained newest-first entries, retirement-gated mirror apply, rotation copy-forward, exact-refinement exclusion). Faithfully implemented with all three design traps closed — and measured **−2.09 fps, 6/6 pairs negative**. Counters: `vtx_remap_hit` 226.7/flip (the remap search became a per-draw fixture), `vtx_remap_full` 1.47/flip (64-entry/2 MiB budget overflows every frame; the all-slot drain fallback is heavier than the targeted waits it replaced). The title rewrites broad vertex ranges every frame — the wrong conflict shape for remapping. Reverted whole; the targeted submitted-slot wait remains the shipped design. Full record: archaeology 1.16 |
| TB jump-cache enlargement (12→16 bits, 2026-07-05) | `XEMU_TB_PROF` measured 15.8M `tb_lookup` calls/s on the heavy scene at 93.44% jump-cache hit — ~1M TB-htable walks/s and 410k translated TBs, so capacity misses looked like free money (a 16-bit probe halved the walks). Interleaved A/B (6 pairs, runtime-sized cache, 12 vs 16 bits): **−1.10 fps, 4/6 pairs negative** — the 4096-entry (64 KiB) cache is L1-resident; a 1 MiB cache pays a few ns extra on each of 14.8M *hits*/s, which outweighs the ~500k avoided walks. Upstream's geometry is already near-optimal on M2; the profitable target is lookup *volume* (indirect-branch/ret chaining), not cache size. Sizing mechanism reverted; the profiler (`XEMU_TB_PROF`) ships |
| Eager report submit (`XEMU_REPORTS_EAGER=N`, 2026-07-04) | Submit the recording CB when its Nth zpass report is *requested*, front-running the guest's poll stall so GPU execution overlaps remaining guest frame work. Measured dead on the F8 scene (interleaved, 6 pairs): mean −0.08 fps (deltas +0.26/+3.04/+0.09/−0.87/−0.65/−2.38 — sign-inconsistent), with a persistent +4% draws/flip composition shift in the eager arm. Mechanism of the neutrality: the 300 µs idle budget (shipped earlier the same day) already sits near the structural minimum — the guest's post-submit wait is GPU catch-up time, which eager submission merely moves without shrinking, while adding submit overhead. Reverted; the idle-budget path remains the shipped design |
| Per-flight vertex-RAM mirrors (vertex shadow copies) | One 128 MiB host mirror per flight slot; in-flight slots read a frozen mirror (cross-slot conflict waits disappear), `uploaded_bitmap` doubles as the delta log applied at slot reclaim. Measured ~**neutral** on the Azurik attract reel (interval-by-interval flips within noise of the single-mirror build; an initial "-30%" read traced to an invalid baseline run parked on a 3-draws/flip static screen). Neutral because the dominant cost is elsewhere: the heavy-reel intervals show 440-700 `finish_vtx_dirty` per 5 s *with or without* mirrors — guest vertex streams write page-boundary-overlapping ranges, and the recording-CB conflict check is page-granular, so consecutive writes false-share the boundary page and cascade through finish → rotate → 20+ ms mid-frame reclaim waits. Mirrors can't remove those (the conflict is with the *recording* CB, not in-flight slots). Reverted as not-worth-it: +128 MiB, swap/delta complexity, no measured win. The real target this exposed: byte-granular (or split-at-page) conflict refinement for the current-slot check — see Future vectors |
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

- **Streamed-vertex stall reduction: attempted twice, both ~neutral —
  heavy scenes are GPU-bound.** (a) Byte-exact conflict refinement:
  per-page written-span tracking + span-restricted memcmp skipped
  75-85% of the boundary-page `finish_vtx_dirty` cascade (440-700 →
  ~100 per 5 s interval, page-padded stream writes false-share
  boundary pages with byte-identical content), but flips/s did not
  move — the eliminated finishes reappeared as cross-slot targeted
  waits. (b) Exact refinement + per-flight mirrors (structurally no
  cross-slot waits): fence-wait *events* ballooned while total wait
  time stayed ~2.5-3.5 s per 5 s. Conclusion: in the heavy attract
  reel the ~22-30 ms/flip of waits is the GPU's actual frame time
  (as the targeted-wait analysis already noted — "real GPU time,
  not slack"); CPU-side wait elimination just relocates which call
  site absorbs it. Future work here must reduce GPU work per frame
  (draw batching, render-pass merging), not CPU synchronization.
  Both implementations preserved in this repo's history for
  reference.
- **GPU frame-cost campaign: draw/pass-reduction menu measured
  exhausted on the current fixtures (2026-07-04).** Attribution
  counters (`vk_draw_call`, `merge_*`, `rpcause_*` — commit
  `038f596332`) plus a 10 s `sample` profile on the heavy savestate
  scene (471 draws/flip, 46.6 fps) killed every ranked mechanism by
  measurement: same-block subrange coalescing has zero opportunity
  (`vk_draw_call/flip == draws/flip` exactly); cross-block merge
  candidates are 10.6% of draws AND per-call recording cost on the
  PFIFO thread is ~0 (with `MVK_CONFIG_PREFILL=0`, Metal encoding
  happens on MoltenVK's queue thread); every per-draw CPU candidate
  site measures <1 ms/flip; pass-count cuts save GPU time that is
  not the constraint (fence waits 2.8 ms/flip, GPU ~14 ms/flip of
  slack); `VK_EXT_multi_draw` is unimplemented in MoltenVK. The
  earlier "heavy scenes are GPU-bound" premise does **not** hold on
  today's savestates: the frame limiter is CPU-side — mean finish
  time 14.9 ms/flip of which the guest's TCG execution owns the
  largest share (vCPU thread 99.7% busy; ≥5.8 ms/flip of PFIFO
  starvation while the guest computes alone; caveat: the title
  busy-polls, so vCPU utilization alone is not a limiter signal —
  starvation time is). Re-run the campaign's Phase 0/2 gates before
  reviving any of the menu on a future GPU-bound scene.
- **Dirty-clear TLB-walk coalescing (measured, not attempted).**
  `physical_memory_test_and_clear_dirty` triggers
  `tlb_reset_dirty()` — a full-TLB walk (all MMU modes + victim
  TLB, under a spinlock) per *dirty* detection regardless of range
  size. The per-draw vertex sync and per-bind texture dirty checks
  pay it constantly: ~1.8 ms/flip on the PFIFO thread (vertex 537 +
  texture 245 of 6827 thread samples) plus a vCPU-side echo
  (notdirty slow-path writes ~2.8% + TB-link dirty clears 3.4% of
  the vCPU thread). Candidate: coalesce the TLB resets for
  NV2A-client clears (batch the union of ranges once per command
  buffer instead of per draw). High design risk: page-granular
  dirty tracking is load-bearing (see byte-exact refinement above)
  and an earlier dirty-range flush variant shipped a
  cleared-before-consumed bug — any attempt needs the full
  predict/validate discipline. **Re-scoped 2026-07-04 (second
  pass):** the only multi-walk caller
  (`physical_memory_clear_dirty_range`, 5 walks per range) is cold
  (ramblock init); the hot paths already pay exactly one walk per
  dirty detection, so the remaining design is cross-call deferral of
  the TLB re-arm — which widens the existing bitmap-cleared/TLB-armed
  race window (the archaeology-1.9 class) and needs a dedicated
  correctness review. Ceiling at the post-PGO ~31 fps floor is
  ~1-1.5 fps; parked until it ranks again.
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
- **Guest TCG throughput (measured bottleneck profile,
  2026-07-04).** On the heavy scene the vCPU thread spends 17.9% in
  `helper_lookup_tb_ptr` (indirect-branch TB lookup), ~14% in TLB
  fill/set machinery, 6.8% in `helper_ldul_mmu`, and ~3.7% in
  SSE packed-float helpers (`mulps`/`addps` — candidate for
  NEON-backed lowering on AArch64 hosts, a hard-FPU-class project).
  A PGO build (`XEMU_PGO=generate/use`, already wired in build.sh)
  is the cheapest unexplored experiment against this profile.
- **Occlusion-report STALLED drains — resolved by the deferred-report
  rework; measurement confirmed 2026-07-04.** The "~5 STALLED
  finishes per flip" figure predates the occlusion rework; the
  default path now delivers reports via non-blocking per-slot
  drains and `VK_FINISH_REASON_STALLED` is reachable only under
  `XEMU_REPORTS_SYNC=1`. Confirmed on the heavy savestate scene:
  `finish_stalled = 0` and `finish_reports_full = 0` across all
  in-game intervals (`finish_reports_submit` ≈ 0.96/flip carries
  the load). Nothing left to do here.
- **Push-model present handoff.** `nv2a_get_present_frame` does a
  PFIFO event-wait round trip per UI frame (the sync handshake is
  also what publishes frames, so a UI-side "skip when unchanged"
  pre-check is not possible in the current pull model). Publishing
  at flip from the PFIFO side would remove the cross-thread
  round trip and let the UI loop pace purely on the drawable.
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
