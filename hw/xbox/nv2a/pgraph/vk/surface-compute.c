/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"
#include <vulkan/vulkan_core.h>

// TODO: Float depth format (low priority, but would be better for accuracy)

/*
 * Apple Silicon GPUs have a SIMD width of 32. Workgroup size 64 provides
 * good occupancy for memory-bound shaders on TBDR without excessive
 * register pressure. Non-Apple GPUs prefer 256.
 */
#if defined(__APPLE__)
#define COMPUTE_2BUF_WORKGROUP_SIZE 64
#else
#define COMPUTE_2BUF_WORKGROUP_SIZE 256
#endif

// FIXME: Below pipeline creation assumes identical 3 buffer setup. For
//        swizzle shader we will need more flexibility.

/*
 * Z-order unswizzle compute shader: transforms Xbox-tiled (Morton/Z-order)
 * surface data to linear layout. Operates on 32-bit elements (4 bpp).
 * Push constants: width, height (in pixels).
 * Binding 0: swizzled input (uint[]), binding 1: linear output (uint[]).
 */
const char *unswizzle_z_order_glsl =
    "layout(push_constant) uniform PushConstants { uint width, height; };\n"
    "layout(set = 0, binding = 0) buffer SwizzledIn { uint swizzled_in[]; };\n"
    "layout(set = 0, binding = 1) buffer LinearOut { uint linear_out[]; };\n"
    "uint morton_x(uint x) {\n"
    "    x &= 0x55555555u;\n"
    "    x = (x | (x >> 1u)) & 0x33333333u;\n"
    "    x = (x | (x >> 2u)) & 0x0f0f0f0fu;\n"
    "    x = (x | (x >> 4u)) & 0x00ff00ffu;\n"
    "    x = (x | (x >> 8u)) & 0x0000ffffu;\n"
    "    return x;\n"
    "}\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    if (idx >= width * height) return;\n"
    "    uint mx = morton_x(idx);\n"
    "    uint my = morton_x(idx >> 1u);\n"
    "    if (mx < width && my < height) {\n"
    "        linear_out[my * width + mx] = swizzled_in[idx];\n"
    "    }\n"
    "}\n";

/*
 * Z-order unswizzle compute shader for 2 bytes-per-pixel surfaces (R5G6B5,
 * etc.). Iterates over destination 32-bit words so each thread writes both
 * packed 16-bit halves in a single store, avoiding SSBO write races without
 * atomics. Uses Morton encode to map linear (x,y) back to Z-order source index.
 * Push constants: width, height (in pixels).
 */
const char *unswizzle_z_order_2bpp_glsl =
    "layout(push_constant) uniform PushConstants { uint width, height; };\n"
    "layout(set = 0, binding = 0) buffer SwizzledIn { uint swizzled_in[]; };\n"
    "layout(set = 0, binding = 1) buffer LinearOut  { uint linear_out[]; };\n"
    "uint morton_encode(uint x, uint y) {\n"
    "    x = (x | (x << 8u)) & 0x00ff00ffu;\n"
    "    x = (x | (x << 4u)) & 0x0f0f0f0fu;\n"
    "    x = (x | (x << 2u)) & 0x33333333u;\n"
    "    x = (x | (x << 1u)) & 0x55555555u;\n"
    "    y = (y | (y << 8u)) & 0x00ff00ffu;\n"
    "    y = (y | (y << 4u)) & 0x0f0f0f0fu;\n"
    "    y = (y | (y << 2u)) & 0x33333333u;\n"
    "    y = (y | (y << 1u)) & 0x55555555u;\n"
    "    return x | (y << 1u);\n"
    "}\n"
    "void main() {\n"
    "    uint word_idx = gl_GlobalInvocationID.x;\n"
    "    uint total_words = (width * height + 1u) / 2u;\n"
    "    if (word_idx >= total_words) return;\n"
    "    uint w_mask = width - 1u;\n"
    "    uint w_shift = uint(findMSB(width));\n"
    "    uint dst_px0 = word_idx * 2u;\n"
    "    uint dst_px1 = dst_px0 + 1u;\n"
    "    uint x0 = dst_px0 & w_mask;\n"
    "    uint y0 = dst_px0 >> w_shift;\n"
    "    uint x1 = dst_px1 & w_mask;\n"
    "    uint y1 = dst_px1 >> w_shift;\n"
    "    uint lo = 0u, hi = 0u;\n"
    "    if (y0 < height) {\n"
    "        uint src0 = morton_encode(x0, y0);\n"
    "        uint sw0 = swizzled_in[src0 >> 1u];\n"
    "        lo = (src0 & 1u) == 0u ? (sw0 & 0xffffu) : (sw0 >> 16u);\n"
    "    }\n"
    "    if (dst_px1 < width * height && y1 < height) {\n"
    "        uint src1 = morton_encode(x1, y1);\n"
    "        uint sw1 = swizzled_in[src1 >> 1u];\n"
    "        hi = (src1 & 1u) == 0u ? (sw1 & 0xffffu) : (sw1 >> 16u);\n"
    "    }\n"
    "    linear_out[word_idx] = lo | (hi << 16u);\n"
    "}\n";

