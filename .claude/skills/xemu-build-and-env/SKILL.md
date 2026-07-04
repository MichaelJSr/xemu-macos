---
name: xemu-build-and-env
description: Recreate the xemu-macos build environment from a clean machine and build every target. Load this skill for - "how do I build xemu" / first-time setup; "./build.sh" usage, flags (-j/-p/-a/--debug), or exit errors; fresh `git clone` and submodule setup; "SDK >= 14.0 not found" or missing Xcode Command Line Tools; MoltenVK provisioning, "libMoltenVK.dylib not found" errors, or scripts/build-moltenvk.sh (the pinned optimized MoltenVK pipeline); "sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T" or other stale pkg-config errors; "duplicate LC_RPATH" crash on launch; incremental rebuilds with `ninja` vs full `./build.sh`; a portable xemu.toml disappearing from dist/xemu.app after a rebuild; PGO builds (XEMU_PGO=generate/use); XEMU_ARM_CPU / -mcpu tuning; XEMU_VIS / XEMU_STRIP binary-size knobs; Windows builds (MSYS2 native or win64-cross via Docker/colima); Linux packaging; the GitHub Actions build matrix (ci.yml/build.yml/build-macos.yml/build-windows.yml/build-linux.yml); or `git status` showing `roms/edk2` as untracked or modified. Covers environment setup through a built, packaged bundle — not running it (see xemu-run-and-operate) and not a flag reference (see xemu-config-and-flags).
---

# xemu-build-and-env

Everything needed to take a clean macOS machine (or a fresh clone on this
one) to a built `dist/xemu.app`, plus the Windows and Linux build paths and
the CI matrix that exercises all three. This is a build-mechanics runbook:
it explains what `./build.sh` and its helper scripts actually do, in the
order they do it, and the traps that catch people who skip a step or assume
too much about incrementality.

**Ground truth as of 2026-07-04** against `xemu-macos` at commit `cf85e96597`
(tag `v0.9`), read directly from `build.sh`, `scripts/build-moltenvk.sh`,
`scripts/download-macos-libs.py`, `.gitmodules`, `subprojects/dsp56300/meson.build`,
`meson.build`, and `.github/workflows/*.yml`. Every volatile fact below has a
re-verification command in "Provenance and maintenance" at the end.

## When NOT to use this skill

- **Running, operating, or releasing the built app** (launching, the monitor
  socket, snapshots, the CI tag→draft-release flow) — see `xemu-run-and-operate`.
- **Looking up what a flag/env var/toml key does or its default** — the flat
  catalog (xemu.toml schema, all `XEMU_*`, `MVK_CONFIG_*`) lives in
  `xemu-config-and-flags`. This skill explains build-time knobs
  (`XEMU_ARM_CPU`, `XEMU_PGO`, `XEMU_VIS`, `XEMU_STRIP`, `XEMU_MOLTENVK_VERSION`,
  `XEMU_MVK_MCPU`) only in the context of *how build.sh uses them*.
- **Debugging a crash/artifact/hang that isn't a build failure** — see
  `xemu-debugging-playbook`.
- **Deciding what evidence a change needs before it ships** — see
  `xemu-change-control` and `xemu-validation-and-qa`.

## 1. Prerequisites (macOS)

| Requirement | Why | Verify |
|---|---|---|
| Apple Silicon Mac, macOS 14.0+ (macOS 26 for MetalFX + frame interpolation) | arm64 build's `-mmacosx-version-min` floor; MetalFX temporal/interpolation APIs are macOS-26-only | `sw_vers` |
| Xcode Command Line Tools with an SDK ≥ 14.0 (arm64) / ≥ 12.7.5 (x86_64) | `build.sh` hard-requires a matching SDK and exits if none found | `xcode-select -p`; see the SDK-probe gotcha below |
| Homebrew `pkg-config` | `configure`/meson dependency discovery | `pkg-config --version` |
| `ninja` | meson's backend; the actual compiler driver `build.sh`/`configure` hand off to | `command -v ninja` |
| `cmake` | several vendored deps build via meson's `cmake.subproject()` (SDL3, glslang, volk, SPIRV-Reflect) — see §3.9 | `command -v cmake` |
| `dylibbundler` | `package_macos` uses it to copy + relink dependency dylibs into the bundle | `command -v dylibbundler` (Homebrew: `brew install dylibbundler`; CI installs it explicitly — see §8) |
| `python3` ≥ 3.9, with `pyyaml` and `requests` importable | meson's Python is used throughout configure; `pyyaml` drives `subprojects/genconfig` (parses `config_spec.yml`); `requests` is used by `scripts/gen-license.py`. CI installs both via `pip install pyyaml requests` (see `.github/workflows/build-macos.yml`) | `python3 -c "import yaml, requests"` |
| Network access at configure/build time | `download-macos-libs.py` fetches MacPorts packages; `build.sh` may fetch a MoltenVK release tarball; `subprojects/dsp56300/meson.build` fetches a prebuilt Rust static lib — see §2.2 | — |

The README states the shorter version of this list (macOS 14.0+, Apple
Silicon, Xcode CLT, Homebrew `pkg-config`) — the rows above are the fuller,
independently-verified set (`ninja`/`cmake`/`dylibbundler`/`python3` deps are
real requirements the README doesn't spell out, confirmed by what CI
installs explicitly in `build-macos.yml` and by what `meson.build`'s
`cmake.subproject()` calls need at configure time).

**The SDK probe has a sharp edge.** `build.sh`'s `most_recent_macosx_sdk_ver()`
globs `/Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.[0-9]*.sdk` —
that pattern requires a two-component version directory name. A bare
`MacOSX26.sdk` (no minor version) does **not** match and is silently
skipped; only forms like `MacOSX26.5.sdk` or `MacOSX15.4.sdk` count. Verified
empirically on this machine: of `MacOSX.sdk`, `MacOSX15.sdk`,
`MacOSX15.4.sdk`, `MacOSX26.sdk`, `MacOSX26.5.sdk` under
`/Library/Developer/CommandLineTools/SDKs/`, only `MacOSX15.4.sdk` and
`MacOSX26.5.sdk` match the glob; the probe picks `26.5` (numeric-sort
descending, then rejects it only if `newest < min_ver`). If your CLT install
only has bare-numbered SDK directories, `build.sh` prints `SDK >= 14.0 not
found. Install Xcode Command Line Tools` even though a working compiler
exists — the fix is a CLT update/reinstall (`xcode-select --install`, or
`softwareupdate --list` for a newer CLT package) that lands a two-component
SDK directory.

