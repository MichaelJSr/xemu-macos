xbox_dsp_env = {
  'G_TEST_SRCDIR': meson.current_source_dir(),
  'G_TEST_BUILDDIR': meson.current_build_dir(),
}

# On macOS the vendored MacPorts dylibs (macos-libs/<arch>/opt/local/lib)
# carry absolute /opt/local/... install names. package_macos rewrites them
# for the app bundle, but test binaries run straight from the build tree,
# where those paths don't exist. DYLD_FALLBACK_LIBRARY_PATH is consulted
# by dyld exactly when an absolute install name fails to resolve, so point
# it at the vendored lib dir. Ad-hoc/linker-signed local binaries are not
# "restricted", so the variable is honored.
if host_os == 'darwin'
  macos_libs_arch = host_machine.cpu_family() == 'aarch64' ? 'arm64' \
                                                           : host_machine.cpu_family()
  xbox_dsp_env += {
    'DYLD_FALLBACK_LIBRARY_PATH':
      meson.project_source_root() / 'macos-libs' / macos_libs_arch
      / 'opt' / 'local' / 'lib',
  }
endif

xbox_dsp_suite = ['xbox', 'xbox-mcpx', 'xbox-mcpx-dsp']

exe = executable('test-xbox-mcpx-dsp',
                 sources: files('test-dsp.c'),
                 dependencies: [qemuutil, dsp, glib])

test('xbox-mcpx-dsp', exe,
     env: environment(xbox_dsp_env),
     args: ['--tap', '-k'],
     protocol: 'tap',
     suite: xbox_dsp_suite)

xbox_test_exes += exe

# Interpreter/JIT differential corpus (see test-dsp-jit-diff.c). The
# corpus itself is engine-independent; XEMU_DSP_JIT selects the engine
# per PROCESS (parsed once), which is why each arm is its own test()
# entry rather than a runtime toggle.
diff_exe = executable('test-xbox-mcpx-dsp-jit-diff',
                      sources: files('test-dsp-jit-diff.c'),
                      dependencies: [qemuutil, dsp, glib])

xbox_test_exes += diff_exe

# Arm 1: pure-interpreter golden regression. Runs on every platform.
test('xbox-mcpx-dsp-corpus-interp', diff_exe,
     env: environment(xbox_dsp_env + {'XEMU_DSP_JIT': '0'}),
     args: ['--tap', '-k', '-p', '/corpus'],
     protocol: 'tap',
     suite: xbox_dsp_suite)

# The inline DSP56300 JIT exists on aarch64 POSIX hosts only
# (DSP56K_JIT_SUPPORTED, hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h);
# the JIT arms are meaningless elsewhere.
if host_machine.cpu_family() == 'aarch64' and host_os != 'windows'
  # Arm 2: same goldens through the JIT, plus a translated-blocks-
  # executed assertion so translator bail-out can't false-green it.
  test('xbox-mcpx-dsp-corpus-jit', diff_exe,
       env: environment(xbox_dsp_env + {'XEMU_DSP_JIT': '1'}),
       args: ['--tap', '-k', '-p', '/corpus'],
       protocol: 'tap',
       suite: xbox_dsp_suite)

  # Arm 3: in-engine per-block bit-exact validator (sync mode): every
  # translated block is replayed through the interpreter and the full
  # dsp_core_t semantic state compared; any divergence aborts.
  test('xbox-mcpx-dsp-jit-diff-sync', diff_exe,
       env: environment(xbox_dsp_env + {
         'XEMU_DSP_JIT': '1',
         'XEMU_DSP_JIT_DIFF': '1',
         'XEMU_DSP_JIT_DIFF_SYNC': '1',
       }),
       args: ['--tap', '-k', '-p', '/corpus'],
       protocol: 'tap',
       suite: xbox_dsp_suite)

  # Arm 4: cross-process differential — runs each corpus program in an
  # interpreter child and a JIT child and byte-compares the final
  # DspCoreState (registers, stacks, all memories, loop state).
  test('xbox-mcpx-dsp-jit-differential', diff_exe,
       env: environment(xbox_dsp_env),
       args: ['--tap', '-k', '-p', '/jit-differential'],
       protocol: 'tap',
       suite: xbox_dsp_suite)
endif
