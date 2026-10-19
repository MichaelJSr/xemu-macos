#!/bin/sh
# Build Mesa's KosmicKrisp Vulkan driver for xemu-macos from a pinned
# Mesa tag, with nothing installed system-wide.
#
#   scripts/build-kosmickrisp.sh [-n] [build|package] [kk-dir]
#
#   build    (default) build and install into $KK_PREFIX, by default
#            kk-dir/install-<mesa-tag>
#   package  build, then write kk-dir/dist/<release>-arm64.tar.xz:
#            lib/libvulkan_kosmickrisp.dylib (install name @rpath/..., ad-hoc
#            signed), share/vulkan/icd.d/*.json (relative library_path),
#            share/kosmickrisp/PROVENANCE and share/kosmickrisp/LICENSES/
#   -n       print the resolved settings as KEY=value lines and exit
#
# The pins are kk_mesa_tag and kk_mesa_sha in build.sh. MESA_TAG and
# MESA_SHA override them, together (CI's dispatch inputs). Mesa comes
# from git, not the release tarball, so the driver reports
# "Mesa <version> (git-<sha10>)", and the fetched tag must resolve to
# the pinned commit. The SPIR-V deps are pinned by tag and commit below.
# <release> is kosmickrisp-<mesa-tag>, or kosmickrisp-<mesa-tag>-<n>
# with KK_RESPIN=<n> (CI's respin input), for a rebuild of a Mesa tag
# whose release is already published.
#
# kk-dir (or $KK_DIR) defaults to `kk` beside the checkout (../kk). It
# holds a venv with pinned meson, the LLVM release and the SPIR-V deps,
# shared across pins, and per tag the Mesa source (src/<tag>), build
# (build-<tag>) and install. An older install such as kk-dir/install
# is never touched unless KK_PREFIX names it.
#
# Run xemu on it with XEMU_VK_DRIVER=<prefix>/lib/libvulkan_kosmickrisp.dylib,
# or copy it into a clone's Contents/Libraries/arm64, where the
# display.vulkan.driver setting finds it (docs/kosmickrisp.md).
#
# Needs Apple Silicon, macOS 26 and Xcode 26 or later (Metal 4 headers),
# git, curl, cmake, ninja and pkg-config on PATH, Homebrew's zstd (the
# LLVM release's static link line names /opt/homebrew/lib/libzstd.a),
# and network for the first run. `gh`, when on PATH, also checks the
# LLVM release's sigstore attestation against its bundle (no login
# needed); the pinned sha256 is always checked.
# Steps are skipped when their output exists, so a rerun only rebuilds
# Mesa. About 14 minutes from scratch on an M2 Ultra; LLVM takes 7.6 GB.
# CI runs `JOBS=3 ... package` in .github/workflows/kosmickrisp-asset.yml.
set -eu

LLVM_VER=22.1.8
LLVM_SHA256=f260f4f7c0d430828a81ae8a3826a1d63fc0963ec2459489308cc23b1f7eab4f
LLVM_LICENSE_SHA256=8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee
SPIRV_TAG=vulkan-sdk-1.4.363.0
SPIRV_HEADERS_SHA=496543121ce6419f23d6fa5d7194ba66c36212d2
SPIRV_TOOLS_SHA=ef96ed763b43b59b33b31b362f09a02b729fa1c9
TRANSLATOR_TAG=v22.1.7
TRANSLATOR_SHA=b4c9189bcba7ed9a1189d6a2c27edc3d9a3d5277
PIP_PINS="meson==1.12.1 mako==1.4.3 markupsafe==3.0.4 pyyaml==6.0.3 packaging==26.3"
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
MINOS=26.0

die() { echo "build-kosmickrisp: $*" >&2; exit 1; }
usage() { sed -n '5,13p' "$0" >&2; exit 2; }
script_sha256=$(shasum -a 256 "$0" | awk '{ print $1 }')
here=$(cd "$(dirname "$0")/.." && pwd)

mode=build dry=0 K=
while [ $# -gt 0 ]; do
    case $1 in
    -n|--dry-run) dry=1 ;;
    build|package) mode=$1 ;;
    -*) usage ;;
    *) [ -z "$K" ] || usage; K=$1 ;;
    esac
    shift