## 2. Getting the source

```bash
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git checkout macos-optimizations
git submodule update --init --recursive
```

This is the README's documented flow and is always correct. Two things
worth knowing before you run the recursive submodule step, covered in
detail in §2.1-§2.2 below: it pulls far more than this fork's macOS build
actually needs, and one dependency (`dsp56300`) is *not* a submodule at all
— it downloads a prebuilt archive over the network the first time you
configure.

### 2.1 `git submodule update --init --recursive` pulls ~2.4 GB you probably don't need

`.gitmodules` registers 14 `roms/*` submodules (seabios, SLOF, ipxe,
openbios, u-boot, edk2, opensbi, qboot, vbootrom, …) plus
`tests/lcitool/libvirt-ci` — the standard upstream QEMU firmware-source
tree for targets this fork doesn't build (arm-softmmu, riscv64-softmmu,
etc.). On this machine, `roms/edk2` alone is **921 MB** and the full
`.git/modules/roms/` metadata is **2.4 GB** (measured 2026-07-04). None of
it is needed to build `qemu-system-i386` (the `xbox` machine type): the
i386 target ships its EDK2/SeaBIOS firmware as **prebuilt** blobs already
committed at `pc-bios/edk2-i386-code.fd.bz2`, `pc-bios/bios.bin`, etc.
(`pc-bios/meson.build` just bzip2-decompresses them when `unpack_edk2_blobs`
is true). Confirming this: CI's own `build.yml` checks out the tree with
plain `actions/checkout` (no `submodules:` option at all) and never runs
`git submodule update` for anything under `roms/` — it only fetches the
curated subproject list in §2.2 via `meson subprojects download`.

Practical takeaway: the plain `git submodule update --init --recursive` is
safe, simple, and matches the README — do it if disk/time don't matter or
you might touch other QEMU targets later. If you only care about this
fork's macOS Xbox build, you can skip it entirely; `./build.sh` does not
need any `roms/*` submodule present. See the traps table (§9) for what an
uninitialized `roms/edk2` looks like in `git status`.

### 2.2 The `dsp56300` dependency downloads a prebuilt static lib at configure time

`hw/xbox/mcpx/apu/dsp/meson.build` declares
`dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'])`.
The fallback resolves to `subprojects/dsp56300/meson.build` — a small,
**regular tracked file** (not a `.wrap`, not a git submodule) that, when the
expected static lib isn't already extracted, shells out to `curl` at
**meson configure time**:

```
url = 'https://github.com/mborgerson/dsp56300/releases/download/v{ver}/dsp56300-{ver}-{rust_target}.tar.gz'
```

(`subprojects/dsp56300/meson.build`, project version `0.1.3` as of this
writing; `rust_target` is derived from the host, e.g.
`aarch64-apple-darwin`). This is the upstream `dsp56300` Rust DSP emulator
core, vendored as a prebuilt static library + C headers
(`libdsp56300_emu_ffi.a`) rather than compiled from source — building the
Rust crate itself is not part of this tree. **This means the very first
`./build.sh` (or any `meson setup`/`ninja` that reconfigures) needs network
access**, even if every other dependency is already vendored. On this
machine the archive is already cached at
`subprojects/dsp56300/dsp56300-0.1.3-aarch64-apple-darwin.tar.gz`, so
reconfigures are offline; a clean clone is not. `scripts/archive-source.sh`
(used by CI to build the source tarball) pre-fetches this archive for both
`x86_64-unknown-linux-gnu` and `aarch64-unknown-linux-gnu` explicitly so the
Linux/Windows-cross CI legs don't need outbound network mid-build — the
macOS CI job fetches its own `aarch64-apple-darwin` / `x86_64-apple-darwin`
copy live, same as a local Mac build would.

## 3. `build.sh` anatomy

`build.sh` (repo root, ~600 lines) is a single script covering all
platforms: parse args → set platform-specific compiler/linker flags →
`configure` → `make` → an optional post-build packaging function. Reading
it top to bottom is the fastest way to understand any build behavior; this
section is that read, organized by concern.

### 3.1 Argument parsing

```
./build.sh [-j<N>] [-p <platform>] [-a <arch>] [--debug] [-- <extra configure/meson args>]
```

| Flag | Form | Meaning |
|---|---|---|
| `-j<N>` | **attached**, e.g. `-j8` | Parallel job count for `make`. `${1:2}` slices the value out of the same token — `-j 8` (space-separated) does **not** work; it leaves `job_count` empty and the auto-detected default (`sysctl -n hw.logicalcpu` on Darwin) is used instead. |
| `-p <platform>` | **space-separated** 2nd arg | Overrides `uname -s` (e.g. `-p win64-cross`). Note the case pattern is `'-p'*)`, so anything starting with `-p` matches, but the value always comes from `$2` regardless — `-pDarwin` would *not* set platform to `Darwin`, it would consume the next token as `$2` and silently drop the `Darwin` suffix. Always use a space. |
| `-a <arch>` | **space-separated** 2nd arg | Overrides `target_arch` (default `uname -m`). Same space-required caveat as `-p`. |
| `--debug` | flag | Debug build: adds `-DXEMU_DEBUG_BUILD=1` and configures with `--enable-debug --enable-trace-backends=log`. Skips all the release-only opts below. |
| anything else | — | Passed straight through to `configure` (and from there into meson via `-D...` options), e.g. `./build.sh -Dx86_version=3`. |

### 3.2 Release vs. debug options

Non-debug, non-Windows (Darwin + Linux) builds get:

```
-Db_lto=true -Db_lto_mode=thin -Db_thinlto_cache=true -Db_thinlto_cache_dir=.lto-cache
-Doptimization=3 -Dqom_cast_debug=false -Dtrace_backends=nop -Dstack_protector=disabled
```

