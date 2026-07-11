test_env = environment()
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())

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
  test_env.set('DYLD_FALLBACK_LIBRARY_PATH',
               meson.project_source_root() / 'macos-libs' / macos_libs_arch
               / 'opt' / 'local' / 'lib')
endif

exe = executable('test-xbox-mcpx-dsp',
                 sources: files('test-dsp.c'),
                 dependencies: [qemuutil, dsp, glib])

test('xbox-mcpx-dsp', exe,
     env: test_env,
     args: ['--tap', '-k'],
     protocol: 'tap',
     suite: ['xbox', 'xbox-mcpx', 'xbox-mcpx-dsp'])

xbox_test_exes += exe