done

# Pins: plain assignments in build.sh.
pin() {
    awk -F= -v k="$1" '$1 == k { v = $2 } END { gsub(/["\047]/, "", v); print v }' \
        "$here/build.sh"
}
if [ -n "${MESA_TAG:-}${MESA_SHA:-}" ]; then
    if [ -z "${MESA_TAG:-}" ] || [ -z "${MESA_SHA:-}" ]; then
        die "set MESA_TAG and MESA_SHA together"
    fi
else
    MESA_TAG=$(pin kk_mesa_tag) MESA_SHA=$(pin kk_mesa_sha)
fi
case $MESA_TAG in
*[!A-Za-z0-9._-]*) die "'$MESA_TAG' is not a Mesa release tag" ;;
mesa-[0-9]*) ;;
*) die "'$MESA_TAG' is not a Mesa release tag" ;;
esac
case $MESA_SHA in *[!0-9a-f]*|'') die "'$MESA_SHA' is not a commit sha" ;; esac
[ ${#MESA_SHA} -eq 40 ] || die "'$MESA_SHA' is not a full 40-character commit sha"
KK_RESPIN=${KK_RESPIN:-$(pin kk_respin)}
case $KK_RESPIN in
'') release=kosmickrisp-$MESA_TAG ;;
0*|*[!0-9]*) die "KK_RESPIN '$KK_RESPIN' is not a respin number (1, 2, ...)" ;;
*) release=kosmickrisp-$MESA_TAG-$KK_RESPIN ;;
esac

