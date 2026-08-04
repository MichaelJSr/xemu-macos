#!/bin/bash
# Build and install the fork's pinned, optimized MoltenVK.
#
#   scripts/build-moltenvk.sh [checkout-dir]
#
# The macOS app bundles whichever libMoltenVK.dylib build.sh finds
# first (macos-libs vendored -> /usr/local -> Homebrew). This fork
# maintains a custom build installed at /usr/local/lib so every
# local build ships a known, optimized driver instead of whatever
# the vendor fetch or Homebrew happens to serve. build.sh logs the
# bundled version+UUID; this script is the only supported way to
# update the installed copy.
#
# Pin: bump MVK_PIN deliberately and re-run the validation gauntlet
# (savestate fps A/B + artifact hunt) before releasing with it.
# Flags: -O3 and a per-machine -mcpu (override with XEMU_MVK_MCPU,
# e.g. apple-m1). LTO is intentionally NOT used: thin-LTO bitcode in
# the static archives breaks the ShaderConverter xcframework
# packaging step, and measured no fps benefit over -O3 alone.
set -euo pipefail

# v1.4.2 final. Was v1.4.2-rc1 (98f3574346) from 2026-07-19 until the
# final tag shipped: every local soak/A/B ran the rc while releases
# vendored stock 1.4.1, so nothing validated the driver users load.
# Both ends now name 1.4.2 (build.sh's XEMU_MOLTENVK_VERSION default is
# the vendored side, and macOS CI asserts the bundled version matches).
MVK_PIN=db66022459ffb663aa2b50f6b018bc2e124f5edf   # v1.4.2, 2026-08-04 (reports 1.4.2)
MVK_DIR="${1:-$(dirname "$0")/../../MoltenVK}"
MCPU="${XEMU_MVK_MCPU:-apple-m2}"

if [ ! -d "$MVK_DIR/.git" ]; then
  echo "error: MoltenVK checkout not found at $MVK_DIR" >&2
  echo "  git clone https://github.com/KhronosGroup/MoltenVK.git \"$MVK_DIR\"" >&2
  exit 1
fi

cd "$MVK_DIR"
echo "== MoltenVK checkout: $MVK_DIR"
git fetch origin --quiet
git checkout --quiet "$MVK_PIN"
echo "== pinned at: $(git log --oneline -1)"

echo "== fetching dependencies (fast when unchanged)..."
./fetchDependencies --macos > /tmp/mvk-fetch.log 2>&1 || {
  tail -5 /tmp/mvk-fetch.log >&2
  echo "error: fetchDependencies failed (see /tmp/mvk-fetch.log)" >&2
  echo "hint: a broken Xcode install manifests here; try: xcodebuild -runFirstLaunch" >&2
  exit 1
}

echo "== building Release (arm64, -O3, -mcpu=${MCPU})..."
xcodebuild build \
  -project MoltenVKPackaging.xcodeproj \
  -scheme "MoltenVK Package (macOS only)" \
  -destination "generic/platform=macOS" \
  -configuration Release \
  ARCHS=arm64 \
  GCC_OPTIMIZATION_LEVEL=3 \
  OTHER_CFLAGS="-mcpu=${MCPU}" \
  -quiet > /tmp/mvk-build.log 2>&1 || {
  tail -15 /tmp/mvk-build.log >&2
  echo "error: xcodebuild failed (see /tmp/mvk-build.log)" >&2
  exit 1
}

# Current MoltenVK main packages a framework; the binary inside is
# the dylib. Older layouts shipped Package/Release/.../dylib/macOS.
BIN="Package/Release/MoltenVK/dynamic/MoltenVK.xcframework/macos-arm64/MoltenVK.framework/Versions/A/MoltenVK"
[ -f "$BIN" ] || BIN="Package/Release/MoltenVK/dylib/macOS/libMoltenVK.dylib"
[ -f "$BIN" ] || { echo "error: built dylib not found" >&2; exit 1; }

DST=/usr/local/lib/libMoltenVK.dylib
cp "$BIN" "${DST}.new"
install_name_tool -id "@rpath/libMoltenVK.dylib" "${DST}.new"
codesign -s - -f "${DST}.new" 2>/dev/null
mv "${DST}.new" "$DST"

VER=$({ strings "$DST" 2>/dev/null || true; } | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
[ -n "$VER" ] || VER=unknown
UUID=$(dwarfdump --uuid "$DST" | grep -m1 arm64 | awk '{print $2}')
echo "== installed $DST"
echo "== MoltenVK ${VER}, pin ${MVK_PIN:0:9}, UUID ${UUID}"
echo "== next: ./build.sh   (watch for the 'Bundling MoltenVK' provenance line)"
