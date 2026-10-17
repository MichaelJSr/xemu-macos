test_nv2a_pfifo_frozen = executable(
  'test-nv2a-pfifo-frozen',
  files('test-nv2a-pfifo-frozen.c'),
  dependencies: [qemuutil, glib],
  build_by_default: false,
)

pfifo_test_env = environment()
if host_os == 'darwin'
  # As tests/xbox/dsp: the vendored dylibs' /opt/local install names.
  macos_arch = host_machine.cpu() == 'aarch64' ? 'arm64' : host_machine.cpu()
  pfifo_test_env.prepend('DYLD_FALLBACK_LIBRARY_PATH',
                         meson.project_source_root() / 'macos-libs' /
                         macos_arch / 'opt/local/lib')
endif

test('test-nv2a-pfifo-frozen', test_nv2a_pfifo_frozen,
     args: ['--tap', '-k'],
     protocol: 'tap',
     env: pfifo_test_env,
     suite: ['xbox'])

xbox_test_exes += test_nv2a_pfifo_frozen
