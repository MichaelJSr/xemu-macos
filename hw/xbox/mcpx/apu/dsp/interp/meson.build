libdsp_cpu_interp = static_library(
  'dsp_cpu_interp',
  # dsp56k_jit_arm64.c is the inline basic-block JIT for this
  # interpreter core; it compiles to nothing outside Apple Silicon
  # (see DSP56K_JIT_SUPPORTED in dsp56k_jit_arm64.h).
  files(['dsp_cpu.c', 'dsp56k_jit_arm64.c', 'debug.c']) + genh,
  include_directories: include_directories('..'),
)