— thin LTO with a persistent cache (`.lto-cache`, relative to the build
dir — see the trap table for its unbounded growth), `-O3`, tracing compiled
out (`nop` backend, avoids the runtime cost of trace points), QOM
cast-debug checks off, and the stack protector disabled (release perf; the
debug build keeps it). Debug builds instead pass
`--enable-debug --enable-trace-backends=log` and skip every optimization
flag above.

### 3.3 Cross-arch compiler flag exports (macOS only)

For `Darwin`, `build.sh` exports `CFLAGS`, `CXXFLAGS`, `OBJCFLAGS`, and
`LDFLAGS` — not just `CFLAGS` — each carrying `-arch $target_arch -target
$target_arch-apple-macos$macos_min_ver -isysroot $sdk -mmacosx-version-min=$macos_min_ver`.
The comment in `build.sh` explains why `CXXFLAGS`/`OBJCFLAGS` matter and not
just `CFLAGS`: *"Without CXXFLAGS, cross-arch builds (x86_64 on an arm64
runner) compile C++ objects for the host arch but link with -arch from
LDFLAGS — the CMake subprojects' (glslang) try-compile fails with an
architecture mismatch."* This is exactly the failure mode CI's universal
(lipo) build would hit without it: building the `x86_64` leg on an
`arm64` CI runner.

### 3.4 MoltenVK resolution (3-tier) + auto-vendor

The Vulkan renderer is the default and requires a working `libMoltenVK.dylib`
at both build time (headers) and bundle time (the dylib). `build.sh` checks,
in order:

1. `macos-libs/<arch>/opt/local/lib/libMoltenVK.dylib` (vendored copy)
2. `/usr/local/lib/libMoltenVK.dylib` (manual install / Vulkan SDK / this
   fork's `scripts/build-moltenvk.sh` — see §4)
3. `/opt/homebrew/lib/libMoltenVK.dylib` (`brew install molten-vk`)

If **none** exist, `build.sh` downloads the pinned official release
(`XEMU_MOLTENVK_VERSION`, default `1.4.1`) as a tarball from
`github.com/KhronosGroup/MoltenVK/releases`, and extracts the dylib plus the
full `vulkan/` header set into `macos-libs/<arch>/opt/local/{lib,include}` —
this also supplies Vulkan headers without needing Homebrew's
`vulkan-headers`. If a dylib is found but headers aren't (e.g. a bare
`/usr/local` install with no dev headers), it vendors just the headers the
same way. `PKG_CONFIG_LIBDIR` is then pinned to
`${lib_prefix}/lib/pkgconfig` (the per-arch `macos-libs` tree only) so
`pkg-config` can't cross-contaminate between architectures.

On **this machine**, `/usr/local/lib/libMoltenVK.dylib` exists (the custom
build from §4, MoltenVK 1.4.2), so tier 2 wins and no vendoring happens —
verify with `ls -la /usr/local/lib/libMoltenVK.dylib`.

### 3.5 `-mcpu` auto-detection (arm64 only)

```
XEMU_ARM_CPU set?  → use it (probe with clang; fall back to apple-m1 if clang rejects it)
otherwise          → read `sysctl -n machdep.cpu.brand_string`, extract the M-number,
                      probe `-mcpu=apple-mN`; if clang doesn't recognize it (bundled
                      clang predates that chip), walk DOWN — apple-m(N-1), apple-m(N-2), …
                      — to the newest one the toolchain accepts, rather than dropping
                      straight to apple-m1.
```

The probe itself is `probe_mcpu()`: compile a trivial `int main(){return 0;}`
with `-mcpu=$1 -target arm64-apple-macos`, discard output, check exit code.
`-mcpu=native` is deliberately avoided — the comment notes it "bakes in
implementation quirks and isn't guaranteed stable across toolchain
versions." The resolved value plus `-ffp-contract=fast` become `sys_cflags`.

### 3.6 PGO (two-stage, opt-in)

```bash
XEMU_PGO=generate ./build.sh      # adds -fprofile-generate=$XEMU_PGO_DIR (default ./pgo)
#  ... run representative games/scenes so .profraw files accumulate in $XEMU_PGO_DIR ...
XEMU_PGO=use ./build.sh           # merges .profraw -> default.profdata (via `xcrun llvm-profdata`
                                   # on macOS) if not already merged, then -fprofile-use=...
```

Both stages apply to `sys_cflags`/`sys_ldflags` only (clang/LLVM), and LTO
stays enabled throughout so cross-translation-unit inlining still happens
under profile guidance. `XEMU_PGO=use` hard-fails early if `$XEMU_PGO_DIR`
has neither `.profraw` files nor an already-merged `default.profdata`. The
same two-stage flow is wired into the Windows/MSYS2 branch too (MSYS2
clang/gcc both accept the same flags; merge uses `llvm-profdata` without the
`xcrun` prefix there).

### 3.7 `XEMU_VIS` / `XEMU_STRIP` (binary size, opt-in, off by default)

