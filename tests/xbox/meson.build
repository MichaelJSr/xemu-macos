# On-target, game-content-free unit tests for xemu's Xbox device models.
# Each subdir appends its test executable(s) to xbox_test_exes; the
# `test-xbox` alias below builds exactly those (and nothing else), so CI
# can `meson compile test-xbox` then `meson test --suite xbox` without
# building the whole tree.
xbox_test_exes = []

subdir('dsp')
subdir('swizzle')

alias_target('test-xbox', xbox_test_exes)