/*
 * YUV (YUY2 / CR8YB8CB8YA8) to RGBA compute shader: converts packed YUV
 * to RGBA for PVIDEO overlay and textures. Operates on pairs of pixels.
 * Push constants: width (in pixels, must be even).
 * Binding 0: YUV input (uint[]), binding 1: RGBA output (uint[]).
 */
const char *yuv_to_rgba_glsl =
    "layout(push_constant) uniform PushConstants { uint width, height; };\n"
    "layout(set = 0, binding = 0) buffer YuvIn { uint yuv_in[]; };\n"
    "layout(set = 0, binding = 1) buffer RgbaOut { uint rgba_out[]; };\n"
    "void main() {\n"
    "    uint pair_idx = gl_GlobalInvocationID.x;\n"
    "    uint row = pair_idx / (width / 2u);\n"
    "    uint col = (pair_idx % (width / 2u)) * 2u;\n"
    "    if (row >= height || col >= width) return;\n"
    "    uint packed = yuv_in[pair_idx];\n"
    "    float y0 = float((packed >> 8u) & 0xffu);\n"
    "    float u  = float(packed & 0xffu) - 128.0;\n"
    "    float y1 = float((packed >> 24u) & 0xffu);\n"
    "    float v  = float((packed >> 16u) & 0xffu) - 128.0;\n"
    "    float r0 = clamp(y0 + 1.402 * v, 0.0, 255.0);\n"
    "    float g0 = clamp(y0 - 0.344 * u - 0.714 * v, 0.0, 255.0);\n"
    "    float b0 = clamp(y0 + 1.772 * u, 0.0, 255.0);\n"
    "    float r1 = clamp(y1 + 1.402 * v, 0.0, 255.0);\n"
    "    float g1 = clamp(y1 - 0.344 * u - 0.714 * v, 0.0, 255.0);\n"
    "    float b1 = clamp(y1 + 1.772 * u, 0.0, 255.0);\n"
    "    uint pixel_idx = row * width + col;\n"
    "    rgba_out[pixel_idx]     = uint(r0) | (uint(g0) << 8u) | (uint(b0) << 16u) | 0xff000000u;\n"
    "    rgba_out[pixel_idx + 1u] = uint(r1) | (uint(g1) << 8u) | (uint(b1) << 16u) | 0xff000000u;\n"
    "}\n";

