# xemu-macos: Optimized Xbox Emulator for Apple Silicon

Fork of [xemu](https://github.com/xemu-project/xemu) tuned for Apple Silicon
(M1/M2+) on macOS 14+ (best on macOS 26 Tahoe). Adds MoltenVK support
(unsupported upstream), an inline ARM64 x87 FPU, full DSP JIT, MetalFX
upscaling + frame interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

## Documentation map

| Doc | Purpose |
|---|---|
| This README | What the fork is, how to build/run/configure it, troubleshooting |
| [docs/optimizations.md](docs/optimizations.md) | The optimization ledger: every landed change with its receipts, the runtime escape-hatch/diagnostic knob catalog, and every attempted-and-settled experiment |
| [docs/roadmap.md](docs/roadmap.md) | Open candidates: the ranked performance roadmap and feature/quality vectors, each with its decision inputs |
| [docs/dsp-jit-design.md](docs/dsp-jit-design.md) | DSP56300 ARM64 JIT design + phased roadmap |
| [docs/RELEASING-macos.md](docs/RELEASING-macos.md) | Release runbook |

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
hard FPU on x86_64) apply automatically; macOS-only pieces (MetalFX,
Metal/IOSurface presentation, CoreAudio, vDSP, the ARM64 DSP JIT, the
AArch64 inline x87 FPU) compile out.

- **Native (MSYS2/MINGW):** `./build.sh` from an MSYS2 shell. Release
  builds default to `-Dx86_version=3`; `XEMU_PGO=generate/use` work
  like on macOS. Preferred route — a Windows VM or x86 box can also
  run-test the result.
- **Cross (Docker):** `./build.sh -p win64-cross` from a Linux
  container with the `xemu-win64-toolchain` image and
  `CROSSPREFIX=x86_64-w64-mingw32.static-` set — see
  `.github/workflows/build-windows.yml` for exact image tags/env.
  Verified from an Apple Silicon Mac via colima
  (`colima start --vm-type vz --vz-rosetta`); use a copy of the tree
  kept under `$HOME`.

### Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use`. A trained profile is committed at `pgo/default.profdata` and applied by CI to arm64 release builds (+9.4% fps). Retrain after large code churn: `generate` build → play the bench scenes → `use` build → commit the regenerated profdata. A `use` build prints a staleness summary (hot-path CFG-hash mismatches + commits since retrain) and warns when the profile has drifted — see the ledger |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Hardened-runtime codesign via `xemu.entitlements` |
| `XEMU_MOLTENVK_VERSION` | `1.4.2` | MoltenVK release auto-vendored into `macos-libs` when no usable system copy exists. This default is the single home for the expected version — macOS CI asserts the packaged bundle's provenance line matches it |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for `scripts/build-moltenvk.sh` (the maintained optimized MoltenVK) |
| `XEMU_VIS` | `0` | `-fvisibility=hidden` (≈1.5 MiB smaller arm64 binary, cleaner LTO) |
| `XEMU_STRIP` | `0` | `-Wl,-dead_strip` (≈4 MiB smaller combined with `XEMU_VIS=1`; validate `type_init` constructors survive before shipping) |

Runtime debug / escape-hatch env vars (`XEMU_*`) are catalogued with
the optimizations they belong to in
[docs/optimizations.md](docs/optimizations.md).

### How Vulkan is provisioned (all platforms)

The NV2A Vulkan renderer loads Vulkan at runtime through **volk** —
nothing links against a Vulkan library at build time.

- **macOS:** volk dlopens `libMoltenVK.dylib`, which `build.sh` bundles
  into `xemu.app/Contents/Libraries/<arch>/`. One resolution rule
  (`resolve_moltenvk`): `macos-libs` (vendored), then `/usr/local`
  (Vulkan SDK / `scripts/build-moltenvk.sh`), then Homebrew — a dylib
  counts only if it has the target arch slice; when none exists the
  pinned official release is vendored automatically. `build.sh` logs
  the bundled version + UUID so tested-vs-shipped driver drift is
  visible. End users need nothing installed.
- **Windows:** volk loads the system `vulkan-1.dll` (ships with GPU
  drivers). No Vulkan ICD in a VM → automatic OpenGL fallback with a
  notification; test Vulkan on real hardware.
- **Linux:** the distro `libvulkan` loader + the GPU's ICD.

### Recommended `xemu.toml`

`~/Library/Application Support/xemu/xemu/xemu.toml`:

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'        # off | spatial | temporal
frame_interpolation = '4x'      # off | 2x | 4x

[display.quality]
surface_scale = 2               # 1 = 640x480 ... 4 = 2560x1920

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

## Performance

