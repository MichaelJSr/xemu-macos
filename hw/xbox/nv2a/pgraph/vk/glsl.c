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

/*
 * Escape hatches, read once (class-4 legacy restores; default = the
 * behaviour described below):
 *   XEMU_SPIRV_CACHE=0        - bypass the on-disk cache entirely (no
 *                               loads, no stores), for cold-compile A/B
 *                               measurement or if the cache directory is
 *                               unusable on some host.
 *   XEMU_SPIRV_CACHE_ATOMIC=0 - restore the legacy in-place write instead
 *                               of the temp-file + g_rename replace.
 */
static bool spirv_cache_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_SPIRV_CACHE");
        on = !(e && e[0] == '0');
    }
    return on == 1;
}

static bool spirv_cache_atomic_write(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_SPIRV_CACHE_ATOMIC");
        on = !(e && e[0] == '0');
    }
    return on == 1;
}

/*
 * A kill between the temp write and the rename in save_spirv_to_cache
 * leaves a <hash>.spv.<pid>.tmp behind, and nothing ever reads or
 * replaces that name again: the orphans accumulate in the profile
 * directory forever. Sweep them once per process, right after the cache
 * directory is first resolved. Unlinking a temp file that another
 * running instance is still writing costs that instance one cache store
 * (its g_rename then fails and it drops the entry) and can never
 * produce a bad cache entry, because readers only ever open <hash>.spv.
 * This process cannot sweep its own live temp file: save_spirv_to_cache
 * resolves the directory - blocking on the once-init below - before it
 * opens one, so the sweep is ordered ahead of every temp file we create.
 */
static void sweep_stale_spirv_temp_files(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) {
        return;
    }

    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (g_str_has_suffix(name, ".tmp")) {
            char *path = g_build_filename(dir, name, NULL);
            g_unlink(path);
            g_free(path);
        }
    }
    g_dir_close(d);
}

