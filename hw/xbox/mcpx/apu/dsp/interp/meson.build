# Interpreter core plus its inline JITs.
#
# dsp56k_jit_arm64.c is the hand-written AArch64 emitter; it compiles to
# nothing outside aarch64 POSIX hosts (see DSP56K_JIT_SUPPORTED in
# dsp56k_jit_arm64.h) — Linux arm64 runs it too, and CI tests it there.
#
# dsp56k_jit_llvm.c is the portable LLVM backend implementing the same API.
# It is built only where the AArch64 emitter is not (they define the same
# symbols) and only when libLLVM + its C API are found; otherwise it is
# left out and the DSP falls back to the interpreter / external Rust JIT.
#
# dsp_llvm_dep carries the LLVM include/link flags *and* the
# -DDSP56K_JIT_LLVM=1 define. The define must reach every TU that includes
# dsp56k_jit_arm64.h (dsp_c.c, dsp.c, gp_ep.c, the DSP tests): it is what
# makes DSP56K_JIT_ANY_SUPPORTED true there, selecting the real API over
# the header's no-op stubs. Putting it in the dependency rather than in one
# target's c_args is what propagates it everywhere.
libdsp_cpu_interp_sources = ['dsp_cpu.c', 'dsp56k_jit_arm64.c', 'debug.c']
dsp_llvm_dep = declare_dependency()

if host_machine.cpu_family() != 'aarch64' or host_os == 'windows'
  # llvm-config spells out the include flags, defines and link libraries
  # (a single -lLLVM-<major> on shared builds). The unversioned name is
  # what distro packages provide; llvm-config-mp-22 is MacPorts, whose
  # suffixed name meson's own LLVM dependency finder does not probe.
  llvm_config = find_program(
    'llvm-config',
    'llvm-config-mp-22',
    'llvm-config-22', 'llvm-config-21', 'llvm-config-20', 'llvm-config-19',
    'llvm-config-18', 'llvm-config-17', 'llvm-config-16', 'llvm-config-15',
    required: false,
  )
  if llvm_config.found()
    llvm_cflags = run_command(
      llvm_config, '--cflags', check: true).stdout().strip().split()
    llvm_ldflags = run_command(
      llvm_config, '--ldflags', '--libs', 'core', 'orcjit', 'native',
      '--system-libs', check: true).stdout().strip().split()
    dsp_llvm_dep = declare_dependency(
      compile_args: llvm_cflags + ['-DDSP56K_JIT_LLVM=1'],
      link_args: llvm_ldflags,
    )
    libdsp_cpu_interp_sources += ['dsp56k_jit_llvm.c']
  endif
endif

libdsp_cpu_interp = static_library(
  'dsp_cpu_interp',
  files(libdsp_cpu_interp_sources) + genh,
  include_directories: include_directories('..'),
  dependencies: dsp_llvm_dep,
)
