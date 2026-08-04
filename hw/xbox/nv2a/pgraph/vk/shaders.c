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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"
#include "renderer.h"
#include "hw/xbox/nv2a/nsprof.h"

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_tex_sets = ARRAY_SIZE(r->descriptor_sets);
    size_t num_ubo_sets = ARRAY_SIZE(r->ubo_descriptor_sets);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2 * num_ubo_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES * num_tex_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = num_tex_sets + num_ubo_sets,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /*
     * Set 0: UBOs only. Re-bound when uniforms change or a new
     * shader is installed; unchanged when only textures change.
     * Binding numbers are 0 and 1 within this set (they match the
     * legacy values so existing SPIR-V keeps its numeric bindings).
     */
    VkDescriptorSetLayoutBinding ubo_bindings[2] = {
        {
            .binding = VSH_UBO_BINDING,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        {
            .binding = PSH_UBO_BINDING,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo ubo_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(ubo_bindings),
        .pBindings = ubo_bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &ubo_info, NULL,
                                         &r->ubo_descriptor_set_layout));

    /*
     * Set 1: combined image samplers. Bindings 0..NV2A_MAX_TEXTURES-1.
     * GLSL/SPIR-V gets these bindings plus layout(set=1). The numeric
     * offset from the legacy PSH_TEX_BINDING (=2) is dropped; the
     * shader generator emits the new numbering.
     */
    VkDescriptorSetLayoutBinding tex_bindings[NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        tex_bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo tex_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(tex_bindings),
        .pBindings = tex_bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &tex_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(r->device, r->ubo_descriptor_set_layout, NULL);
    r->ubo_descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    {
        VkDescriptorSetLayout layouts[ARRAY_SIZE(r->ubo_descriptor_sets)];
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            layouts[i] = r->ubo_descriptor_set_layout;
        }
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = r->descriptor_pool,
            .descriptorSetCount = ARRAY_SIZE(r->ubo_descriptor_sets),
            .pSetLayouts = layouts,
        };
        VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                          r->ubo_descriptor_sets));
    }
    {
        VkDescriptorSetLayout layouts[ARRAY_SIZE(r->descriptor_sets)];
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            layouts[i] = r->descriptor_set_layout;
        }
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = r->descriptor_pool,
            .descriptorSetCount = ARRAY_SIZE(r->descriptor_sets),
            .pSetLayouts = layouts,
        };
        VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                          r->descriptor_sets));
    }
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->ubo_descriptor_sets),
                         r->ubo_descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->ubo_descriptor_sets); i++) {
        r->ubo_descriptor_sets[i] = VK_NULL_HANDLE;
    }

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->descriptor_sets), r->descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->descriptor_sets); i++) {
        r->descriptor_sets[i] = VK_NULL_HANDLE;
    }
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    bool need_uniform_write =
        r->uniforms_changed ||
        r->shader_bindings_changed ||
        (r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset ==
         r->flight[r->current_flight].uniform_staging_base);

    bool need_ubo_advance =
        need_uniform_write ||
        r->ubo_descriptor_set_index ==
            r->flight[r->current_flight].ubo_descriptor_set_base;
    bool need_tex_advance =
        r->shader_bindings_changed ||
        r->texture_bindings_changed ||
        r->descriptor_set_index ==
            r->flight[r->current_flight].descriptor_set_base;

    if (!need_ubo_advance && !need_tex_advance) {
        return; /* Nothing changed; reuse currently-bound sets */
    }

    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };
    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }
    bool need_ubo_staging_buffer_reset =
        need_uniform_write &&
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_UNIFORM_STAGING,
                                        ubo_buffer_total_size, 1,
                                        r->device_props.limits.minUniformBufferOffsetAlignment);

    bool need_ubo_descriptor_reset =
        (r->ubo_descriptor_set_index >=
         r->flight[r->current_flight].ubo_descriptor_set_limit);
    bool need_tex_descriptor_reset =
        (r->descriptor_set_index >=
         r->flight[r->current_flight].descriptor_set_limit);

    if (need_ubo_descriptor_reset || need_tex_descriptor_reset ||
        need_ubo_staging_buffer_reset) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        /* Finish resets the per-flight counters; re-evaluate. */
        need_uniform_write = true;
        need_ubo_advance = true;
        need_tex_advance = true;
    }

    assert(r->ubo_descriptor_set_index <
           r->flight[r->current_flight].ubo_descriptor_set_limit);
    assert(r->descriptor_set_index <
           r->flight[r->current_flight].descriptor_set_limit);

    /* ---- Set 0: UBOs ---- */
    if (need_ubo_advance) {
        if (need_uniform_write) {
            for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
                void *data = layouts[i]->allocation;
                VkDeviceSize size = layouts[i]->total_size;
                r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                    pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                    r->device_props.limits.minUniformBufferOffsetAlignment);
            }
            r->uniforms_changed = false;
        }

        VkDescriptorBufferInfo ubo_buffer_infos[2];
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
                .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
                .offset = r->uniform_buffer_offsets[i],
                .range = layouts[i]->total_size,
            };
        }
        /*
         * VSH_UBO_BINDING=0 and PSH_UBO_BINDING=1 are consecutive
         * same-type (UNIFORM_BUFFER) in the same set. Per Vulkan spec
         * §14.2.3, a single write with descriptorCount=2 and
         * dstArrayElement=0 overflows into binding 1 when binding 0's
         * array is exhausted. Saves one VkWriteDescriptorSet struct
         * init and one internal driver dispatch per UBO advance. Same
         * pattern as the texture-descriptor coalescing below.
         */
        QEMU_BUILD_BUG_ON(ARRAY_SIZE(layouts) != 2);
        QEMU_BUILD_BUG_ON(VSH_UBO_BINDING != 0 || PSH_UBO_BINDING != 1);
        VkWriteDescriptorSet ubo_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->ubo_descriptor_sets[r->ubo_descriptor_set_index],
            .dstBinding = VSH_UBO_BINDING,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = ARRAY_SIZE(ubo_buffer_infos),
            .pBufferInfo = ubo_buffer_infos,
        };
        vkUpdateDescriptorSets(r->device, 1, &ubo_write, 0, NULL);
        r->ubo_descriptor_set_index++;
    }

    /* ---- Set 1: Textures ---- */
    if (need_tex_advance) {
        VkDescriptorImageInfo image_infos[NV2A_MAX_TEXTURES];
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            TextureBinding *tex_binding = r->texture_bindings[i];
            if (!tex_binding) {
                tex_binding = &r->dummy_texture;
            }

            SamplerCacheEntry *sampler_binding = r->sampler_bindings[i];
            VkSampler sampler = sampler_binding ? sampler_binding->sampler
                                                : r->dummy_sampler.sampler;

            image_infos[i] = (VkDescriptorImageInfo){
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .imageView = tex_binding->image_view,
                .sampler = sampler,
            };
        }
        /*
         * Texture bindings 0..NV2A_MAX_TEXTURES-1 are consecutive
         * same-type (COMBINED_IMAGE_SAMPLER). Per Vulkan spec §14.2.3,
         * a single VkWriteDescriptorSet with descriptorCount=N and
         * dstArrayElement=0 overflows into the next binding when the
         * current binding's array size is exhausted. Collapsing the
         * four separate writes into one saves three struct
         * initializations and three internal driver dispatches per
         * advancing draw.
         */
        VkWriteDescriptorSet tex_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES,
            .pImageInfo = image_infos,
        };
        vkUpdateDescriptorSets(r->device, 1, &tex_write, 0, NULL);
        r->descriptor_set_index++;
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_ref_shader_module(module->module_info);
    return module->module_info;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
    int64_t nsprof_t0 = nsprof_begin();

    ShaderModuleCacheKey key;

    // TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
    // The r->supports_geometry_shaders flag is false on macOS since Metal
    // lacks native Geometry Shaders. This safely skips building and binding
    // the Z-buffer calculations via geom shaders on Apple chips.
    bool need_geometry_shader = r->supports_geometry_shaders &&
                                pgraph_glsl_need_geom(&binding->state.geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key.geom.state = binding->state.geom;
        key.geom.glsl_opts.vulkan = true;
        binding->geom.module_info = get_and_ref_shader_module_for_key(r, &key);
    } else {
        binding->geom.module_info = NULL;
    }

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    key.vsh.state = binding->state.vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    binding->vsh.module_info = get_and_ref_shader_module_for_key(r, &key);

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key.psh.state = binding->state.psh;
    // Tell the fragment shader to fallback to hardware depth interpolation
    // (gl_FragCoord.z) if Geometry Shaders were bypassed (like on macOS).
    key.psh.state.use_hw_depth = !need_geometry_shader;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    binding->psh.module_info = get_and_ref_shader_module_for_key(r, &key);

    update_shader_uniform_locs(binding);
    nsprof_end(NSPROF_SHADER_GEN, nsprof_t0);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return memcmp(&snode->state, key, sizeof(ShaderState));
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, mstring_get_str(code));
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_unref_shader_module(r, module->module_info);
    module->module_info = NULL;
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const size_t shader_cache_size = 1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((void *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

static void apply_uniform_updates(ShaderUniformLayout *layout,
                                  const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] != -1) {
            uniform_copy(layout, locs[i], (char*)values + info[i].val_offs,
                         4, (info[i].size * info[i].count) / 4);
        }
    }
}