const char *pack_d24_unorm_s8_uint_to_z24s8_glsl =
    "layout(push_constant) uniform PushConstants { uint width_in, width_out; };\n"
    "layout(set = 0, binding = 0) buffer DepthIn { uint depth_in[]; };\n"
    "layout(set = 0, binding = 1) buffer StencilIn { uint stencil_in[]; };\n"
    "layout(set = 0, binding = 2) buffer DepthStencilOut { uint depth_stencil_out[]; };\n"
    "uint get_input_idx(uint idx_out) {\n"
    "    uint scale = width_in / width_out;\n"
    "    uint y = (idx_out / width_out) * scale;\n"
    "    uint x = (idx_out % width_out) * scale;\n"
    "    return y * width_in + x;\n"
    "}\n"
    "void main() {\n"
    "    uint idx_out = gl_GlobalInvocationID.x;\n"
    "    uint idx_in = get_input_idx(idx_out);\n"
    "    uint depth_value = depth_in[idx_in];\n"
    "    uint stencil_value = (stencil_in[idx_in / 4] >> ((idx_in % 4) * 8)) & 0xff;\n"
    "    depth_stencil_out[idx_out] = depth_value << 8 | stencil_value;\n"
    "}\n";

const char *unpack_z24s8_to_d24_unorm_s8_uint_glsl =
    "layout(push_constant) uniform PushConstants { uint width_in, width_out; };\n"
    "layout(set = 0, binding = 0) buffer DepthOut { uint depth_out[]; };\n"
    "layout(set = 0, binding = 1) buffer StencilOut { uint stencil_out[]; };\n"
    "layout(set = 0, binding = 2) buffer DepthStencilIn { uint depth_stencil_in[]; };\n"
    "uint get_input_idx(uint idx_out) {\n"
    "    uint scale = width_out / width_in;\n"
    "    uint y = (idx_out / width_out) / scale;\n"
    "    uint x = (idx_out % width_out) / scale;\n"
    "    return y * width_in + x;\n"
    "}\n"
    "void main() {\n"
    "    uint idx_out = gl_GlobalInvocationID.x;\n"
    "    uint idx_in = get_input_idx(idx_out);\n"
    "    depth_out[idx_out] = depth_stencil_in[idx_in] >> 8;\n"
    "    if (idx_out % 4 == 0) {\n"
    "       uint stencil_value = 0;\n"
    "       for (int i = 0; i < 4; i++) {\n" // Include next 3 pixels
    "           uint v = depth_stencil_in[get_input_idx(idx_out + i)] & 0xff;\n"
    "           stencil_value |= v << (i * 8);\n"
    "       }\n"
    "       stencil_out[idx_out / 4] = stencil_value;\n"
    "    }\n"
    "}\n";

const char *pack_d32_sfloat_s8_uint_to_z24s8_glsl =
    "layout(push_constant) uniform PushConstants { uint width_in, width_out; };\n"
    "layout(set = 0, binding = 0) buffer DepthIn { float depth_in[]; };\n"
    "layout(set = 0, binding = 1) buffer StencilIn { uint stencil_in[]; };\n"
    "layout(set = 0, binding = 2) buffer DepthStencilOut { uint depth_stencil_out[]; };\n"
    "uint get_input_idx(uint idx_out) {\n"
    "    uint scale = width_in / width_out;\n"
    "    uint y = (idx_out / width_out) * scale;\n"
    "    uint x = (idx_out % width_out) * scale;\n"
    "    return y * width_in + x;\n"
    "}\n"
    "void main() {\n"
    "    uint idx_out = gl_GlobalInvocationID.x;\n"
    "    uint idx_in = get_input_idx(idx_out);\n"
    "    uint depth_value = int(depth_in[idx_in] * float(0xffffff));\n"
    "    uint stencil_value = (stencil_in[idx_in / 4] >> ((idx_in % 4) * 8)) & 0xff;\n"
    "    depth_stencil_out[idx_out] = depth_value << 8 | stencil_value;\n"
    "}\n";

