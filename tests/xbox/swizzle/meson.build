# Swizzle round-trip + cross-method differential over a
# w/h/d/bpp/pitch matrix — guards the texture unswizzle (morton-square)
# path in hw/xbox/nv2a/pgraph/swizzle.c.
#
# The upstream Makefile renamed swizzle_box/unswizzle_box to *_A with
# `objcopy --redefine-sym` so several method implementations could be
# linked side by side. objcopy is not portable (absent on macOS), so we
# do the same rename purely in the preprocessor: the MULTIVERSION macro
# in swizzle.c pastes `##_internal`, so both the public entry points and
# their internal helpers must be renamed for the token paste to resolve.
# -UNDEBUG forces the test's assert()-based checks on even in a release
# (NDEBUG) build, so a mismatch can never silently pass.

swizzle_rename_args = [
  '-Dswizzle_box=swizzle_box_A',
  '-Dunswizzle_box=unswizzle_box_A',
  '-Dswizzle_box_internal=swizzle_box_A_internal',
  '-Dunswizzle_box_internal=unswizzle_box_A_internal',
  '-UNDEBUG',
]

swizzle_method_a = static_library(
  'xbox-swizzle-method-a',
  sources: files('../../../hw/xbox/nv2a/pgraph/swizzle.c'),
  c_args: swizzle_rename_args,
)

exe = executable('test-xbox-nv2a-swizzle',
                 sources: files('swizzle-test.c'),
                 c_args: ['-UNDEBUG'],
                 link_with: swizzle_method_a)

test('xbox-nv2a-swizzle', exe,
     suite: ['xbox', 'xbox-nv2a', 'xbox-nv2a-swizzle'])

xbox_test_exes += exe