static void update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);
    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->vsh.uniform_locs, &vsh_values);
    apply_uniform_updates(&binding->vsh.module_info->uniforms, VshUniformInfo,
                          binding->vsh.uniform_locs, &vsh_values,
                          VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    for (int i = 0; i < 4; i++) {
        assert(r->texture_bindings[i] != NULL);
        float scale = r->texture_bindings[i]->key.scale;

        BasicColorFormatInfo f_basic =
            kelvin_color_format_info_map[pg->vk_renderer_state
                                             ->texture_bindings[i]
                                             ->key.state.color_format];
        if (!f_basic.linear) {
            scale = 1.0;
        }

        psh_values.texScale[i] = scale;
    }
    apply_uniform_updates(&binding->psh.module_info->uniforms, PshUniformInfo,
                          binding->psh.uniform_locs, &psh_values,
                          PshUniform__COUNT);

    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        r->uniforms_changed |= layouts[i]->dirty;
        layouts[i]->dirty = false;
    }

    nv2a_profile_inc_counter(r->uniforms_changed ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    if (!r->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        if (!r->shader_binding || memcmp(&r->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
            /* Invalidate the cached sub-hash so create_pipeline re-seeds. */
            r->cached_shader_state_hash = 0;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    /*
     * update_shader_uniforms pulls PGRAPH state into VshUniformValues /
     * PshUniformValues and re-stages the UBO. Skip it when no input
     * has changed since the last pull: no shader rebind, no texture
     * rebind (texScale[] depends on current texture bindings), and no
     * pgraph_mark_uniforms_dirty since last clear (covers reg writes,
     * ltctxa/b/c1/vsh_constants writes, and inline_value writes).
     */
    if (r->shader_bindings_changed || r->texture_bindings_changed ||
        pg->shader_uniform_inputs_dirty) {
        update_shader_uniforms(pg);
        pg->shader_uniform_inputs_dirty = false;
    }

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_init_glsl_compiler();
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    shader_cache_init(pg);

    r->use_push_constants_for_uniform_attrs =
        (r->device_props.limits.maxPushConstantsSize >=
         MAX_UNIFORM_ATTR_VALUES_SIZE);
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    shader_cache_finalize(pg);
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    pgraph_vk_finalize_glsl_compiler();
}