const char *unpack_z24s8_to_d32_sfloat_s8_uint_glsl =
    "layout(push_constant) uniform PushConstants { uint width_in, width_out; };\n"
    "layout(set = 0, binding = 0) buffer DepthOut { float depth_out[]; };\n"
    "layout(set = 0, binding = 1) buffer StencilOut { uint stencil_out[]; };\n"
    "layout(set = 0, binding = 2) buffer DepthStencilIn { uint depth_stencil_in[]; };\n"
    "uint get_input_idx(uint idx_out) {\n"
    "    uint scale = width_out / width_in;\n"
    "    uint y = (idx_out / width_out) / scale;\n"
    "    uint x = (idx_out % width_out) / scale;\n"
    "    return y * width_in + x;\n"
    "}\n"
    "void main() {\n"
    "    uint idx_out = gl_GlobalInvocationID.x;\n"
    "    uint idx_in = get_input_idx(idx_out);\n"
    // Conversion to float depth must be the same as in fragment shader
    "    depth_out[idx_out] = uintBitsToFloat(floatBitsToUint(float(depth_stencil_in[idx_in] >> 8) / 16777216.0) + 1u);\n"
    "    if (idx_out % 4 == 0) {\n"
    "       uint stencil_value = 0;\n"
    "       for (int i = 0; i < 4; i++) {\n" // Include next 3 pixels
    "           uint v = depth_stencil_in[get_input_idx(idx_out + i)] & 0xff;\n"
    "           stencil_value |= v << (i * 8);\n"
    "       }\n"
    "       stencil_out[idx_out / 4] = stencil_value;\n"
    "    }\n"
    "}\n";

static gchar *get_compute_shader_glsl(VkFormat host_fmt, bool pack,
                                      int workgroup_size)
{
    const char *template;

    switch (host_fmt) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
        template = pack ? pack_d24_unorm_s8_uint_to_z24s8_glsl :
                          unpack_z24s8_to_d24_unorm_s8_uint_glsl;
        break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        template = pack ? pack_d32_sfloat_s8_uint_to_z24s8_glsl :
                          unpack_z24s8_to_d32_sfloat_s8_uint_glsl;
        break;
    default:
        assert(!"Unsupported host fmt");
        break;
    }
    assert(template);

    gchar *glsl = g_strdup_printf(
        "#version 450\n"
        "layout(local_size_x = %d, local_size_y = 1, local_size_z = 1) in;\n"
        "%s", workgroup_size, template);
    assert(glsl);

    return glsl;
}

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 3 * ARRAY_SIZE(r->compute.descriptor_sets),
        },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = ARRAY_SIZE(r->compute.descriptor_sets),
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->compute.descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->compute.descriptor_pool, NULL);
    r->compute.descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

#define COMPUTE_NUM_BUFFERS 3

    VkDescriptorSetLayoutBinding bindings[COMPUTE_NUM_BUFFERS];
    for (int i = 0; i < COMPUTE_NUM_BUFFERS; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->compute.descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->compute.descriptor_set_layout,
                                 NULL);
    r->compute.descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[ARRAY_SIZE(r->compute.descriptor_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->compute.descriptor_set_layout;
    }
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->compute.descriptor_pool,
        .descriptorSetCount = ARRAY_SIZE(r->compute.descriptor_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                      r->compute.descriptor_sets));
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->compute.descriptor_pool,
                         ARRAY_SIZE(r->compute.descriptor_sets),
                         r->compute.descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->compute.descriptor_sets); i++) {
        r->compute.descriptor_sets[i] = VK_NULL_HANDLE;
    }
}

static void create_compute_pipeline_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = 2 * sizeof(uint32_t),
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->compute.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &r->compute.pipeline_layout));
}

static void destroy_compute_pipeline_layout(PGRAPHVkState *r)
{
    vkDestroyPipelineLayout(r->device, r->compute.pipeline_layout, NULL);
    r->compute.pipeline_layout = VK_NULL_HANDLE;
}

