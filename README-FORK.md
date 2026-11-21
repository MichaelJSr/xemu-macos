# xemu macOS fork

> THIS IS ONLY FOR MACOS X86_64 AND ARM AND ITS HIGLY EXPERIMNETAL. IDK IF I WILL MAKE ANY WINDOWNS OR LINUX VERSION OR IDK IF IT COMPILES ON OTHER PLATFORMS

## What this is

A fork of [xemu](https://xemu.app), the QEMU-based Xbox emulator, with
macOS-specific graphics work. The upstream QEMU/xemu history is preserved;
the changes in this fork are on top of it.

The target platforms are macOS on Intel (x86_64) and Apple Silicon (arm64).

## What is different here

- Vulkan renderer on macOS through MoltenVK, with Metal/MetalFX integration.
- Asynchronous pipeline compilation on first encounter of a draw state.
- SPIR-V cache blobs written on a background thread.
- MetalFX completion waits that do not spin.
- Fractional render scale below native resolution.
- assorted macOS build and packaging fixes.

## What is NOT included

For legal and practical reasons this repository does not contain:

- Xbox BIOS/ROM dumps (`mcpx_1.0.bin`, `Complex_4627.bin`, …)
- EEPROM images (`eeprom.bin`)
- Xbox HDD images (`xbox_hdd.qcow2`)
- game ISOs or other game images
- proprietary binary libraries (MoltenVK, KosmicKrisp) downloaded by `build.sh`

You must supply those yourself, from your own legally owned copies. The
`pc-bios/*.bin`, `*.rom` and `*.img` files already tracked in the tree are
upstream QEMU blobs with their own licenses, not Xbox ROMs.

No PS2 controller driver/configuration work is part of this fork. The
`hw/input/ps2.c`, `ps2.h`, `lasips2.c` and `mps2*` files in the tree are
upstream QEMU device emulation, not a custom PS2 controller driver.

## Building from source

1. Install the Xcode Command Line Tools, Python 3, Meson, Ninja and pkg-config.
2. Run `./build.sh` to download the pinned third-party macOS libraries into
   `macos-libs/`. They are intentionally not tracked by Git.
3. Initialise the submodules: `git submodule update --init --recursive`.
4. Configure a build directory with Meson (see `docs/RELEASING-macos.md` for the
   exact flags used by the validated build), then compile with Ninja.
5. Package with `./build.sh package` if you want the `.app` bundle.

## Privacy

The commits authored for this fork use the neutral identity
`xemu fork <noreply@users.noreply.github.com>`. No personal e-mail addresses,
home-directory paths, gamepad IDs or local configuration files are included.
Upstream QEMU commit metadata is preserved unchanged.