K=${K:-${KK_DIR:-$(dirname "$here")/kk}}
case $K in /*) ;; *) K=$PWD/$K ;; esac
prefix=${KK_PREFIX:-}
case $prefix in /*|'') ;; *) prefix=$PWD/$prefix ;; esac
[ "$dry" = 1 ] || mkdir -p "$K"
[ ! -d "$K" ] || K=$(cd "$K" && pwd)
prefix=${prefix:-$K/install-$MESA_TAG}
asset=$K/dist/$release-arm64.tar.xz
if [ "$dry" = 1 ]; then
    printf '%s\n' "MESA_TAG=$MESA_TAG" "MESA_SHA=$MESA_SHA" "KK_RESPIN=$KK_RESPIN" "KK_RELEASE=$release" \
        "KK_DIR=$K" "KK_PREFIX=$prefix" "KK_ASSET=$asset"
    exit 0
fi

[ "$(uname -m)" = arm64 ] || die "needs Apple Silicon"
for t in git curl cmake ninja pkg-config python3 xcrun; do
    command -v "$t" >/dev/null 2>&1 || die "$t is not on PATH"
done
sdk=$(xcrun --sdk macosx --show-sdk-version)
[ "${sdk%%.*}" -ge 26 ] 2>/dev/null ||
    die "needs the macOS 26 SDK or later (Xcode 26+, Metal 4 headers); xcrun has '$sdk'"

L=$K/deps/LLVM-$LLVM_VER-macOS-ARM64
P=$K/deps/prefix
M=$K/src/$MESA_TAG
B=$K/build-$MESA_TAG
JOBS=${JOBS:-6}
want="Mesa ${MESA_TAG#mesa-} (git-$(printf %.10s "$MESA_SHA"))"
export MACOSX_DEPLOYMENT_TARGET=$MINOS
unset MESA_GIT_SHA1_OVERRIDE
echo "build-kosmickrisp: $mode $MESA_TAG ($MESA_SHA) as $release in $K, installing to $prefix"

# Download $1 to $2 unless it exists, then check sha256 $3.
fetch() {
    [ -f "$2" ] || { curl -sSfL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"; }
    echo "$3  $2" | shasum -a 256 -c -
}

# 0. Python venv with pinned meson, mako, pyyaml and packaging.
if [ "$(cat "$K/venv/kk-pins" 2>/dev/null)" != "$PIP_PINS" ]; then
    [ -x "$K/venv/bin/python3" ] || python3 -m venv "$K/venv"
    # shellcheck disable=SC2086 # one word per pin
    "$K/venv/bin/pip" install -q --disable-pip-version-check $PIP_PINS
    echo "$PIP_PINS" > "$K/venv/kk-pins"
fi

# 1. Mesa source: a shallow fetch of the tag, which must be the pinned
# commit. A tree with local changes is refused even at the pinned HEAD.
head=
if [ -d "$M/.git" ]; then
    head=$(git -C "$M" rev-parse -q --verify 'HEAD^{commit}' || true)
    [ -z "$(git -C "$M" status --porcelain --untracked-files=no)" ] ||
        die "$M has local changes"
fi
if [ "$head" != "$MESA_SHA" ]; then
    if [ ! -d "$M/.git" ]; then
        mkdir -p "$M"
        git init -q "$M"
        git -C "$M" remote add origin "$MESA_URL"
    fi
    n=0
    until git -C "$M" fetch -q --depth 1 origin "refs/tags/$MESA_TAG"; do
        n=$((n + 1))
        [ "$n" -lt 4 ] || die "could not fetch $MESA_TAG from $MESA_URL"
        echo "build-kosmickrisp: fetch failed, retrying in 15 s"
        sleep 15
    done
    got=$(git -C "$M" rev-parse 'FETCH_HEAD^{commit}')
    [ "$got" = "$MESA_SHA" ] ||
        die "$MESA_TAG is commit $got upstream, not the pinned $MESA_SHA"
    git -C "$M" checkout -q --detach "$MESA_SHA"
fi

# The package's license texts, checked before the long build.
mkdir -p "$K/dl-llvm"
LLVM_LICENSE=$K/dl-llvm/LICENSE-llvmorg-$LLVM_VER.TXT
if [ "$mode" = package ]; then
    for f in docs/license.rst licenses/MIT licenses/BSL-1.0; do
        [ -f "$M/$f" ] || die "$MESA_TAG has no $f: update the package's LICENSES"
    done
    fetch "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LLVM_VER/llvm/LICENSE.TXT" \
        "$LLVM_LICENSE" "$LLVM_LICENSE_SHA256"
fi

# 2. LLVM/Clang prebuilt: the official release, in its own download dir.
# Only the build-time CL compiler (mesa_clc) links it; the driver doesn't.
if [ ! -x "$L/bin/llvm-config" ]; then
    T=LLVM-$LLVM_VER-macOS-ARM64.tar.xz
    U=https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER
    mkdir -p "$K/deps"
    fetch "$U/$T" "$K/dl-llvm/$T" "$LLVM_SHA256"
    if command -v gh >/dev/null 2>&1; then
        [ -f "$K/dl-llvm/$T.jsonl" ] || curl -sSfL --retry 3 -o "$K/dl-llvm/$T.jsonl" "$U/$T.jsonl"
        gh attestation verify "$K/dl-llvm/$T" --repo llvm/llvm-project \
            --bundle "$K/dl-llvm/$T.jsonl"
    fi
    (cd "$K/deps" && nice -n 5 tar -xJf "$K/dl-llvm/$T")
    # A runner has about 14 GB free; the extracted tree is 7.3 GB.
    [ -z "${CI:-}" ] || rm -f "$K/dl-llvm/$T"
fi
# The release's static link line names absolute paths outside it
# (Homebrew's /opt/homebrew/lib/libzstd.a): fail now, not at mesa_clc's
# link. PROVENANCE records the zstd that the build tools link.
zstd=none
for f in $("$L/bin/llvm-config" --link-static --system-libs); do
    case $f in /*) ;; *) continue ;; esac
    [ -e "$f" ] || die "LLVM $LLVM_VER links $f, which is missing (for libzstd: brew install zstd)"
    case ${f##*/} in
    libzstd*)
        r=$(realpath "$f")
        v=$(awk '$1 == "#define" && $2 ~ /^ZSTD_VERSION_(MAJOR|MINOR|RELEASE)$/ {
            v = v (v == "" ? "" : ".") $3 } END { print (v == "" ? "unknown" : v) }' \
            "$(dirname "$r")/../include/zstd.h" 2>/dev/null || echo unknown)
        zstd="$v $r sha256 $(shasum -a 256 "$r" | awk '{ print $1 }')"
        ;;
    esac
