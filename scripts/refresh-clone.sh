#!/bin/zsh
# Put a freshly built xemu binary into a bundle clone (a bare cp dies in dyld).
#
#   scripts/refresh-clone.sh <dst.app> <binary> [--from <src.app>]
#
# With --from, dst is first made a fresh cp -Rc clone of src (an existing dst
# is replaced). The binary's MacPorts dependencies are re-pointed into the
# bundle's Contents/Libraries/<arch>, its rpaths reset to that folder, and the
# bundle re-signed ad hoc. Never touches dist/xemu.app (the owner's bundle) and
# never runs the binary. Prints the binary's sha256[:12].
set -e
ROOT=${0:A:h:h}

usage() { print -u2 "usage: refresh-clone.sh <dst.app> <binary> [--from <src.app>]"; exit 2 }
(( $# == 2 || $# == 4 )) || usage
dst=$1 bin=$2 src=
if (( $# == 4 )); then
  [[ $3 == --from ]] || usage
  src=$4
fi
[[ -f $bin ]] || { print -u2 "refresh-clone: no binary at $bin"; exit 1 }
[[ ${dst:A} == ${ROOT}/dist(|/*) ]] && { print -u2 "refresh-clone: refusing $dst (dist/ is the owner's)"; exit 1 }

if [[ -n $src ]]; then
  [[ -d $src/Contents ]] || { print -u2 "refresh-clone: $src is not a bundle"; exit 1 }
  rm -rf $dst
  cp -Rc $src $dst
fi
[[ -d $dst/Contents/MacOS ]] || { print -u2 "refresh-clone: $dst is not a bundle (use --from)"; exit 1 }

arch=$(ls $dst/Contents/Libraries 2>/dev/null | head -1)
[[ -n $arch ]] || { print -u2 "refresh-clone: $dst has no Contents/Libraries/<arch>"; exit 1 }
if ! lipo -archs $bin | tr ' ' '\n' | grep -qx $arch; then
  print -u2 "refresh-clone: $bin is $(lipo -archs $bin), the bundle is $arch; an x86_64 bundle comes from ./build.sh -a x86_64"
  exit 1
fi

B=$dst/Contents/MacOS/xemu L=@executable_path/../Libraries/$arch
cp $bin $B && xattr -c $B
otool -L $B | awk '/\/opt\/local\//{print $1}' | while read d; do install_name_tool -change $d $L/${d:t} $B; done
otool -l $B | awk '/LC_RPATH/{f=1} f&&/path /{print $2;f=0}' | while read r; do install_name_tool -delete_rpath $r $B 2>/dev/null; done
install_name_tool -add_rpath $L/ $B
codesign -f --deep --preserve-metadata=entitlements,requirements,flags,runtime -s - $B 2>&1 | grep -v "replacing existing signature" || true
print "refresh-clone: $dst ($(shasum -a 256 $B | cut -c1-12))"
