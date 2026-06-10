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

#include "renderer.h"
#include "ui/xemu-settings.h"
#include <math.h>

#ifndef NDEBUG
#define DISPLAY_DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define DISPLAY_DPRINTF(fmt, ...) do {} while (0)
#endif

#if HAVE_IOSURFACE_SHARING
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/CGLCurrent.h>
#include "metalfx_upscale.h"
#include "ui/xemu-present.h"
#include "ui/xemu-metal.h"

/*
 * Metal-native presentation: publish the frame the UI thread should
 * present next (consumed via pgraph_vk_get_present_frame under the
 * sync handshake). Exactly one of {IOSurface, MTLTexture} is current
 * at a time; setting one clears the other.
 */
static void display_set_present_surface(PGRAPHVkDisplayState *disp,
                                        IOSurfaceRef surf)
{
    if (disp->present_mtl_texture) {
        CFRelease(disp->present_mtl_texture);
        disp->present_mtl_texture = NULL;
    }
    if (disp->present_iosurface != (void *)surf) {
        if (disp->present_iosurface) {
            CFRelease((IOSurfaceRef)disp->present_iosurface);
        }
        disp->present_iosurface = (void *)CFRetain(surf);
    }
    disp->present_width = (int)IOSurfaceGetWidth(surf);
    disp->present_height = (int)IOSurfaceGetHeight(surf);

    /* New published content; default unpaced (paced sites override) */
    disp->present_frame_seq++;
    disp->present_duration_ns = 0;
}

/* Takes ownership of the retained texture handle. */
static void display_set_present_texture(PGRAPHVkDisplayState *disp,
                                        void *texture)
{
    if (disp->present_iosurface) {
        CFRelease((IOSurfaceRef)disp->present_iosurface);
        disp->present_iosurface = NULL;
    }
    if (disp->present_mtl_texture && disp->present_mtl_texture != texture) {
        CFRelease(disp->present_mtl_texture);
    }
    if (disp->present_mtl_texture == texture) {
        /* Same object handed back; drop the extra retain. */
        CFRelease(texture);
    } else {
        disp->present_mtl_texture = texture;
    }
    metalfx_texture_dims(disp->present_mtl_texture, &disp->present_width,
                         &disp->present_height);

    /* New published content; default unpaced (paced sites override) */
    disp->present_frame_seq++;
    disp->present_duration_ns = 0;
}
#endif

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void destroy_pvideo_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(r->device, d->pvideo.sampler, NULL);
        d->pvideo.sampler = VK_NULL_HANDLE;
    }

    if (d->pvideo.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(r->device, d->pvideo.image_view, NULL);
        d->pvideo.image_view = VK_NULL_HANDLE;
    }

    if (d->pvideo.image != VK_NULL_HANDLE) {
        vmaDestroyImage(r->allocator, d->pvideo.image, d->pvideo.allocation);
        d->pvideo.image = VK_NULL_HANDLE;
        d->pvideo.allocation = VK_NULL_HANDLE;
    }
}

static void create_pvideo_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.image == VK_NULL_HANDLE || d->pvideo.width != width ||
        d->pvideo.height != height) {
        destroy_pvideo_image(pg);
    }

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };
    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &d->pvideo.image,
                            &d->pvideo.allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = d->pvideo.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &d->pvideo.image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &d->pvideo.sampler));
}

static void upload_pvideo_to_cmd(PGRAPHState *pg, PvideoState state,
                                VkCommandBuffer cmd)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    create_pvideo_image(pg, state.in_width, state.in_height);

    VkDeviceSize staging_base = r->flight[r->current_flight].staging_buffer_base;
    if (r->storage_buffers[BUFFER_STAGING_SRC].buffer_offset > staging_base) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        r->storage_buffers[BUFFER_STAGING_SRC].buffer_offset = staging_base;
    }

    size_t yuv_size = (size_t)(state.in_width / 2) * state.in_height * 4;

    uint8_t *mapped_memory_ptr =
        r->storage_buffers[BUFFER_STAGING_SRC].mapped + staging_base;
    assert(r->storage_buffers[BUFFER_STAGING_SRC].mapped);

    uint8_t *src = d->vram_ptr + state.base + state.offset;
    size_t row_bytes = (size_t)state.in_width * 2;
    if ((size_t)state.pitch == row_bytes) {
        memcpy(mapped_memory_ptr, src, row_bytes * state.in_height);
    } else {
        for (int y = 0; y < state.in_height; y++) {
            memcpy(mapped_memory_ptr + y * row_bytes,
                   src + y * state.pitch,
                   row_bytes);
        }
    }

    if (!r->storage_buffers[BUFFER_STAGING_SRC].is_coherent) {
        /*
         * Flush what we actually wrote: the memcpy loop above touches
         * row_bytes * in_height bytes. For even in_width this matches
         * yuv_size, but for odd in_width (rare but possible) yuv_size
         * under-counts by up to 2 * in_height bytes and the flush
         * would be short on non-coherent memory. The subsequent
         * vkCmdCopyBuffer still moves yuv_size bytes; a superset
         * flush is safe.
         */
        vmaFlushAllocation(r->allocator,
                           r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                           staging_base,
                           row_bytes * (size_t)state.in_height);
    }

    size_t rgba_size = (size_t)state.in_width * state.in_height * 4;

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer,
        .offset = staging_base,
        .size = yuv_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    VkBufferCopy yuv_copy = { .srcOffset = staging_base, .size = yuv_size };
    vkCmdCopyBuffer(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                    r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                    1, &yuv_copy);

    VkBufferMemoryBarrier compute_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
        .size = yuv_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1,
                         &compute_barrier, 0, NULL);

    pgraph_vk_dispatch_yuv_to_rgba(pg, cmd,
                                   r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                                   r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                                   state.in_width, state.in_height);

    VkBufferMemoryBarrier post_compute = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = rgba_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_compute, 0, NULL);

    pgraph_vk_transition_image_layout(
        pg, cmd, disp->pvideo.image, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ state.in_width, state.in_height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                           disp->pvideo.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, disp->pvideo.image,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static const char *display_frag_glsl =
    "#version 450\n"
    "layout(binding = 0) uniform sampler2D tex;\n"
    "layout(binding = 1) uniform sampler2D pvideo_tex;\n"
    "layout(push_constant, std430) uniform PushConstants {\n"
    "    float line_offset;\n"
    "    vec2 display_size;\n"
    "    bool pvideo_enable;\n"
    "    vec2 pvideo_in_pos;\n"
    "    vec4 pvideo_pos;\n"
    "    vec4 pvideo_scale;\n"
    "    bool pvideo_color_key_enable;\n"
    "    vec3 pvideo_color_key;\n"
    "};\n"
    "layout(location = 0) out vec4 out_Color;\n"
    "void main()\n"
    "{\n"
    "    vec2 tex_coord = gl_FragCoord.xy/display_size;\n"
    "    float rel = display_size.y/textureSize(tex, 0).y/line_offset;\n"
    "    tex_coord.y = 1 + rel*(tex_coord.y - 1);\n"
    "    tex_coord.y = 1 - tex_coord.y;\n" // GL compat
    "    out_Color.rgba = texture(tex, tex_coord);\n"
    "    if (pvideo_enable) {\n"
    "        vec2 screen_coord = vec2(gl_FragCoord.x, display_size.y - gl_FragCoord.y) * pvideo_scale.z;\n"
    "        vec4 output_region = vec4(pvideo_pos.xy, pvideo_pos.xy + pvideo_pos.zw);\n"
    "        bvec4 clip = bvec4(lessThan(screen_coord, output_region.xy),\n"
    "                           greaterThan(screen_coord, output_region.zw));\n"
    "        if (!any(clip) && (!pvideo_color_key_enable || out_Color.rgb == pvideo_color_key)) {\n"
    "            vec2 out_xy = screen_coord - pvideo_pos.xy;\n"
    "            vec2 in_st = (pvideo_in_pos + out_xy * pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
    "            out_Color.rgba = texture(pvideo_tex, in_st);\n"
    "        }\n"
    "    }\n"
    "}\n";

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorPoolSize pool_sizes = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2,
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_sizes,
        .maxSets = 1,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->display.descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->display.descriptor_pool, NULL);
    r->display.descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2];

    for (int i = 0; i < ARRAY_SIZE(bindings); i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->display.descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->display.descriptor_set_layout,
                                 NULL);
    r->display.descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layout = r->display.descriptor_set_layout;

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->display.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                      &r->display.descriptor_set));
}