done
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
# The translator's llvm-spirv is a ThinLTO link over LLVM's bitcode:
# one link at a time, for the 7 GB runner.
mkdir -p "$K/deps-src"
cd "$K/deps-src"
# Clone KhronosGroup/$1 at tag $2 unless present, then check that it is
# at commit $3.
dep() {
    [ -d "$1" ] || git clone -q --depth 1 -b "$2" "https://github.com/KhronosGroup/$1.git"
    got=$(git -C "$1" rev-parse 'HEAD^{commit}')
    [ "$got" = "$3" ] ||
        die "$K/deps-src/$1 is at $got, not $2's pinned $3 (remove it and $P)"
}
dep SPIRV-Headers "$SPIRV_TAG" "$SPIRV_HEADERS_SHA"
dep SPIRV-Tools "$SPIRV_TAG" "$SPIRV_TOOLS_SHA"
dep SPIRV-LLVM-Translator "$TRANSLATOR_TAG" "$TRANSLATOR_SHA"
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
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    nice -n 5 ninja -j"$JOBS" -C build-spirv-tools
    cmake --install build-spirv-tools
fi
if [ ! -f "$P/lib/libLLVMSPIRVLib.a" ]; then
    cmake -S SPIRV-LLVM-Translator -B build-spirv-llvm -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DLLVM_DIR="$L/lib/cmake/llvm" \
        -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$K/deps-src/SPIRV-Headers" \
        -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link \
        -DCMAKE_EXE_LINKER_FLAGS="$LTO" -DCMAKE_SHARED_LINKER_FLAGS="$LTO"
    nice -n 5 ninja -j"$JOBS" -C build-spirv-llvm
    cmake --install build-spirv-llvm
fi

# 4. Mesa: KosmicKrisp only, release. The headerpad lets build.sh
# rewrite the install name; backend_max_links=1 serializes the mesa_clc
# ThinLTO link with the rest.
N=$K/native-$MESA_TAG.ini
MIN="-mmacosx-version-min=$MINOS"
LINK="'$LTO', '-Wl,-headerpad_max_install_names', '$MIN'"
cat > "$N" <<EOF
[binaries]
c = '/usr/bin/clang'
cpp = '/usr/bin/clang++'
objc = '/usr/bin/clang'
objcpp = '/usr/bin/clang++'
llvm-config = '$L/bin/llvm-config'
pkg-config = '$(command -v pkg-config)'

