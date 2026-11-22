# xemu macOS fork (Experimental)

> [!WARNING]
> THIS IS ONLY FOR MACOS X86_64 AND ARM AND ITS HIGLY EXPERIMNETAL. IDK IF I WILL MAKE ANY WINDOWNS OR LINUX VERSION OR IDK IF IT COMPILES ON OTHER PLATFORMS

## Overview

This is a fork of [MichaelJSr/xemu-macos](https://github.com/MichaelJSr/xemu-macos),
the macOS port of [xemu](https://xemu.app), the QEMU-based Xbox emulator.
It preserves that upstream history and adds macOS graphics work.
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
- 
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

## Credits
Fork of [MichaelJSr/xemu-macos](https://github.com/MichaelJSr/xemu-macos);
thanks to its author and to all upstream xemu and QEMU contributors.
