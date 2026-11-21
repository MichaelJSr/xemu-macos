# Manually supplied files

This fork is **source-only**. It does not contain Xbox BIOS/ROM dumps, EEPROM
images, HDD images, game ISOs, or proprietary binary libraries. You must provide
those yourself, from your own legally owned copies, before running xemu.

## Xbox files

Place these in your xemu data directory (on macOS usually
`~/Library/Application Support/xemu/xemu/`):

| File | Description |
| --- | --- |
| `mcpx_1.0.bin` | MCPX boot ROM |
| `Complex_4627.bin` | Xbox kernel ROM (also called "Complex" ROM) |
| `eeprom.bin` | Machine EEPROM; contains unique per-console keys. Back it up. |
| `xbox_hdd.qcow2` (or your configured HDD image) | Xbox hard disk image |
| `*.iso` / `*.xiso` / `*.chd` | Game or homebrew images |

> Note: the `pc-bios/*.bin`, `*.rom` and `*.img` files already tracked in this
> repository are upstream QEMU blobs distributed with their own licenses. They
> are **not** Xbox ROMs and are left untouched.

## macOS runtime libraries

`build.sh` downloads third-party binaries (MoltenVK, KosmicKrisp, ICD manifests
and their license notices) into local directories such as `macos-libs/` and
`build/`. Those directories are intentionally **not tracked**. Re-run the build
script to fetch them again on a new machine; do not commit them.

## Submodules

This repository uses submodules. After cloning, run:

```sh
git submodule update --init --recursive
```

## Local configuration

`xemu.toml` is machine-specific (video, audio, input bindings and paths) and
is not part of the repository. Keep your gamepad/keyboard bindings there.
