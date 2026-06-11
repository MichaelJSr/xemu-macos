if vulkan.found()

vk_sources = files(
	'blit.c',
	'buffer.c',
	'command.c',
	'debug.c',
	'display.c',
	'draw.c',
	'glsl.c',
	'gpuprops.c',
	'image.c',
	'instance.c',
	'nsprof.c',
	'renderer.c',
	'reports.c',
	'shaders.c',
	'surface-compute.c',
	'surface.c',
	'texture.c',
	'vertex.c',
	)

vk_deps = [volk, libglslang, vma, vulkan, spirv_reflect]

if host_os == 'darwin'
  vk_sources += files('metalfx_upscale.m')
  vk_deps += [
    dependency('appleframeworks', modules: ['Metal', 'MetalFX', 'IOSurface', 'OpenGL']),
  ]
endif

specific_ss.add([vk_deps, vk_sources])

endif
