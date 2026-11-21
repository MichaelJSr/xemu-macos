# xemu macOS fork — source export

This disk image contains the complete source tree of a xemu fork with the
Vulkan/Metal rendering changes, exported from a local clone. The full Git
history is included.

## Contents

- Complete QEMU/xemu source tree with all Git history.
- 15 git submodules (initialise with `git submodule update --init --recursive`
  if you re-clone instead of using this image).
- `MANUAL-FILES.md` — list of BIOS/ROM/EEPROM/HDD/game images and proprietary
  libraries that must be supplied manually for legal reasons.
- English documentation only.

## Build from source (macOS)

1. Install Xcode Command Line Tools, Python 3, Meson, Ninja and pkg-config.
2. Run `./build.sh` to download the pinned third-party macOS libraries
   (MoltenVK, KosmicKrisp) into `macos-libs/`. These binaries are **not**
   committed to this repository.
3. Configure a build directory, for example:
   `meson setup build -Dcocoa=disabled -Dauto_features=disabled -Ddocs=disabled`
4. Compile with `ninja -C build`.
5. Package with `./build.sh package` (see `docs/RELEASING-macos.md`).

The exact configure flags used for the validated build are documented in the
macOS release notes in `docs/RELEASING-macos.md`.

## Privacy

The commits authored for this fork use a neutral identity
(`xemu fork <noreply@users.noreply.github.com>`). No personal e-mail addresses,
home directory paths, gamepad IDs or local configuration files are part of this
export. Upstream QEMU commit metadata is preserved unchanged.

## Legal

This export does **not** include Xbox BIOS/ROM dumps, EEPROM images, HDD images,
game ISOs or proprietary binary libraries. See `MANUAL-FILES.md` for details.