static void create_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkAttachmentDescription attachment;

    VkAttachmentReference color_reference;
    attachment = (VkAttachmentDescription){
        .format = r->display.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    color_reference = (VkAttachmentReference){
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    dependency.srcStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &r->display.render_pass));
}

static void destroy_render_pass(PGRAPHState *pg)
{
    /* vkDestroyRenderPass(VK_NULL_HANDLE) is a spec-safe no-op. */
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyRenderPass(r->device, r->display.render_pass, NULL);
    r->display.render_pass = VK_NULL_HANDLE;
}

static void create_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->display.display_frag =
        pgraph_vk_create_shader_module_from_glsl(
            r, VK_SHADER_STAGE_FRAGMENT_BIT, display_frag_glsl);

    VkPipelineShaderStageCreateInfo shader_stages[] = {
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        },
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->display.display_frag->module,
            .pName = "main",
        },
     };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = r->display.display_frag->push_constants.total_size,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->display.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &r->display.pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_SIZE(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = r->display.pipeline_layout,
        .renderPass = r->display.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL,
                                       &r->display.pipeline));
}

static void destroy_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyPipeline(r->device, r->display.pipeline, NULL);
    r->display.pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, r->display.pipeline_layout, NULL);
    r->display.pipeline_layout = VK_NULL_HANDLE;

    pgraph_vk_destroy_shader_module(r, r->display.display_frag);
    r->display.display_frag = NULL;
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->display.render_pass,
        .attachmentCount = 1,
        .pAttachments = &r->display.image_view,
        .width = r->display.width,
        .height = r->display.height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->display.framebuffer));
}

static void destroy_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyFramebuffer(r->device, r->display.framebuffer, NULL);
    r->display.framebuffer = NULL;
}

static void destroy_current_display_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->image == VK_NULL_HANDLE) {
        return;
    }

    destroy_frame_buffer(pg);

#if HAVE_EXTERNAL_MEMORY
    glDeleteTextures(1, &d->gl_texture_id);
    d->gl_texture_id = 0;

    glDeleteMemoryObjectsEXT(1, &d->gl_memory_obj);
    d->gl_memory_obj = 0;

#ifdef WIN32
    CloseHandle(d->handle);
    d->handle = 0;
#endif
#elif HAVE_IOSURFACE_SHARING
    if (d->gl_texture_id) {
        glDeleteTextures(1, &d->gl_texture_id);
        d->gl_texture_id = 0;
    }
    if (d->iosurface) {
        CFRelease((IOSurfaceRef)d->iosurface);
        d->iosurface = NULL;
    }
    if (d->mtl_texture) {
        CFRelease(d->mtl_texture);
        d->mtl_texture = NULL;
    }
    if (d->present_iosurface) {
        CFRelease((IOSurfaceRef)d->present_iosurface);
        d->present_iosurface = NULL;
    }
    if (d->present_mtl_texture) {
        CFRelease(d->present_mtl_texture);
        d->present_mtl_texture = NULL;
    }
    if (d->pending_real_texture) {
        CFRelease(d->pending_real_texture);
        d->pending_real_texture = NULL;
    }
    d->pending_real_event_value = 0;
    if (d->interp_midpoint_texture) {
        CFRelease(d->interp_midpoint_texture);
        d->interp_midpoint_texture = NULL;
    }
    d->interp_midpoint_event_value = 0;
    d->present_width = 0;
    d->present_height = 0;
    d->present_event = NULL;
    d->present_event_value = 0;
    d->last_cgl_surface_id = 0;
    d->last_cgl_width = 0;
    d->last_cgl_height = 0;
#endif

    vkDestroyImageView(r->device, d->image_view, NULL);
    d->image_view = VK_NULL_HANDLE;

    vkDestroyImage(r->device, d->image, NULL);
    d->image = VK_NULL_HANDLE;

    vkFreeMemory(r->device, d->memory, NULL);
    d->memory = VK_NULL_HANDLE;

    d->draw_time = 0;
}

static void create_display_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (r->display.image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

#if HAVE_IOSURFACE_SHARING
    /*
     * Drop any deferred frame-interpolation work. The saved prev/cur
     * IOSurface pair was captured at the old display resolution;
     * generating interpolated frames from it after a resize would
     * present stale-resolution output (interp_width/height no longer
     * match the new display image) for up to interp_remaining frames.
     */
    if (d->interp_prev_surface) {
        CFRelease((IOSurfaceRef)d->interp_prev_surface);
        d->interp_prev_surface = NULL;
    }
    if (d->interp_cur_surface) {
        CFRelease((IOSurfaceRef)d->interp_cur_surface);
        d->interp_cur_surface = NULL;
    }
    d->interp_remaining = 0;
#endif

    bool use_optimal_tiling = true;

#if HAVE_EXTERNAL_MEMORY
    GLint num_tiling_types;
    glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                          GL_NUM_TILING_TYPES_EXT, 1, &num_tiling_types);
    // XXX: Apparently on AMD GL_OPTIMAL_TILING_EXT is reported to be
    // supported, but doesn't work? On nVidia, GL_LINEAR_TILING_EXT may not
    // be supported so we must use optimal. Default to optimal unless
    // linear is explicitly specified...
    GLint tiling_types[num_tiling_types];
    glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                          GL_TILING_TYPES_EXT, num_tiling_types, tiling_types);
    for (int i = 0; i < num_tiling_types; i++) {
        if (tiling_types[i] == GL_LINEAR_TILING_EXT) {
            use_optimal_tiling = false;
            break;
        }
    }
#endif

    // Create image
    VkFormat display_format = VK_FORMAT_R8G8B8A8_UNORM;
