# Building xemu-macos

`./build.sh [flags] [configure args]` builds and packages xemu as CI
does (`.github/workflows/build-{macos,windows,linux}.yml`). Flags come
first (`--debug` for asserts, `-jN`, `-p <platform>`, `-a <arch>`); the
first unknown argument and all after it go to `configure` (e.g.
`-Db_lto=true`). Log: `build.log`.

## macOS

Needs Apple Silicon, macOS 14.0+ (26 for MetalFX and frame
interpolation), Xcode Command Line Tools,
`brew install pkg-config ninja cmake dylibbundler`,
`pip3 install pyyaml requests`, and network on the first build.

```bash
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git submodule update --init --recursive
./build.sh                       # wipes dist/, builds dist/xemu.app
ninja -C build qemu-system-i386  # incremental; updates no bundle
rm -rf macos-libs macos-pkgs build dist && ./build.sh   # clean rebuild
```

- Unit tests, no game content: `cd build && ninja test-xbox &&
  ./pyvenv/bin/meson test --suite xbox --no-rebuild`.
- `scripts/refresh-clone.sh <dst.app> build/qemu-system-i386
  [--from dist/xemu.app]` puts an incremental binary into a bundle clone
  (a bare `cp` dies in dyld). It refuses `dist/`.
- `./build.sh -a x86_64` builds an Intel bundle (macOS 12.7.5+).
- `open dist/xemu.app` reads your own `xemu.toml`. Never run a binary
  without `-config_path <scratch toml>`, not even `--version`: it boots
  your config and HDD and re-saves the toml. Automation runs clones with
  `-name xemu-fork-auto`, their own `XEMU_BASE_PATH` and `-config_path`.

**PGO.** CI applies the committed `pgo/default.profdata` to arm64
release builds (+9.4% fps); locally use `XEMU_PGO=use ./build.sh`. A
`use` build warns when the profile has drifted (hot-path CFG-hash
mismatches, commits since the retrain). Retrain after large churn:

```bash
XEMU_PGO=generate ./build.sh
rm -f pgo/*.profraw                      # keep pgo/default.profdata
cp -Rc dist/xemu.app /tmp/xemu-pgo.app   # never run dist/xemu.app itself
scripts/pgo-train.py /tmp/xemu-pgo.app   # ~15 min, muted windows
XEMU_PGO=use ./build.sh                  # then commit pgo/default.profdata
```

