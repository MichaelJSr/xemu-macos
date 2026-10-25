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
	'prewarm.c',
	'renderer.c',
	'reports.c',
	'shaders.c',
	'surface-compute.c',
	'surface.c',
	'texture.c',
	'vertex.c',
	)

vk_deps = [volk, libglslang, vma, vulkan, spirv_reflect]

# The pipeline prewarm's manifest schema (prewarm.c): the key headers' hash
vk_sources += custom_target('nv2a-key-schema.h',
  input: files('keys.h', '../glsl/common.h', '../glsl/geom.h', '../glsl/psh.h',
               '../glsl/shaders.h', '../glsl/vsh.h', '../vsh_regs.h',
               '../psh_regs.h'),
  output: 'nv2a-key-schema.h',
  command: [python, meson.project_source_root() / 'scripts/nv2a-key-schema.py',
            '@OUTPUT@', '@INPUT@'])

if host_os == 'darwin'
  vk_sources += files('metalfx_upscale.m')
  vk_deps += [
    dependency('appleframeworks', modules: ['Metal', 'MetalFX', 'IOSurface', 'OpenGL']),
  ]
endif

specific_ss.add([vk_deps, vk_sources])

endif