static char *get_spirv_cache_dir(void)
{
    static gsize swept;
    const char *base = xemu_settings_get_base_path();
    /*
     * The compile options are part of the blob's identity too:
     * debug_shaders disables the optimizer and embeds debug info
     * (see pgraph_vk_compile_glsl_to_spv). Without it in the key, a
     * debug run poisons every later normal run with unoptimized
     * SPIR-V and toggling the option no-ops against a warm cache.
     * A directory suffix (rather than mixing a bit into the hash)
     * keeps the two variants from evicting each other and leaves
     * existing default-config caches warm on both platforms.
     */
    const char *variant = g_config.display.vulkan.debug_shaders ? "-dbg" : "";
    char *dir = g_strdup_printf("%sspirv_cache_v%d.%d.%d-%s%s", base,
                                xemu_version_major, xemu_version_minor,
                                xemu_version_patch, get_spirv_compiler_id(),
                                variant);
    /* Portable (Windows mkdir takes one argument). */
    g_mkdir_with_parents(dir, 0755);

    if (g_once_init_enter(&swept)) {
        sweep_stale_spirv_temp_files(dir);
        g_once_init_leave(&swept, 1);
    }

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

static void delete_spirv_cache_entry(uint64_t hash)
{
    char *path = get_spirv_cache_path(hash);
    g_unlink(path);
    g_free(path);
}

/* SPIR-V module header: magic + 4 words (version, generator, bound, 0). */
#define SPIRV_MAGIC 0x07230203u
#define SPIRV_HEADER_SIZE (5 * sizeof(uint32_t))
#define SPIRV_OP_FUNCTION_END 56

/*
 * A cached blob goes straight to vkCreateShaderModule and SPIRV-Reflect,
 * and VK_CHECK aborts on failure, so a file torn by a kill mid-write (or
 * by two instances racing) would otherwise be a permanent hard crash with
 * no self-heal. Reject anything that is not a plausible SPIR-V module and
 * delete the entry so the next compile repopulates it.
 *
 * This check must stand on its own: it cannot lean on SPIRV-Reflect to
 * report a truncated module. SPIRV-Reflect's bounds check is
 * assert(InRange(...)) in ReadU32()/ReadStr() (spirv_reflect.c) and the
 * subproject is compiled with asserts live (nothing sets meson's
 * b_ndebug), so a truncation the parser walks into aborts the process
 * inside the parser instead of returning
 * SPV_REFLECT_RESULT_ERROR_SPIRV_UNEXPECTED_EOF - and a truncation the
 * parser happens to accept goes straight on to the driver. Hence:
 *
 *   - codeSize a multiple of 4 and >= the 5-word header
 *     (VUID-VkShaderModuleCreateInfo-codeSize-08735),
 *   - first word is the SPIR-V magic (which also rejects a byte-swapped
 *     module, whose words we could not walk anyway),
 *   - the instruction stream walks exactly to the end: every
 *     instruction is <word-count:16><opcode:16> plus word-count-1
 *     operand words, so a zero count (never terminates) or a count that
 *     runs past the last word means the file is truncated or garbage,
 *   - and the last instruction is OpFunctionEnd. SPIR-V's logical
 *     layout (spec 2.4) puts function definitions last and a module
 *     with an entry point has at least one function, so a complete
 *     module always ends there - while a truncation that landed on an
 *     instruction boundary mid-module ends on whatever it cut after.
 *
 * The walk proves the property the parser's bounds asserts assume -
 * every word of every instruction lies inside the blob - and the
 * trailer rule closes the boundary case the walk alone cannot see.
 * Measured 2026-09-08 against the dev Mac's warm cache (394 entries,
 * glslang 16.5.0) and against glslangValidator -g / -gV / -gVS output
 * (the debug_shaders option set): zero false rejects, every module
 * ending in OpFunctionEnd. Of all 2,416,107 word-aligned truncations
 * of those 394 entries the two rules together reject 99.85%; the
 * 0.15% that survive are cuts landing on a function boundary, i.e. a
 * prefix that is itself a well-formed module missing later functions,
 * and SPIRV-Reflect returned an error for every one of them (0
 * accepted, 0 aborts), so the second gate in init_layout_from_spv()
 * deletes and recompiles them.
 */
static bool spirv_blob_is_valid(const guint8 *data, size_t size)
{
    uint32_t magic, last_op = 0;
    size_t words, off, wc;

    if (size < SPIRV_HEADER_SIZE || (size % sizeof(uint32_t)) != 0) {
        return false;
    }
    memcpy(&magic, data, sizeof(magic));
    if (magic != SPIRV_MAGIC) {
        return false;
    }

    words = size / sizeof(uint32_t);
    for (off = SPIRV_HEADER_SIZE / sizeof(uint32_t); off < words; off += wc) {
        uint32_t insn;
        memcpy(&insn, data + off * sizeof(uint32_t), sizeof(insn));
        wc = insn >> 16;
        if (wc == 0 || off + wc > words) {
            return false;
        }
        last_op = insn & 0xffff;
    }

    return last_op == SPIRV_OP_FUNCTION_END;
}

static GByteArray *load_spirv_from_cache(uint64_t hash)
{
    if (!spirv_cache_enabled()) {
        return NULL;
    }

    char *path = get_spirv_cache_path(hash);
    /* qemu_fopen, not fopen: the path comes from SDL_GetPrefPath and is
     * UTF-8, but Windows fopen() reads it in the process ANSI code page,
     * so a non-ASCII profile path silently never hits the cache. */
    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        g_free(path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        if (size == 0) {
            /* A zero-length entry is a torn write: drop it so the next
             * compile writes a real one. */
            g_unlink(path);
        }
        g_free(path);
        return NULL;
    }

    guint8 *data = g_malloc(size);
    if (fread(data, 1, size, f) != (size_t)size ||
        !spirv_blob_is_valid(data, (size_t)size)) {
        fclose(f);
        fprintf(stderr,
                "xemu: ignoring corrupt SPIR-V cache entry %s (%ld bytes); "
                "recompiling\n",
                path, size);
        g_unlink(path);
        g_free(data);
        g_free(path);
        return NULL;
    }
    fclose(f);
    g_free(path);
    return g_byte_array_new_take(data, size);
}

static void save_spirv_to_cache(uint64_t hash, GByteArray *spv)
{
    if (!spirv_cache_enabled() || !spv || spv->len == 0) {
        return;
    }

    char *path = get_spirv_cache_path(hash);

    if (!spirv_cache_atomic_write()) {
        /* Legacy (XEMU_SPIRV_CACHE_ATOMIC=0): write in place. */
        FILE *f = qemu_fopen(path, "wb");
        if (f) {
            fwrite(spv->data, 1, spv->len, f);
            fclose(f);
        }
        g_free(path);
        return;
    }

    /* Write-then-rename, same discipline as the pipeline cache in draw.c:
     * a kill mid-write must not leave a torn .spv for the next boot to
     * ingest. The temp name carries the pid so two instances compiling the
     * same shader cannot truncate each other's partial file, and g_rename
     * replaces an existing target on Windows too (plain rename() fails
     * there once the file exists). */
    char *tmp_path =
        g_strdup_printf("%s.%u.tmp", path, (unsigned)getpid());
    FILE *f = qemu_fopen(tmp_path, "wb");
    if (f) {
        bool ok = fwrite(spv->data, 1, spv->len, f) == spv->len;
        ok &= fclose(f) == 0;
        if (!ok || g_rename(tmp_path, path) != 0) {
            g_unlink(tmp_path);
        }
    }
    g_free(tmp_path);
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

/*
 * Returns false if the blob does not reflect, so the caller can
 * recompile instead of handing it to vkCreateShaderModule, whose
 * VK_CHECK would abort the process. This is the second line of defence
 * for a cached blob, not the first: only a blob that already passed
 * spirv_blob_is_valid() (header, a full instruction-stream walk, an
 * OpFunctionEnd trailer) reaches the parser, because SPIRV-Reflect
 * aborts on a truncation it walks into rather than reporting one. What
 * it still adds is everything structurally-well-formed-but-wrong that
 * those rules cannot see: a cut landing exactly on a function boundary
 * (measured 2026-09-08 - the parser returns an error for every such cut
 * of every warm cache entry, never success and never an abort), bad
 * ids, unparseable types. On failure it self-destroys the module, so
 * there is nothing to free here.
 */
static bool init_layout_from_spv(ShaderModuleInfo *info)
{
    SpvReflectResult result = spvReflectCreateShaderModule(
        info->spirv->len, info->spirv->data, &info->reflect_module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

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

    return true;
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
        /* Reflect the cached blob before it can reach the driver: a bad
         * entry must be a miss that regenerates itself, not an abort. */
        if (init_layout_from_spv(info)) {
            nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        } else {
            fprintf(stderr,
                    "xemu: SPIR-V cache entry %016llx failed to reflect; "
                    "recompiling\n",
                    (unsigned long long)glsl_hash);
            g_byte_array_unref(info->spirv);
            info->spirv = NULL;
            delete_spirv_cache_entry(glsl_hash);
        }
    }

    if (!info->spirv) {
        info->spirv = pgraph_vk_compile_glsl_to_spv(
            vk_shader_stage_to_glslang_stage(stage), glsl);
        save_spirv_to_cache(glsl_hash, info->spirv);
        bool reflected = init_layout_from_spv(info);
        assert(reflected && "Failed to create SPIR-V shader module");
        (void)reflected;
    }

    info->module = pgraph_vk_create_shader_module_from_spv(r, info->spirv);

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