#if HAVE_IOSURFACE_SHARING
    if (r->metal_objects_extension_enabled) {
        display_format = VK_FORMAT_B8G8R8A8_UNORM;
    }
#endif

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = display_format,
        .tiling = use_optimal_tiling ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

#if HAVE_EXTERNAL_MEMORY
    VkExternalMemoryImageCreateInfo external_memory_image_create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
#ifdef WIN32
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
    };
    image_create_info.pNext = &external_memory_image_create_info;
#elif HAVE_IOSURFACE_SHARING
    IOSurfaceRef imported_iosurface = NULL;
    VkImportMetalIOSurfaceInfoEXT import_iosurface_info;
    VkExportMetalObjectCreateInfoEXT export_metal_info;
    bool export_texture_mode =
        r->metal_objects_extension_enabled && xemu_present_is_metal() &&
        r->metal_texture_export_enabled;
    if (export_texture_mode) {
        /*
         * Metal backend, texture-export mode: no IOSurface at all.
         * The VkImage's backing MTLTexture is exported after creation
         * and consumed directly by MetalFX / the UI. Without an
         * IOSurface, the macOS 26 >1920px BGRA bytesPerRow bug cannot
         * affect the compositor image, so MetalFX engages at
         * surface_scale=4 (2560-wide input).
         */
        export_metal_info = (VkExportMetalObjectCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
            .exportObjectType =
                VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
        };
        image_create_info.pNext = &export_metal_info;
    } else if (r->metal_objects_extension_enabled) {
        unsigned bpe = 4;
        unsigned long long vals[] = {
            width, height, bpe, width * bpe,
            width * height * bpe, 'BGRA'
        };
        CFStringRef cf_keys[] = {
            kIOSurfaceWidth, kIOSurfaceHeight,
            kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
            kIOSurfaceAllocSize, kIOSurfacePixelFormat
        };
        CFNumberRef cf_vals[6];
        for (int i = 0; i < 6; i++) {
            cf_vals[i] = CFNumberCreate(NULL, kCFNumberLongLongType, &vals[i]);
        }
        CFDictionaryRef props = CFDictionaryCreate(NULL,
            (const void **)cf_keys, (const void **)cf_vals, 6,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        for (int i = 0; i < 6; i++) {
            CFRelease(cf_vals[i]);
        }

        imported_iosurface = IOSurfaceCreate(props);
        CFRelease(props);

        DISPLAY_DPRINTF("[IOSurface] Created IOSurface %dx%d: %p\n",
                width, height, (void *)imported_iosurface);

        if (imported_iosurface) {
            import_iosurface_info = (VkImportMetalIOSurfaceInfoEXT){
                .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
                .ioSurface = imported_iosurface,
            };
            export_metal_info = (VkExportMetalObjectCreateInfoEXT){
                .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
                .pNext = &import_iosurface_info,
                .exportObjectType =
                    VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT,
            };
            image_create_info.pNext = &export_metal_info;
        }
    }
#endif

    VK_CHECK(vkCreateImage(r->device, &image_create_info, NULL, &d->image));

    // Allocate and bind image memory
    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(r->device, d->image, &memory_requirements);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex =
            pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

#if HAVE_EXTERNAL_MEMORY
    VkExportMemoryAllocateInfo export_memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes =
#ifdef WIN32
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
#else
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
            ,
    };
    alloc_info.pNext = &export_memory_alloc_info;
#endif

    VK_CHECK(vkAllocateMemory(r->device, &alloc_info, NULL, &d->memory));
    VK_CHECK(vkBindImageMemory(r->device, d->image, d->memory, 0));

    // Create Image View
    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = d->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_create_info.format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &d->image_view));

#if HAVE_EXTERNAL_MEMORY

#ifdef WIN32

    VkMemoryGetWin32HandleInfoKHR handle_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = d->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
    };
    VK_CHECK(vkGetMemoryWin32HandleKHR(r->device, &handle_info, &d->handle));

    glCreateMemoryObjectsEXT(1, &d->gl_memory_obj);
    glImportMemoryWin32HandleEXT(d->gl_memory_obj, memory_requirements.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, d->handle);
    assert(glGetError() == GL_NO_ERROR);

#else

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = d->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VK_CHECK(vkGetMemoryFdKHR(r->device, &fd_info, &d->fd));

    glCreateMemoryObjectsEXT(1, &d->gl_memory_obj);
    glImportMemoryFdEXT(d->gl_memory_obj, memory_requirements.size,
                        GL_HANDLE_TYPE_OPAQUE_FD_EXT, d->fd);
    assert(glIsMemoryObjectEXT(d->gl_memory_obj));
    assert(glGetError() == GL_NO_ERROR);

#endif // WIN32

    glGenTextures(1, &d->gl_texture_id);
    glBindTexture(GL_TEXTURE_2D, d->gl_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT,
                    use_optimal_tiling ? GL_OPTIMAL_TILING_EXT :
                                         GL_LINEAR_TILING_EXT);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, gl_internal_format,
                         image_create_info.extent.width,
                         image_create_info.extent.height, d->gl_memory_obj, 0);
    assert(glGetError() == GL_NO_ERROR);

#elif HAVE_IOSURFACE_SHARING
    if (export_texture_mode) {
        VkExportMetalTextureInfoEXT tex_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
            .image = d->image,
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT export_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &tex_info,
        };
        ((PFN_vkExportMetalObjectsEXT)r->export_metal_objects_fn)(
            r->device, &export_info);
        if (tex_info.mtlTexture) {
            d->mtl_texture = (void *)CFRetain(tex_info.mtlTexture);
            DISPLAY_DPRINTF(
                    "[Metal] Exported compositor MTLTexture %dx%d\n",
                    image_create_info.extent.width,
                    image_create_info.extent.height);
        } else {
            fprintf(stderr,
                    "nv2a: compositor MTLTexture export failed at %ux%u "
                    "despite successful probe; display will be black "
                    "until the next mode change\n",
                    image_create_info.extent.width,
                    image_create_info.extent.height);
        }
    } else if (r->metal_objects_extension_enabled && imported_iosurface) {
        d->iosurface = (void *)CFRetain(imported_iosurface);

        /* Under the Metal presentation backend the UI consumes the
         * IOSurface directly; no CGL rect-texture is created. */
        CGLContextObj cgl_ctx =
            xemu_present_is_metal() ? NULL : CGLGetCurrentContext();
        if (cgl_ctx) {
            glGenTextures(1, &d->gl_texture_id);
            glBindTexture(GL_TEXTURE_RECTANGLE, d->gl_texture_id);
            CGLError cgl_err = CGLTexImageIOSurface2D(
                cgl_ctx, GL_TEXTURE_RECTANGLE,
                GL_RGBA,
                image_create_info.extent.width,
                image_create_info.extent.height,
                GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                imported_iosurface, 0);

            if (cgl_err == kCGLNoError &&
                glGetError() == GL_NO_ERROR) {
                glTexParameteri(GL_TEXTURE_RECTANGLE,
                                GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_RECTANGLE,
                                GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                DISPLAY_DPRINTF(
                        "IOSurface zero-copy display: %dx%d "
                        "(GL RECTANGLE tex %u)\n",
                        image_create_info.extent.width,
                        image_create_info.extent.height,
                        d->gl_texture_id);
            } else {
                DISPLAY_DPRINTF(
                        "IOSurface CGLTexImageIOSurface2D failed "
                        "(CGL err %d)\n", cgl_err);
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                glDeleteTextures(1, &d->gl_texture_id);
                d->gl_texture_id = 0;
            }
        } else if (!xemu_present_is_metal()) {
            DISPLAY_DPRINTF("[IOSurface] No CGL context on PFIFO thread!\n");
        }
        CFRelease(imported_iosurface);
    }
#endif // HAVE_EXTERNAL_MEMORY / HAVE_IOSURFACE_SHARING

    d->width = image_create_info.extent.width;
    d->height = image_create_info.extent.height;
    d->format = image_create_info.format;

    create_frame_buffer(pg);
}