- `XEMU_VIS=1` → `-fvisibility=hidden`. Verified against this tree before
  landing (QEMU's externally-visible symbols already self-tag as default:
  `IMGUI_API`, glslang's public API, etc.), so this doesn't hide anything
  that needs to stay visible; it gives LTO more freedom to inline/drop
  internal-only functions.
- `XEMU_STRIP=1` → `-Wl,-dead_strip`. Off by default because QEMU leans on
  `__attribute__((constructor))` for `type_init`/`module_init` registration;
  macOS `ld`'s `-dead_strip` keeps constructors by default but the comment
  flags it as something to validate per change, not assume safe forever.
- Measured on this fork: `XEMU_VIS=1` alone shaves ~1.5 MiB off the arm64
  unsigned binary (mostly TCG helper debug-name strings); both together,
  ~4 MiB. Runtime perf delta was within noise at measurement time — the
  main win is link time and cleaner LTO, not runtime speed.

### 3.8 `configure` invocation and the actual build

```bash
"${configure}" \
    --extra-cflags="-DXBOX=1 ${build_cflags} ${sys_cflags} ${CFLAGS}" \
    --extra-ldflags="${sys_ldflags}" \
    --target-list=i386-softmmu \
    ${opts} "$@"
time make -j"${job_count}" ${target} 2>&1 | tee build.log
"${postbuild}"   # package_macos / package_windows / package_wincross / package_linux
```

`-DXBOX=1` is the compile-time switch that enables the Xbox machine and its
device models inside the generic i386 target. `--target-list=i386-softmmu`
is the only QEMU target built — nothing else in the tree compiles. `make`
here is the top-level QEMU `Makefile` (auto-generated by `configure` into
the source root — see §5) delegating into `build/` where it's really
invoking `ninja` underneath (`build/build.ninja`); `tee build.log` mirrors
full compiler output to `build.log` in whatever directory you ran
`./build.sh` from (repo root, in the normal in-source flow — see §5.4). On
this machine `build.log` is ~850 KB after a full build; it's overwritten
(not appended) each run.

### 3.9 CMake subprojects

Several dependencies are meson `cmake.subproject()`s, meaning **meson calls
out to `cmake` as a subprocess** to configure and build them (not meson's
native build backend): glslang (`glslang.wrap`, pinned commit
`275822a6261ee689aadb1da5f09a0ec2f058685c`, `ENABLE_OPT=false
ENABLE_HLSL=false`), volk, and SPIRV-Reflect are **always** built this way —
none of the three has a MacPorts package in `download-macos-libs.py`'s list
(§2), so there's no pkg-config path to find them any other way on macOS.
SDL3 is also wired as a `cmake.subproject()` (`sdl3.wrap`, with a long list
of `SDL_*` CMake defines disabling unused SDL subsystems) but meson.build
tries `dependency('sdl3', required: false, allow_fallback: false)` (plain
pkg-config) first; on macOS, `download-macos-libs.py` vendors a matching
`sdl3.pc` into `macos-libs/<arch>/opt/local/lib/pkgconfig/` (verified
present on this machine), and since `PKG_CONFIG_LIBDIR` is pinned exactly
there (§3.4), that's what normally satisfies SDL3 on macOS — the SDL3 cmake
subproject only actually builds when pkg-config comes up empty (typically
Windows/Linux, or a macOS tree where the vendor step didn't run). Either
way, `cmake` is a hard local prerequisite: glslang/volk/SPIRV-Reflect always
need it, so configure fails partway through Vulkan-renderer dependency
resolution without it regardless of what happens with SDL3.

### 3.10 `package_macos` walkthrough

Runs after `make` succeeds. In order:

1. **`rm -rf dist`** — the entire `dist/` tree is deleted and rebuilt from
   scratch every time. This is the source of the "portable config wiped"
   trap (§5, §9).
2. Copy `build/qemu-system-i386` → `dist/xemu.app/Contents/MacOS/xemu`.
3. **`dylibbundler`**: `dylibbundler -cd -of -b -x dist/xemu.app/Contents/MacOS/xemu -d Contents/Libraries/<arch>/ -p "@executable_path/../Libraries/<arch>/" -s macos-libs/<arch>/opt/local/lib/ -s /usr/local/lib/ -s /opt/homebrew/lib/` — walks the binary's dependency dylibs, copies each into `Contents/Libraries/<arch>/`, and rewrites their load paths to `@executable_path`-relative. `-s` lists the same three search tiers as §3.4, in the same order.
4. **Fixup pass for paths dylibbundler misses**: any remaining `/opt/local/` reference in the executable is patched with `install_name_tool -change`; each bundled dylib's own remaining `/opt/local/` references are patched to `@rpath/`-relative and the dylib is re-signed ad-hoc (`codesign -s -`) immediately after, since `install_name_tool` invalidates any existing signature.
5. **rpath dedup**: every existing `LC_RPATH` entry on the executable is deleted (`install_name_tool -delete_rpath`, errors ignored — some may already be gone), then exactly one is added back: `@executable_path/../Libraries/<arch>/`. The comment: *"macOS 26+ dyld rejects binaries with duplicate LC_RPATH entries."* This is why re-running `dylibbundler` (which appends rather than dedupes) used to be able to brick a binary on macOS 26 before this strip-then-add step existed.
6. **MoltenVK bundling**: re-resolves the same 3-tier search from §3.4 (a build-time resolution can differ from what actually got vendored, so this is resolved independently at package time), copies the winner to `Contents/Libraries/<arch>/libMoltenVK.dylib`, sets its install name to `@rpath/libMoltenVK.dylib`, ad-hoc codesigns it, and **logs provenance**: `Bundling MoltenVK from <path> (version <X.Y.Z>, <arch> UUID <uuid>)`. If no candidate exists at all, this is a **hard error** (`exit 1`) — the comment explains why: "an app bundle without MoltenVK is broken for this fork" (Vulkan is the default renderer; Metal presentation and MetalFX both depend on it being present). The provenance line matters because a stray `/usr/local` custom build can silently shadow the vendored copy — driver-version drift between what you tested and what you shipped is exactly the mechanism behind the pink-tile-class bug (see `xemu-failure-archaeology`).
7. **Resources**: creates `Contents/Resources/`, builds `xemu.icns` from the five PNG sizes in `ui/icons/` via `iconutil`, copies `Info.plist` in.
8. **Version stamp**: if a file named `XEMU_VERSION` exists at repo root, its content (up to the first `-`) is written into `Info.plist`'s `CFBundleShortVersionString`/`CFBundleVersion` via `plutil -replace`; otherwise both are set to `"0.0.0"`. **This file does not exist in a normal git checkout** — it's created only by CI's source-package step (`.github/workflows/build.yml`, `echo -n <version> > XEMU_VERSION`). Verified on this machine: a local build's `Info.plist` reports `CFBundleShortVersionString = "0.0.0"` even at tag `v0.9`. This is a *separate* mechanism from the in-app window title / `git describe` version (`scripts/xemu-version.sh`, which runs live `git describe --tags` whenever `.git` exists and feeds `xemu-version-macro.h` — that one **does** show `v0.9`-ish on a local build). Don't be surprised that `Get Info` on a locally built bundle says 0.0.0 while the app's own title bar shows the real version.
9. **Codesign**: ad-hoc by default — `codesign --force --deep --preserve-metadata=entitlements,requirements,flags,runtime --sign - dist/xemu.app/Contents/MacOS/xemu`. `--deep` re-signs every bundled dylib (SDL3, MoltenVK, …) under the same identity so dyld's same-team-ID check passes. Ad-hoc signing is sufficient for `MAP_JIT`/TCG to work locally — no entitlements file needed. Set `XEMU_CODESIGN_ENTITLEMENTS=1` to instead sign every dylib *and* the executable with `xemu.entitlements` (`com.apple.security.cs.allow-jit`, `com.apple.security.cs.allow-unsigned-executable-memory`) under the hardened runtime — rarely needed locally; real distribution signing/notarization is `scripts/sign-macos-release.sh` (separate script, not part of `build.sh`).
10. **License**: `python3 scripts/gen-license.py --version-file=macos-libs/<arch>/INSTALLED > dist/LICENSE.txt`.

### 3.11 Windows/Linux packaging (brief; full sections below)

`package_windows` and `package_wincross` just copy the built exe to
`dist/xemu.exe`; the native path also runs `get_deps.py` (repo-root script,
DLL-dependency copier analogous to `dylibbundler`) and the cross path emits
a license file instead. `package_linux` copies `build/qemu-system-i386` to
`dist/xemu` and writes a license file. Neither does the dylib/rpath/codesign
choreography macOS needs.

## 4. The maintained MoltenVK pipeline (`scripts/build-moltenvk.sh`)

This fork maintains its own optimized MoltenVK build, installed to
`/usr/local/lib`, so local builds ship a known driver instead of whatever
the vendor-fetch (§3.4 tier 3, official release) or Homebrew happens to
carry. Usage:

```bash
scripts/build-moltenvk.sh [checkout-dir]      # default: ../../MoltenVK relative to scripts/
```

Which resolves, from this repo's location, to
`/Users/michaelsrouji/Documents/Xemu/tools/MoltenVK` — **verified present**
on this machine as a git checkout (`ls -d .../tools/MoltenVK/.git`). If
you're setting this up fresh and that directory doesn't exist, the script
tells you exactly what to run:
`git clone https://github.com/KhronosGroup/MoltenVK.git <dir>`.

What it does:

1. `git fetch origin --quiet && git checkout --quiet $MVK_PIN` — pins to a
   specific commit, currently `096714a2954fc8e9db9daae97c426d7dd7f8a838` on
   `main` (comment: "2026-07, reports 1.4.2" — i.e. this pre-1.4.2-tag
   commit self-reports version 1.4.2 via `strings`).
2. `./fetchDependencies --macos` (MoltenVK's own dependency-vendoring
   script; fast on repeat runs when nothing changed). Failure hint baked
   into the script: "a broken Xcode install manifests here; try:
   `xcodebuild -runFirstLaunch`".
3. `xcodebuild build -project MoltenVKPackaging.xcodeproj -scheme "MoltenVK Package (macOS only)" -configuration Release ARCHS=arm64 GCC_OPTIMIZATION_LEVEL=3 OTHER_CFLAGS="-mcpu=$MCPU"` — **arm64 only**, `-O3`, and a per-machine `-mcpu` (`XEMU_MVK_MCPU`, default `apple-m2`). **No LTO** — the header comment is explicit: *"thin-LTO bitcode in the static archives breaks the ShaderConverter xcframework packaging step, and measured no fps benefit over -O3 alone."* This is a real, tested constraint, not caution — do not add `-flto` here.
4. Locates the built dylib — current MoltenVK packages a framework
   (`Package/Release/MoltenVK/dynamic/MoltenVK.xcframework/macos-arm64/MoltenVK.framework/Versions/A/MoltenVK`), falling back to the older flat-dylib layout
   (`Package/Release/MoltenVK/dylib/macOS/libMoltenVK.dylib`) if the
   framework path doesn't exist (older MoltenVK checkouts).
5. Installs to `/usr/local/lib/libMoltenVK.dylib` via a `.new`-then-`mv`
   swap (avoids a half-written dylib if something else reads it mid-copy),
   sets its install name to `@rpath/libMoltenVK.dylib`, ad-hoc codesigns.
6. Prints the installed version (parsed via `strings | grep -E
   '^[0-9]+\.[0-9]+\.[0-9]+$'`) and arm64 UUID (`dwarfdump --uuid`), and
   reminds you: `next: ./build.sh (watch for the 'Bundling MoltenVK' provenance line)`.

**Verified on this machine (2026-07-04):** `/usr/local/lib/libMoltenVK.dylib`
exists and reports version `1.4.2` — this is the custom pipeline's output,
and per §3.4 tier 2 it is what `build.sh` bundles by default on this Mac
(ahead of the official 1.4.1 vendor-fetch in tier 3).

**Bumping the pin.** `MVK_PIN` is a deliberate value at the top of the
script — bump it only after re-running the full validation gauntlet
(interleaved savestate fps A/B + an artifact-hunt soak) per
`xemu-validation-and-qa`. A MoltenVK version bump changed shader-compile and
command-buffer behavior before (see the prefill saga in
`xemu-failure-archaeology`); treat every pin bump as a renderer-behavior
change, not a mechanical version stamp.

## 5. Incremental workflow

### 5.1 What picks up automatically with a bare `ninja`

```bash
cd build && ninja
```

This is meson/ninja underneath, and the dependency graph is broader than
"recompile changed `.c` files." Verified directly in `build/build.ninja`:

- **Source changes** (`.c`/`.cc`/`.m`/`.mm`) — normal incremental
  compile+link, as expected.
- **`config_spec.yml` changes** — picked up automatically. It's declared as
  a `custom_target` input (`meson.build:3808`,
  `input: [files('config_spec.yml')]`) feeding the `genconfig` script that
  produces `xemu-config.h`; ninja tracks that file as a build-graph input
  like any other, so editing a setting's default/type/range in
  `config_spec.yml` and running plain `ninja` regenerates the header and
  rebuilds everything that includes it. **No reconfigure needed.**
- **`meson.build` changes anywhere in the tree** (yours or a vendored
  subproject's) — also picked up automatically. `build/build.ninja` contains
  a self-regenerating rule (`build build.ninja: REGENERATE_BUILD ../meson.build
  ...`) listing essentially every `meson.build`/`Kconfig`/`meson_options.txt`
  in the source tree as an input; ninja reruns meson's configure step for
  you before building anything else. You do not need to manually reconfigure
  after adding a source file to a `meson.build`.

### 5.2 What a bare `ninja` does *not* do

- **It does not touch `dist/`.** `ninja`/`make` only produce
  `build/qemu-system-i386`; copying it into the app bundle, dylib-bundling,
  rpath surgery, MoltenVK bundling, and codesigning are all inside
  `package_macos`, a shell function that only runs as `build.sh`'s
  `postbuild` step. After a `ninja`-only rebuild, `dist/xemu.app` still
  contains the **previous** build's binary.
- **It does not re-run `configure`-level decisions.** Anything decided by
  `build.sh`'s own shell logic before it invokes `configure` — the
  MoltenVK 3-tier resolution, the `-mcpu` walk-down, switching `-a`/`-p`,
  flipping `--debug`, or any change to the exported `CFLAGS`/`LDFLAGS`
  themselves — needs a real `./build.sh` re-run to take effect.

### 5.3 The safe way to refresh `dist/xemu.app` without a full repackage

If you've only changed source (or `config_spec.yml`) and want to update the
already-built app bundle without a full `./build.sh` repackage, replicate
just the parts of `package_macos` that actually need to change:

```bash
cd build && ninja qemu-system-i386      # or plain `ninja`
cd ..
cp build/qemu-system-i386 dist/xemu.app/Contents/MacOS/xemu
codesign --force --deep \
    --preserve-metadata=entitlements,requirements,flags,runtime \
    --sign - dist/xemu.app/Contents/MacOS/xemu
```

This is safe exactly when your dependency set hasn't changed (no new
dylibs, no rpath changes) — i.e. almost always, for a source-only edit. If
you've changed dependencies (added a library, bumped MoltenVK), just accept
the full `./build.sh` repackage. (`qemu-system-i386` is a real ninja target
name, verified in `build/build.ninja`; it's the same target `build.sh`
passes to `make`.)

Note: do NOT plant a portable `xemu.toml` inside `dist/xemu.app` at all —
that bundle is the one the user actually launches, and a planted portable
config hijacks their real session (change-control non-negotiable 3;
`xemu-testing` cardinal rule 2). For a test configuration, pass
`-config_path <scratch>/test.toml`, or work against an isolated APFS
clone — `cp -Rc dist/xemu.app <scratch>/xemu-test.app` — and plant/launch
there, refreshing the clone after every rebuild (`xemu-run-and-operate`
§8).

### 5.4 The nuclear clean

```bash
rm -rf macos-libs macos-pkgs build dist && ./build.sh
```

README's documented fix for `sizeof(size_t) doesn't match
GLIB_SIZEOF_SIZE_T` and the general "something in the build graph is stale
in a way incremental rebuilds can't fix" hammer — see the traps table for
when to reach for it versus a lighter fix.

### 5.5 In-source vs. out-of-tree build directory

If you run `./build.sh` from the repo root (the normal flow), `configure`
detects `$PWD` equals the source directory and re-execs itself from inside
a freshly created `build/` directory, leaving a generated `GNUmakefile` at
the repo root that forwards `make <goal>` into `build/`. This is why
`build/` (and the `GNUmakefile` marker file
`build/auto-created-by-configure`) always exist after a first build, and
why deleting `build/` without also handling `GNUmakefile` at the root is
harmless (the marker-based logic recreates both cleanly on next run — but
`build.sh` always deletes and recreates `build/` on a full run anyway since
`configure` refuses to reuse a `build/` directory it didn't create itself:
*"ERROR: ./build dir already exists and was not previously created by
configure"*).

## 6. Windows builds

The upstream Windows build paths are preserved and CI-tested — this fork's
portable optimizations (Vulkan renderer work, APU work, pfifo/BQL fixes,
`XEMU_NV2A_NSPROF`, the helper-based hard FPU) apply automatically there.
macOS-only pieces (MetalFX, Metal/IOSurface presentation, CoreAudio, vDSP,
the ARM64 DSP JIT, the AArch64 inline x87 FPU) don't build on Windows at
all; Windows uses the bit-equivalent helper-based hard FPU instead.

### 6.1 Native (MSYS2) — preferred

```bash
./build.sh                     # from an MSYS2/MINGW shell
```

Preferred because it can actually run and be tested, unlike the cross
build. Release builds default to `-Dx86_version=3` (AVX2/BMI2/FMA) unless
you already passed your own `-Dx86_version=`
(`build.sh`'s Windows-native branch checks `echo "$@" | grep -q
'x86_version'` before adding it). `XEMU_PGO=generate`/`use` work the same
two-stage way as macOS (§3.6).

### 6.2 Cross-compile (Docker)

```bash
docker run --rm --platform linux/amd64 -v "$PWD:/xemu" -w /xemu \
    -e CROSSPREFIX=x86_64-w64-mingw32.static- \
    -e CROSSAR=x86_64-w64-mingw32.static-gcc-ar \
    ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd \
    ./build.sh -p win64-cross -Dx86_version=3
```

Image tag and env vars verified against `.github/workflows/build-windows.yml`:
the x86_64 leg uses `xemu-win64-toolchain-gcc:sha-2881edd`
(`CROSSPREFIX=x86_64-w64-mingw32.static-`,
`CROSSAR=x86_64-w64-mingw32.static-gcc-ar`); the arm64 leg uses a different
image, `xemu-win64-toolchain:sha-0d06ce8` (no `-gcc` suffix — an
llvm-mingw toolchain, `CROSSPREFIX=aarch64-w64-mingw32.static-`,
`CROSSAR=aarch64-w64-mingw32.static-ar`). `build.sh`'s `win64-cross`
platform branch is a thin one: `export AR=${AR:-$CROSSAR}`, then
`--cross-prefix=$CROSSPREFIX --static`.

Verified from an Apple Silicon Mac via colima:
`colima start --vm-type vz --vz-rosetta` — a full clean static `xemu.exe`
build took ~12 minutes. **Use a copy of the tree kept under `$HOME`**
(colima's default VM only shares the home directory, not arbitrary mount
points), and reuse that copy across builds since the cross build reuses
`build/`/`dist/` inside it like any other platform.

### 6.3 LTO ownership: build.sh must not force it on Windows

`build.sh`'s Windows branch deliberately does **not** add the release LTO
opts from §3.2. The comment is explicit about why — this is CI's call, not
build.sh's, because the right LTO strategy differs per Windows toolchain:

- x86_64 uses **GCC**: `-Db_lto_mode=thin` is a meson *setup error* on GCC
  (thin LTO is a clang/LLVM-specific mode); CI instead passes
  `--extra-cflags="-flto-incremental=$LTO_CACHE_DIR -flto-partition=cache" -Db_lto=true`
  itself (`build-windows.yml`).
- arm64 uses **llvm-mingw**, and ThinLTO there breaks
  `qemu_build_not_reached_always` elision — a QEMU idiom where an
  intentionally-undefined symbol marks unreachable code paths so the
  linker fails loudly if optimization doesn't actually eliminate them; with
  regular LTO instead of Thin, the arm64 release link failed the same way,
  and regular LTO doesn't pass through `cv2pdb` (the PDB-generation step) at
  all. CI's `build-windows.yml` currently passes **no LTO options** on the
  arm64 leg (`opts=()`) pending a real fix — see the `FIXME` comment there.

Net: if you're touching Windows build logic, don't "helpfully" add the
Darwin/Linux LTO opts to the Windows branch — that regresses one or both
Windows legs in a way only visible on CI's actual toolchains.

## 7. Linux builds

`package_linux` (build.sh) is minimal: copy `build/qemu-system-i386` to
`dist/xemu`, write a license file. CI's Linux leg (`build-linux.yml`) does
substantially more than `build.sh` alone — it builds a `.deb` via
`dpkg-buildpackage`, pulls a pinned LLVM 21 toolchain from `apt.llvm.org`,
and wraps the result into an AppImage via `linuxdeploy`. That packaging
logic lives entirely in the CI workflow, not in `build.sh`. Two
architectures are covered: `x86_64` and `aarch64` (`ubuntu-22.04`/
`ubuntu-22.04-arm` runners), debug + release each.

## 8. CI build matrix

`ci.yml` (push/PR) calls `build.yml`, which builds one source tarball
(`scripts/archive-source.sh` output) and fans it out to three
platform-specific workflows. **No workflow runs any test suite — the whole
matrix is compile-and-package only.**

| Workflow | Runner(s) | Matrix | Notes |
|---|---|---|---|
| `build-macos.yml` | `macos-15` | `{x86_64,arm64} × {debug,release}`, then a `build_universal` job lipo-merges the two arches per configuration | Installs `ccache coreutils dylibbundler` via Homebrew + `pip install pyyaml requests`; caches `macos-pkgs` + ccache + thinLTO cache keyed by arch/configuration/sha |
| `build-windows.yml` | `ubuntu-latest` container (`xemu-win64-toolchain[-gcc]`), PDB step on `windows-latest` | `{x86_64,arm64} × {debug,release}` | Cross-compiles via `win64-cross`; separate job runs `cv2pdb` to extract PDBs from DWARF |
| `build-linux.yml` | `ubuntu-22.04[-arm]` | `{x86_64,aarch64} × {debug,release}` | Builds `.deb` + AppImage; installs LLVM 21 from the apt.llvm.org pool at a pinned version |

Release (`release-on-tag.yml` on `v*` tag push, gated to
`xemu-project`/`MichaelJSr` repo owners) reuses the same `build.yml` and
publishes a **draft** GitHub release — see `xemu-run-and-operate` for the
full tag→draft→publish flow; that process, not the build matrix, is out of
scope here.

### Local-vs-CI differences (know these before comparing a local build to a CI/release artifact)

| Axis | This machine (local) | CI |
|---|---|---|
| macOS SDK | Xcode CLT with SDK 26.5 available (probe picks the newest ≥ min) | `macos-15` runner — SDK 15. This is why MetalFX/frame-interpolation code is guarded by `#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000` in `hw/xbox/nv2a/pgraph/vk/metalfx_upscale.m` — CI's SDK doesn't even declare the macOS-26 MetalFX interpolator APIs, so the guard is a compile-time necessity, not just a runtime `@available` check. |
| MoltenVK source | `/usr/local/lib/libMoltenVK.dylib`, **1.4.2**, from this fork's custom pipeline (§4) | Vendors the **official 1.4.1** release into `macos-libs` (§3.4 tier 3) — CI has no `/usr/local` custom build and no MoltenVK checkout. |
| dylibbundler / ccache | Present via Homebrew already | Installed fresh every run (`brew install ccache coreutils dylibbundler`) |
| Compiler cache | None configured locally by default | ccache + `.lto-cache`, both restored from `actions/cache` keyed by arch/configuration/sha |
| Bundle version stamp | `Info.plist` shows `0.0.0` (no `XEMU_VERSION` file — §3.10 step 8) | Real version (`git describe`-derived) baked in via the source-package step |

**This MoltenVK-version gap is a known, dated release-parity gap (as of
2026-07-04): the release artifacts CI builds and ships run the official
1.4.1 MoltenVK, while this development machine runs the custom 1.4.2 build.**
Any perf/artifact conclusion reached only on this machine should be
understood as "true for the 1.4.2 pipeline," not automatically true for
what ships. The MoltenVK pin-bump validation gauntlet
(`xemu-validation-and-qa`) exists partly to close this gap before a version
bump is trusted for release.

## 9. Traps table

| Symptom | Cause | Fix |
|---|---|---|
| `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T` | Stale `pkg-config` paths / cached configure state disagreeing with a freshly-fetched or moved `macos-libs` tree | `rm -rf macos-libs macos-pkgs build dist && ./build.sh` (§5.4) |
| Crash on launch: "duplicate LC_RPATH" | An older `build.sh` (pre rpath-dedup fix, §3.10 step 5) built the bundle you're running; macOS 26+ dyld rejects duplicate `LC_RPATH` load commands | Rebuild with current `build.sh` — it strips all rpaths and re-adds exactly one |
| `git status` shows `?? roms/edk2` (whole directory untracked) | The `roms/edk2` submodule (registered in `.gitmodules`, pinned at `edk2-stable201903-7289-g4dfdca63a9` in the index) was never initialized — typical after a non-recursive clone | Harmless for this fork's build (§2.1) — the i386-softmmu target uses prebuilt `pc-bios/*.bz2` blobs, not `roms/edk2` source. Run `git submodule update --init --recursive` only if you want a fully clean `git status` or plan to touch other QEMU targets. |
| `git status` shows `roms/edk2` **modified** (not `??`) with "untracked content" | The submodule *is* initialized; the modification is nested `.DS_Store` Finder droppings and/or a nested `libspdm`→`openssl` submodule pointer drift inside edk2's own vendored sub-submodules — cosmetic, not a real change to this repo | Ignore, or `git -C roms/edk2 clean -fdx` if it bothers you (do this yourself if you want it — it's inside a submodule you're not editing) |
| First `./build.sh` on a clean clone hangs or fails partway with a `curl`/network error unrelated to MoltenVK | `subprojects/dsp56300/meson.build` fetches a prebuilt static lib from GitHub Releases at configure time (§2.2) — needs outbound network the *first* time (cached after) | Ensure network access during the first configure; the archive is small and only needed once per fork/arch combo |
| Submodule/network needs in general | `download-macos-libs.py` hits `packages.macports.org` for SDL3/glib2/libsamplerate/libpixman/libepoxy/libpcap/libslirp/libusb every time a package isn't already in `macos-pkgs`; `build.sh` may fetch MoltenVK from GitHub Releases (§3.4); `dsp56300` fetches from GitHub Releases (§2.2) | Keep network available for first builds and arch switches; all three cache to disk (`macos-pkgs`, `macos-libs`, `subprojects/dsp56300/*.tar.gz`) so repeat builds on the same arch are offline |
| `build.log` growing every run | `build.sh` always does `make ... \| tee build.log` — the file is truncated and rewritten (not appended) each run, so it doesn't accumulate across builds, but a single verbose full build is already ~850 KB | Not actually a trap — just don't expect history across builds; check `build.log` from the *most recent* run only |
| `.lto-cache` (`build/.lto-cache`) growing unbounded across many rebuilds | The macOS Darwin branch of `build.sh` sets `-Db_thinlto_cache=true -Db_thinlto_cache_dir=.lto-cache` with **no size-cap flag** (unlike CI's Linux leg, which passes `-Wl,--thinlto-cache-policy=cache_size_bytes=...`). Measured 185 MiB after one full build on this machine (2026-07-04). | Periodically `rm -rf build/.lto-cache`, or fold it into the nuclear clean (§5.4) |
| A rebuilt binary "isn't taking effect" | A **currently running** `xemu` process keeps executing the old binary/dylibs it mapped at launch — rebuilding `dist/xemu.app` on disk doesn't affect an already-running instance | Quit the running instance fully before testing a rebuild; see `xemu-run-and-operate` for how to confirm what's actually running |
| Hand-placed portable `xemu.toml` (in `dist/xemu.app/Contents/Resources/`) disappears after a rebuild | `package_macos` starts with `rm -rf dist` (§3.10 step 1) — a full `./build.sh` run always wipes and recreates the whole bundle, `Resources/` included | Don't plant a portable config in `dist/xemu.app` in the first place — that's the user-launched bundle (change-control non-negotiable 3; `xemu-testing` cardinal rule 2). Dev loop: use the default config path (`~/Library/Application Support/xemu/xemu/xemu.toml`, survives rebuilds) or `-config_path <scratch>/test.toml`. Portable-mode testing: use a `cp -Rc` clone (`<scratch>/xemu-test.app`), refreshed after each rebuild; the `ninja`+manual-copy path (§5.3) refreshes only the clone's executable |
| `SDK >= 14.0 not found` despite Xcode CLT being installed | The SDK probe's glob requires a two-component version dir (`MacOSXNN.N.sdk`); bare `MacOSXNN.sdk` dirs don't match (§1) | Update/reinstall CLT so a versioned SDK directory with a minor version exists |
| `-p`/`-a` flag "not working" | Both require a **space-separated** second argument (`-p Darwin`, not `-pDarwin`); `-j` is the opposite — attached only (`-j8`, not `-j 8`) (§3.1) | Match the exact form per flag |
| Config change "needs a full rebuild" assumption | Both `config_spec.yml` edits and `meson.build` edits are already tracked ninja inputs — plain `ninja` reconfigures and rebuilds automatically (§5.1) | Don't reach for a full `./build.sh` (and its Resources wipe) just because you edited a build-description file; only true configure-*argument* changes (arch, debug flag, LTO toggles, MoltenVK re-resolution) need it |

## Provenance and maintenance

Re-run these after any pull to catch drift in this document:

- Build script structure/flags: `grep -n "opts=\"\$opts" build.sh` and diff against §3.1-§3.7.
- MoltenVK 3-tier + auto-vendor version: `grep -n "XEMU_MOLTENVK_VERSION\|moltenvk_ver=" build.sh`.
- Custom MoltenVK pin + flags: `grep -n "MVK_PIN\|MCPU=\|GCC_OPTIMIZATION_LEVEL\|-flto" scripts/build-moltenvk.sh`.
- Installed custom MoltenVK version on this machine: `strings /usr/local/lib/libMoltenVK.dylib | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$'`.
- MoltenVK checkout present: `ls -d "$(dirname scripts/build-moltenvk.sh)/../../MoltenVK/.git"` (run from repo root).
- dsp56300 fetch URL/version: `grep -n "ver =\|url =" subprojects/dsp56300/meson.build`.
- roms/edk2 submodule state: `git submodule status roms/edk2` and `git status --porcelain roms/edk2`.
- CI matrix shape: `grep -n "runs-on\|matrix:" .github/workflows/build-macos.yml .github/workflows/build-windows.yml .github/workflows/build-linux.yml`.
- CI macOS SDK level (vs. this machine's): the `runs-on:` value in `build-macos.yml` (`macos-15` as of this writing) vs. `ls /Library/Developer/CommandLineTools/SDKs/` here.
- Windows LTO ownership split: `grep -n "FIXME\|Dqom_cast_debug\|flto" .github/workflows/build-windows.yml`.
- `.lto-cache` size: `du -sh build/.lto-cache`.
- Info.plist local-build version stamp: `plutil -p dist/xemu.app/Contents/Info.plist | grep -i BundleVersion` after a local build (expect `0.0.0` absent an `XEMU_VERSION` file).
