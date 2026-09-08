/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/xemu-settings.h"
#include "renderer.h"
#include "qemu/fast-hash.h"

#include <assert.h>
#include <glslang/Include/glslang_c_interface.h>
#include "xemu-version.h"
#include <stdio.h>
#include <glib/gstdio.h>

/*
 * The cached blobs are one specific shader compiler's output, so that
 * compiler's identity belongs in the cache key: without it, changing
 * glslang serves the previous compiler's SPIR-V out of a warm cache,
 * and any A/B of the change measures nothing. The version is asked of
 * the linked library at runtime rather than read from glslang's
 * generated <glslang/build_info.h>: the header that wins on the include
 * path is not necessarily the one the linked glslang was built from (a
 * system/Homebrew glslang ahead of the subproject's include dir keyed
 * the cache on a version this binary never compiled with). Residual
 * gap: a glslang revision bump that keeps the same version number is
 * not distinguished.
 *
 * Format is "glslang<major>.<minor>.<patch><flavor>" -- unchanged from
 * the old build_info.h spelling, so warm caches survive this change.
 */
static const char *get_spirv_compiler_id(void)
{
    static char id[64];
    static gsize id_init;

    if (g_once_init_enter(&id_init)) {
        glslang_version_t version = { 0 };
        glslang_get_version(&version);
        snprintf(id, sizeof(id), "glslang%d.%d.%d%s", version.major,
                 version.minor, version.patch,
                 version.flavor ? version.flavor : "");
        g_once_init_leave(&id_init, 1);
    }

    return id;
}

static char *get_spirv_cache_dir(void)
{
    const char *base = xemu_settings_get_base_path();
    char *dir = g_strdup_printf("%sspirv_cache_v%d.%d.%d-%s", base,
                                xemu_version_major, xemu_version_minor,
                                xemu_version_patch, get_spirv_compiler_id());
    /* Portable (Windows mkdir takes one argument). */
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

static char *get_spirv_cache_path(uint64_t hash)
{
    char *dir = get_spirv_cache_dir();
    char *path = g_strdup_printf("%s/%016llx.spv", dir,
                                 (unsigned long long)hash);
    g_free(dir);
    return path;
}

static GByteArray *load_spirv_from_cache(uint64_t hash)
{
    char *path = get_spirv_cache_path(hash);
    FILE *f = fopen(path, "rb");
    g_free(path);
    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return NULL;
    }

    guint8 *data = g_malloc(size);
    if (fread(data, 1, size, f) != size) {
        g_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return g_byte_array_new_take(data, size);
}

static void save_spirv_to_cache(uint64_t hash, GByteArray *spv)
{
    char *path = get_spirv_cache_path(hash);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(spv->data, 1, spv->len, f);
        fclose(f);
    }
    g_free(path);
}