static void update_descriptor_set(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    bool pvideo_on = disp->pvideo.state.enabled;
    if (surface == disp->last_descriptor_surface &&
        pvideo_on == disp->last_descriptor_pvideo) {
        return;
    }
    disp->last_descriptor_surface = surface;
    disp->last_descriptor_pvideo = pvideo_on;

    VkDescriptorImageInfo image_infos[2];
    VkWriteDescriptorSet descriptor_writes[2];

    // Display surface
    image_infos[0] = (VkDescriptorImageInfo){
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = surface->image_view,
        .sampler = r->display.sampler,
    };
    descriptor_writes[0] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = r->display.descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[0],
    };

    // FIXME: PVIDEO Overlay
    if (r->display.pvideo.state.enabled) {
        assert(r->display.pvideo.image_view != VK_NULL_HANDLE);
        assert(r->display.pvideo.sampler != VK_NULL_HANDLE);
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->display.pvideo.image_view,
            .sampler = r->display.pvideo.sampler,
        };
    } else {
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->dummy_texture.image_view,
            .sampler = r->dummy_sampler.sampler,
        };
    }
    descriptor_writes[1] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = r->display.descriptor_set,
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[1],
    };

    vkUpdateDescriptorSets(r->device, ARRAY_SIZE(descriptor_writes),
                           descriptor_writes, 0, NULL);
}

static PvideoState get_pvideo_state(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PvideoState state;
    memset(&state, 0, sizeof(state));

    // FIXME: This check against PVIDEO_SIZE_IN does not match HW behavior.
    // Many games seem to pass this value when initializing or tearing down
    // PVIDEO. On its own, this generally does not result in the overlay being
    // hidden, however there are certain games (e.g., Ultimate Beach Soccer)
    // that use an unknown mechanism to hide the overlay without explicitly
    // stopping it.
    // Since the value seems to be set to 0xFFFFFFFF only in cases where the
    // content is not valid, it is probably good enough to treat it as an
    // implicit stop.
    state.enabled = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE)
        && d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!state.enabled) {
        return state;
    }

    state.base = d->pvideo.regs[NV_PVIDEO_BASE];
    state.limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    state.offset = d->pvideo.regs[NV_PVIDEO_OFFSET];

    state.pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    state.format =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    /* TODO: support other color formats */
    assert(state.format == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    state.in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    state.in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);

    state.out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    state.out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);

    state.in_s = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_S);
    state.in_t = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_T);

    uint32_t ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    state.scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(ds_dx, state.out_width);
    state.scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(dt_dy, state.out_height);

    // On HW, setting NV_PVIDEO_SIZE_IN larger than NV_PVIDEO_SIZE_OUT results
    // in them being capped to the output size, content is not scaled. This is
    // particularly important as NV_PVIDEO_SIZE_IN may be set to 0xFFFFFFFF
    // during initialization or teardown.
    if (state.in_width > state.out_width) {
        state.in_width = floorf((float)state.out_width * state.scale_x + 0.5f);
    }
    if (state.in_height > state.out_height) {
        state.in_height = floorf((float)state.out_height * state.scale_y + 0.5f);
    }

    state.out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    state.out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);

    state.color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);

    // Note: PVIDEO color keying ignores alpha.
    state.color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    assert(state.offset + state.pitch * state.in_height <= state.limit);
    hwaddr end = state.base + state.offset + state.pitch * state.in_height;
    assert(end <= memory_region_size(d->vram));

    return state;
}

static void resolve_display_uniform_locations(PGRAPHVkDisplayState *disp)
{
    if (disp->ulocs_resolved || !disp->display_frag) return;
    ShaderUniformLayout *l = &disp->display_frag->push_constants;
    disp->uloc_display_size = uniform_index(l, "display_size");
    disp->uloc_line_offset = uniform_index(l, "line_offset");
    disp->uloc_pvideo_enable = uniform_index(l, "pvideo_enable");
    disp->uloc_pvideo_color_key_enable = uniform_index(l, "pvideo_color_key_enable");
    disp->uloc_pvideo_color_key = uniform_index(l, "pvideo_color_key");
    disp->uloc_pvideo_in_pos = uniform_index(l, "pvideo_in_pos");
    disp->uloc_pvideo_pos = uniform_index(l, "pvideo_pos");
    disp->uloc_pvideo_scale = uniform_index(l, "pvideo_scale");
    disp->ulocs_resolved = true;
}

static void update_uniforms(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;
    ShaderUniformLayout *l = &disp->display_frag->push_constants;

    resolve_display_uniform_locations(disp);

    uniform2f(l, disp->uloc_display_size, disp->width, disp->height);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    int line_offset = vga_display_params.line_offset ?
                          surface->pitch / vga_display_params.line_offset :
                          1;
    uniform1f(l, disp->uloc_line_offset, line_offset);

    PvideoState *pvideo = &disp->pvideo.state;
    uniform1i(l, disp->uloc_pvideo_enable, pvideo->enabled);
    if (pvideo->enabled) {
        uniform1i(l, disp->uloc_pvideo_color_key_enable,
                  pvideo->color_key_enabled);
        uniform3f(
            l, disp->uloc_pvideo_color_key,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0);
        uniform2f(l, disp->uloc_pvideo_in_pos, pvideo->in_s / 16.f,
                  pvideo->in_t / 8.f);
        uniform4f(l, disp->uloc_pvideo_pos, pvideo->out_x,
                  pvideo->out_y, pvideo->out_width, pvideo->out_height);
        uniform4f(l, disp->uloc_pvideo_scale, pvideo->scale_x,
                  pvideo->scale_y, 1.0f / pg->surface_scale_factor, 1.0);
    }
}

