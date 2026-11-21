# xemu macOS fork

> [!WARNING]
> THIS IS ONLY FOR MACOS X86_64 AND ARM AND ITS HIGLY EXPERIMNETAL. IDK IF I WILL MAKE ANY WINDOWNS OR LINUX VERSION OR IDK IF IT COMPILES ON OTHER PLATFORMS

---

## Overview

This is an experimental fork of [xemu](https://xemu.app), the QEMU-based Xbox
emulator. It contains macOS-specific graphics work on top of the upstream
QEMU/xemu history, which is preserved in full.

| | |
| --- | --- |
| **Base** | upstream xemu / QEMU |
| **Targets** | macOS `x86_64` and `arm64` |
| **Status** | highly experimental |
| **Other platforms** | untested, not planned, no guarantees |

---

## Highlights

| Area | Change |
| --- | --- |
| Renderer | Vulkan on macOS via MoltenVK, with Metal/MetalFX integration |
| Pipelines | asynchronous compilation on first encounter of a draw state |
| SPIR-V cache | cache blobs written on a background thread |
| Frame pacing | MetalFX completion waits that do not spin |
| Display | fractional render scale below native resolution |
| Build | assorted macOS build and packaging fixes |

---

## What is not included

For legal and practical reasons, the following are **not** part of this
repository and must be supplied manually from your own legally owned copies:

- Xbox BIOS/ROM dumps — `mcpx_1.0.bin`, `Complex_4627.bin`, …
- EEPROM images — `eeprom.bin`
- Xbox HDD images — `xbox_hdd.qcow2`
- Game ISOs or other game images
- Proprietary binary libraries — MoltenVK, KosmicKrisp — downloaded by `build.sh`

The `pc-bios/*.bin`, `*.rom` and `*.img` files already tracked in the tree are
upstream QEMU blobs with their own licenses, **not** Xbox ROMs.

No PS2 controller driver or configuration work is included. Files such as
`hw/input/ps2.c`, `ps2.h`, `lasips2.c` and `mps2*` are upstream QEMU device
emulation, not a custom PS2 controller driver.

---

## Building from source

Requirements: Xcode Command Line Tools, Python 3, Meson, Ninja, pkg-config.

```sh
# 1. Get the source and its submodules
git clone <this-repository-url> xemu-fork
cd xemu-fork
git submodule update --init --recursive

# 2. Download the pinned third-party macOS libraries (not tracked by Git)
./build.sh

# 3. Configure and build (see docs/RELEASING-macos.md for the exact flags
#    used by the validated build)
meson setup build -Dcocoa=disabled -Dauto_features=disabled -Ddocs=disabled
ninja -C build

# 4. Optional: produce the .app bundle
./build.sh package
```

---

## Privacy

Commits authored for this fork use the neutral identity
`xemu fork <noreply@users.noreply.github.com>`. No personal e-mail addresses,
home-directory paths, gamepad IDs or local configuration files are included.
Upstream QEMU commit metadata is preserved unchanged.

---

## Credits

All upstream xemu and QEMU contributors. The macOS Vulkan/Metal changes in this
fork are the work of this fork's authors.
