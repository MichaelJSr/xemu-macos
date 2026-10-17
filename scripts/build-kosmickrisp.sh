#!/bin/sh
# Build Mesa's KosmicKrisp Vulkan driver from source for xemu-macos,
# with nothing installed system-wide.
#
#   scripts/build-kosmickrisp.sh [kk-dir]
#
# kk-dir (or $KK_DIR) defaults to a `kk` directory beside the checkout
# (../kk). Everything goes under it: a Python venv with meson, the LLVM
# release, the SPIR-V deps, the Mesa source, the build and the install:
#
#   kk-dir/install/lib/libvulkan_kosmickrisp.dylib
#   kk-dir/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
#
# Run xemu on it with XEMU_VK_DRIVER=<kk-dir>/install/lib/libvulkan_kosmickrisp.dylib
# (docs/kosmickrisp.md). Needs Xcode 26 or later (Metal 4 headers), Apple
# Silicon, macOS 26, git, curl, cmake, ninja and pkg-config on PATH, and
# network for the first run. `gh` is used to check the LLVM release's
# sigstore attestation when present; the pinned sha256 is always checked.
# Steps are skipped when their output exists, so a rerun only rebuilds
# Mesa. About 14 minutes from scratch; LLVM takes 7.6 GB.
#
# Pins: MESA_REF is the commit the prototype was measured on (Mesa main,
# 27.0.0-devel). Flipping the default needs a release tag here (condition
# C3 in docs/kosmickrisp.md); a tag name works too (MESA_REF=mesa-26.3.0),
# but a new ref needs a fresh build-release directory.
set -eu

MESA_REF=${MESA_REF:-335d5698dca1d1b9d11ef18cf5f523cc0f7f58fb}
LLVM_VER=22.1.8
LLVM_SHA256=f260f4f7c0d430828a81ae8a3826a1d63fc0963ec2459489308cc23b1f7eab4f
SPIRV_TAG=vulkan-sdk-1.4.363.0
TRANSLATOR_TAG=v22.1.7

here=$(cd "$(dirname "$0")/.." && pwd)
K=${1:-${KK_DIR:-$(dirname "$here")/kk}}
mkdir -p "$K"
K=$(cd "$K" && pwd)
L=$K/deps/LLVM-$LLVM_VER-macOS-ARM64
JOBS=${JOBS:-6}
echo "build-kosmickrisp: building in $K (Mesa $MESA_REF)"

# 0. Python venv with meson, mako, pyyaml and packaging.
[ -x "$K/venv/bin/meson" ] || {
    python3 -m venv "$K/venv" &&
    "$K/venv/bin/pip" install -q meson mako pyyaml packaging
}

# 1. Mesa source at the pinned ref (shallow).
if [ ! -d "$K/src/mesa/.git" ]; then
    mkdir -p "$K/src"
    git init -q "$K/src/mesa"
    git -C "$K/src/mesa" remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
    git -C "$K/src/mesa" fetch -q --depth 1 origin "$MESA_REF"
    git -C "$K/src/mesa" checkout -q FETCH_HEAD
fi

# 2. LLVM/Clang prebuilt: the official release, in its own download dir.
# Only the build-time CL compiler (mesa_clc) links it; the driver doesn't.
if [ ! -x "$L/bin/llvm-config" ]; then
    T=LLVM-$LLVM_VER-macOS-ARM64.tar.xz
    U=https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER
    mkdir -p "$K/dl-llvm" "$K/deps"
    [ -f "$K/dl-llvm/$T" ] || curl -sSfL -o "$K/dl-llvm/$T" "$U/$T"
    echo "$LLVM_SHA256  $K/dl-llvm/$T" | shasum -a 256 -c -
    if command -v gh >/dev/null 2>&1; then
        [ -f "$K/dl-llvm/$T.jsonl" ] || curl -sSfL -o "$K/dl-llvm/$T.jsonl" "$U/$T.jsonl"
        gh attestation verify "$K/dl-llvm/$T" --repo llvm/llvm-project \
            --bundle "$K/dl-llvm/$T.jsonl"
    fi
    (cd "$K/deps" && nice -n 5 tar -xJf "$K/dl-llvm/$T")