static VkPipeline create_compute_pipeline(PGRAPHVkState *r, const char *glsl)
{
    ShaderModuleInfo *module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_COMPUTE_BIT, glsl);

    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = r->compute.pipeline_layout,
        .stage =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .pName = "main",
                .module = module->module,
            },
    };
    VkPipeline pipeline;
    VK_CHECK(vkCreateComputePipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL,
                                       &pipeline));

    pgraph_vk_destroy_shader_module(r, module);

    return pipeline;
}

static void update_descriptor_sets(PGRAPHState *pg,
                                   VkDescriptorBufferInfo *buffers, int count)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(count == 3);
    VkWriteDescriptorSet descriptor_writes[3];

    assert(r->compute.descriptor_set_index <
           r->flight[r->current_flight].compute_descriptor_set_limit);

    for (int i = 0; i < count; i++) {
        descriptor_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet =
                r->compute.descriptor_sets[r->compute.descriptor_set_index],
            .dstBinding = i,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &buffers[i],
        };
    }
    vkUpdateDescriptorSets(r->device, count, descriptor_writes, 0, NULL);

    r->compute.descriptor_set_index += 1;
}

bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r)
{
    int limit = r->flight[r->current_flight].compute_descriptor_set_limit;
    return r->compute.descriptor_set_index >= limit;
}

void pgraph_vk_compute_finish_complete(PGRAPHVkState *r)
{
    r->compute.descriptor_set_index =
        r->flight[r->current_flight].compute_descriptor_set_base;
}

static int get_workgroup_size_for_output_units(PGRAPHVkState *r, int output_units)
{
    int group_size = 1024;

    // FIXME: Smarter workgroup size calculation could factor in multiple
    //        submissions. For now we will just pick the highest number that
    //        evenly divides output_units.

    while (group_size > 1) {
        if (group_size <= r->device_props.limits.maxComputeWorkGroupSize[0] &&
            output_units % group_size == 0) {
            break;
        }
        group_size /= 2;
    }

    return group_size;
}

static ComputePipeline *get_compute_pipeline(PGRAPHVkState *r, VkFormat host_fmt, bool pack, int output_units)
{
    int workgroup_size = get_workgroup_size_for_output_units(r, output_units);

    ComputePipelineKey key;
    memset(&key, 0, sizeof(key));

    key.host_fmt = host_fmt;
    key.pack = pack;
    key.workgroup_size = workgroup_size;

    LruNode *node = lru_lookup(&r->compute.pipeline_cache,
                      fast_hash((void *)&key, sizeof(key)), &key);
    ComputePipeline *pipeline = container_of(node, ComputePipeline, node);

    assert(pipeline);

    return pipeline;
}