[built-in options]
c_args = ['$MIN']
cpp_args = ['$MIN']
objc_args = ['$MIN']
objcpp_args = ['$MIN']
c_link_args = [$LINK]
cpp_link_args = [$LINK]
objc_link_args = [$LINK]
objcpp_link_args = [$LINK]
EOF
# A fixed PATH, so host tools can't change what gets built. Homebrew's
# glslangValidator, for one, would add Mesa's ASTC decoder to the driver.
mkdir -p "$K/tools"
for t in ninja git; do ln -sf "$(command -v "$t")" "$K/tools/$t"; done
PATH=$K/venv/bin:$K/tools:/usr/bin:/bin:/usr/sbin:/sbin
PKG_CONFIG_PATH=$P/lib/pkgconfig:$P/share/pkgconfig
export PATH PKG_CONFIG_PATH
cd "$K"
if [ -f "$B/build.ninja" ]; then
    cur=$(python3 -c 'import json, sys
print(next(o["value"] for o in json.load(open(sys.argv[1])) if o["name"] == "prefix"))' \
        "$B/meson-info/intro-buildoptions.json")
    [ "$cur" = "$prefix" ] || meson configure "$B" -Dprefix="$prefix"
else
    rm -rf "$B"
    meson setup "$B" "$M" --native-file "$N" \
        --buildtype=release --prefix="$prefix" -Dbackend_max_links=1 \
        -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
        -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static -Dllvm=enabled \
        -Dshared-llvm=disabled -Dcpp_rtti=false -Dxmlconfig=disabled -Dexpat=disabled \
        -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled
fi
nice -n 5 ninja -j"$JOBS" -C "$B"
meson install -C "$B" --quiet
lib=$prefix/lib/libvulkan_kosmickrisp.dylib
ver=$(strings "$lib" | awk '/^Mesa [0-9]/ && !v { v = $0 } END { print v }')
[ "$ver" = "$want" ] || die "$lib reports '$ver', not '$want'"
echo "build-kosmickrisp: $ver: $lib"
[ "$mode" = package ] || exit 0

# 5. Package: the dylib with its final install name, a relocatable ICD
# manifest (for the validation-layer recipe), PROVENANCE and LICENSES.
S=$K/dist/stage-$MESA_TAG
rm -rf "$S"
mkdir -p "$S/lib" "$S/share/vulkan/icd.d" "$S/share/kosmickrisp/LICENSES"
d=$S/lib/libvulkan_kosmickrisp.dylib
cp "$lib" "$d"
install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib "$d"
codesign -s - -f "$d"
for j in "$prefix"/share/vulkan/icd.d/kosmickrisp_*.json; do
    sed 's#"library_path": *"[^"]*"#"library_path": "../../../lib/libvulkan_kosmickrisp.dylib"#' \
        "$j" > "$S/share/vulkan/icd.d/${j##*/}"
done
C=$S/share/kosmickrisp/LICENSES
cp "$M/docs/license.rst" "$C/Mesa-license.rst"
cp "$M/licenses/MIT" "$C/Mesa-MIT.txt"
cp "$M/licenses/BSL-1.0" "$C/Mesa-BSL-1.0.txt"
cp "$K/deps-src/SPIRV-Tools/LICENSE" "$C/SPIRV-Tools-Apache-2.0.txt"
cp "$K/deps-src/SPIRV-Headers/LICENSE" "$C/SPIRV-Headers.txt"
cp "$LLVM_LICENSE" "$C/LLVM-Apache-2.0-WITH-LLVM-exception.txt"
gitrev() { git -C "$K/deps-src/$1" rev-parse HEAD; }
{
    echo "release=$release"
    echo "mesa_tag=$MESA_TAG"
    echo "mesa_commit=$MESA_SHA"
    echo "mesa_version=$ver"
    echo "llvm=$LLVM_VER LLVM-$LLVM_VER-macOS-ARM64.tar.xz sha256 $LLVM_SHA256"
    echo "spirv_headers=$SPIRV_TAG $(gitrev SPIRV-Headers)"
    echo "spirv_tools=$SPIRV_TAG $(gitrev SPIRV-Tools)"
    echo "spirv_llvm_translator=$TRANSLATOR_TAG $(gitrev SPIRV-LLVM-Translator)"
    echo "zstd=$zstd"
    echo "python=$(python3 --version) $PIP_PINS"
    echo "xcode=$(xcodebuild -version 2>/dev/null |
        awk 'NR == 1 { v = $2 } NR == 2 { b = $3 } END { print (v ? v " (" b ")" : "none") }')"
    echo "sdk=macosx $sdk ($(xcrun --sdk macosx --show-sdk-build-version))"
    echo "clang=$(/usr/bin/clang --version | awk 'NR == 1')"
    echo "host=macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "runner_image=${ImageOS:-local}${ImageVersion:+ $ImageVersion}"
    [ -z "${GITHUB_RUN_ID:-}" ] ||
        echo "run_url=$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/attempts/${GITHUB_RUN_ATTEMPT:-1}"
    echo "script_sha256=$script_sha256"
    echo "dylib_sha256=$(shasum -a 256 "$d" | awk '{ print $1 }')"
    echo "dylib_uuid=$(dwarfdump --uuid "$d" | awk '{ print $2 }')"
} > "$S/share/kosmickrisp/PROVENANCE"
rm -f "$asset"
COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 --numeric-owner \
    -cJf "$asset" -C "$S" lib share
echo "build-kosmickrisp: $asset"
shasum -a 256 "$asset"