**MoltenVK.** `build.sh` bundles the first `libMoltenVK.dylib` with the
target arch slice from `macos-libs` (vendored), `/usr/local` (Vulkan SDK
or `scripts/build-moltenvk.sh`, the fork's optimized build), Homebrew;
else it vendors the `XEMU_MOLTENVK_VERSION` release (what releases ship).
It logs `Bundling MoltenVK from <path> (version <v>, <arch> UUID <uuid>)`.

**KosmicKrisp** (arm64 only). `build.sh` bundles the pinned CI-built
`libvulkan_kosmickrisp.dylib` (`kk_asset_sha256`, cached in
`macos-libs/arm64`, checked every build) and logs
`Bundling KosmicKrisp from … (Mesa …)`.
`XEMU_KK_PREFIX=../kk/install-<tag> ./build.sh` takes a local
`scripts/build-kosmickrisp.sh` build ([kosmickrisp.md](kosmickrisp.md)).

## Windows

Upstream's Windows build is kept and CI-tested. MetalFX, Metal
presentation, vDSP, the AArch64 DSP JIT and the AArch64 inline x87 FPU
compile out.

**Native (MSYS2 MINGW64).** `./build.sh` from a MINGW64 shell writes
`dist/xemu.exe` and its DLLs. A native CLANGARM64 build is untested; on
ARM64 use the release zips. Package list, validation layers, PGO
training: [windows-test-plan.md](windows-test-plan.md); what still needs
real hardware: [windows-gating-audit.md](windows-gating-audit.md) §W.

- x86_64 release: `-Dx86_version=3` (AVX2/BMI2/FMA), `-O3`, no stack
  protector; arm64 stays at `-O2`. CI's x86_64 release adds GCC LTO
  (`./build.sh -Db_lto=true`, slower).
- SDL3 links statically from the wrap (patches: `subprojects/packagefiles`).
- Incremental: `ninja -C build qemu-system-i386w.exe && cp
  build/qemu-system-i386w.exe dist/xemu.exe`.
- Traps: CRLF checkouts (`git config core.autocrlf false`) and a missing
  `USERPROFILE` when another tool drives bash (export it).
- `XEMU_PGO` works with MSYS2 GCC through gcov `.gcda` profiles (det
  heavy-f1 -3.1% core cycles). `pgo/default.profdata` is clang-only, so
  run your own `generate` first. Don't commit `.gcda` files.

**Cross (Docker)**, CI's recipe. On a Mac, run it under
`colima start --vm-type vz --vz-rosetta` on a copy of the tree in `$HOME`.

```bash
docker run --rm -v "$PWD:/src" -w /src \
  -e CROSSPREFIX=x86_64-w64-mingw32.static- \
  -e CROSSAR=x86_64-w64-mingw32.static-gcc-ar \
  ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd \
  ./build.sh -p win64-cross -Db_lto=true -Dx86_version=3
```

arm64: `ghcr.io/xemu-project/xemu-win64-toolchain:sha-0d06ce8`, no extra
options, `CROSSPREFIX=aarch64-w64-mingw32.static-`,
`CROSSAR=aarch64-w64-mingw32.static-ar`.

## Linux

`./build.sh` writes `dist/xemu`; `sudo apt-get build-dep .` installs the
dependencies from `debian/control` (CI: Ubuntu 22.04). Release builds
force ThinLTO, which meson rejects under GCC: use clang and lld (CI:
`CC=clang-21 CXX=clang++-21 CC_LD=lld-21 CXX_LD=lld-21`), or `--debug`
with gcc. CI builds `.deb`s and AppImages; releases ship the AppImages.

**DSP JIT.** aarch64 POSIX hosts (macOS, Linux arm64) build the AArch64
emitter. Elsewhere the DSP runs the interpreter or upstream's dsp56300
JIT. The portable LLVM DSP JIT (a Phase 1 that wraps the interpreter)
builds only with `-Ddsp_llvm_jit=enabled`, LLVM 21 or newer and a native
(or cross-file) `llvm-config`.

## Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto | `-mcpu=`; auto picks `apple-mN` from `sysctl machdep.cpu.brand_string`. Use `apple-m1` for anything you redistribute (CI does) |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` or `use` |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | `1`: hardened-runtime codesign with `xemu.entitlements` |
| `XEMU_MOLTENVK_VERSION` | `1.4.2` | MoltenVK release to vendor; the single home of the expected version. macOS CI fails if the `Bundling MoltenVK from …` version differs from this default |
| `XEMU_KK_PREFIX` | unset | arm64: bundle `$XEMU_KK_PREFIX/lib/libvulkan_kosmickrisp.dylib` |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for `scripts/build-moltenvk.sh` |
| `CROSSPREFIX` / `CROSSAR` | unset | Toolchain prefix and `ar` for `-p win64-cross` |

Runtime `XEMU_*` diagnostics, tuning variables and the few normal-mode
switches are in [optimizations.md](optimizations.md).

## How Vulkan is loaded at runtime

Nothing links against Vulkan at build time; the renderer loads it
through volk. macOS dlopens the bundled driver by path from
`xemu.app/Contents/Libraries/<arch>/`, so users install nothing. Windows
uses the GPU driver's `vulkan-1.dll`; with no Vulkan ICD (a VM) xemu
falls back to OpenGL with a notification, so test Vulkan on real
hardware. Linux uses the distro `libvulkan` loader and the GPU's ICD.
The renderer calls `volkLoadDevice()` on Windows, Linux and KosmicKrisp
to skip the loader trampoline. `[display.vulkan] force_physical_device`
picks a GPU by name (xemu never writes it); otherwise xemu takes the
display's GPU where the display is shared through external memory, else
the first compatible device. stderr prints
`Available physical devices:` and `Selected physical device: …`.

On macOS, `[display.vulkan] driver` (Settings > Display > Vulkan
driver, read once per launch; a fallback never rewrites it) is `auto`
(the default) or `moltenvk` for MoltenVK, or `kosmickrisp` to opt in to
KosmicKrisp. Before macOS 26, without a Metal 4 GPU or off Metal
presentation, `kosmickrisp` runs MoltenVK with a toast that says why;
MoltenVK stays bundled, so macOS 14/15 and Intel Macs keep working. If
KosmicKrisp fails to load, start or pass its acceptance check, xemu
prints `xemu: KosmicKrisp unavailable (<why>); using MoltenVK`. With
`driver = kosmickrisp`, a fatal Vulkan error or an early crash leaves
`kosmickrisp-strike.txt` in the data folder and later launches use
MoltenVK (strike rules and `XEMU_VK_DRIVER`:
[kosmickrisp.md](kosmickrisp.md)). `-icount` and `XEMU_HEADLESS=1` runs
always use MoltenVK. stderr names the result:
`xemu: Vulkan driver: MoltenVK (auto)` or `KosmicKrisp (kosmickrisp)`,
then `- Driver: MoltenVK, 1.4.3 (id 14)`.

## Build failures

| Symptom | Fix |
|---|---|
| `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T` | `rm -rf macos-libs macos-pkgs build && ./build.sh` |
| `SDK >= 14.0 not found` | Install a CLT with a `MacOSX<N>.<M>.sdk` |