//
// Pack depth+stencil into NV097_SET_SURFACE_FORMAT_ZETA_Z24S8
// formatted buffer with depth in bits 31-8 and stencil in bits 7-0.
//
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int input_width = surface->width, input_height = surface->height;
    pgraph_apply_scaling_factor(pg, &input_width, &input_height);

    unsigned int output_width = surface->width, output_height = surface->height;
    if (!downscale) {
        pgraph_apply_scaling_factor(pg, &output_width, &output_height);
    }

    size_t depth_bytes_per_pixel = 4;
    size_t depth_size = input_width * input_height * depth_bytes_per_pixel;

    size_t stencil_bytes_per_pixel = 1;
    size_t stencil_size = input_width * input_height * stencil_bytes_per_pixel;

    size_t output_bytes_per_pixel = 4;
    size_t output_size = output_width * output_height * output_bytes_per_pixel;

    VkDescriptorBufferInfo buffers[] = {
        {
            .buffer = src,
            .offset = 0,
            .range = depth_size,
        },
        {
            .buffer = src,
            .offset = ROUND_UP(
                depth_size,
                r->device_props.limits.minStorageBufferOffsetAlignment),
            .range = stencil_size,
        },
        {
            .buffer = dst,
            .offset = 0,
            .range = output_size,
        },
    };

    update_descriptor_sets(pg, buffers, ARRAY_SIZE(buffers));

    size_t output_size_in_units = output_width * output_height;
    ComputePipeline *pipeline = get_compute_pipeline(
        r, surface->host_fmt.vk_format, true, output_size_in_units);

    size_t workgroup_size_in_units = pipeline->key.workgroup_size;
    assert(output_size_in_units % workgroup_size_in_units == 0);
    size_t group_count = output_size_in_units / workgroup_size_in_units;

    assert(r->device_props.limits.maxComputeWorkGroupSize[0] >= workgroup_size_in_units);
    assert(r->device_props.limits.maxComputeWorkGroupCount[0] >= group_count);

    // FIXME: Smarter workgroup scaling

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_PINK, __func__);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->compute.pipeline_layout, 0, 1,
        &r->compute.descriptor_sets[r->compute.descriptor_set_index - 1], 0,
        NULL);

    uint32_t push_constants[2] = { input_width, output_width };
    assert(sizeof(push_constants) == 8);
    vkCmdPushConstants(cmd, r->compute.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                       push_constants);

    // FIXME: Check max group count

    vkCmdDispatch(cmd, group_count, 1, 1);
    pgraph_vk_end_debug_marker(r, cmd);
}

void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int input_width = surface->width, input_height = surface->height;

    unsigned int output_width = surface->width, output_height = surface->height;
    pgraph_apply_scaling_factor(pg, &output_width, &output_height);

    size_t depth_bytes_per_pixel = 4;
    size_t depth_size = output_width * output_height * depth_bytes_per_pixel;

    size_t stencil_bytes_per_pixel = 1;
    size_t stencil_size = output_width * output_height * stencil_bytes_per_pixel;

    size_t input_bytes_per_pixel = 4;
    size_t input_size = input_width * input_height * input_bytes_per_pixel;

    VkDescriptorBufferInfo buffers[] = {
        {
            .buffer = dst,
            .offset = 0,
            .range = depth_size,
        },
        {
            .buffer = dst,
            .offset = ROUND_UP(
                depth_size,
                r->device_props.limits.minStorageBufferOffsetAlignment),
            .range = stencil_size,
        },
        {
            .buffer = src,
            .offset = 0,
            .range = input_size,
        },
    };
    update_descriptor_sets(pg, buffers, ARRAY_SIZE(buffers));

    size_t output_size_in_units = output_width * output_height;
    ComputePipeline *pipeline = get_compute_pipeline(
        r, surface->host_fmt.vk_format, false, output_size_in_units);

    size_t workgroup_size_in_units = pipeline->key.workgroup_size;
    assert(output_size_in_units % workgroup_size_in_units == 0);
    size_t group_count = output_size_in_units / workgroup_size_in_units;

    assert(r->device_props.limits.maxComputeWorkGroupSize[0] >= workgroup_size_in_units);
    assert(r->device_props.limits.maxComputeWorkGroupCount[0] >= group_count);

    // FIXME: Smarter workgroup scaling

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_PINK, __func__);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->compute.pipeline_layout, 0, 1,
        &r->compute.descriptor_sets[r->compute.descriptor_set_index - 1], 0,
        NULL);

    assert(output_width >= input_width);
    uint32_t push_constants[2] = { input_width, output_width };
    assert(sizeof(push_constants) == 8);
    vkCmdPushConstants(cmd, r->compute.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                       push_constants);
    vkCmdDispatch(cmd, group_count, 1, 1);
    pgraph_vk_end_debug_marker(r, cmd);
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, compute.pipeline_cache);
    ComputePipeline *snode = container_of(node, ComputePipeline, node);

    memcpy(&snode->key, state, sizeof(snode->key));

    if (snode->key.workgroup_size == 1) {
        fprintf(stderr,
                "Warning: Needed compute shader with workgroup size = 1\n");
    }

    gchar *glsl = get_compute_shader_glsl(
        snode->key.host_fmt, snode->key.pack, snode->key.workgroup_size);
    assert(glsl);
    snode->pipeline = create_compute_pipeline(r, glsl);
    g_free(glsl);
}