static void render_display(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    if (!surface->image || !surface->initialized) {
        return;
    }

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_PRESENTING);
    }

    pgraph_vk_upload_surface_data(d, surface, !tcg_enabled());

    disp->pvideo.state = get_pvideo_state(pg);
    update_uniforms(pg, surface);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_YELLOW,
        "Display Surface %08"HWADDR_PRIx, surface->vram_addr);
    if (disp->pvideo.state.enabled) {
        if (memcmp(&disp->pvideo.state, &disp->pvideo.last_uploaded_state,
                   sizeof(PvideoState)) != 0) {
            upload_pvideo_to_cmd(pg, disp->pvideo.state, cmd);
            disp->pvideo.last_uploaded_state = disp->pvideo.state;
        }
    }

    update_descriptor_set(pg, surface);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_transition_image_layout(
        pg, cmd, disp->image, disp->format,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = disp->render_pass,
        .framebuffer = disp->framebuffer,
        .renderArea.extent.width = disp->width,
        .renderArea.extent.height = disp->height,
    };
    vkCmdBeginRenderPass(cmd, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      disp->pipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            disp->pipeline_layout, 0, 1, &disp->descriptor_set,
                            0, NULL);

    VkViewport viewport = {
        .width = disp->width,
        .height = disp->height,
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .extent.width = disp->width,
        .extent.height = disp->height,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, disp->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, disp->display_frag->push_constants.total_size,
                       disp->display_frag->push_constants.allocation);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, disp->image,
                                      disp->format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
#if HAVE_IOSURFACE_SHARING
    if (xemu_present_is_metal() && r->present_timeline_event) {
        /*
         * Async submit: signal the exported timeline event instead of
         * blocking the PFIFO thread in vkWaitForFences. MetalFX / the
         * UI present pass encode GPU-side waits on this value before
         * sampling the compositor output.
         */
        uint64_t value = r->present_timeline_value + 1;
        pgraph_vk_end_single_time_commands_async(pg, cmd,
                                                 r->present_timeline, value);
        r->present_timeline_value = value;
        metalfx_set_input_wait(r->present_timeline_event, value);
    } else
#endif
    {
        pgraph_vk_end_single_time_commands(pg, cmd);
    }
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);

    disp->draw_time = surface->draw_time;
}

static void create_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &r->display.sampler));
}

static void destroy_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroySampler(r->device, r->display.sampler, NULL);
    r->display.sampler = VK_NULL_HANDLE;
}

#if HAVE_IOSURFACE_SHARING
/*
 * Async compositor handoff: create a timeline VkSemaphore and export
 * its backing MTLSharedEvent (VK_EXT_metal_objects). When this
 * succeeds, render_display submits the compositor pass without the
 * synchronous fence wait; MetalFX and the UI present pass order
 * GPU-side against the exported event instead. One-time probe with
 * fallback to the synchronous path (the API class that previously
 * deadlocked MoltenVK was *per-frame* depth-image export; this is a
 * single semaphore export at init with the device idle).
 */
static void create_present_timeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->present_timeline = VK_NULL_HANDLE;
    r->present_timeline_event = NULL;
    r->present_timeline_value = 0;

    if (!xemu_present_is_metal() || !r->timeline_semaphore_enabled ||
        !r->metal_objects_extension_enabled) {
        return;
    }

    PFN_vkExportMetalObjectsEXT export_fn =
        (PFN_vkExportMetalObjectsEXT)r->export_metal_objects_fn;
    if (!export_fn) {
        fprintf(stderr,
                "nv2a: vkExportMetalObjectsEXT unavailable; compositor "
                "submit stays synchronous\n");
        return;
    }

    VkExportMetalObjectCreateInfoEXT export_create = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .exportObjectType =
            VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT,
    };
    VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &export_create,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
    };
    if (vkCreateSemaphore(r->device, &create_info, NULL,
                          &r->present_timeline) != VK_SUCCESS) {
        r->present_timeline = VK_NULL_HANDLE;
        fprintf(stderr,
                "nv2a: exportable timeline semaphore creation failed; "
                "compositor submit stays synchronous\n");
        return;
    }

    VkExportMetalSharedEventInfoEXT shared_event_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
        .semaphore = r->present_timeline,
    };
    VkExportMetalObjectsInfoEXT export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &shared_event_info,
    };
    export_fn(r->device, &export_info);

    if (shared_event_info.mtlSharedEvent) {
        r->present_timeline_event =
            (void *)CFRetain(shared_event_info.mtlSharedEvent);
        fprintf(stderr,
                "nv2a: async compositor submit enabled "
                "(timeline MTLSharedEvent exported)\n");
    } else {
        vkDestroySemaphore(r->device, r->present_timeline, NULL);
        r->present_timeline = VK_NULL_HANDLE;
        fprintf(stderr,
                "nv2a: MTLSharedEvent export returned nil; compositor "
                "submit stays synchronous\n");
    }
}

static void destroy_present_timeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->present_timeline_event) {
        CFRelease(r->present_timeline_event);
        r->present_timeline_event = NULL;
    }
    if (r->present_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(r->device, r->present_timeline, NULL);
        r->present_timeline = VK_NULL_HANDLE;
    }
    r->present_timeline_value = 0;
}

/*
 * Probe whether MoltenVK can export the MTLTexture backing a VkImage
 * (VK_EXT_metal_objects). When it can, the compositor image is
 * created without an IOSurface and handed to MetalFX / the UI as a
 * texture directly — removing the macOS 26 >1920px BGRA IOSurface
 * constraint from the present chain (MetalFX then engages at
 * surface_scale=4). One-time, device idle, tiny throwaway image.
 */
static void probe_metal_texture_export(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->metal_texture_export_enabled = false;

    if (!xemu_present_is_metal() || !r->metal_objects_extension_enabled ||
        !r->export_metal_objects_fn) {
        return;
    }
    PFN_vkExportMetalObjectsEXT export_fn =
        (PFN_vkExportMetalObjectsEXT)r->export_metal_objects_fn;

    VkExportMetalObjectCreateInfoEXT export_create = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &export_create,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { 16, 16, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkCreateImage(r->device, &image_info, NULL, &image) != VK_SUCCESS) {
        goto out;
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(r->device, image, &reqs);
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = pgraph_vk_get_memory_type(
            pg, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    if (vkAllocateMemory(r->device, &alloc_info, NULL, &memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(r->device, image, memory, 0) != VK_SUCCESS) {
        goto out;
    }

    {
        VkExportMetalTextureInfoEXT tex_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
            .image = image,
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT export_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &tex_info,
        };
        export_fn(r->device, &export_info);
        r->metal_texture_export_enabled = tex_info.mtlTexture != NULL;
    }

out:
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(r->device, image, NULL);
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, memory, NULL);
    }
    fprintf(stderr,
            r->metal_texture_export_enabled ?
                "nv2a: compositor MTLTexture export enabled "
                "(IOSurface-free present chain)\n" :
                "nv2a: MTLTexture export unavailable; present chain "
                "keeps IOSurface\n");
}
#endif /* HAVE_IOSURFACE_SHARING */

