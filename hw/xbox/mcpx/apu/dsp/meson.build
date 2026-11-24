dsp56300_emu_ffi = dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'])

subdir('interp')

# dsp_llvm_dep is needed here too: dsp.c and dsp_c.c include
# dsp56k_jit_arm64.h, so they must see the same -DDSP56K_JIT_LLVM as the
# backend TU or they would compile against the header's no-op stubs.
libdsp = static_library(
  'dsp',
  files(['dsp.c', 'dsp_c.c', 'dsp_jit.c', 'dsp_dma.c']) + genh,
  dependencies: [dsp56300_emu_ffi, dsp_llvm_dep],
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
  dependencies: [dsp56300_emu_ffi, dsp_llvm_dep],
)

mcpx_ss.add(dsp, files('gp_ep.c'))
