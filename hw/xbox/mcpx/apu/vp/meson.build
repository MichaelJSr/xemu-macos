vp_deps = [libsamplerate]

if host_os == 'darwin'
  # Use Accelerate.framework (vDSP) for vectorized FIR/mix on Apple Silicon
  vp_deps += dependency('appleframeworks', modules: ['Accelerate'])
endif

mcpx_ss.add(vp_deps, files(
	'vp.c'
	))