static void pipeline_cache_release_node_resources(PGRAPHVkState *r, ComputePipeline *snode)
{
    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, compute.pipeline_cache);
    ComputePipeline *snode = container_of(node, ComputePipeline, node);
    pipeline_cache_release_node_resources(r, snode);
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    ComputePipeline *snode = container_of(node, ComputePipeline, node);
    return memcmp(&snode->key, key, sizeof(ComputePipelineKey));
}

static void pipeline_cache_init(PGRAPHVkState *r)
{
    const size_t pipeline_cache_size = 100; // FIXME: Trim
    lru_init(&r->compute.pipeline_cache);
    r->compute.pipeline_cache_entries = g_malloc_n(pipeline_cache_size, sizeof(ComputePipeline));
    assert(r->compute.pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->compute.pipeline_cache, &r->compute.pipeline_cache_entries[i].node);
    }
    r->compute.pipeline_cache.init_node = pipeline_cache_entry_init;
    r->compute.pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->compute.pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void pipeline_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->compute.pipeline_cache);
    g_free(r->compute.pipeline_cache_entries);
    r->compute.pipeline_cache_entries = NULL;
}

static VkPipeline create_2buf_compute_pipeline(PGRAPHVkState *r,
                                                const char *shader_body,
                                                int workgroup_size)
{
    gchar *glsl = g_strdup_printf(
        "#version 450\n"
        "layout(local_size_x = %d, local_size_y = 1, local_size_z = 1) in;\n"
        "%s", workgroup_size, shader_body);
    VkPipeline pipeline = create_compute_pipeline(r, glsl);
    g_free(glsl);
    return pipeline;
}

void pgraph_vk_init_compute(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    create_compute_pipeline_layout(pg);
    pipeline_cache_init(r);

    r->compute.unswizzle_pipeline = create_2buf_compute_pipeline(
        r, unswizzle_z_order_glsl, COMPUTE_2BUF_WORKGROUP_SIZE);
    r->compute.unswizzle_2bpp_pipeline = create_2buf_compute_pipeline(
        r, unswizzle_z_order_2bpp_glsl, COMPUTE_2BUF_WORKGROUP_SIZE);
    r->compute.yuv_to_rgba_pipeline = create_2buf_compute_pipeline(
        r, yuv_to_rgba_glsl, COMPUTE_2BUF_WORKGROUP_SIZE);
}

void pgraph_vk_finalize_compute(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    if (r->compute.unswizzle_pipeline) {
        vkDestroyPipeline(r->device, r->compute.unswizzle_pipeline, NULL);
        r->compute.unswizzle_pipeline = VK_NULL_HANDLE;
    }
    if (r->compute.unswizzle_2bpp_pipeline) {
        vkDestroyPipeline(r->device, r->compute.unswizzle_2bpp_pipeline, NULL);
        r->compute.unswizzle_2bpp_pipeline = VK_NULL_HANDLE;
    }
    if (r->compute.yuv_to_rgba_pipeline) {
        vkDestroyPipeline(r->device, r->compute.yuv_to_rgba_pipeline, NULL);
        r->compute.yuv_to_rgba_pipeline = VK_NULL_HANDLE;
    }

    pipeline_cache_finalize(r);
    destroy_compute_pipeline_layout(r);
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
}

