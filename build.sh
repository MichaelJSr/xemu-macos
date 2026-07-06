#!/usr/bin/env bash

set -e # exit if a command fails
set -o pipefail # Will return the exit status of make if it fails
set -o physical # Resolve symlinks when changing directory

project_source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

target_arch=$(uname -m)

package_windows() {
    rm -rf dist
    mkdir -p dist
    cp build/qemu-system-i386w.exe dist/xemu.exe
    python3 "${project_source_dir}/get_deps.py" dist/xemu.exe dist
}

package_wincross() {
    rm -rf dist
    mkdir -p dist
    cp build/qemu-system-i386w.exe dist/xemu.exe
    python3 ./scripts/gen-license.py --platform windows > dist/LICENSE.txt
}

# --- MoltenVK helpers (macOS) ---
# The Vulkan renderer is the default; volk dlopens libMoltenVK.dylib at
# runtime, so the build must (a) see Vulkan headers at configure time
# and (b) bundle a dylib at package time. ONE resolution rule serves
# both steps, in tier order: vendored macos-libs, /usr/local (Vulkan
# SDK / scripts/build-moltenvk.sh custom build), Homebrew.
#
# A dylib only counts if it contains the target arch: a single-arch
# system install (e.g. the custom arm64-only /usr/local build) must
# not short-circuit vendoring for a cross build — that silently
# disabled the whole Vulkan renderer for '-a x86_64' and broke the
# link via the UI's MetalFX references (hit 2026-07-05). The official
# release tar is universal, so the vendored copy satisfies any arch.
resolve_moltenvk() {
    local candidate
    for candidate in \
        "${PWD}/macos-libs/${target_arch}/opt/local/lib/libMoltenVK.dylib" \
        "/usr/local/lib/libMoltenVK.dylib" \
        "/opt/homebrew/lib/libMoltenVK.dylib"; do
        if [ -f "$candidate" ] && \
           lipo -archs "$candidate" 2>/dev/null | grep -qw "${target_arch}"; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

# Download the pinned official MoltenVK release and vendor it into
# macos-libs. Always installs the headers (the release tar ships the
# full vulkan/ set, so no Homebrew vulkan-headers dependency); pass
# "dylib" to also install the dylib, or "headers" when a dylib exists
# in a headerless system location.
vendor_moltenvk() {
    local what="$1"
    local ver="${XEMU_MOLTENVK_VERSION:-1.4.1}"
    local tmp
    tmp="$(mktemp -d)"
    echo "Vendoring MoltenVK v${ver} (${what}) into ${lib_prefix}..."
    curl -fsSL -o "${tmp}/MoltenVK-macos.tar" \
        "https://github.com/KhronosGroup/MoltenVK/releases/download/v${ver}/MoltenVK-macos.tar"
    tar -xf "${tmp}/MoltenVK-macos.tar" -C "${tmp}"
    mkdir -p "${lib_prefix}/include"
    cp -R "${tmp}/MoltenVK/MoltenVK/include/" "${lib_prefix}/include/"
    if [ "${what}" = "dylib" ]; then
        mkdir -p "${lib_prefix}/lib"
        cp "${tmp}/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" \
           "${lib_prefix}/lib/libMoltenVK.dylib"
    fi
    rm -rf "${tmp}"
}

# --- PGO (all clang platforms) ---
# Optional two-stage flow:
#   XEMU_PGO=generate ./build.sh   # build with -fprofile-generate
#   # run target games so .profraw files accumulate in the profile dir
#   XEMU_PGO=use ./build.sh        # rebuild with -fprofile-use
# Profile dir defaults to ${PWD}/pgo; override with XEMU_PGO_DIR.
# clang/LLVM only (merge via llvm-profdata; ${profdata_prefix} is
# "xcrun " on Darwin, empty elsewhere — expanded unquoted so the
# prefix word-splits). GCC PGO is a different mechanism and is not
# wired. LTO stays enabled so profile guidance crosses translation
# units.
setup_pgo() {
    [ -n "${XEMU_PGO}" ] || return 0
    local pgo_dir="${XEMU_PGO_DIR:-${PWD}/pgo}"
    mkdir -p "${pgo_dir}"
    case "${XEMU_PGO}" in
    generate)
        sys_cflags="${sys_cflags} -fprofile-generate=${pgo_dir}"
        sys_ldflags="${sys_ldflags:-} -fprofile-generate=${pgo_dir}"
        echo "PGO: profile-generate build; profiles will land in ${pgo_dir}"
        ;;
    use)
        # Re-merge when any .profraw is newer than the existing
        # profdata — otherwise a retrain silently loses to a stale
        # committed default.profdata (bit us 2026-07-04: the first
        # use-build after retraining ran on the old profile because
        # this only merged when profdata was absent).
        local newest_raw
        newest_raw=$(ls -t "${pgo_dir}"/*.profraw 2>/dev/null | head -1 || true)
        if [ -n "${newest_raw}" ] && \
           { [ ! -f "${pgo_dir}/default.profdata" ] || \
             [ "${newest_raw}" -nt "${pgo_dir}/default.profdata" ]; }; then
            ${profdata_prefix}llvm-profdata merge \
                -output="${pgo_dir}/default.profdata" "${pgo_dir}"/*.profraw
        fi
        if [ ! -f "${pgo_dir}/default.profdata" ]; then
            echo "PGO: no profiles found in ${pgo_dir}"
            exit 1
        fi
        sys_cflags="${sys_cflags} -fprofile-use=${pgo_dir}/default.profdata"
        sys_ldflags="${sys_ldflags:-} -fprofile-use=${pgo_dir}/default.profdata"
        echo "PGO: profile-use build using ${pgo_dir}/default.profdata"
        ;;
    *)
        echo "PGO: unknown XEMU_PGO value '${XEMU_PGO}' (want 'generate' or 'use')"
        exit 1
        ;;
    esac
}

package_macos() {
    rm -rf dist

    # Copy in executable
    mkdir -p dist/xemu.app/Contents/MacOS/
    exe_path=dist/xemu.app/Contents/MacOS/xemu
    lib_path=dist/xemu.app/Contents/Libraries/${target_arch}
    lib_rpath=../Libraries/${target_arch}
    cp build/qemu-system-i386 ${exe_path}

    # Copy in in executable dylib dependencies
    dylibbundler -cd -of -b -x dist/xemu.app/Contents/MacOS/xemu \
        -d ${lib_path}/ \
        -p "@executable_path/${lib_rpath}/" \
        -s ${PWD}/macos-libs/${target_arch}/opt/local/lib/ \
        -s /usr/local/lib/ \
        -s /opt/homebrew/lib/

    # Fixup some paths dylibbundler missed
    for dep in $(otool -L "$exe_path" | grep -e '/opt/local/' | cut -d' ' -f1); do
      dep_basename="$(basename $dep)"
      new_path="@executable_path/${lib_rpath}/${dep_basename}"
      echo "Fixing $exe_path dependency $dep_basename -> $new_path"
      install_name_tool -change "$dep" "$new_path" "$exe_path"
    done

    # (loop variable deliberately distinct from lib_path — shadowing it
    # left the variable pointing at the last dylib after the loop)
    for dylib in "${lib_path}"/*.dylib; do
      for dep in $(otool -L "$dylib" | grep -e '/opt/local/' | cut -d' ' -f1); do
        dep_basename="$(basename $dep)"
        new_path="@rpath/${dep_basename}"
        echo "Fixing $dylib dependency $dep_basename -> $new_path"
        install_name_tool -change "$dep" "$new_path" "$dylib"
        codesign -s - -f "${dylib}"
      done
    done

    # Strip duplicate/stale rpaths left by dylibbundler; macOS 26+ dyld
    # rejects binaries with duplicate LC_RPATH entries.
    for rpath in $(otool -l "$exe_path" | awk '/cmd LC_RPATH/{getline;getline;print $2}'); do
      install_name_tool -delete_rpath "$rpath" "$exe_path" 2>/dev/null || true
    done
    install_name_tool -add_rpath "@executable_path/${lib_rpath}/" "$exe_path"

    # Bundle MoltenVK for Vulkan support (loaded at runtime by Volk via dlopen).
    # The binary's LC_RPATH already points to the Libraries directory, so
    # dlopen("libMoltenVK.dylib") will find it there. Same tier walk +
    # arch rule as the configure-time resolution (resolve_moltenvk),
    # re-run here because the two can drift between configure and
    # package on a changed system.
    moltenvk_src="$(resolve_moltenvk || true)"
    if [ -n "$moltenvk_src" ]; then
      moltenvk_dst="${lib_path}/libMoltenVK.dylib"
      # Provenance: which MoltenVK is being shipped matters — a local
      # /usr/local install (custom build) can silently shadow the
      # vendored copy, and driver-version drift between the tested
      # binary and the released binary is exactly how the prefill
      # pink-tile class of bug escapes testing. Log version + UUID.
      # awk reads all input (no early pipe close): grep -m1 here made
      # strings exit on SIGPIPE, which pipefail turned into a failed
      # pipeline, so the old `|| echo unknown` fired even on a match.
      moltenvk_bundled_ver=$(strings "$moltenvk_src" | \
          awk '/^[0-9]+\.[0-9]+\.[0-9]+$/ && !v {v=$0} END {print (v ? v : "unknown")}')
      moltenvk_uuid=$(dwarfdump --uuid "$moltenvk_src" 2>/dev/null | grep -m1 "($target_arch)" | awk '{print $2}')
      echo "Bundling MoltenVK from $moltenvk_src (version ${moltenvk_bundled_ver}, ${target_arch} UUID ${moltenvk_uuid:-n/a})"
      cp "$moltenvk_src" "$moltenvk_dst"
      install_name_tool -id "@rpath/libMoltenVK.dylib" "$moltenvk_dst"
      codesign -s - -f "$moltenvk_dst"
    else
      # The Vulkan renderer is the default (and the MetalFX / Metal
      # presentation paths depend on it); an app bundle without
      # MoltenVK is broken for this fork. The compile step should
      # have vendored it — treat absence as a build error rather
      # than shipping a silently degraded bundle.
      echo "Error: libMoltenVK.dylib not found; cannot bundle Vulkan support." >&2
      echo "Re-run ./build.sh (it auto-downloads MoltenVK), or install it to /usr/local/lib or via 'brew install molten-vk'." >&2
      exit 1
    fi

    # Copy in runtime resources
    mkdir -p dist/xemu.app/Contents/Resources

    # Generate icon file
    mkdir -p xemu.iconset
    for r in 16 32 128 256 512; do cp "${project_source_dir}/ui/icons/xemu_${r}x${r}.png" "xemu.iconset/icon_${r}x${r}.png"; done
    iconutil --convert icns --output dist/xemu.app/Contents/Resources/xemu.icns xemu.iconset

    cp Info.plist dist/xemu.app/Contents/

    if [[ -e "${project_source_dir}/XEMU_VERSION" ]]; then
      xemu_version="$(cat ${project_source_dir}/XEMU_VERSION | cut -f1 -d-)"
    else
      xemu_version="0.0.0"
    fi

    plutil -replace CFBundleShortVersionString -string "${xemu_version}" dist/xemu.app/Contents/Info.plist
    plutil -replace CFBundleVersion            -string "${xemu_version}" dist/xemu.app/Contents/Info.plist

    # Ad-hoc signing for local builds. MAP_JIT / TCG works under ad-hoc
    # without hardened runtime, so we don't need to apply the
    # `allow-jit` entitlement here. `--deep` re-signs all bundled
    # dylibs (SDL3, MoltenVK, etc.) with the same ad-hoc identity as
    # the main binary so dyld's same-team-ID check is satisfied.
    # `--preserve-metadata=entitlements,...` is a no-op on a fresh
    # build and preserves anything upstream may have embedded.
    #
    # For notarized/distributable builds, use `scripts/sign-macos-
    # release.sh`, which enables the hardened runtime and applies
    # `xemu.entitlements` (allow-jit + allow-unsigned-executable-memory)
    # consistently across every binary in the bundle.
    #
    # Opt-in via XEMU_CODESIGN_ENTITLEMENTS=1 if you specifically need
    # hardened-runtime behavior locally (rare).
    if [[ "${XEMU_CODESIGN_ENTITLEMENTS:-0}" == "1" \
          && -f "${project_source_dir}/xemu.entitlements" ]]; then
      # Re-sign every bundled dylib with hardened runtime + entitlements
      # first so they have a matching signing policy to the main exe.
      # Without this, dyld rejects the main binary with
      # "mapping process and mapped file have different Team IDs".
      find dist/xemu.app/Contents/Libraries -name '*.dylib' -print0 | \
        while IFS= read -r -d '' dyl; do
          codesign --force --sign - \
                   --entitlements "${project_source_dir}/xemu.entitlements" \
                   --options runtime "${dyl}"
        done
      codesign --force --deep --sign - \
               --entitlements "${project_source_dir}/xemu.entitlements" \
               --options runtime "${exe_path}"
    else
      codesign --force --deep \
               --preserve-metadata=entitlements,requirements,flags,runtime \
               --sign - "${exe_path}"
    fi
    python3 ./scripts/gen-license.py --version-file=macos-libs/$target_arch/INSTALLED > dist/LICENSE.txt
}

package_linux() {
    rm -rf dist
    mkdir -p dist
    cp build/qemu-system-i386 dist/xemu
    if test -e "${project_source_dir}/XEMU_LICENSE"; then
      cp "${project_source_dir}/XEMU_LICENSE" dist/LICENSE.txt
    else
      python3 ./scripts/gen-license.py > dist/LICENSE.txt
    fi
}

postbuild=''
debug_opts=''
build_cflags=''
default_job_count='12'
sys_ldflags=''

get_job_count () {
	if command -v 'nproc' >/dev/null
	then
		nproc
	else
		case "$(uname -s)" in
			'Linux')
				egrep "^processor" /proc/cpuinfo | wc -l
				;;
			'FreeBSD')
				sysctl -n hw.ncpu
				;;
			'Darwin')
				sysctl -n hw.logicalcpu 2>/dev/null \
				|| sysctl -n hw.ncpu
				;;
			'MSYS_NT-'*|'CYGWIN_NT-'*|'MINGW'*'_NT-'*)
				if command -v 'wmic' >/dev/null
				then
					wmic cpu get NumberOfLogicalProcessors/Format:List \
						| grep -m1 '=' | cut -f2 -d'='
				else
					echo "${NUMBER_OF_PROCESSORS:-${default_job_count}}"
				fi
				;;
			*)
				echo "${default_job_count}"
				;;
		esac
	fi
}

job_count="$(get_job_count)" 2>/dev/null
job_count="${job_count:-${default_job_count}}"
debug=""
opts=""
platform="$(uname -s)"

while [ ! -z "${1}" ]
do
    case "${1}" in
    '-j'*)
        job_count="${1:2}"
        shift
        ;;
    '--debug')
        debug="y"
        shift
        ;;
    '-p'*)
        platform="${2}"
        shift 2
        ;;
    '-a'*)
        target_arch="${2}"
        shift 2
        ;;
    *)
        break
        ;;
    esac
done

target="qemu-system-i386"
if test ! -z "$debug"; then
    build_cflags='-DXEMU_DEBUG_BUILD=1'
    opts="--enable-debug --enable-trace-backends=log"
else
    case "$platform" in
    win64*|MINGW*|MSYS*)
        # Don't force LTO on Windows targets. The CI workflows own
        # the per-toolchain LTO strategy there: x86_64 uses GCC
        # (b_lto_mode=thin is a meson setup error on GCC; the
        # workflow passes -flto-incremental itself) and arm64
        # llvm-mingw ThinLTO breaks the qemu_build_not_reached_always
        # elision (see the FIXME in build-windows.yml) — with LTO
        # forced here, the arm64 release link failed exactly that
        # way. The LTO-independent knobs are kept.
        opts="$opts -Dqom_cast_debug=false -Dtrace_backends=nop"
        ;;
    *)
        opts="$opts -Db_lto=true -Db_lto_mode=thin -Db_thinlto_cache=true -Db_thinlto_cache_dir=.lto-cache"
        opts="$opts -Doptimization=3 -Dqom_cast_debug=false"
        opts="$opts -Dtrace_backends=nop -Dstack_protector=disabled"
        ;;
    esac
fi

most_recent_macosx_sdk_ver () {
  local min_ver="${1}"
  local macos_sdk_base=/Library/Developer/CommandLineTools/SDKs
  local sdks=("${macos_sdk_base}"/MacOSX[0-9]*.[0-9]*.sdk)
  for i in "${!sdks[@]}"; do
    local newval="${sdks[i]##${macos_sdk_base}/MacOSX}"
    sdks[$i]="${newval%%.sdk}"
  done

  IFS=$'\n' sdks=($(sort -nr <<<"${sdks[*]}"))
  unset IFS

  local newest_sdk_ver="${sdks[0]}"

  local sdk_path="${macos_sdk_base}/MacOSX${newest_sdk_ver}.sdk"
  if ! test -d "${sdk_path}"; then
    echo ""
    return
  fi

  if ! LC_ALL=C awk 'BEGIN {exit ('${newest_sdk_ver}' < '${min_ver}')}'; then
    echo ""
    return
  fi
  echo "${sdk_path}"
}

case "$platform" in # Adjust compilation options based on platform
    Linux)
        echo 'Compiling for Linux...'
        sys_cflags='-Wno-error=redundant-decls'
        opts="$opts --disable-werror"
        profdata_prefix=""
        setup_pgo
        postbuild='package_linux'
        ;;
    Darwin)
        echo "Compiling for MacOS for $target_arch..."
        # A pre-existing build/ configured for a different arch is
        # silently reused by configure (no wipe outside its marker
        # window), and its stale config-meson.cross -arch wins over
        # the exported flags — an '-a x86_64' run then "succeeds"
        # with a pure arm64 binary (hit 2026-07-05). Force a clean
        # configure on arch change.
        if [ -f build/config-meson.cross ] && \
           ! grep -q "'${target_arch}'" build/config-meson.cross; then
            echo "build/ was configured for a different arch; cleaning for ${target_arch}"
            rm -rf build
        fi
        if [ "$target_arch" == "arm64" ]; then
            macos_min_ver=14.0
        elif [ "$target_arch" == "x86_64" ]; then
            macos_min_ver=12.7.5
        else
            echo "Unsupported arch $target_arch"
            exit 1
        fi

        sdk="$(most_recent_macosx_sdk_ver ${macos_min_ver})"
        if [[ -z "${sdk}" ]]; then
          echo "SDK >= ${macos_min_ver} not found. Install Xcode Command Line Tools"
          exit 1
        fi

        python3 ./scripts/download-macos-libs.py ${target_arch}
        lib_prefix=${PWD}/macos-libs/${target_arch}/opt/local

        # Vulkan (MoltenVK): the renderer defaults to Vulkan, so a
        # usable libMoltenVK.dylib is required. Prefer deliberate
        # system installs (tier walk + arch rule in resolve_moltenvk
        # at the top of this script); when none exists, vendor the
        # pinned official release into macos-libs so a fresh clone
        # works out of the box.
        if ! resolve_moltenvk >/dev/null; then
            vendor_moltenvk dylib
        fi
        # Headers can still be missing when the dylib came from a
        # system location without development headers (e.g. bare
        # /usr/local install). Vendor just the headers in that case.
        if [ ! -f "${lib_prefix}/include/vulkan/vulkan.h" ] && \
           [ ! -f /opt/homebrew/include/vulkan/vulkan.h ] && \
           [ ! -f /usr/local/include/vulkan/vulkan.h ]; then
            vendor_moltenvk headers
        fi
        export CFLAGS="${CFLAGS} \
                       -arch ${target_arch} \
                       -target ${target_arch}-apple-macos${macos_min_ver} \
                       -isysroot ${sdk} \
                       -I${lib_prefix}/include \
                       -I/opt/homebrew/include \
                       -mmacosx-version-min=$macos_min_ver"
        # C++ and ObjC need the same target selection. Without
        # CXXFLAGS, cross-arch builds (x86_64 on an arm64 runner)
        # compile C++ objects for the host arch but link with
        # -arch from LDFLAGS — the CMake subprojects' (glslang)
        # try-compile fails with an architecture mismatch.
        export CXXFLAGS="${CXXFLAGS} \
                       -arch ${target_arch} \
                       -target ${target_arch}-apple-macos${macos_min_ver} \
                       -isysroot ${sdk} \
                       -mmacosx-version-min=$macos_min_ver"
        export OBJCFLAGS="${OBJCFLAGS} \
                       -arch ${target_arch} \
                       -target ${target_arch}-apple-macos${macos_min_ver} \
                       -isysroot ${sdk} \
                       -mmacosx-version-min=$macos_min_ver"
        export LDFLAGS="${LDFLAGS} \
                        -arch ${target_arch} \
                        -isysroot ${sdk} \
                        -mmacosx-version-min=$macos_min_ver"
        if [ "$target_arch" == "arm64" ]; then
            # ARM64 -mcpu selection:
            #   - XEMU_ARM_CPU overrides everything (e.g. `apple-m4`).
            #   - Otherwise, auto-detect the host chip via sysctl and pick
            #     the matching `-mcpu=apple-mN`. This produces a binary
            #     tuned for the build host, which is almost always what a
            #     personal build wants.
            #   - If clang doesn't recognize the newest chip yet
            #     (e.g. sysctl reports "Apple M6" but the bundled clang
            #     tops out at apple-m4), walk down the list to the
            #     nearest known-good CPU rather than dropping all the
            #     way to apple-m1 — preserves most of the tuning win.
            #
            # `-mcpu=native` is avoided because clang's "native" on
            # Apple Silicon bakes in implementation quirks and isn't
            # guaranteed stable across toolchain versions.

            # Verify a -mcpu= argument is accepted by the toolchain.
            probe_mcpu() {
              echo 'int main(void){return 0;}' | \
                "${CC:-clang}" -x c - -mcpu="$1" \
                -o /dev/null -target arm64-apple-macos >/dev/null 2>&1
            }

            if [ -n "${XEMU_ARM_CPU}" ]; then
              arm_cpu="${XEMU_ARM_CPU}"
              if ! probe_mcpu "${arm_cpu}"; then
                echo "Warning: clang doesn't recognize -mcpu=${arm_cpu} " \
                     "(from XEMU_ARM_CPU); falling back to apple-m1."
                arm_cpu='apple-m1'
              fi
              echo "ARM64 -mcpu=${arm_cpu} (from XEMU_ARM_CPU)"
            else
              brand="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
              # Extract the M-number; default to 1 on non-Apple-Silicon
              # hosts (cross-compile, Rosetta etc.).
              m_num="$(echo "${brand}" | \
                       sed -n 's/.*Apple M\([0-9][0-9]*\).*/\1/p')"
              if [ -z "${m_num}" ]; then
                m_num=1
              fi
              # Try apple-mN, then apple-m(N-1), ... down to apple-m1.
              arm_cpu=''
              candidate_n="${m_num}"
              while [ "${candidate_n}" -ge 1 ]; do
                cand="apple-m${candidate_n}"
                if probe_mcpu "${cand}"; then
                  arm_cpu="${cand}"
                  break
                fi
                candidate_n=$((candidate_n - 1))
              done
              if [ -z "${arm_cpu}" ]; then
                # Shouldn't happen — every modern clang knows apple-m1 —
                # but guard against it anyway.
                arm_cpu='apple-m1'
                echo "Warning: clang rejected every apple-mN candidate; " \
                     "forcing apple-m1."
              elif [ "${candidate_n}" -lt "${m_num}" ]; then
                echo "Note: clang tops out below Apple M${m_num}; " \
                     "using -mcpu=${arm_cpu}."
              fi
              echo "ARM64 -mcpu=${arm_cpu} (auto-detected from '${brand}')"
            fi
            sys_cflags="-mcpu=${arm_cpu} -ffp-contract=fast"
        fi

        profdata_prefix="xcrun "
        setup_pgo

        sys_ldflags="${sys_ldflags:-}${sys_ldflags:+ }-headerpad_max_install_names"

        # Optional binary-size / LTO passes. Off by default because
        # QEMU uses __attribute__((constructor)) for type_init /
        # module_init; macOS ld's -dead_strip keeps constructors by
        # default but can prune sibling static helpers whose only
        # reference is via the constructor, so validate before baking
        # in. Opt in with XEMU_STRIP=1.
        #
        # -fvisibility=hidden alone is a smaller hammer: hides
        # undeclared exports and gives LTO more freedom to inline /
        # remove internal-only functions across the binary. QEMU's
        # external-facing APIs (HS_DEF_HELPER_*, ImGui IMGUI_API,
        # glslang public API) already tag themselves default; verified
        # against this tree before landing. Opt in with XEMU_VIS=1.
        #
        # Binary size impact observed on this fork: XEMU_VIS=1 alone
        # reduces arm64 qemu-system-i386-unsigned by ~1.5 MiB (mostly
        # debug name strings from TCG helpers). Combined XEMU_VIS=1 +
        # XEMU_STRIP=1 reduces ~4 MiB. Runtime perf delta is within
        # noise on measurements so far; the main benefit is faster
        # link time and cleaner LTO.
        if [ "${XEMU_VIS:-0}" = "1" ]; then
          sys_cflags="${sys_cflags} -fvisibility=hidden"
          echo "Visibility: hidden default (XEMU_VIS=1)"
        fi
        if [ "${XEMU_STRIP:-0}" = "1" ]; then
          sys_ldflags="${sys_ldflags:-}${sys_ldflags:+ }-Wl,-dead_strip"
          echo "Linker: -dead_strip enabled (XEMU_STRIP=1)"
        fi
        export PKG_CONFIG_LIBDIR="${lib_prefix}/lib/pkgconfig"
        opts="$opts --disable-cocoa --cross-prefix="
        postbuild='package_macos'
        ;;
    CYGWIN*|MINGW*|MSYS*)
        echo 'Compiling for Windows...'
        sys_cflags='-Wno-error'
        CFLAGS="${CFLAGS} -lIphlpapi -lCrypt32" # workaround for linking libs on mingw
        # Match the CI release configuration: x86-64-v3 (AVX2 / BMI2 /
        # FMA) on release builds unless the user already passed an
        # x86_version. meson's default is v1 (baseline x86-64), which
        # leaves measurable performance on the table on any CPU from
        # the last decade. Override with: ./build.sh -- -Dx86_version=1
        if [ -z "$debug" ] && ! echo "$@" | grep -q 'x86_version'; then
          opts="$opts -Dx86_version=3"
        fi
        # PGO: same two-stage flow as Darwin/Linux (MSYS2 clang accepts
        # the same flags; llvm-profdata without the xcrun prefix). The
        # shared setup_pgo also fixes this branch's former stale-merge
        # behavior (it only re-merged when profdata was absent).
        profdata_prefix=""
        setup_pgo
        postbuild='package_windows' # set the above function to be called after build
        target="qemu-system-i386w.exe"
        ;;
    win64-cross)
        echo 'Cross-compiling for Windows...'
        export AR=${AR:-$CROSSAR}
        sys_cflags='-Wno-error'
        opts="$opts --cross-prefix=$CROSSPREFIX --static"
        postbuild='package_wincross' # set the above function to be called after build
        target="qemu-system-i386w.exe"
        ;;
    *)
        echo "Unsupported platform $platform, aborting" >&2
        exit 1
        ;;
esac

# find absolute path (and resolve symlinks) to build out of tree
configure="${project_source_dir}/configure"

set -x # Print commands from now on

"${configure}" \
    --extra-cflags="-DXBOX=1 ${build_cflags} ${sys_cflags} ${CFLAGS}" \
    --extra-ldflags="${sys_ldflags}" \
    --target-list=i386-softmmu \
    ${opts} \
    "$@"

time make -j"${job_count}" ${target} 2>&1 | tee build.log

"${postbuild}" # call post build functions