static const glslang_resource_t
    resource_limits = { .max_lights = 32,
                        .max_clip_planes = 6,
                        .max_texture_units = 32,
                        .max_texture_coords = 32,
                        .max_vertex_attribs = 64,
                        .max_vertex_uniform_components = 4096,
                        .max_varying_floats = 64,
                        .max_vertex_texture_image_units = 32,
                        .max_combined_texture_image_units = 80,
                        .max_texture_image_units = 32,
                        .max_fragment_uniform_components = 4096,
                        .max_draw_buffers = 32,
                        .max_vertex_uniform_vectors = 128,
                        .max_varying_vectors = 8,
                        .max_fragment_uniform_vectors = 16,
                        .max_vertex_output_vectors = 16,
                        .max_fragment_input_vectors = 15,
                        .min_program_texel_offset = -8,
                        .max_program_texel_offset = 7,
                        .max_clip_distances = 8,
                        .max_compute_work_group_count_x = 65535,
                        .max_compute_work_group_count_y = 65535,
                        .max_compute_work_group_count_z = 65535,
                        .max_compute_work_group_size_x = 1024,
                        .max_compute_work_group_size_y = 1024,
                        .max_compute_work_group_size_z = 64,
                        .max_compute_uniform_components = 1024,
                        .max_compute_texture_image_units = 16,
                        .max_compute_image_uniforms = 8,
                        .max_compute_atomic_counters = 8,
                        .max_compute_atomic_counter_buffers = 1,
                        .max_varying_components = 60,
                        .max_vertex_output_components = 64,
                        .max_geometry_input_components = 64,
                        .max_geometry_output_components = 128,
                        .max_fragment_input_components = 128,
                        .max_image_units = 8,
                        .max_combined_image_units_and_fragment_outputs = 8,
                        .max_combined_shader_output_resources = 8,
                        .max_image_samples = 0,
                        .max_vertex_image_uniforms = 0,
                        .max_tess_control_image_uniforms = 0,
                        .max_tess_evaluation_image_uniforms = 0,
                        .max_geometry_image_uniforms = 0,
                        .max_fragment_image_uniforms = 8,
                        .max_combined_image_uniforms = 8,
                        .max_geometry_texture_image_units = 16,
                        .max_geometry_output_vertices = 256,
                        .max_geometry_total_output_components = 1024,
                        .max_geometry_uniform_components = 1024,
                        .max_geometry_varying_components = 64,
                        .max_tess_control_input_components = 128,
                        .max_tess_control_output_components = 128,
                        .max_tess_control_texture_image_units = 16,
                        .max_tess_control_uniform_components = 1024,
                        .max_tess_control_total_output_components = 4096,
                        .max_tess_evaluation_input_components = 128,
                        .max_tess_evaluation_output_components = 128,
                        .max_tess_evaluation_texture_image_units = 16,
                        .max_tess_evaluation_uniform_components = 1024,
                        .max_tess_patch_components = 120,
                        .max_patch_vertices = 32,
                        .max_tess_gen_level = 64,
                        .max_viewports = 16,
                        .max_vertex_atomic_counters = 0,
                        .max_tess_control_atomic_counters = 0,
                        .max_tess_evaluation_atomic_counters = 0,
                        .max_geometry_atomic_counters = 0,
                        .max_fragment_atomic_counters = 8,
                        .max_combined_atomic_counters = 8,
                        .max_atomic_counter_bindings = 1,
                        .max_vertex_atomic_counter_buffers = 0,
                        .max_tess_control_atomic_counter_buffers = 0,
                        .max_tess_evaluation_atomic_counter_buffers = 0,
                        .max_geometry_atomic_counter_buffers = 0,
                        .max_fragment_atomic_counter_buffers = 1,
                        .max_combined_atomic_counter_buffers = 1,
                        .max_atomic_counter_buffer_size = 16384,
                        .max_transform_feedback_buffers = 4,
                        .max_transform_feedback_interleaved_components = 64,
                        .max_cull_distances = 8,
                        .max_combined_clip_and_cull_distances = 8,
                        .max_samples = 4,
                        .max_mesh_output_vertices_nv = 256,
                        .max_mesh_output_primitives_nv = 512,
                        .max_mesh_work_group_size_x_nv = 32,
                        .max_mesh_work_group_size_y_nv = 1,
                        .max_mesh_work_group_size_z_nv = 1,
                        .max_task_work_group_size_x_nv = 32,
                        .max_task_work_group_size_y_nv = 1,
                        .max_task_work_group_size_z_nv = 1,
                        .max_mesh_view_count_nv = 4,
                        .maxDualSourceDrawBuffersEXT = 1,
                        .limits = {
                            .non_inductive_for_loops = 1,
                            .while_loops = 1,
                            .do_while_loops = 1,
                            .general_uniform_indexing = 1,
                            .general_attribute_matrix_vector_indexing = 1,
                            .general_varying_indexing = 1,
                            .general_sampler_indexing = 1,
                            .general_variable_indexing = 1,
                            .general_constant_matrix_vector_indexing = 1,
                        } };

void pgraph_vk_init_glsl_compiler(void)
{
    glslang_initialize_process();
}

void pgraph_vk_finalize_glsl_compiler(void)
{
    glslang_finalize_process();
}

GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source)
{
    const glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_3,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_6,
        .code = glsl_source,
        .default_version = 460,
        .default_profile = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = &resource_limits,
    };

    glslang_shader_t *shader = glslang_shader_create(&input);

    if (!glslang_shader_preprocess(shader, &input)) {
        fprintf(stderr,
                "GLSL preprocessing failed\n"
                "[INFO]: %s\n"
                "[DEBUG]: %s\n"
                "%s\n",
                glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader), input.code);
        assert(!"glslang preprocess failed");
        glslang_shader_delete(shader);
        return NULL;
    }

    if (!glslang_shader_parse(shader, &input)) {
        fprintf(stderr,
                "GLSL parsing failed\n"
                "[INFO]: %s\n"
                "[DEBUG]: %s\n"
                "%s\n",
                glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader),
                glslang_shader_get_preprocessed_code(shader));
        assert(!"glslang parse failed");
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_t *program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                           GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr,
                "GLSL linking failed\n"
                "[INFO]: %s\n"
                "[DEBUG]: %s\n",
                glslang_program_get_info_log(program),
                glslang_program_get_info_debug_log(program));
        assert(!"glslang link failed");
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_spv_options_t spv_options = {
        .validate = true,
    };

    if (g_config.display.vulkan.debug_shaders) {
        spv_options.disable_optimizer = true;
        spv_options.generate_debug_info = true;
        spv_options.emit_nonsemantic_shader_debug_info = true;
        spv_options.emit_nonsemantic_shader_debug_source = true;

        // XXX: Note emit_nonsemantic_shader_debug_source actually does nothing
        // as of 2024.07.25. To actually get glsl source embedded in spv, we
        // must do the following...
        //
        // ref: https://github.com/KhronosGroup/glslang/issues/3252
        glslang_program_add_source_text(program, input.stage, input.code,
                                        strlen(input.code));
    }
    glslang_program_SPIRV_generate_with_options(program, stage, &spv_options);

    const char *spirv_messages = glslang_program_SPIRV_get_messages(program);
    if (spirv_messages) {
        fprintf(stderr, "%s\n", spirv_messages);
    }

    size_t num_program_bytes =
        glslang_program_SPIRV_get_size(program) * sizeof(uint32_t);

    guint8 *data = g_malloc(num_program_bytes);
    glslang_program_SPIRV_get(program, (unsigned int *)data);

    glslang_program_delete(program);
    glslang_shader_delete(shader);

    return g_byte_array_new_take(data, num_program_bytes);
}

VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r, GByteArray *spv)
{
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv->len,
        .pCode = (uint32_t *)spv->data,
    };
    VkShaderModule module;
    VK_CHECK(
        vkCreateShaderModule(r->device, &create_info, NULL, &module));
    return module;
}

static void block_to_uniforms(const SpvReflectBlockVariable *block, ShaderUniformLayout *layout)
{
    assert(!layout->uniforms);

    layout->num_uniforms = block->member_count;
    layout->uniforms = g_malloc0_n(block->member_count, sizeof(ShaderUniform));
    layout->total_size = block->size;
    layout->allocation = g_malloc0(block->size);
    layout->dirty = true;

    for (uint32_t k = 0; k < block->member_count; ++k) {
        const SpvReflectBlockVariable *member = &block->members[k];

        assert(member->array.dims_count < 2);

        int dim = 1;
        for (int i = 0; i < member->array.dims_count; i++) {
            dim *= member->array.dims[i];
        }
        int stride = MAX(member->array.stride, member->numeric.matrix.stride);
        if (member->numeric.matrix.column_count) {
            dim *= member->numeric.matrix.column_count;
            if (member->array.stride) {
                stride =
                    member->array.stride / member->numeric.matrix.column_count;
            }
        }
        layout->uniforms[k] = (ShaderUniform){
            .name = strdup(member->name),
            .offset = member->offset,
            .dim_v = MAX(1, member->numeric.vector.component_count),
            .dim_a = dim,
            .stride = stride,
        };

        // fprintf(stderr, "<%s offset=%zd dim_v=%zd dim_a=%zd stride=%zd>\n",
        //     layout->uniforms[k].name,
        //     layout->uniforms[k].offset,
        //     layout->uniforms[k].dim_v,
        //     layout->uniforms[k].dim_a,
        //     layout->uniforms[k].stride
        //     );
    }
    // fprintf(stderr, "--\n");
}

