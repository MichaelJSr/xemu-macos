/*
 * NV2A Vulkan renderer: the cache keys of draw pipelines and shader modules
 *
 * Kept apart so the pipeline prewarm's manifest schema (prewarm.c, the
 * build's NV2A_KEY_SCHEMA) hashes only what defines a key: this file, the
 * GLSL state headers and the register enums they hold. Include it through
 * renderer.h (after the Vulkan headers).
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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_KEYS_H
#define HW_XBOX_NV2A_PGRAPH_VK_KEYS_H

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
} RenderPassState;

/*
 * The rasterizer, depth-stencil and colour-blend state a draw pipeline is
 * built from, derived from BLEND, CONTROL_0-2 and SETUPRASTER by
 * pgraph_vk_get_raster_state (draw.c). A field its enable turns off stays
 * zero, as does depth write without depth test (the spec disables it
 * then), so register bits no pipeline reads don't split the key. Each
 * value is a Vulkan enum, flag or 8-bit stencil value; init checks that
 * the enum maps fit (check_raster_state_maps in draw.c).
 */
typedef struct RasterState {
    uint8_t cull_mode;              /* VkCullModeFlags */
    uint8_t front_face;             /* VkFrontFace, always: gl_FrontFacing */
    uint8_t depth_bias_enable;
    uint8_t depth_test_enable;
    uint8_t depth_write_enable;
    uint8_t depth_compare_op;       /* VkCompareOp */
    uint8_t stencil_test_enable;
    uint8_t stencil_fail_op;        /* VkStencilOp, front and back */
    uint8_t stencil_pass_op;
    uint8_t stencil_depth_fail_op;
    uint8_t stencil_compare_op;     /* VkCompareOp */
    uint8_t stencil_compare_mask;
    uint8_t stencil_write_mask;
    uint8_t stencil_reference;
    uint8_t color_write_mask;       /* VkColorComponentFlags, always */
    uint8_t blend_enable;
    uint8_t blend_src_factor;       /* VkBlendFactor, colour and alpha */
    uint8_t blend_dst_factor;
    uint8_t blend_op;               /* VkBlendOp, colour and alpha */
    uint8_t reserved;               /* zero */
} RasterState;

typedef struct PipelineKey {
    bool clear;
    /* Draw keys: the active vertex input counts (padding before) */
    uint8_t num_vertex_bindings;
    uint8_t num_vertex_attributes;
    uint32_t clear_parameter; /* clear keys only */
    RenderPassState render_pass_state;
    ShaderState shader_state;
    /*
     * Draw keys only. NV_PGRAPH_CONTROL_3 feeds none of it: its bits
     * (SHADEMODE, FOG_MODE, FOGENABLE, POINTPARAMSENABLE,
     * PROVOKING_VERTEX) are captured by ShaderState or consumed CPU-side
     * (check_pipeline_dirty in draw.c, pgraph_glsl_check_shader_state_dirty
     * in glsl/shaders.c). create_pipeline hashes it with the bytes after
     * shader_state, whose own hash it caches.
     */
    RasterState raster;
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

QEMU_BUILD_BUG_ON(offsetof(PipelineKey, clear_parameter) != 4);

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

#endif