Everything is measured with the interleaved savestate A/B protocol
(`scripts/bench-savestate-ab.sh`: monitor-loadvm fixtures, scene-gated
nsprof intervals, mid-run visual artifact checks, receipts) — full
numbers and methods live in the
[optimization ledger](docs/optimizations.md).

Headline arc on the heavy in-game bench scene (Azurik savestate, M2
Ultra, real fps — no interpolation): **15.8 → ~48.1 fps** across the
fork's campaigns — the occlusion/report rework (+144%), PGO (+9.4%),
sub-page code dirty tracking (+14%), cross-page TB chaining, inline
dispatch probes, the sticky SSE/NEON bracket, the async Metal present
chain, the inline NOTDIRTY store-skip (+12% heavy-scene rendering
throughput at equal fps), and the 2026-08-04 optimization wave.
Lighter scenes hold the 60 fps vblank cap. MetalFX frame interpolation
presents 60/120 Hz from real-30+ without changing simulation rate.

The most recent step is the largest single one since the occlusion
rework. Full protocol receipt for it (2026-08-04, interleaved
cross-binary A/B on the same fixture): **38.12 ± 0.59 → ~48.1 fps,
+10.0 fps mean, +26% fps, +20.4% draws/s, 5 clean pairs, unanimously
positive** (one further pair excluded because its run hit a known
upstream-class crash, noted in the receipt). Its two biggest
contributors are the `can_do_io`/`cpu_io_recompile` elision (+6.53 fps,
3/3) and the x87 exception-pointer deferral (+7.46 fps, 3/3) — with
each mechanism's own receipt, the wave's honest kills, and every escape
hatch in the [ledger](docs/optimizations.md).

The remaining wall is still guest-TCG throughput. The
superblock/region-formation campaign against the measured ~40%
dead-flag mass at block boundaries closed 2026-07-18 with five policies
measured negative and the enabling TCG machinery preserved dark; the
constant-factor work above is what moved instead. The full record, the
honest verdicts, and what is still open live in
[docs/roadmap.md](docs/roadmap.md) and the
[ledger](docs/optimizations.md).

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Crash on launch with "duplicate LC_RPATH".** Older builds — rebuild;
`build.sh` now strips rpaths.

**MetalFX not activating.** Requires `renderer = 'VULKAN'` and
`metalfx_mode = 'spatial'` or `'temporal'`. Under the GL presentation
backend, `surface_scale = 4` produces 2560×1920 input and is skipped
(1920 px BGRA-IOSurface safe cap); the Metal backend's IOSurface-free
chain has no such cap. Check for
`MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.** Requires macOS 26.0+ and MetalFX
upscaling active.

**Interpolation ghosting on moving objects.** Should be fixed (motion
estimation + paced presentation order); `XEMU_MFX_INTERP_ZERO_MOTION=1`
restores the old zero-motion binding for A/B comparison.

**Black/garbled window or Metal init failure on launch.** Set
`[display.window] presentation_backend = 'opengl'` in `xemu.toml` to
fall back to the GL presentation path, and report the issue.

**FPU precision bug.** `[perf] hard_fpu = false`. Inline FPU uses IEEE
double (52-bit mantissa) vs x87 extended (64-bit); extremely rare.

**Depth artifacts after a render-target switch.** Set
`XEMU_ZETA_SHAPE_READBACK=1` to restore the full GPU→CPU round trip on
zeta shape switches (costs ~2 finishes + multi-MB copies per frame on
affected titles) and report the title.

**Stale/late-updating textures.** Set `XEMU_TEX_BIND_RECHECK=1` to
restore per-bind content checks (a CPU write between two binds of the
same texture within one frame otherwise lands a frame late).

**Audio glitches.** `XEMU_COREAUDIO_FRAMES=2048` for a larger buffer.
For DSP-heavy titles, confirm DSP JIT is enabled.

**Windows: "Failed to initialize Vulkan renderer" / falls back to
OpenGL.** The Vulkan loader (`vulkan-1.dll`) comes from the GPU driver;
xemu bundles nothing. Update the vendor driver on real hardware. Inside
VMs there is usually no Vulkan ICD at all — the OpenGL fallback is
expected. `vulkaninfo` shows what the loader sees.

**Windows: black screen with working audio on Vulkan.** Fixed in
v0.13.2 — an invalid cross-stage uniform-buffer descriptor write that
native Vulkan drivers rendered as black (MoltenVK tolerated it). Update
to v0.13.2 or newer. As a fallback on older builds, set
`renderer = 'OPENGL'` in `xemu.toml`.

More escape hatches (every optimization ships one) are listed with
their owning changes in [docs/optimizations.md](docs/optimizations.md).

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
