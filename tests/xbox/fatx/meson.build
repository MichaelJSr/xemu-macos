test_xbox_fatx = executable(
  'test-xbox-fatx',
  files('test-fatx.c'),
  dependencies: [qemuutil, fatx_full],
  build_by_default: false,
)

fatx_test_env = environment()
if host_os == 'darwin'
  # As tests/xbox/dsp: the vendored dylibs' /opt/local install names.
  macos_arch = host_machine.cpu() == 'aarch64' ? 'arm64' : host_machine.cpu()
  fatx_test_env.prepend('DYLD_FALLBACK_LIBRARY_PATH',
                        meson.project_source_root() / 'macos-libs' /
                        macos_arch / 'opt/local/lib')
endif

test('test-xbox-fatx', test_xbox_fatx,
     args: ['--tap', '-k'],
     protocol: 'tap',
     env: fatx_test_env,
     suite: ['xbox'])

xbox_test_exes += test_xbox_fatx