void pgraph_vk_dispatch_unswizzle(PGRAPHState *pg, VkCommandBuffer cmd,
                                  VkBuffer src, VkBuffer dst,
                                  unsigned int width, unsigned int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    size_t pixel_count = (size_t)width * height;
    size_t buf_size = pixel_count * 4;

    VkDescriptorBufferInfo buffers[] = {
        { .buffer = src, .offset = 0, .range = buf_size },
        { .buffer = dst, .offset = 0, .range = buf_size },
        { .buffer = dst, .offset = 0, .range = buf_size },
    };
    update_descriptor_sets(pg, buffers, ARRAY_SIZE(buffers));

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_PINK, "Unswizzle %ux%u",
                                 width, height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      r->compute.unswizzle_pipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->compute.pipeline_layout, 0, 1,
        &r->compute.descriptor_sets[r->compute.descriptor_set_index - 1], 0,
        NULL);

    uint32_t push_constants[2] = { width, height };
    vkCmdPushConstants(cmd, r->compute.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                       push_constants);

    size_t group_count =
        (pixel_count + COMPUTE_2BUF_WORKGROUP_SIZE - 1) / COMPUTE_2BUF_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, group_count, 1, 1);
    pgraph_vk_end_debug_marker(r, cmd);
}

void pgraph_vk_dispatch_unswizzle_2bpp(PGRAPHState *pg, VkCommandBuffer cmd,
                                      VkBuffer src, VkBuffer dst,
                                      unsigned int width, unsigned int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    size_t pixel_count = (size_t)width * height;
    size_t buf_size = pixel_count * 2;
    size_t buf_size_aligned = ROUND_UP(buf_size, 4);

    VkDescriptorBufferInfo buffers[] = {
        { .buffer = src, .offset = 0, .range = buf_size_aligned },
        { .buffer = dst, .offset = 0, .range = buf_size_aligned },
        { .buffer = dst, .offset = 0, .range = buf_size_aligned },
    };
    update_descriptor_sets(pg, buffers, ARRAY_SIZE(buffers));

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_PINK, "Unswizzle2bpp %ux%u",
                                 width, height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      r->compute.unswizzle_2bpp_pipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->compute.pipeline_layout, 0, 1,
        &r->compute.descriptor_sets[r->compute.descriptor_set_index - 1], 0,
        NULL);

    uint32_t push_constants[2] = { width, height };
    vkCmdPushConstants(cmd, r->compute.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                       push_constants);

    size_t word_count = (pixel_count + 1) / 2;
    size_t group_count =
        (word_count + COMPUTE_2BUF_WORKGROUP_SIZE - 1) / COMPUTE_2BUF_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, group_count, 1, 1);
    pgraph_vk_end_debug_marker(r, cmd);
}

void pgraph_vk_dispatch_yuv_to_rgba(PGRAPHState *pg, VkCommandBuffer cmd,
                                    VkBuffer src, VkBuffer dst,
                                    unsigned int width, unsigned int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    size_t yuv_size = (size_t)(width / 2) * height * 4;
    size_t rgba_size = (size_t)width * height * 4;

    VkDescriptorBufferInfo buffers[] = {
        { .buffer = src, .offset = 0, .range = yuv_size },
        { .buffer = dst, .offset = 0, .range = rgba_size },
        { .buffer = dst, .offset = 0, .range = rgba_size },
    };
    update_descriptor_sets(pg, buffers, ARRAY_SIZE(buffers));

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_PINK, "YUV->RGBA %ux%u",
                                 width, height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      r->compute.yuv_to_rgba_pipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->compute.pipeline_layout, 0, 1,
        &r->compute.descriptor_sets[r->compute.descriptor_set_index - 1], 0,
        NULL);

    uint32_t push_constants[2] = { width, height };
    vkCmdPushConstants(cmd, r->compute.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                       push_constants);

    size_t pair_count = (size_t)(width / 2) * height;
    size_t group_count =
        (pair_count + COMPUTE_2BUF_WORKGROUP_SIZE - 1) / COMPUTE_2BUF_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, group_count, 1, 1);
    pgraph_vk_end_debug_marker(r, cmd);
}