void pgraph_vk_init_display(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->display.format = VK_FORMAT_R8G8B8A8_UNORM;
#if HAVE_IOSURFACE_SHARING
    if (r->metal_objects_extension_enabled) {
        r->display.format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    r->export_metal_objects_fn = (void *)vkGetDeviceProcAddr(
        r->device, "vkExportMetalObjectsEXT");
    create_present_timeline(pg);
    probe_metal_texture_export(pg);
#endif

    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    create_render_pass(pg);
    create_display_pipeline(pg);
    create_surface_sampler(pg);
}

void pgraph_vk_finalize_display(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

#if HAVE_IOSURFACE_SHARING
    metalfx_destroy();
    metalfx_temporal_destroy();
    metalfx_interpolation_destroy();

    if (r->display.interp_prev_surface) {
        CFRelease((IOSurfaceRef)r->display.interp_prev_surface);
        r->display.interp_prev_surface = NULL;
    }
    if (r->display.interp_cur_surface) {
        CFRelease((IOSurfaceRef)r->display.interp_cur_surface);
        r->display.interp_cur_surface = NULL;
    }
    r->display.interp_remaining = 0;

    /* Drain any pending async compositor submit before destroying the
     * semaphore (the fence is still signaled by async submits). */
    if (r->aux_async_pending) {
        vkWaitForFences(r->device, 1, &r->aux_fence, VK_TRUE,
                        5ull * 1000 * 1000 * 1000);
        r->aux_async_pending = false;
    }
    destroy_present_timeline(pg);
#endif

    destroy_pvideo_image(pg);

    if (r->display.image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

    destroy_surface_sampler(pg);
    destroy_display_pipeline(pg);
    destroy_render_pass(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
}

void pgraph_vk_render_display(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height) {
        return;
    }

    PGRAPHVkDisplayState *disp = &r->display;
    if (disp->image && surface->draw_time == disp->draw_time) {
#if HAVE_IOSURFACE_SHARING
        /*
         * No new frame.
         *
         * Metal backend: step through the interpolation presentation
         * queue, paced at frame_period / interp_mode. The interpolated
         * frame(s) were generated and published when the real frame
         * arrived (they lie temporally BEFORE it); the real frame
         * itself was held back and is published as the final step of
         * the cycle. Without this ordering+pacing, the real frame
         * appeared first and the midpoint frame after it — motion ran
         * forward-backward-forward (frames visibly out of order).
         */
        if (xemu_present_is_metal()) {
            uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            uint64_t step = disp->interp_step_ns ? disp->interp_step_ns
                                                 : 8000000ull;
            if (now - disp->last_present_step_ns >= step) {
                if (disp->interp_remaining > 0 &&
                    disp->interp_midpoint_texture) {
                    /*
                     * Re-present the cycle's cached midpoint (generated
                     * once when the real frame arrived). The API has no
                     * phase parameter — regenerating from the same pair
                     * produced the identical image while violating the
                     * interpolator's prevColorTexture history contract.
                     */
                    display_set_present_texture(
                        disp, (void *)CFRetain(disp->interp_midpoint_texture));
                    disp->present_event = metalfx_present_event();
                    disp->present_event_value =
                        disp->interp_midpoint_event_value;
                    disp->interp_remaining--;
                    disp->last_present_step_ns = now;
                    disp->present_duration_ns = disp->interp_step_ns;
                } else if (disp->pending_real_texture) {
                    /* Final step of the cycle: show the real frame */
                    display_set_present_texture(disp,
                                                disp->pending_real_texture);
                    disp->pending_real_texture = NULL;
                    disp->present_event = metalfx_present_event();
                    disp->present_event_value =
                        disp->pending_real_event_value;
                    disp->last_present_step_ns = now;
                    disp->present_duration_ns = disp->interp_step_ns;
                } else {
                    disp->interp_remaining = 0;
                }
            }
        } else if (disp->interp_remaining > 0 &&
            disp->interp_prev_surface && disp->interp_cur_surface &&
            metalfx_interpolation_is_supported() &&
            disp->gl_texture_id) {
            /*
             * GL backend deferred generation.
             *
             * MTLFXFrameInterpolator.deltaTime expects the wall-clock
             * interval in seconds between the two input frames (used to
             * scale motion-vector magnitudes). Feed the delta between
             * the two CFRetain capture timestamps, clamped to a sane
             * range to guard against pause / unpause jumps and the
             * first-frame case (prev_ns == 0).
             */
            float delta_sec =
                (float)(disp->interp_cur_surface_ns -
                        disp->interp_prev_surface_ns) / 1.0e9f;
            if (delta_sec < 1.0f / 240.0f) delta_sec = 1.0f / 240.0f;
            if (delta_sec > 1.0f / 10.0f)  delta_sec = 1.0f / 10.0f;

            if (metalfx_interpolation_generate(
                    (IOSurfaceRef)disp->interp_prev_surface,
                    (IOSurfaceRef)disp->interp_cur_surface,
                    NULL, NULL, delta_sec)) {
                IOSurfaceRef interp =
                    metalfx_interpolation_get_output_surface();
                uint32_t interp_id = interp ? IOSurfaceGetID(interp) : 0;
                if (interp &&
                    (interp_id != disp->last_cgl_surface_id ||
                     disp->interp_width != disp->last_cgl_width ||
                     disp->interp_height != disp->last_cgl_height)) {
                    CGLContextObj cgl_ctx = CGLGetCurrentContext();
                    if (cgl_ctx) {
                        glBindTexture(GL_TEXTURE_RECTANGLE,
                                      disp->gl_texture_id);
                        CGLTexImageIOSurface2D(
                            cgl_ctx, GL_TEXTURE_RECTANGLE, GL_RGBA,
                            disp->interp_width, disp->interp_height,
                            GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                            interp, 0);
                        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                        disp->last_cgl_surface_id = interp_id;
                        disp->last_cgl_width = disp->interp_width;
                        disp->last_cgl_height = disp->interp_height;
                    }
                }
                if (interp) CFRelease(interp);
                disp->interp_remaining--;
            } else {
                /*
                 * Interpolation generate failed; skip this slot instead
                 * of busy-retrying every sync. Without this, has_interp_work
                 * stays true and we bypass the 8 ms throttle repeatedly.
                 */
                disp->interp_remaining--;
            }
        }
#endif
        return;
    }

    unsigned int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);

    /* Adjust viewport height for interlaced mode, used only in 1080i */
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] != NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }

    pgraph_apply_scaling_factor(pg, &width, &height);

    if (!disp->image || disp->width != width || disp->height != height) {
        create_display_image(pg, width, height);
        disp->pvideo.last_uploaded_state = (PvideoState){ 0 };
#if HAVE_IOSURFACE_SHARING
        metalfx_temporal_reset();
#endif
    }

    if (!disp->image) {
        return;
    }

#if HAVE_IOSURFACE_SHARING
    /*
     * Async MetalFX (Metal presentation backend): the previous frame's
     * upscale may still be reading disp->iosurface on the MetalFX
     * queue. Drain it before the compositor pass below overwrites the
     * surface, or the upscaler samples a torn mix of two frames.
     * Near-zero cost in steady state (the work finished during the
     * preceding >=8 ms sync interval).
     */
    if (xemu_present_is_metal()) {
        metalfx_drain_inflight();
    }
#endif

    render_display(pg, surface);