fi
# The release ships its own libc++ and libunwind in lib/. With -L$L/lib
# and --prefer-static, ld picks libc++.a (without libc++abi) and mesa_clc
# fails on ___cxa_throw. Hide them so the system libc++ is used, as
# LLVM's own tools do.
if ls "$L"/lib/libc++* >/dev/null 2>&1; then
    mkdir -p "$L/lib/hidden-runtimes"
    mv "$L"/lib/libc++* "$L"/lib/libunwind* "$L/lib/hidden-runtimes/"
fi
# The release's static libs carry LLVM 22 LTO bitcode, which Apple ld's
# older libLTO can't read: every link that pulls LLVM uses the release's.
LTO="-Wl,-lto_library,$L/lib/libLTO.dylib"

# 3. SPIRV-Headers, static SPIRV-Tools, static SPIRV-LLVM-Translator.
P=$K/deps/prefix
mkdir -p "$K/deps-src"
cd "$K/deps-src"
[ -d SPIRV-Headers ] || git clone -q --depth 1 -b $SPIRV_TAG https://github.com/KhronosGroup/SPIRV-Headers.git
[ -d SPIRV-Tools ] || git clone -q --depth 1 -b $SPIRV_TAG https://github.com/KhronosGroup/SPIRV-Tools.git
[ -d SPIRV-LLVM-Translator ] || git clone -q --depth 1 -b $TRANSLATOR_TAG https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git
if [ ! -f "$P/share/pkgconfig/SPIRV-Headers.pc" ]; then
    cmake -S SPIRV-Headers -B build-spirv-headers -G Ninja \
        -DCMAKE_INSTALL_PREFIX="$P" -DSPIRV_HEADERS_ENABLE_TESTS=OFF
    cmake --install build-spirv-headers
fi
if [ ! -f "$P/lib/libSPIRV-Tools.a" ]; then
    cmake -S SPIRV-Tools -B build-spirv-tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DSPIRV-Headers_SOURCE_DIR="$K/deps-src/SPIRV-Headers" \
        -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF \
        -DBUILD_SHARED_LIBS=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    nice -n 5 ninja -j"$JOBS" -C build-spirv-tools
    cmake --install build-spirv-tools
fi
if [ ! -f "$P/lib/libLLVMSPIRVLib.a" ]; then
    cmake -S SPIRV-LLVM-Translator -B build-spirv-llvm -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DLLVM_DIR="$L/lib/cmake/llvm" \
        -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$K/deps-src/SPIRV-Headers" \
        -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_EXE_LINKER_FLAGS="$LTO" -DCMAKE_SHARED_LINKER_FLAGS="$LTO"
    nice -n 5 ninja -j"$JOBS" -C build-spirv-llvm
    cmake --install build-spirv-llvm
fi

# 4. Mesa: KosmicKrisp only, release, into $K/install.
cat > "$K/native.ini" <<EOF
[binaries]
c = '/usr/bin/clang'
cpp = '/usr/bin/clang++'
objc = '/usr/bin/clang'
objcpp = '/usr/bin/clang++'
llvm-config = '$L/bin/llvm-config'
pkg-config = '$(command -v pkg-config)'

[built-in options]
c_link_args = ['$LTO']
cpp_link_args = ['$LTO']
objc_link_args = ['$LTO']
EOF
cd "$K"
export PATH="$K/venv/bin:$PATH" PKG_CONFIG_PATH="$P/lib/pkgconfig:$P/share/pkgconfig"
[ -d build-release ] || meson setup build-release src/mesa --native-file native.ini \
    --buildtype=release --prefix="$K/install" -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
    -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static -Dllvm=enabled \
    -Dshared-llvm=disabled -Dcpp_rtti=false -Dxmlconfig=disabled -Dexpat=disabled \
    -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled
nice -n 5 ninja -j"$JOBS" -C build-release
meson install -C build-release
strings "$K/install/lib/libvulkan_kosmickrisp.dylib" | grep -m1 '^Mesa [0-9]' || true
echo "build-kosmickrisp: $K/install/lib/libvulkan_kosmickrisp.dylib"