static void init_layout_from_spv(ShaderModuleInfo *info)
{
    SpvReflectResult result = spvReflectCreateShaderModule(
        info->spirv->len, info->spirv->data, &info->reflect_module);
    assert(result == SPV_REFLECT_RESULT_SUCCESS &&
           "Failed to create SPIR-V shader module");

    uint32_t descriptor_set_count = 0;
    result = spvReflectEnumerateDescriptorSets(&info->reflect_module,
                                               &descriptor_set_count, NULL);
    assert(result == SPV_REFLECT_RESULT_SUCCESS &&
           "Failed to enumerate descriptor sets");

    info->descriptor_sets =
        g_malloc_n(descriptor_set_count, sizeof(SpvReflectDescriptorSet *));
    result = spvReflectEnumerateDescriptorSets(
        &info->reflect_module, &descriptor_set_count, info->descriptor_sets);
    assert(result == SPV_REFLECT_RESULT_SUCCESS &&
           "Failed to enumerate descriptor sets");

    info->uniforms.num_uniforms = 0;
    info->uniforms.uniforms = NULL;

    for (uint32_t i = 0; i < descriptor_set_count; ++i) {
        const SpvReflectDescriptorSet *descriptor_set =
            info->descriptor_sets[i];
        for (uint32_t j = 0; j < descriptor_set->binding_count; ++j) {
            const SpvReflectDescriptorBinding *binding =
                descriptor_set->bindings[j];
            if (binding->descriptor_type !=
                SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                continue;
            }

            const SpvReflectBlockVariable *block = &binding->block;
            block_to_uniforms(block, &info->uniforms);
        }
    }

    info->push_constants.num_uniforms = 0;
    info->push_constants.uniforms = NULL;
    assert(info->reflect_module.push_constant_block_count < 2);
    if (info->reflect_module.push_constant_block_count) {
        block_to_uniforms(&info->reflect_module.push_constant_blocks[0],
                          &info->push_constants);
    }
}

static glslang_stage_t vk_shader_stage_to_glslang_stage(VkShaderStageFlagBits stage)
{
    switch (stage) {
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return GLSLANG_STAGE_GEOMETRY;
    case VK_SHADER_STAGE_VERTEX_BIT:
        return GLSLANG_STAGE_VERTEX;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return GLSLANG_STAGE_FRAGMENT;
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return GLSLANG_STAGE_COMPUTE;
    default:
        assert(0);
    }
}

ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl)
{
    ShaderModuleInfo *info = g_malloc0(sizeof(*info));
    info->refcnt = 0;
    info->glsl = strdup(glsl);

    uint64_t glsl_hash = fast_hash((const uint8_t *)glsl, strlen(glsl));

    info->spirv = load_spirv_from_cache(glsl_hash);
    if (info->spirv) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    } else {
        info->spirv = pgraph_vk_compile_glsl_to_spv(
            vk_shader_stage_to_glslang_stage(stage), glsl);
        save_spirv_to_cache(glsl_hash, info->spirv);
    }

    info->module = pgraph_vk_create_shader_module_from_spv(r, info->spirv);
    init_layout_from_spv(info);

    free(info->glsl);
    info->glsl = NULL;

    return info;
}

static void finalize_uniform_layout(ShaderUniformLayout *layout)
{
    for (int i = 0; i < layout->num_uniforms; i++) {
        free((void*)layout->uniforms[i].name);
    }
    if (layout->uniforms) {
        g_free(layout->uniforms);
    }
    g_free(layout->allocation);
    layout->allocation = NULL;
}

void pgraph_vk_ref_shader_module(ShaderModuleInfo *info)
{
    info->refcnt++;
}

void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    assert(info->refcnt >= 1);

    info->refcnt--;
    if (info->refcnt == 0) {
        pgraph_vk_destroy_shader_module(r, info);
    }
}

void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    assert(info->refcnt == 0);
    if (info->glsl) {
        free(info->glsl);
    }
    finalize_uniform_layout(&info->uniforms);
    finalize_uniform_layout(&info->push_constants);
    free(info->descriptor_sets);
    spvReflectDestroyShaderModule(&info->reflect_module);
    vkDestroyShaderModule(r->device, info->module, NULL);
    g_byte_array_unref(info->spirv);
    g_free(info);
}