#if HAVE_IOSURFACE_SHARING
    if ((!disp->iosurface && !disp->mtl_texture) ||
        !r->metal_objects_extension_enabled) {
        goto done_metalfx;
    }

    {
        /* Texture-export mode: current_surface is NULL and the
         * compositor frame is disp->mtl_texture. */
        IOSurfaceRef current_surface = (IOSurfaceRef)disp->iosurface;
        IOSurfaceRef present_surface = current_surface;

        /* --- MetalFX Upscaling --- */
        int mfx_mode = 0;
        if (g_config.display.metalfx_mode == CONFIG_DISPLAY_METALFX_MODE_SPATIAL) {
            mfx_mode = 1;
        } else if (g_config.display.metalfx_mode == CONFIG_DISPLAY_METALFX_MODE_TEMPORAL) {
            mfx_mode = 2;
        }

        /*
         * macOS 26 IOSurface bug: BGRA8 surfaces wider than ~1920px get
         * wrong bytesPerRow, breaking Metal texture wrapping. Under the
         * GL presentation backend (IOSurface outputs) MetalFX only runs
         * when the display IOSurface is at a safe width.
         *
         * At 1x (640x480): temporal 640x480 -> 1920x1440, interp at 1920x1440
         * At 2x (1280x960): temporal 1280x960 -> 1920x1440, interp at 1920x1440
         * At 4x (2560x1920): skipped (display too wide), GL scales directly
         *
         * Under the Metal presentation backend, MetalFX outputs are
         * private MTLTextures (no IOSurface) so the 1920px cap is
         * lifted: the output targets the CAMetalLayer's pixel size
         * (aspect-fit), e.g. 1280x960 -> 2880x2160 on a 4K panel.
         * Only the *input* IOSurface (MoltenVK compositor output)
         * keeps the width constraint; texture_from_iosurface() bails
         * gracefully on affected surfaces.
         */
        #define METALFX_SAFE_MAX_OUTPUT 1920

        bool metal_native = xemu_present_is_metal();
        void *present_tex = NULL; /* retained id<MTLTexture> */
        bool temporal_used = false;

        int out_w = 0, out_h = 0;
        if (mfx_mode > 0 && metal_native) {
            int layer_w = 0, layer_h = 0;
            xemu_metal_layer_pixel_size(&layer_w, &layer_h);
            if (layer_w > (int)disp->width && layer_h > (int)disp->height) {
                double sx = (double)layer_w / (double)disp->width;
                double sy = (double)layer_h / (double)disp->height;
                double s = (sx < sy) ? sx : sy;
                out_w = ((int)((double)disp->width * s)) & ~1;
                out_h = ((int)((double)disp->height * s)) & ~1;
            }
        }
        if (mfx_mode > 0 && !out_w &&
            (int)disp->width <= METALFX_SAFE_MAX_OUTPUT) {
            out_w = METALFX_SAFE_MAX_OUTPUT;
            out_h = (int)disp->height * out_w / (int)disp->width;
            if (out_h > METALFX_SAFE_MAX_OUTPUT) {
                out_h = METALFX_SAFE_MAX_OUTPUT;
            }
        }

        if (out_w > (int)disp->width || out_h > (int)disp->height) {
            IOSurfaceRef upscaled = NULL;

            if (mfx_mode == 2 && metalfx_temporal_is_supported()) {
                if (metalfx_temporal_init(disp->width, disp->height,
                                         out_w, out_h)) {
                    /*
                     * TODO: Provide real depth. With MTLTexture export
                     * now proven (metal_texture_export_enabled), the
                     * viable path is exporting the zeta surface's
                     * texture at creation (same chain as the
                     * compositor image) and feeding its depth plane —
                     * no IOSurface-backed zeta or CPU round-trip
                     * needed. Needs zeta-binding selection at sync
                     * time + projection-range mapping; until then the
                     * temporal scaler uses synthetic luminance depth.
                     */
                    IOSurfaceRef depth_surface = NULL;
                    bool ok = disp->mtl_texture ?
                        metalfx_temporal_upscale_tex(disp->mtl_texture) :
                        metalfx_temporal_upscale(current_surface,
                                                 depth_surface);
                    if (ok) {
                        if (metal_native) {
                            present_tex =
                                metalfx_temporal_get_output_texture();
                        }
                        if (!present_tex) {
                            upscaled = metalfx_temporal_get_output_surface();
                        }
                        temporal_used = present_tex != NULL || upscaled != NULL;
                    }
                }
            }

            if (!upscaled && !present_tex) {
                if (metalfx_init(disp->width, disp->height, out_w, out_h)) {
                    bool ok = disp->mtl_texture ?
                        metalfx_upscale_tex(disp->mtl_texture) :
                        metalfx_upscale(current_surface);
                    if (ok) {
                        if (metal_native) {
                            present_tex = metalfx_get_output_texture();
                        }
                        if (!present_tex) {
                            upscaled = metalfx_get_output_surface();
                        }
                    }
                }
            }

            if (upscaled) {
                present_surface = upscaled;
            }
        }

        /* --- Frame Interpolation (runs independently of upscaling) --- */
        int interp_mode = 0;
        if (g_config.display.frame_interpolation ==
                CONFIG_DISPLAY_FRAME_INTERPOLATION_2X) {
            interp_mode = 2;
        } else if (g_config.display.frame_interpolation ==
                       CONFIG_DISPLAY_FRAME_INTERPOLATION_4X) {
            interp_mode = 4;
        }

        /*
         * Set up deferred frame interpolation for successive sync calls.
         * 2x: 1 interpolated frame at dt=0.5 (30fps -> 60fps)
         * 4x: 3 interpolated frames at dt=0.25, 0.5, 0.75 (30fps -> 120fps)
         *
         * Metal-native mode: interp_prev/cur_surface hold retained
         * id<MTLTexture> handles (MetalFX ring entries, or a wrap of
         * the base compositor IOSurface). The ring guarantees prev and
         * cur reference *distinct* frames — under the single shared
         * IOSurface output they aliased the same content.
         */
        if (interp_mode >= 2 && metalfx_interpolation_is_supported()) {
            int iw = 0, ih = 0;
            if (present_tex) {
                metalfx_texture_dims(present_tex, &iw, &ih);
            } else if (present_surface) {
                iw = (int)IOSurfaceGetWidth(present_surface);
                ih = (int)IOSurfaceGetHeight(present_surface);
            } else if (disp->mtl_texture) {
                metalfx_texture_dims(disp->mtl_texture, &iw, &ih);
            }

            if (iw > 0 && ih > 0 &&
                metalfx_interpolation_init(iw, ih,
                                           metal_native && temporal_used)) {
                void *cur = NULL;
                if (metal_native) {
                    if (present_tex) {
                        cur = (void *)CFRetain(present_tex);
                    } else if (disp->mtl_texture) {
                        cur = (void *)CFRetain(disp->mtl_texture);
                    } else {
                        cur = metalfx_wrap_iosurface_texture(present_surface);
                    }
                } else {
                    cur = (void *)CFRetain(present_surface);
                }

                if (cur) {
                    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

                    /*
                     * Hitch guard (Metal backend): if this frame's
                     * capture gap spikes versus the recent average
                     * (load hitch, level transition, scene cut), skip
                     * interpolation for this cycle and reset history —
                     * blending across a content jump produces garbage
                     * ghost frames.
                     */
                    bool hitch = false;
                    if (metal_native && disp->interp_cur_surface_ns) {
                        uint64_t gap_ns =
                            now_ns - disp->interp_cur_surface_ns;
                        uint64_t avg = disp->interp_avg_gap_ns;
                        hitch = gap_ns > 50000000ull ||
                                (avg && gap_ns > (avg * 5) / 2);
                        /* Don't pollute the average with spike samples */
                        uint64_t sample = hitch ? avg : gap_ns;
                        if (sample) {
                            disp->interp_avg_gap_ns =
                                avg ? (avg * 7 + sample) / 8 : sample;
                        }
                    }

                    /* Release old prev, shift current to prev */
                    if (disp->interp_prev_surface) {
                        CFRelease(disp->interp_prev_surface);
                    }
                    disp->interp_prev_surface = disp->interp_cur_surface;
                    disp->interp_prev_surface_ns =
                        disp->interp_cur_surface_ns;
                    disp->interp_cur_surface = cur;
                    disp->interp_cur_surface_ns = now_ns;

                    if (hitch) {
                        disp->interp_remaining = 0;
                        metalfx_interpolation_reset();
                    } else if (disp->interp_prev_surface) {
                        disp->interp_remaining = (interp_mode == 4) ? 3 : 1;
                        disp->interp_width = iw;
                        disp->interp_height = ih;
                    }
                }
            }
        }

        /*
         * Publish the frame. Metal backend: hand the MetalFX output
         * texture (or the base IOSurface) to the UI thread, along
         * with the shared-event value its producer signals. GL
         * backend: bind to the CGL rect texture (skip if the same
         * IOSurface is already bound). Key on IOSurfaceGetID, not the
         * IOSurfaceRef pointer: the pointer can be reused after
         * release, producing a false-cache hit and stale display.
         */
        if (metal_native) {
            uint64_t real_event_value = metalfx_present_event_last_value();
            bool interp_first_presented = false;

            /*
             * Frame interpolation: the interpolated frame lies
             * temporally BETWEEN prev and cur, so it must be shown
             * BEFORE the new real frame. Generate it now, publish it
             * as this sync's frame, and hold the real frame back for
             * the final paced step of the cycle. Only meaningful on
             * the MetalFX texture path: the base-IOSurface path's
             * prev/cur wrap the same reused surface (identical
             * content), so interpolation degenerates there anyway.
             */
            if (disp->interp_remaining > 0 && present_tex &&
                disp->interp_prev_surface && disp->interp_cur_surface &&
                metalfx_interpolation_is_supported()) {
                float delta_sec =
                    (float)(disp->interp_cur_surface_ns -
                            disp->interp_prev_surface_ns) / 1.0e9f;
                if (delta_sec < 1.0f / 240.0f) delta_sec = 1.0f / 240.0f;
                if (delta_sec > 1.0f / 10.0f)  delta_sec = 1.0f / 10.0f;

                if (metalfx_interpolation_generate_tex(
                        disp->interp_prev_surface,
                        disp->interp_cur_surface, delta_sec)) {
                    void *itex = metalfx_interpolation_get_output_texture();
                    if (itex) {
                        /*
                         * Cache the midpoint for the remaining steps
                         * of a 4x cycle (the API has no phase
                         * parameter; regenerating from the same pair
                         * would produce the identical image and
                         * violate the history contract).
                         */
                        if (disp->interp_midpoint_texture) {
                            CFRelease(disp->interp_midpoint_texture);
                        }
                        disp->interp_midpoint_texture =
                            (void *)CFRetain(itex);
                        disp->interp_midpoint_event_value =
                            metalfx_present_event_last_value();

                        display_set_present_texture(disp, itex);
                        disp->present_event = metalfx_present_event();
                        disp->present_event_value =
                            disp->interp_midpoint_event_value;
                        interp_first_presented = true;

                        /* Hold the real frame; pace the cycle's steps
                         * across the captured frame period. */
                        if (disp->pending_real_texture) {
                            CFRelease(disp->pending_real_texture);
                        }
                        disp->pending_real_texture = present_tex;
                        disp->pending_real_event_value = real_event_value;
                        present_tex = NULL;
                        disp->interp_remaining--;
                        disp->interp_step_ns =
                            (uint64_t)(delta_sec * 1.0e9f) /
                            (uint64_t)interp_mode;
                        disp->last_present_step_ns =
                            qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                        disp->present_duration_ns = disp->interp_step_ns;
                    }
                }
                if (!interp_first_presented) {
                    /* Generation failed: present the real frame now and
                     * cancel the cycle (no midpoint to step through). */
                    disp->interp_remaining = 0;
                }
            }

            if (!interp_first_presented) {
                if (present_tex) {
                    /* ownership transferred to the display state */
                    display_set_present_texture(disp, present_tex);
                    present_tex = NULL;
                    disp->present_event = metalfx_present_event();
                    disp->present_event_value = real_event_value;
                } else {
                    if (present_surface) {
                        display_set_present_surface(disp, present_surface);
                    } else if (disp->mtl_texture) {
                        /* Texture-export mode: publish the exported
                         * compositor texture directly. */
                        display_set_present_texture(
                            disp, (void *)CFRetain(disp->mtl_texture));
                    }
                    if (r->present_timeline_event) {
                        /* Base compositor frame, async submit: wait
                         * the exported compositor timeline. */
                        disp->present_event = r->present_timeline_event;
                        disp->present_event_value =
                            r->present_timeline_value;
                    } else {
                        disp->present_event = metalfx_present_event();
                        disp->present_event_value = real_event_value;
                    }
                }
                /* No interp step pending before this frame; drop any
                 * stale held frame from a previous cycle. */
                if (disp->pending_real_texture) {
                    CFRelease(disp->pending_real_texture);
                    disp->pending_real_texture = NULL;
                }
            }
            if (present_surface) {
                disp->last_cgl_surface_id = IOSurfaceGetID(present_surface);
                disp->last_cgl_width =
                    (int)IOSurfaceGetWidth(present_surface);
                disp->last_cgl_height =
                    (int)IOSurfaceGetHeight(present_surface);
            }
        } else if (disp->gl_texture_id) {
            int surf_w = (int)IOSurfaceGetWidth(present_surface);
            int surf_h = (int)IOSurfaceGetHeight(present_surface);
            uint32_t surf_id = IOSurfaceGetID(present_surface);
            if (surf_id != disp->last_cgl_surface_id ||
                surf_w != disp->last_cgl_width ||
                surf_h != disp->last_cgl_height) {
                CGLContextObj cgl_ctx = CGLGetCurrentContext();
                if (cgl_ctx) {
                    glBindTexture(GL_TEXTURE_RECTANGLE, disp->gl_texture_id);
                    CGLTexImageIOSurface2D(
                        cgl_ctx, GL_TEXTURE_RECTANGLE, GL_RGBA,
                        surf_w, surf_h,
                        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                        present_surface, 0);
                    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                    disp->last_cgl_surface_id = surf_id;
                    disp->last_cgl_width = surf_w;
                    disp->last_cgl_height = surf_h;
                }
            }
        }

        if (present_surface && present_surface != current_surface) {
            CFRelease(present_surface);
        }
    }
done_metalfx: (void)0;
#endif
}
