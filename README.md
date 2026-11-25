# xemu-macos

A performance fork of [xemu](https://github.com/xemu-project/xemu), the
Original Xbox emulator, tuned for Apple silicon Macs. Windows and Linux
builds carry every portable part of the work.

The 2026-07/08 work took the heavy Azurik bench scene (M2 Ultra) from
15.8 to about 48.1 fps; lighter scenes hold the 60 fps cap.

## What's different from upstream

- **Vulkan on macOS** through the bundled MoltenVK; Mesa KosmicKrisp is
  an opt-in on macOS 26 (`[display.vulkan] driver = 'kosmickrisp'`, see
  [docs/kosmickrisp.md](docs/kosmickrisp.md)).
- **Metal presentation and MetalFX:** spatial or temporal upscaling
  (`display.metalfx_mode`) and, on macOS 26, frame interpolation
  (`display.frame_interpolation`, `2x` or `4x`).
- **CPU:** an inline ARM64 x87 FPU (`perf.hard_fpu`; on x86_64 hot x87
  ops run on the host x87 unit), PGO-trained arm64 release builds (+9.4%
  fps), cross-page TB chaining and sub-page code dirty tracking.
- **Audio:** an inline ARM64 DSP56300 JIT (`audio.dsp_jit`) and
  Accelerate (vDSP) mixing on macOS.
- **GPU:** first-use pipelines compile on a worker thread
  (`perf.async_pipeline_compile`), NV09F surface blits stay on the GPU
  (`perf.optimizations`), Vulkan starts on Intel/AMD Macs (16-bit
  surfaces hosted as RGBA8 where Metal lacks the format), and FMV played
  through the PVIDEO overlay (Midtown Madness 3, for example) shows on
  Vulkan.
- **Resolution and filters:** `display.quality.surface_scale` takes
  fractions such as `0.5` to render below native for speed;
  `display.filtering` adds the `fsr`, `bicubic` and `lanczos` upscalers
  (View > Filter Method), with `display.fsr_sharpness` for FSR.
- **Tools under Machine:** Game Library (scan the games folder, load a
  disc), Xbox Hard Drive (browse the HDD partitions, copy files in and
  out, back up saves) and Game Patches (apply IPS/BPS/JMP patches or a
  custom XBE through a qcow2 overlay; the disc image is never modified).
- **UI and input:** an FPS counter (`display.ui.show_fps`), CPU and GPU
  load in the performance overlay, `display.ui.pause_on_focus_loss`,
  per-stick dead zones (`input.deadzone_left`, `input.deadzone_right`)
  and `input.rumble_strength`.
- **PAL 50 Hz** vblank as an opt-in (`[sys] vblank_rate`) and a **headless
  mode** for test harnesses ([docs/automation.md](docs/automation.md)).

## Download

[GitHub Releases](https://github.com/MichaelJSr/xemu-macos/releases):

| Asset | Platform |
|---|---|
| `xemu-<ver>-macos-universal-unsigned.zip` | macOS, Apple silicon and Intel |
| `xemu-<ver>-windows-x86_64.zip`, `xemu-<ver>-windows-arm64.zip` | Windows |
| `xemu-<ver>-x86_64.AppImage`, `xemu-<ver>-aarch64.AppImage` | Linux |
| `xemu-<ver>.tar.zst` | Source |

The macOS app is ad-hoc signed, not notarized. If macOS refuses to open
it, allow it under System Settings > Privacy & Security, or run
`xattr -dr com.apple.quarantine xemu.app`.

As with upstream, you supply the Xbox BIOS, the MCPX boot ROM and an HDD
image: see <https://xemu.app/docs/required-files/>.

## Requirements

- macOS 14 or later. Apple silicon (M1 or newer) is the target; Intel
  Macs run the universal app's x86_64 slice.
- macOS 26 for frame interpolation and for KosmicKrisp, which also needs
  a Metal 4 GPU and the Metal presentation backend.
- x86_64 builds (Intel Mac, Windows, Linux) need an x86-64-v3 CPU (AVX2,
  BMI2, FMA).
- Windows: the GPU driver's Vulkan (`vulkan-1.dll`); without it xemu
  falls back to OpenGL. Linux: the distro Vulkan loader and the GPU's ICD.

## Recommended `xemu.toml`

macOS: `~/Library/Application Support/xemu/xemu/xemu.toml` (Windows:
`%APPDATA%\xemu\xemu\xemu.toml`). In-app Settings covers the main
toggles; window mode and size are left to Settings > Display.

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'        # off | spatial | temporal
frame_interpolation = '2x'      # off | 2x | 4x (macOS 26)

[display.quality]
surface_scale = 2               # 0.5 = faster, 1 = 640x480, 2 = 1280x960

[display.vulkan]
driver = 'auto'                 # auto (MoltenVK) | kosmickrisp | moltenvk

[display.ui]
fit = 'stretch'

[perf]
hard_fpu = true                 # inline x87 FPU
cache_shaders = true

[audio.dsp_jit]
enabled = true                  # ARM64 DSP JIT
```

## Build from source (macOS)

Needs the Xcode Command Line Tools, Homebrew and network access on the
first build.

```bash
brew install pkg-config ninja cmake dylibbundler
pip3 install pyyaml requests
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
./build.sh               # writes dist/xemu.app
open dist/xemu.app       # reads your own xemu.toml
```

Clean rebuild: `rm -rf macos-libs macos-pkgs build dist && ./build.sh`.
Windows, Linux, PGO, build knobs and how Vulkan is bundled:
[docs/building.md](docs/building.md).

## Common problems

| Problem | Fix |
|---|---|
| Black or garbled window, Metal init failure | `[display.window] presentation_backend = 'opengl'`, and report it |
| Stutter the first minutes after an upgrade | The shader cache refills once; it settles |
| MetalFX or frame interpolation does nothing | Needs `renderer = 'VULKAN'` and MetalFX on; interpolation needs macOS 26 and runs only while the display can show its steps |
| Audio glitches | Check that the DSP JIT is on: `[audio.dsp_jit] enabled` on ARM64 macOS and Linux, `[audio] use_dsp_jit` on other hosts |
| Windows falls back to OpenGL | Update the GPU driver; VMs usually have no Vulkan |
| Build fails with `GLIB_SIZEOF_SIZE_T` | `rm -rf macos-libs macos-pkgs build && ./build.sh` |

More problems, and the performance settings:
[docs/troubleshooting.md](docs/troubleshooting.md).

## Credits

- [xemu-project/xemu](https://github.com/xemu-project/xemu) (upstream)
  and [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu) (MoltenVK seed).
- [Yuri4Darwin](https://github.com/Yuri4Darwin/Better-xemu4macOS): the
  LLVM DSP JIT, async pipeline compiles, the upscalers and the Machine
  tools (adapted from the xemu-modern-gui fork), among others.
- [willemabc](https://github.com/willemabc/xemu-macos): the PVIDEO
  overlay fix.

## Documentation

| Doc | Contents |
|---|---|
| [docs/building.md](docs/building.md) | Building on macOS, Windows and Linux; build knobs; Vulkan provisioning |
| [docs/automation.md](docs/automation.md) | Headless mode, QMP and gdbstub extensions, PAL 50 Hz, guest RAM |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Problems and fixes; performance settings |
| [docs/optimizations.md](docs/optimizations.md) | Every landed optimization, the `XEMU_*` knob catalog, settled experiments |
| [docs/kosmickrisp.md](docs/kosmickrisp.md) | The opt-in KosmicKrisp Vulkan driver |
| [docs/README.md](docs/README.md) | Index of every fork doc |
