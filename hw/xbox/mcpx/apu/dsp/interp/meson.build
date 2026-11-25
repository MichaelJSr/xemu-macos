# Interpreter core plus its inline JITs.
#
# dsp56k_jit_arm64.c is the hand-written AArch64 emitter; it compiles to
# nothing outside aarch64 POSIX hosts (see DSP56K_JIT_SUPPORTED in
# dsp56k_jit_arm64.h) — Linux arm64 runs it too, and CI tests it there.
#
# dsp56k_jit_llvm.c is the portable LLVM backend implementing the same API.
# It is opt-in (-Ddsp_llvm_jit=enabled): Phase 1 wraps the interpreter and
# is no faster, so default builds neither probe for llvm-config nor link
# libLLVM. It is built only where the AArch64 emitter is not (they define
# the same symbols), and needs LLVM 21 or newer.
#
# dsp_llvm_dep carries the LLVM include/link flags *and* the
# -DDSP56K_JIT_LLVM=1 define. The define must reach every TU that includes
# dsp56k_jit_arm64.h (dsp_c.c, dsp.c, gp_ep.c, the DSP tests): it is what
# makes DSP56K_JIT_ANY_SUPPORTED true there, selecting the real API over
# the header's no-op stubs. Putting it in the dependency rather than in one
# target's c_args is what propagates it everywhere. dsp_llvm_jit tells
# tests/xbox/dsp whether the backend is built.
libdsp_cpu_interp_sources = ['dsp_cpu.c', 'dsp56k_jit_arm64.c', 'debug.c']
dsp_llvm_dep = declare_dependency()
dsp_llvm_jit = false

dsp_llvm_opt = get_option('dsp_llvm_jit').require(
  host_machine.cpu_family() != 'aarch64' or host_os == 'windows',
  error_message: 'aarch64 POSIX hosts build the AArch64 DSP JIT instead')

if dsp_llvm_opt.allowed()
  llvm_dep = dependency('', required: false)
  if meson.is_cross_build()
    # PATH's llvm-config targets the build machine. Meson's config-tool
    # lookup takes a cross build's llvm-config from the cross file or
    # LLVM_CONFIG only.
    llvm_dep = dependency('llvm', method: 'config-tool', version: '>=21',
                          modules: ['core', 'orcjit', 'native'],
                          required: false)
  else
    # find_program checks the version of the first name it finds only, so
    # newest names go first and the unversioned one last. LLVM_CONFIG or a
    # [binaries] llvm-config entry overrides the list. Meson's own LLVM
    # lookup probes neither -21..-23 nor MacPorts' llvm-config-mp-2x.
    llvm_config = find_program(
      'llvm-config-mp-23', 'llvm-config-23',
      'llvm-config-mp-22', 'llvm-config-22', 'llvm-config-21', 'llvm-config',
      version: '>=21',
      required: false,
    )
    if llvm_config.found()
      llvm_cflags = run_command(
        llvm_config, '--cflags', check: true).stdout().strip().split()
      llvm_ldflags = run_command(
        llvm_config, '--ldflags', '--libs', 'core', 'orcjit', 'native',
        '--system-libs', check: true).stdout().strip().split()
      llvm_dep = declare_dependency(
        compile_args: llvm_cflags,
        link_args: llvm_ldflags,
      )
    endif
  endif

  # LLVM 21 added this C API; the check also catches missing headers.
  if llvm_dep.found() and cc.has_header_symbol(
      'llvm-c/Orc.h', 'LLVMOrcCreateNewThreadSafeContextFromLLVMContext',
      dependencies: llvm_dep)
    dsp_llvm_dep = declare_dependency(
      compile_args: ['-DDSP56K_JIT_LLVM=1'],
      dependencies: llvm_dep,
    )
    libdsp_cpu_interp_sources += ['dsp56k_jit_llvm.c']
    dsp_llvm_jit = true
  elif dsp_llvm_opt.enabled()
    error('dsp_llvm_jit needs llvm-config and the llvm-c headers from ' +
          'LLVM 21 or newer; LLVM_CONFIG or a [binaries] llvm-config ' +
          'entry selects a specific llvm-config' +
          (meson.is_cross_build() ? ' (cross builds: PATH is not searched)'
                                  : ''))
  endif
endif

libdsp_cpu_interp = static_library(
  'dsp_cpu_interp',
  files(libdsp_cpu_interp_sources) + genh,
  include_directories: include_directories('..'),
  dependencies: dsp_llvm_dep,
)
