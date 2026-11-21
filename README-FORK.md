# xemu macOS fork

> [!WARNING]
> THIS IS ONLY FOR MACOS X86_64 AND ARM AND ITS HIGLY EXPERIMNETAL. IDK IF I WILL MAKE ANY WINDOWNS OR LINUX VERSION OR IDK IF IT COMPILES ON OTHER PLATFORMS

## Overview

Experimental fork of [xemu](https://xemu.app), the QEMU-based Xbox emulator,
with macOS-specific graphics work on top of the full upstream history.
Targets: macOS `x86_64` and `arm64`. Other platforms are untested.

## Highlights

- Vulkan renderer on macOS via MoltenVK, with Metal/MetalFX integration.
- Asynchronous pipeline compilation on first encounter of a draw state.
- SPIR-V cache blobs written on a background thread.
- MetalFX completion waits that do not spin.
- Fractional render scale below native resolution.
- Assorted macOS build and packaging fixes.

## Not included

- Xbox BIOS/ROM dumps, EEPROM images, HDD images and game ISOs — supply your own legally owned copies.
- Proprietary binaries (MoltenVK, KosmicKrisp) downloaded by `build.sh`, not tracked by Git.
- Tracked `pc-bios/*.bin`, `*.rom`, `*.img` are upstream QEMU blobs, not Xbox ROMs.
- No PS2 controller driver: `ps2.c`, `lasips2.c`, `mps2*` are upstream QEMU device code.

## Build

Needs Xcode Command Line Tools, Python 3, Meson, Ninja and pkg-config.

```sh
git submodule update --init --recursive
./build.sh
meson setup build -Dcocoa=disabled -Dauto_features=disabled -Ddocs=disabled
ninja -C build
```

See `docs/RELEASING-macos.md` for the validated flags; `./build.sh package`
produces the `.app` bundle.

## Privacy

Fork commits use `xemu fork <noreply@users.noreply.github.com>`. No personal
e-mails, home paths, gamepad IDs or local config are included. Upstream
metadata is unchanged.

## Credits

Upstream xemu and QEMU contributors.
