/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/compiler.h"
#include "qemu/timer.h"
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "hw/xbox/nv2a/nsprof.h"

#if HAVE_IOSURFACE_SHARING
#include <CoreFoundation/CoreFoundation.h>
#endif

const int num_invalid_surfaces_to_keep = 128;
const int max_surface_frame_time_delta = 5;

static void surface_ranges_insert(PGRAPHVkState *r, SurfaceBinding *s)
{
    hwaddr start = s->vram_addr;
    hwaddr end = s->vram_addr + s->size;

    if (r->surface_range_count >= r->surface_range_capacity) {
        r->surface_range_capacity = MAX(32, r->surface_range_capacity * 2);
        r->surface_ranges = g_realloc_n(r->surface_ranges,
            r->surface_range_capacity, sizeof(r->surface_ranges[0]));
    }

    /*
     * lower_bound on start — array is kept sorted by start. Overlapping
     * surfaces at identical starts are rare but not forbidden; the
     * stable insertion point is the first index whose start >= our
     * start, matching the original linear probe's semantics.
     */
    int lo = 0, hi = r->surface_range_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r->surface_ranges[mid].start < start) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    int pos = lo;

    memmove(&r->surface_ranges[pos + 1], &r->surface_ranges[pos],
            (r->surface_range_count - pos) * sizeof(r->surface_ranges[0]));
    r->surface_ranges[pos].start = start;
    r->surface_ranges[pos].end = end;
    r->surface_ranges[pos].surface = s;
    r->surface_range_count++;

    /*
     * Fix up surface_range_slot on each shifted entry and on the newly
     * inserted surface. memmove itself is O(count-pos); the fixup is
     * the same cost, so no asymptotic regression relative to the old
     * linear-probe insert.
     */
    s->surface_range_slot = pos;
    for (int j = pos + 1; j < r->surface_range_count; j++) {
        r->surface_ranges[j].surface->surface_range_slot = j;
    }
}

static void surface_ranges_remove(PGRAPHVkState *r, SurfaceBinding *s)
{
    int slot = s->surface_range_slot;
    if (slot < 0 || slot >= r->surface_range_count ||
        r->surface_ranges[slot].surface != s) {
        /*
         * Should never happen — insert is the only writer of
         * surface_range_slot and sets it to a valid index. Assert
         * loudly in debug to catch any missed init site; bail silently
         * in release so a stale slot can't corrupt the ranges table.
         * Emit one warning per process so a release-build bug is at
         * least observable without log spam.
         */
        nv2a_vk_assert(false && "surface_range_slot invariant broken");
        static bool warned;
        if (!warned) {
            warned = true;
            fprintf(stderr,
                    "nv2a/pgraph/vk: surface_range_slot invariant broken "
                    "(slot=%d, count=%d, s=%p) — range-table entry leaked\n",
                    slot, r->surface_range_count, (void *)s);
        }
        return;
    }

    memmove(&r->surface_ranges[slot], &r->surface_ranges[slot + 1],
            (r->surface_range_count - slot - 1) *
            sizeof(r->surface_ranges[0]));
    r->surface_range_count--;

    for (int j = slot; j < r->surface_range_count; j++) {
        r->surface_ranges[j].surface->surface_range_slot = j;
    }
    s->surface_range_slot = -1;
}

static SurfaceBinding *surface_ranges_find_containing(PGRAPHVkState *r,
                                                       hwaddr addr)
{
    int lo = 0, hi = r->surface_range_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr < r->surface_ranges[mid].start) {
            hi = mid - 1;
        } else if (addr >= r->surface_ranges[mid].end) {
            lo = mid + 1;
        } else {
            return r->surface_ranges[mid].surface;
        }
    }
    return NULL;
}

void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    // FIXME: It's just flush
    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor; // FIXME: Move internal to renderer
}

void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    pg->surface_scale_factor = MAX(factor, 1);

    /* Scale changed underneath the cached scaled dims. */
    if (pg->vk_renderer_state) {
        pg->vk_renderer_state->cached_scaled_binding_dim_valid = false;
    }
}

// FIXME: Move to common
static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

static void memcpy_image(void *dst, void const *src, int dst_stride,
                         int src_stride, int height)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, (size_t)dst_stride * height);
        return;
    }

    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t const *src_ptr = (uint8_t *)src;

    size_t copy_stride = MIN(src_stride, dst_stride);

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, copy_stride);
        dst_ptr += dst_stride;
        src_ptr += src_stride;
    }
}

void pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                   hwaddr start, hwaddr size,
                                                   int nsprof_ev)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    hwaddr range_end = start + size;

    for (int i = 0; i < r->surface_range_count; i++) {
        if (r->surface_ranges[i].start >= range_end) {
            break;
        }
        if (r->surface_ranges[i].end <= start) {
            continue;
        }
        if (r->surface_ranges[i].surface->draw_dirty) {
            nsprof_event(nsprof_ev);
        }
        pgraph_vk_surface_download_if_dirty(
            container_of(pg, NV2AState, pgraph),
            r->surface_ranges[i].surface);
    }
}

static void download_surface_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!surface->width || !surface->height) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
    int64_t nsprof_t0 = nsprof_begin();

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    nv2a_vk_assert(surface->color ||
                   use_compute_to_convert_depth_stencil_format ||
                   surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM);

    bool compute_needs_finish = (use_compute_to_convert_depth_stencil_format &&
                                 pgraph_vk_compute_needs_finish(r));

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    } else if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    bool downscale = (pg->surface_scale_factor != 1);

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    // Read surface into memory
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        nv2a_vk_assert(pg->surface_scale_factor == 1 || downscale);
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int num_copy_regions = 1;
    VkBufferImageCopy copy_regions[2];
    copy_regions[0] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
    };

    VkImage surface_image_loc;
    if (downscale && !use_compute_to_convert_depth_stencil_format) {
        copy_regions[0].imageExtent =
            (VkExtent3D){ surface->width, surface->height, 1 };

        if (surface->image_scratch_current_layout !=
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image_scratch, surface->host_fmt.vk_format,
                surface->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            surface->image_scratch_current_layout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }

        VkImageBlit blit_region = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},
        };

        vkCmdBlitImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       surface->image_scratch,
                       surface->image_scratch_current_layout, 1, &blit_region,
                       surface->color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        surface_image_loc = surface->image_scratch;
    } else {
        copy_regions[0].imageExtent =
            (VkExtent3D){ scaled_width, scaled_height, 1 };
        surface_image_loc = surface->image;
    }

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        size_t depth_size = scaled_width * scaled_height * 4;
        copy_regions[num_copy_regions++] = (VkBufferImageCopy){
            .bufferOffset = ROUND_UP(
                depth_size,
                r->device_props.limits.minStorageBufferOffsetAlignment),
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
        };
    }

    size_t image_copy_size;
    if (num_copy_regions > 1) {
        image_copy_size = copy_regions[1].bufferOffset +
                          (size_t)copy_regions[1].imageExtent.width *
                          copy_regions[1].imageExtent.height;
    } else {
        image_copy_size = (size_t)copy_regions[0].imageExtent.width *
                          copy_regions[0].imageExtent.height *
                          surface->host_fmt.host_bytes_per_pixel;
    }

    //
    // Copy image to staging buffer, or to compute_dst if we need to pack it
    //

    nv2a_vk_bounds_check((surface->host_fmt.host_bytes_per_pixel *
                    surface->width * surface->height) <=
           r->storage_buffers[BUFFER_STAGING_DST].buffer_size);

    int copy_buffer_idx = use_compute_to_convert_depth_stencil_format ?
                             BUFFER_COMPUTE_DST :
                             BUFFER_STAGING_DST;
    VkBuffer copy_buffer = r->storage_buffers[copy_buffer_idx].buffer;

    {
        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = image_copy_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);
    }
    vkCmdCopyImageToBuffer(cmd, surface_image_loc,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                           num_copy_regions, copy_regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    // FIXME: Verify output of depth stencil conversion
    // FIXME: Track current layout and only transition when required

    if (use_compute_to_convert_depth_stencil_format) {
        size_t bytes_per_pixel = 4;
        size_t packed_size =
            downscale ? (surface->width * surface->height * bytes_per_pixel) :
                        (scaled_width * scaled_height * bytes_per_pixel);

        //
        // Pack the depth-stencil image into compute_src buffer
        //

        VkBufferMemoryBarrier pre_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = image_copy_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_src_barrier, 0, NULL);

        VkBuffer pack_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

        VkBufferMemoryBarrier pre_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(pg, surface, cmd, copy_buffer, pack_buffer,
                                     downscale);

        VkBufferMemoryBarrier post_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = image_copy_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_dst_barrier, 0, NULL);

        //
        // Copy packed image over to staging buffer for host download
        //

        copy_buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer;

        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);

        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, pack_buffer, copy_buffer, 1, &buffer_copy_region);

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);
    }

    //
    // Download image data to host
    //

    size_t download_size = (size_t)surface->width *
                           surface->fmt.bytes_per_pixel * surface->height;

    VkBufferMemoryBarrier post_copy_dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer,
        .size = download_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &post_copy_dst_barrier, 0, NULL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);
    if (!r->storage_buffers[BUFFER_STAGING_DST].is_coherent) {
        vmaInvalidateAllocation(r->allocator,
                                r->storage_buffers[BUFFER_STAGING_DST].allocation,
                                0, download_size);
    }

    memcpy_image(gl_read_buf, r->storage_buffers[BUFFER_STAGING_DST].mapped,
                 surface->pitch,
                 surface->width * surface->fmt.bytes_per_pixel,
                 surface->height);

    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                     surface->pitch, surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        g_free(swizzle_buf);
    }
    nsprof_end(NSPROF_SURF_DOWNLOAD, nsprof_t0);
}

static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }

    // FIXME: Respect write enable at last TOU?

    download_surface_to_buffer(d, surface, d->vram_ptr + surface->vram_addr);

    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);

    surface->download_pending = false;
    surface->draw_dirty = false;
}

void pgraph_vk_wait_for_surface_download(SurfaceBinding *surface)
{
    NV2AState *d = g_nv2a;

    if (qatomic_read(&surface->draw_dirty)) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&d->pgraph.vk_renderer_state->downloads_complete);
        qatomic_set(&surface->download_pending, true);
        qatomic_set(&d->pgraph.vk_renderer_state->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.vk_renderer_state->downloads_complete);
    }
}

void pgraph_vk_process_pending_downloads(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        download_surface(d, surface, false);
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_vk_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        pgraph_vk_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;

    bool had_bql = bql_locked();
    if (had_bql) {
        bql_unlock();
    }

    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    bool wait_for_downloads = false;
    hwaddr range_end = addr + len;

    for (int i = 0; i < r->surface_range_count; i++) {
        if (r->surface_ranges[i].start >= range_end) {
            break;
        }
        if (r->surface_ranges[i].end <= addr) {
            continue;
        }
        SurfaceBinding *surface = r->surface_ranges[i].surface;
        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            nsprof_event(write ? NSPROF_EV_SDOWN_ACCESS_W
                               : NSPROF_EV_SDOWN_ACCESS_R);
            surface->download_pending = true;
            wait_for_downloads = true;
        }

        if (write) {
            surface->upload_pending = true;
        }
    }

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }

    if (had_bql) {
        bql_lock();
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
}

static void bind_surface(PGRAPHVkState *r, SurfaceBinding *surface)
{
    if (surface->color) {
        r->color_binding = surface;
    } else {
        r->zeta_binding = surface;
    }

    r->framebuffer_dirty = true;
    r->render_pass_state_dirty = true;
}

static void unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (color) {
        if (r->color_binding) {
            r->color_binding = NULL;
            r->framebuffer_dirty = true;
            r->render_pass_state_dirty = true;
        }
    } else {
        if (r->zeta_binding) {
            r->zeta_binding = NULL;
            r->framebuffer_dirty = true;
            r->render_pass_state_dirty = true;
        }
    }
}

/*
 * True while the surface's image may be referenced by the currently
 * recording (unsubmitted) command buffer. Such an image must not be
 * migrated to a new binding or destroyed: draws recorded against it
 * would read the wrong content once submitted. Images referenced only
 * by *submitted* work are safe to reuse — MoltenVK's single queue
 * executes later submissions after earlier ones.
 */
static bool surface_image_in_recording_cb(PGRAPHVkState *r,
                                          SurfaceBinding const *surface)
{
    return r->in_command_buffer &&
           surface->draw_time >= r->command_buffer_start_time;
}

static void invalidate_surface_full(NV2AState *d, SurfaceBinding *surface,
                                    bool allow_deferred)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    /*
     * allow_deferred: skip the finish when the caller has discarded
     * the surface's content (no readback wanted). The image stays
     * quarantined in the invalid pool — get_any_compatible_invalid_-
     * surface and prune_invalid_surfaces skip entries still
     * referenced by the recording command buffer — so no submit +
     * fence round trip is needed.
     */
    if (!allow_deferred && surface_image_in_recording_cb(r, surface)) {
        pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_SURFACE_DOWN);
    }

    nv2a_vk_assert((allow_deferred ||
            !surface_image_in_recording_cb(r, surface)) &&
           "Surface evicted while in use!");

    /*
     * The newest submission that may still reference this image: every
     * already-submitted CB, plus the currently recording one (it
     * becomes submission submit_count+1). prune_invalid_surfaces must
     * not destroy the image before that submission's fence retires —
     * with flight-slot pipelining, pgraph_vk_finish leaves the
     * just-submitted CB executing, so "not in the recording CB" alone
     * never proves the GPU is done with it.
     */
    surface->evict_submit_seq =
        (uint64_t)r->submit_count + (r->in_command_buffer ? 1 : 0);

    if (surface == r->color_binding) {
        nv2a_vk_assert(d->pgraph.surface_color.buffer_dirty);
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        nv2a_vk_assert(d->pgraph.surface_zeta.buffer_dirty);
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    surface_ranges_remove(r, surface);
    if (r->surface_lookup) {
        g_hash_table_remove(r->surface_lookup,
                            GSIZE_TO_POINTER((gsize)surface->vram_addr));
    }
    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    QTAILQ_INSERT_HEAD(&r->invalid_surfaces, surface, entry);
    r->invalid_surface_count++;
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    invalidate_surface_full(d, surface, false);
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    hwaddr start = surface->vram_addr;
    hwaddr range_end = surface->vram_addr + surface->size;

    /*
     * Typical overlap is 0-3 surfaces. A stack cap of 64 covers the
     * vast majority of real cases without touching the allocator; the
     * rare >64 overlap path falls back to a re-scan. Collect pointers
     * first, then invalidate — invalidate_surface -> surface_ranges_-
     * remove memmove's the array, so iteration + invalidation can't
     * be interleaved without corrupting the walk.
     */
    enum { STACK_CAP = 64 };
    SurfaceBinding *to_invalidate[STACK_CAP];
    int count = 0;
    bool overflow = false;

    for (int i = 0; i < r->surface_range_count; i++) {
        if (r->surface_ranges[i].start >= range_end) {
            break;
        }
        if (r->surface_ranges[i].end <= start) {
            continue;
        }
        if (count < STACK_CAP) {
            to_invalidate[count++] = r->surface_ranges[i].surface;
        } else {
            overflow = true;
            break;
        }
    }

    for (int i = 0; i < count; i++) {
        trace_nv2a_pgraph_surface_evict_overlapping(
            to_invalidate[i]->vram_addr, to_invalidate[i]->width,
            to_invalidate[i]->height, to_invalidate[i]->pitch);
        if (to_invalidate[i]->draw_dirty) {
            nsprof_event(NSPROF_EV_SDOWN_EVICT);
        }
        pgraph_vk_surface_download_if_dirty(d, to_invalidate[i]);
        invalidate_surface(d, to_invalidate[i]);
    }

    /*
     * Rare fallback: >STACK_CAP overlapping surfaces. Re-enter with
     * the (now partially-invalidated) array. The overlap window can
     * only shrink because we never add surfaces here, so this
     * terminates.
     */
    if (overflow) {
        invalidate_overlapping_surfaces(d, surface);
    }
}

static void surface_put(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    nv2a_vk_assert(pgraph_vk_surface_get(d, surface->vram_addr) == NULL);

    invalidate_overlapping_surfaces(d, surface);
    register_cpu_access_callback(d, surface);

    QTAILQ_INSERT_HEAD(&r->surfaces, surface, entry);
    surface_ranges_insert(r, surface);
    if (r->surface_lookup) {
        g_hash_table_insert(r->surface_lookup,
                            GSIZE_TO_POINTER((gsize)surface->vram_addr),
                            surface);
    }
}

SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (r->surface_lookup) {
        return (SurfaceBinding *)g_hash_table_lookup(
            r->surface_lookup, GSIZE_TO_POINTER((gsize)addr));
    }

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *exact = pgraph_vk_surface_get(d, addr);
    if (exact) {
        return exact;
    }

    return surface_ranges_find_containing(r, addr);
}

static void set_surface_label(PGRAPHState *pg, SurfaceBinding const *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Surface %" HWADDR_PRIx "h fmt:%s,%02xh %dx%d aa:%d",
        surface->vram_addr, surface->color ? "Color" : "Zeta",
        surface->color ? surface->shape.color_format :
                         surface->shape.zeta_format,
        surface->width, surface->height, pg->surface_shape.anti_aliasing);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)surface->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, surface->allocation, label);

    if (surface->image_scratch) {
        g_autofree gchar *label_scratch =
            g_strdup_printf("%s (scratch)", label);
        name_info.objectHandle = (uint64_t)surface->image_scratch;
        name_info.pObjectName = label_scratch;
        if (r->debug_utils_extension_enabled) {
            vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
        }
        vmaSetAllocationName(r->allocator, surface->allocation_scratch,
                             label_scratch);
    }
}

/*
 * Real-depth feed for the MetalFX temporal scaler. The single guest
 * zeta feeds the scaler one frame too late — by present time the guest
 * has begun the next frame and overwritten it — so this is opt-in:
 *   0 (unset) synthetic luminance depth (default)
 *   1         live zeta read: export every zeta, hand the bound one to
 *             the scaler at present (documented one-frame-ahead limit)
 *   2         flip snapshot: copy the bound zeta into a dedicated image
 *             at FLIP_STALL, when it is temporally correct, and feed
 *             that (see pgraph_vk_zeta_snapshot_capture)
 * Defined unconditionally (plain env parse) so it links on every
 * platform; only consulted under HAVE_IOSURFACE_SHARING.
 */
int pgraph_vk_mfx_real_depth_mode(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("XEMU_MFX_REAL_DEPTH");
        if (!env || !env[0] || env[0] == '0') {
            cached = 0;
        } else if (env[0] == '2') {
            cached = 2;
        } else {
            cached = 1; /* back-compat: "1"/any other truthy value */
        }
    }
    return cached;
}

#if HAVE_IOSURFACE_SHARING
/* Live-read (mode 1) export gate: only mode 1 makes every zeta image
 * exportable. Mode 2 leaves live zetas unexported and blits into a
 * dedicated snapshot instead, so it pays no per-surface export cost. */
static bool zeta_export_wanted(PGRAPHVkState *r, SurfaceBinding *surface)
{
    return pgraph_vk_mfx_real_depth_mode() == 1 && !surface->color &&
           r->metal_texture_export_enabled && r->export_metal_objects_fn &&
           g_config.display.metalfx_mode == CONFIG_DISPLAY_METALFX_MODE_TEMPORAL;
}
#endif

static void create_surface_image(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width = surface->width ? surface->width : 1;
    unsigned int height = surface->height ? surface->height : 1;
    pgraph_apply_scaling_factor(pg, &width, &height);

    nv2a_vk_assert(!surface->image);
    nv2a_vk_assert(!surface->image_scratch);

    NV2A_VK_DPRINTF(
        "Creating new surface image width=%d height=%d @ %08" HWADDR_PRIx,
        width, height, surface->vram_addr);

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = surface->host_fmt.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | surface->host_fmt.usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

#if HAVE_IOSURFACE_SHARING
    VkExportMetalObjectCreateInfoEXT export_metal_info;
    bool export_zeta = zeta_export_wanted(r, surface);
    if (export_zeta) {
        export_metal_info = (VkExportMetalObjectCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
            .exportObjectType =
                VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
        };
        image_create_info.pNext = &export_metal_info;
    }
#endif

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image,
                            &surface->allocation, NULL));

#if HAVE_IOSURFACE_SHARING
    if (export_zeta) {
        VkExportMetalTextureInfoEXT tex_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
            .image = surface->image,
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT export_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &tex_info,
        };
        ((PFN_vkExportMetalObjectsEXT)r->export_metal_objects_fn)(
            r->device, &export_info);
        if (tex_info.mtlTexture) {
            surface->mtl_texture = (void *)CFRetain(tex_info.mtlTexture);
        }
        static bool logged_once;
        if (!logged_once) {
            logged_once = true;
            fprintf(stderr,
                    "nv2a: zeta MTLTexture export (real depth): %s "
                    "(%ux%u, vk_format %u)\n",
                    tex_info.mtlTexture ? "ok" : "FAILED",
                    width, height, (unsigned)surface->host_fmt.vk_format);
        }
    }
#endif

#if HAVE_IOSURFACE_SHARING
    /* The scratch image is internal; don't mark it exportable. */
    image_create_info.pNext = NULL;
#endif

#if defined(__APPLE__)
    if (pg->surface_scale_factor > 1) {
#endif
        VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                                &alloc_create_info, &surface->image_scratch,
                                &surface->allocation_scratch, NULL));
#if defined(__APPLE__)
    } else {
        surface->image_scratch = VK_NULL_HANDLE;
        surface->allocation_scratch = VK_NULL_HANDLE;
    }
#endif
    surface->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = surface->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = surface->host_fmt.vk_format,
        .subresourceRange.aspectMask = surface->host_fmt.aspect,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &surface->image_view));

    {
        if (r->in_render_pass) {
            nsprof_event(NSPROF_EV_RENDERPASS_CAUSE_OTHER);
        }
        VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
        pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image, surface->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_UNDEFINED,
            surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        pgraph_vk_end_debug_marker(r, cmd);
    }
    nv2a_profile_inc_counter(NV2A_PROF_SURF_CREATE);
}

static void migrate_surface_image(SurfaceBinding *dst, SurfaceBinding *src)
{
    dst->image = src->image;
    dst->image_view = src->image_view;
    dst->allocation = src->allocation;
    dst->image_scratch = src->image_scratch;
    dst->image_scratch_current_layout = src->image_scratch_current_layout;
    dst->allocation_scratch = src->allocation_scratch;
#if HAVE_IOSURFACE_SHARING
    dst->mtl_texture = src->mtl_texture;
    src->mtl_texture = NULL;
#endif

    src->image = VK_NULL_HANDLE;
    src->image_view = VK_NULL_HANDLE;
    src->allocation = VK_NULL_HANDLE;
    src->image_scratch = VK_NULL_HANDLE;
    src->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    src->allocation_scratch = VK_NULL_HANDLE;
}

static void destroy_surface_image(PGRAPHVkState *r, SurfaceBinding *surface)
{
#if HAVE_IOSURFACE_SHARING
    if (surface->mtl_texture) {
        CFRelease(surface->mtl_texture);
        surface->mtl_texture = NULL;
    }
#endif

    vkDestroyImageView(r->device, surface->image_view, NULL);
    surface->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image, surface->allocation);
    surface->image = VK_NULL_HANDLE;
    surface->allocation = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image_scratch,
                    surface->allocation_scratch);
    surface->image_scratch = VK_NULL_HANDLE;
    surface->allocation_scratch = VK_NULL_HANDLE;
}

#if HAVE_IOSURFACE_SHARING
/*
 * Flip-time zeta snapshot (XEMU_MFX_REAL_DEPTH=2). See the mode note at
 * pgraph_vk_mfx_real_depth_mode(). The snapshot mirrors the bound zeta's
 * scaled dimensions and format and is created exportable, so its backing
 * MTLTexture feeds the temporal scaler exactly as a live zeta's does —
 * only temporally correct, because the copy is recorded into the frame's
 * command buffer at FLIP_STALL before that frame is submitted.
 */
static void zeta_snapshot_release(PGRAPHVkState *r)
{
    if (r->zeta_snapshot_mtl_texture) {
        CFRelease(r->zeta_snapshot_mtl_texture);
        r->zeta_snapshot_mtl_texture = NULL;
    }
    if (r->zeta_snapshot_image != VK_NULL_HANDLE) {
        vmaDestroyImage(r->allocator, r->zeta_snapshot_image,
                        r->zeta_snapshot_allocation);
        r->zeta_snapshot_image = VK_NULL_HANDLE;
        r->zeta_snapshot_allocation = VK_NULL_HANDLE;
    }
    r->zeta_snapshot_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    r->zeta_snapshot_width = 0;
    r->zeta_snapshot_height = 0;
    r->zeta_snapshot_format = VK_FORMAT_UNDEFINED;
    r->zeta_snapshot_valid = false;
}

static bool zeta_snapshot_ensure_image(PGRAPHState *pg, SurfaceBinding *zeta,
                                       unsigned scaled_w, unsigned scaled_h)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->zeta_snapshot_image != VK_NULL_HANDLE &&
        r->zeta_snapshot_width == (int)scaled_w &&
        r->zeta_snapshot_height == (int)scaled_h &&
        r->zeta_snapshot_format == zeta->host_fmt.vk_format) {
        return r->zeta_snapshot_mtl_texture != NULL;
    }

    zeta_snapshot_release(r);

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { scaled_w, scaled_h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = zeta->host_fmt.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        /* Same usage as the live zeta so the exported MTLTexture is
         * shaped identically for MetalFX; TRANSFER_DST for the blit. */
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | zeta->host_fmt.usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkExportMetalObjectCreateInfoEXT export_metal_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
    };
    image_create_info.pNext = &export_metal_info;

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    if (vmaCreateImage(r->allocator, &image_create_info, &alloc_create_info,
                       &r->zeta_snapshot_image, &r->zeta_snapshot_allocation,
                       NULL) != VK_SUCCESS) {
        r->zeta_snapshot_image = VK_NULL_HANDLE;
        r->zeta_snapshot_allocation = VK_NULL_HANDLE;
        return false;
    }

    VkExportMetalTextureInfoEXT tex_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = r->zeta_snapshot_image,
        .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
    };
    VkExportMetalObjectsInfoEXT export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &tex_info,
    };
    ((PFN_vkExportMetalObjectsEXT)r->export_metal_objects_fn)(r->device,
                                                              &export_info);
    if (tex_info.mtlTexture) {
        r->zeta_snapshot_mtl_texture = (void *)CFRetain(tex_info.mtlTexture);
    }
    r->zeta_snapshot_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    r->zeta_snapshot_width = (int)scaled_w;
    r->zeta_snapshot_height = (int)scaled_h;
    r->zeta_snapshot_format = zeta->host_fmt.vk_format;

    static bool logged_once;
    if (!logged_once) {
        logged_once = true;
        fprintf(stderr,
                "nv2a: zeta snapshot MTLTexture export (flip real depth): "
                "%s (%ux%u, vk_format %u)\n",
                r->zeta_snapshot_mtl_texture ? "ok" : "FAILED", scaled_w,
                scaled_h, (unsigned)zeta->host_fmt.vk_format);
    }
    return r->zeta_snapshot_mtl_texture != NULL;
}
#endif /* HAVE_IOSURFACE_SHARING */

void pgraph_vk_zeta_snapshot_capture(PGRAPHState *pg)
{
#if HAVE_IOSURFACE_SHARING
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* Stale until proven fresh for this flip. */
    r->zeta_snapshot_valid = false;

    if (pgraph_vk_mfx_real_depth_mode() != 2 ||
        !r->metal_texture_export_enabled || !r->export_metal_objects_fn ||
        g_config.display.metalfx_mode != CONFIG_DISPLAY_METALFX_MODE_TEMPORAL) {
        return;
    }

    /*
     * Snapshot the zeta whose dimensions match the bound color target
     * (what gets presented), mirroring the live-read consumer's
     * selection in render_display. The *bound* zeta can be a wider
     * combined / AA-squished buffer that never matches the display, so
     * snapshotting it blindly would only ever be rejected downstream by
     * the scaler's dims check. Prefer the most recently drawn match; no
     * match -> leave the snapshot invalid and fall back to synthetic,
     * exactly as the live path does.
     */
    SurfaceBinding *color = r->color_binding;
    if (!color || !color->width || !color->height) {
        return;
    }
    SurfaceBinding *zeta = NULL, *it;
    QTAILQ_FOREACH (it, &r->surfaces, entry) {
        if (!it->color && it->initialized && it->image != VK_NULL_HANDLE &&
            it->width == color->width && it->height == color->height &&
            (!zeta || it->draw_time > zeta->draw_time)) {
            zeta = it;
        }
    }
    if (!zeta) {
        return; /* no display-matching zeta: consumer falls back to synthetic */
    }

    unsigned scaled_w = zeta->width, scaled_h = zeta->height;
    pgraph_apply_scaling_factor(pg, &scaled_w, &scaled_h);

    if (!zeta_snapshot_ensure_image(pg, zeta, scaled_w, scaled_h)) {
        return; /* alloc/export failed: consumer falls back to synthetic */
    }

    int64_t nsprof_t0 = nsprof_begin();

    /* Ride the frame's command buffer: recorded here, submitted by the
     * FLIP_STALL finish immediately after, so the copy executes after
     * this frame's draws and before the next frame overwrites the zeta.
     * begin_nondraw_commands ends any open render pass first. */
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, zeta->image, zeta->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(
        pg, cmd, r->zeta_snapshot_image, r->zeta_snapshot_format,
        r->zeta_snapshot_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy copy_region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                            .layerCount = 1 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                            .layerCount = 1 },
        .extent = { scaled_w, scaled_h, 1 },
    };
    vkCmdCopyImage(cmd, zeta->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   r->zeta_snapshot_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

    /* Restore the zeta for the next frame's draws. */
    pgraph_vk_transition_image_layout(
        pg, cmd, zeta->image, zeta->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    /* Leave the snapshot in a sampled layout for the Metal-side read. */
    pgraph_vk_transition_image_layout(
        pg, cmd, r->zeta_snapshot_image, r->zeta_snapshot_format,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r->zeta_snapshot_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    r->zeta_snapshot_valid = r->zeta_snapshot_mtl_texture != NULL;
    nsprof_end(NSPROF_ZETA_SNAPSHOT, nsprof_t0);
#else
    (void)pg;
#endif
}

void pgraph_vk_zeta_snapshot_destroy(PGRAPHState *pg)
{
#if HAVE_IOSURFACE_SHARING
    zeta_snapshot_release(pg->vk_renderer_state);
#else
    (void)pg;
#endif
}

static SurfaceBinding *
get_any_compatible_invalid_surface(PGRAPHVkState *r, SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        /* Quarantine: deferred-invalidated image still referenced by
         * the recording command buffer (see invalidate_surface_full).
         *
         * REUSE while an earlier *submitted* CB still executes is safe
         * on any conformant driver, not just MoltenVK's serialized
         * queue: a migrated image is not re-transitioned (it keeps its
         * ATTACHMENT_OPTIMAL layout), so its first GPU touch as a new
         * binding is either (a) a draw render pass, whose explicit
         * VK_SUBPASS_EXTERNAL dependency (create_render_pass) covers
         * COLOR_ATTACHMENT_OUTPUT + EARLY/LATE_FRAGMENT_TESTS with
         * attachment read|write access — exactly the stages any prior
         * submission used it with, and external dependencies order
         * against all earlier same-queue submissions; (b) a transfer
         * clear/blit via pgraph_vk_transition_image_layout's
         * ATTACHMENT->TRANSFER_DST branches, whose src masks name the
         * same attachment stages; or (c) an upload, which on non-Apple
         * takes the conservative full-finish branch. DESTRUCTION has
         * no such barrier and is gated separately (evict_submit_seq).
         */
        if (surface_image_in_recording_cb(r, surface)) {
            continue;
        }
        if (surface->host_fmt.vk_format == target->host_fmt.vk_format &&
            surface->width == target->width &&
            surface->height == target->height &&
            surface->host_fmt.usage == target->host_fmt.usage) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            r->invalid_surface_count--;
            return surface;
        }
    }

    return NULL;
}

static void prune_invalid_surfaces(PGRAPHVkState *r, int keep)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        if (r->invalid_surface_count <= keep) {
            break;
        }
        if (surface_image_in_recording_cb(r, surface)) {
            continue;
        }
        /*
         * vkDestroyImage on a resource referenced by a *pending*
         * command buffer is invalid on every driver. The recording-CB
         * check above cannot prove completion — pgraph_vk_finish
         * pipelines: it submits the current CB and only waits the
         * previous slot's fence. Hold the entry until every
         * submission that may reference it has retired.
         */
        if (surface->evict_submit_seq > r->retired_submit_count) {
            continue;
        }
        QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
        r->invalid_surface_count--;
        destroy_surface_image(r, surface);
        g_free(surface);
    }
}

static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            if (s->draw_dirty) {
                nsprof_event(NSPROF_EV_SDOWN_EVICT);
            }
            pgraph_vk_surface_download_if_dirty(d, s);
            invalidate_surface(d, s);
        }
    }
}

static bool zeta_shape_readback_forced(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("XEMU_ZETA_SHAPE_READBACK");
        cached = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

static bool check_surface_compatibility(SurfaceBinding const *s1,
                                        SurfaceBinding const *s2, bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->host_fmt.vk_format == s2->host_fmt.vk_format) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        download_surface(d, surface, true);
    }
}

void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(surface->upload_pending || force)) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);

    /*
     * The finish here is only required when the upload target is
     * actively bound as color/zeta or otherwise referenced by the
     * currently recording command buffer, to preserve the read-after-
     * write ordering on the guest-visible side. When the target isn't
     * the active color/zeta binding and no CB is recording, no
     * unsubmitted GPU work references this surface: MoltenVK's single
     * queue serializes the aux-CB upload behind any prior submitted
     * main CB, and pgraph_vk_end_single_time_commands waits on
     * aux_fence before returning so host-visible ordering still holds.
     *
     * Call sites where the gate fails (finish still runs):
     *   - update_surface_part() passes r->color_binding / r->zeta_binding
     *   - texture-as-surface from bind_textures (in_command_buffer == true
     *     mid-frame between draws)
     * Call sites where the fast path fires:
     *   - render_display() upload, which already ran a PRESENTING finish
     *     a few lines earlier, so in_command_buffer == false and the
     *     source surface is typically not the active color/zeta.
     *
     * Apple-only: the "prior submitted main CB can't race the aux
     * upload" half of this argument comes from Metal's automatic
     * hazard tracking behind MoltenVK's single MTLCommandQueue. Core
     * Vulkan only orders same-queue submissions at execution *start*;
     * a native driver may overlap an in-flight slot's reads/writes of
     * this surface's image with the aux-CB upload (visible under WHPX,
     * where render_display force-uploads every present). Keep the
     * upstream full-finish semantics on non-Apple hosts.
     */
#ifdef __APPLE__
    bool target_is_active =
        (surface == r->color_binding) ||
        (surface == r->zeta_binding) ||
        r->in_command_buffer;
#else
    bool target_is_active = true;
#endif

    nsprof_event(surface->color ? NSPROF_EV_SUPLOAD_COLOR
                                : NSPROF_EV_SUPLOAD_ZETA);

    if (target_is_active) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE);
    }

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        surface->initialized = true;
        return;
    }

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    g_autofree uint8_t *swizzle_buf = NULL;
    uint8_t *gl_read_buf = NULL;

    /*
     * GPU unswizzle (surface-compute.c) accepts any power-of-two
     * dimensions via CPU-computed mask_x / mask_y push constants
     * (Phase 3.2). The POT test is defensive: swizzled Xbox surfaces
     * are always POT by hardware constraint.
     */
    bool use_gpu_unswizzle =
        surface->swizzle &&
        (surface->fmt.bytes_per_pixel == 4 ||
         surface->fmt.bytes_per_pixel == 2) &&
        r->compute.unswizzle_pipeline != VK_NULL_HANDLE &&
        (surface->width  & (surface->width  - 1)) == 0 &&
        (surface->height & (surface->height - 1)) == 0;

    if (surface->swizzle && !use_gpu_unswizzle) {
        swizzle_buf = (uint8_t*)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       swizzle_buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    } else if (!surface->swizzle) {
        gl_read_buf = buf;
    } else {
        gl_read_buf = buf;
    }

    StorageBuffer *copy_buffer = &r->storage_buffers[BUFFER_STAGING_SRC];

    size_t uploaded_image_size = surface->height * surface->width *
                                 surface->fmt.bytes_per_pixel;
    nv2a_vk_bounds_check(uploaded_image_size <= copy_buffer->buffer_size);

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    nv2a_vk_assert(surface->color ||
                   surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM ||
                   use_compute_to_convert_depth_stencil_format);

    memcpy_image(copy_buffer->mapped, gl_read_buf,
                 surface->width * surface->fmt.bytes_per_pixel, surface->pitch,
                 surface->height);

    if (!copy_buffer->is_coherent) {
        vmaFlushAllocation(r->allocator, copy_buffer->allocation, 0,
                           uploaded_image_size);
    }

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .size = uploaded_image_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    VkBuffer upload_src_buffer = copy_buffer->buffer;
    VkDeviceSize upload_data_size = uploaded_image_size;

    if (use_gpu_unswizzle) {
        VkBufferCopy swz_copy = { .size = uploaded_image_size };
        vkCmdCopyBuffer(cmd, copy_buffer->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                        1, &swz_copy);

        VkBufferMemoryBarrier pre_compute = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = uploaded_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute, 0, NULL);

        if (surface->fmt.bytes_per_pixel == 2) {
            pgraph_vk_dispatch_unswizzle_2bpp(
                pg, cmd,
                r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                surface->width, surface->height);
        } else {
            pgraph_vk_dispatch_unswizzle(
                pg, cmd,
                r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                surface->width, surface->height);
        }

        VkBufferMemoryBarrier post_compute = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = uploaded_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             1, &post_compute, 0, NULL);

        upload_src_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    }

    // Set up image copy regions (which may be modified by compute unpack)

    VkBufferImageCopy regions[2];
    int num_regions = 0;

    regions[num_regions++] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
        .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        regions[num_regions++] = (VkBufferImageCopy){
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
        };
    }


    unsigned int scaled_width = surface->width, scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    if (use_compute_to_convert_depth_stencil_format) {

        //
        // Copy packed image buffer to compute_dst for unpacking
        //

        size_t packed_size = uploaded_image_size;
        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, copy_buffer->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer, 1,
                        &buffer_copy_region);

        size_t num_pixels = scaled_width * scaled_height;
        size_t unpacked_depth_image_size = num_pixels * 4;
        size_t unpacked_stencil_image_size = num_pixels;
        size_t unpacked_size =
            unpacked_depth_image_size + unpacked_stencil_image_size;

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer->buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);

        //
        // Unpack depth-stencil image into compute_src
        //

        VkBufferMemoryBarrier pre_unpack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_unpack_src_barrier, 0, NULL);

        StorageBuffer *unpack_buffer = &r->storage_buffers[BUFFER_COMPUTE_SRC];

        VkBufferMemoryBarrier pre_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1,
                             &pre_unpack_dst_barrier, 0, NULL);

        pgraph_vk_unpack_depth_stencil(
            pg, surface, cmd, r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            unpack_buffer->buffer);

        VkBufferMemoryBarrier post_unpack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unpack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unpack_dst_barrier, 0, NULL);

        // Already scaled during compute. Adjust copy regions.
        regions[0].imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 };
        regions[1].imageExtent = regions[0].imageExtent;
        regions[1].bufferOffset =
            ROUND_UP(unpacked_depth_image_size,
                     r->device_props.limits.minStorageBufferOffsetAlignment);

        copy_buffer = unpack_buffer;
        upload_src_buffer = unpack_buffer->buffer;
        upload_data_size = unpacked_size;
    }

    //
    // Copy image data from buffer to destination image
    //

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    bool upscale = pg->surface_scale_factor > 1 &&
                   !use_compute_to_convert_depth_stencil_format;

    if (surface->image_scratch != VK_NULL_HANDLE) {
        if (surface->image_scratch_current_layout !=
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image_scratch, surface->host_fmt.vk_format,
                surface->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            surface->image_scratch_current_layout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }

        vkCmdCopyBufferToImage(cmd, upload_src_buffer, surface->image_scratch,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               num_regions, regions);

        VkBufferMemoryBarrier post_copy_src_buffer_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer->buffer,
            .size = upload_data_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_buffer_barrier, 0, NULL);

        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image_scratch, surface->host_fmt.vk_format,
            surface->image_scratch_current_layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        if (upscale) {
            VkImageBlit blitRegion = {
                .srcSubresource.aspectMask = surface->host_fmt.aspect,
                .srcSubresource.mipLevel = 0,
                .srcSubresource.baseArrayLayer = 0,
                .srcSubresource.layerCount = 1,
                .srcOffsets[0] = (VkOffset3D){0, 0, 0},
                .srcOffsets[1] = (VkOffset3D){surface->width,
                                              surface->height, 1},
                .dstSubresource.aspectMask = surface->host_fmt.aspect,
                .dstSubresource.mipLevel = 0,
                .dstSubresource.baseArrayLayer = 0,
                .dstSubresource.layerCount = 1,
                .dstOffsets[0] = (VkOffset3D){0, 0, 0},
                .dstOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},
            };
            vkCmdBlitImage(cmd, surface->image_scratch,
                           surface->image_scratch_current_layout,
                           surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blitRegion,
                           surface->color ? VK_FILTER_LINEAR
                                          : VK_FILTER_NEAREST);
        } else {
            for (int i = 0; i < num_regions; i++) {
                VkImageAspectFlags aspect =
                    regions[i].imageSubresource.aspectMask;
                VkImageCopy copy_region = {
                    .srcSubresource.aspectMask = aspect,
                    .srcSubresource.layerCount = 1,
                    .dstSubresource.aspectMask = aspect,
                    .dstSubresource.layerCount = 1,
                    .extent = regions[i].imageExtent,
                };
                vkCmdCopyImage(cmd, surface->image_scratch,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy_region);
            }
        }
    } else {
        vkCmdCopyBufferToImage(cmd, upload_src_buffer, surface->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               num_regions, regions);
    }

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_2);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    surface->initialized = true;
}

static void compare_surfaces(SurfaceBinding const *a, SurfaceBinding const *b)
{
    #define DO_CMP(fld) \
        if (a->fld != b->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)a->fld, (long int)b->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(host_fmt.vk_format)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_target_sized(NV2AState *d, bool color,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        nv2a_vk_bounds_check(pg->surface_shape.color_format != 0);
        nv2a_vk_bounds_check(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_vk_map));
        fmt = kelvin_surface_color_format_map[pg->surface_shape.color_format];
        host_fmt = kelvin_surface_color_format_vk_map[pg->surface_shape.color_format];
        if (host_fmt.host_bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        nv2a_vk_bounds_check(pg->surface_shape.zeta_format != 0);
        nv2a_vk_bounds_check(pg->surface_shape.zeta_format <
               ARRAY_SIZE(r->kelvin_surface_zeta_vk_map));
        fmt = kelvin_surface_zeta_format_map[pg->surface_shape.zeta_format];
        host_fmt = r->kelvin_surface_zeta_vk_map[pg->surface_shape.zeta_format];
        // FIXME: Support float 16,24b float format surface
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    // There's a bunch of bugs that could cause us to hit this function
    // at the wrong time and get a invalid dma object.
    // Check that it's sane.
    nv2a_vk_bounds_check(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    nv2a_vk_bounds_check(surface->offset <= dma.limit);
    nv2a_vk_bounds_check(surface->offset + surface->pitch * height <= dma.limit + 1);
    nv2a_vk_bounds_check(surface->pitch % fmt.bytes_per_pixel == 0);
    nv2a_vk_bounds_check((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    target->fmt = fmt;
    target->host_fmt = host_fmt;
    target->color = color;
    target->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    target->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    target->upload_pending = true;
    target->download_pending = false;
    target->draw_dirty = false;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->cleared = false;

    target->initialized = false;
}

static void populate_surface_binding_target(NV2AState *d, bool color,
                                            SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        // Since we determine surface dimensions based on the clipping
        // rectangle, make sure to include the surface offset as well.
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_target_sized(d, color, width, height, target);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    SurfaceBinding target;
    memset(&target, 0, sizeof(target));
    target.surface_range_slot = -1;
    populate_surface_binding_target(d, color, &target);

    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    bool mem_dirty = !tcg_enabled() && memory_region_test_and_clear_dirty(
                                           d->vram, target.vram_addr,
                                           target.size, DIRTY_MEMORY_NV2A);

    SurfaceBinding *current_binding = color ? r->color_binding
                                            : r->zeta_binding;

    if (!current_binding ||
        (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        if (r->in_render_pass) {
            nsprof_event(NSPROF_EV_RENDERPASS_CAUSE_SURFACE);
        }
        pgraph_vk_ensure_not_in_render_pass(pg);

        unbind_surface(d, color);

        SurfaceBinding *surface = pgraph_vk_surface_get(d, target.vram_addr);
        if (surface != NULL) {
            // FIXME: Support same color/zeta surface target? In the mean time,
            // if the surface we just found is currently bound, just unbind it.
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
            if (surface == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", target.vram_addr,
            target.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;
        bool zeta_shape_discard = false;

        if (surface != NULL) {
            bool is_compatible =
                check_surface_compatibility(surface, &target, false);

            void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                             const char *layout, uint32_t anti_aliasing,
                             uint32_t clip_x, uint32_t clip_width,
                             uint32_t clip_y, uint32_t clip_height,
                             uint32_t pitch) =
                surface->color ? trace_nv2a_pgraph_surface_match_color :
                               trace_nv2a_pgraph_surface_match_zeta;

            trace_fn(surface->vram_addr, surface->width, surface->height,
                     surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                     surface->shape.clip_x, surface->shape.clip_width,
                     surface->shape.clip_y, surface->shape.clip_height,
                     surface->pitch);

            nv2a_vk_assert(!(target.swizzle && pg->clearing));

            if (is_compatible && color &&
                !check_surface_compatibility(surface, &target, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_target_sized(
                    d, !color, surface->width, surface->height, &zeta_entry);
                hwaddr color_end = surface->vram_addr + surface->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= surface->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (surface->width == r->color_binding->width) &&
                                 (surface->height == r->color_binding->height);
            }

            if (is_compatible) {
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.height = surface->height;
                r->cached_scaled_binding_dim_valid = false;
                surface->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", surface->vram_addr);
                compare_surfaces(surface, &target);
                /*
                 * Shape switch at the same VRAM address. For zeta
                 * surfaces, skip the GPU->CPU readback: its only
                 * effect is seeding guest RAM with the old shape's
                 * depth bytes so a hypothetical consumer sees aliased
                 * content, but titles that re-shape a depth target
                 * (e.g. Azurik ping-pongs one zeta allocation between
                 * the 1280x480 scene and a 256x256 swizzled RTT pass
                 * every frame) clear it before drawing, and the
                 * genuine RAM consumers (CPU access callbacks,
                 * texture binds over the range) are zero in measured
                 * gameplay. The readback costs a finish + fence +
                 * D24S8 compute conversion + multi-MB memcpy, twice
                 * per flip on affected titles. An already-requested
                 * CPU download (download_pending) is still honored.
                 * XEMU_ZETA_SHAPE_READBACK=1 restores the readback.
                 */
                bool skip_readback = !surface->color &&
                                     !surface->download_pending &&
                                     !zeta_shape_readback_forced();
                if (skip_readback) {
                    zeta_shape_discard = true;
                    surface->draw_dirty = false;
                    surface->download_pending = false;
                    /* Content discarded: defer image reuse to the
                     * next command buffer instead of finishing. */
                    invalidate_surface_full(d, surface, true);
                } else {
                    if (surface->draw_dirty) {
                        nsprof_event(NSPROF_EV_SDOWN_INCOMPAT);
                    }
                    pgraph_vk_surface_download_if_dirty(d, surface);
                    invalidate_surface(d, surface);
                }
            }
        }

        if (should_create) {
            /*
             * Companion to the readback skip above: the discarded
             * zeta shape's RAM bytes weren't refreshed, and a zeta
             * target re-shaped at the same address gets cleared by
             * the guest before use — seeding it from (stale or
             * unrelated) RAM costs a 2+ MB compute-converted upload
             * plus a finish when a command buffer is recording,
             * twice per flip on ping-pong titles.
             */
            if (zeta_shape_discard && !target.color) {
                target.upload_pending = false;
            }
            surface = get_any_compatible_invalid_surface(r, &target);
            if (surface) {
                migrate_surface_image(&target, surface);
            } else {
                surface = g_malloc(sizeof(SurfaceBinding));
                create_surface_image(pg, &target);
            }

            *surface = target;
            set_surface_label(pg, surface);
            surface_put(d, surface);

            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.height = target.height;
            r->cached_scaled_binding_dim_valid = false;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                         const char *layout, uint32_t anti_aliasing,
                         uint32_t clip_x, uint32_t clip_width, uint32_t clip_y,
                         uint32_t clip_height, uint32_t pitch) =
            color ? (should_create ? trace_nv2a_pgraph_surface_create_color :
                                     trace_nv2a_pgraph_surface_hit_color) :
                    (should_create ? trace_nv2a_pgraph_surface_create_zeta :
                                     trace_nv2a_pgraph_surface_hit_zeta);
        trace_fn(surface->vram_addr, surface->width, surface->height,
                 surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                 surface->shape.clip_x, surface->shape.clip_width,
                 surface->shape.clip_y, surface->shape.clip_height, surface->pitch);

        bind_surface(r, surface);
        pg_surface->buffer_dirty = false;
    }

    if (!upload && pg_surface->draw_dirty) {
        if (!tcg_enabled()) {
            // FIXME: Cannot monitor for reads/writes; flush now
            download_surface(d, color ? r->color_binding : r->zeta_binding,
                             true);
        }

        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
    }
}

// FIXME: Move to common?
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_vk_upload_surface_data(d, r->color_binding, false);
            r->color_binding->draw_time = pg->draw_time;
            r->color_binding->swizzle = swizzle;
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_vk_upload_surface_data(d, r->zeta_binding, false);
            r->zeta_binding->draw_time = pg->draw_time;
            r->zeta_binding->swizzle = swizzle;
        }
    }

    // Sanity check color and zeta dimensions match
    if (r->color_binding && r->zeta_binding) {
        nv2a_vk_assert(r->color_binding->width == r->zeta_binding->width);
        nv2a_vk_assert(r->color_binding->height == r->zeta_binding->height);
    }

    /*
     * Throttle the O(n) surface list scans so they don't run on every
     * draw. We throttle on host wall-clock (not pg->frame_time), because
     * pg->frame_time only advances in NV097_FLIP_INCREMENT_WRITE — during
     * a rapid level transition or loading burst the guest can issue
     * thousands of draws between flips, and a frame_time-only gate would
     * never fire. That lets r->invalid_surfaces accumulate unbounded;
     * each invalid SurfaceBinding still holds a live VkImage +
     * allocation, and eventually either VRAM pressure or get_any_-
     * compatible_invalid_surface matching the wrong stale candidate
     * stalls the renderer (reproducible as a black-screen freeze on
     * rapid double-level-load + die, confirmed via bisect against
     * v0.8.141 → v0.8.142).
     *
     * ~33 ms (~2 frames at 60 Hz) keeps the per-draw amortization that
     * motivated the throttle while guaranteeing the prune runs during
     * long non-flip bursts.
     */
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    const int64_t surface_expire_interval_ns = 33 * 1000 * 1000;
    if (now_ns - r->last_expire_ns >= surface_expire_interval_ns) {
        expire_old_surfaces(d);
        prune_invalid_surfaces(r, num_invalid_surfaces_to_keep);
        r->last_expire_ns = now_ns;
    }
}

static bool check_format_and_usage_supported(PGRAPHVkState *r, VkFormat format,
                                             VkImageUsageFlags usage)
{
    VkPhysicalDeviceImageFormatInfo2 pdif2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        r->physical_device, &pdif2, &props);
    return result == VK_SUCCESS;
}

static bool check_surface_internal_formats_supported(
    PGRAPHVkState *r, const SurfaceFormatInfo *fmts, size_t count)
{
    bool all_supported = true;
    for (int i = 0; i < count; i++) {
        const SurfaceFormatInfo *f = &fmts[i];
        if (f->host_bytes_per_pixel) {
            all_supported &=
                check_format_and_usage_supported(r, f->vk_format, f->usage);
        }
    }
    return all_supported;
}

void pgraph_vk_init_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Make sure all surface format types are supported. We don't expect issue
    // with these, and therefore have no fallback mechanism.
    nv2a_vk_assert(check_surface_internal_formats_supported(
        r, kelvin_surface_color_format_vk_map,
        ARRAY_SIZE(kelvin_surface_color_format_vk_map)));

    // Check if the device supports preferred VK_FORMAT_D24_UNORM_S8_UINT
    // format, fall back to D32_SFLOAT_S8_UINT otherwise.
    r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z16] = zeta_d16;
    if (check_surface_internal_formats_supported(r, &zeta_d24_unorm_s8_uint,
                                                 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d24_unorm_s8_uint;
    } else if (check_surface_internal_formats_supported(
                   r, &zeta_d32_sfloat_s8_uint, 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d32_sfloat_s8_uint;
    } else {
        nv2a_vk_assert(!"No suitable depth-stencil format supported");
    }

    QTAILQ_INIT(&r->surfaces);
    QTAILQ_INIT(&r->invalid_surfaces);
    r->invalid_surface_count = 0;
    r->surface_lookup = g_hash_table_new(g_direct_hash, g_direct_equal);
    r->surface_ranges = NULL;
    r->surface_range_count = 0;
    r->surface_range_capacity = 0;

    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->framebuffer_dirty = true;
    r->render_pass_state_dirty = true;

    pgraph_vk_reload_surface_scale_factor(pg); // FIXME: Move internal
}

void pgraph_vk_finalize_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_surface_flush(container_of(pg, NV2AState, pgraph));
    /* surface_flush drained every slot, so no in-flight work references
     * the snapshot image; safe to release it now. */
    pgraph_vk_zeta_snapshot_destroy(pg);
    if (r->surface_lookup) {
        g_hash_table_destroy(r->surface_lookup);
        r->surface_lookup = NULL;
    }
    g_free(r->surface_ranges);
    r->surface_ranges = NULL;
    r->surface_range_count = 0;
    r->surface_range_capacity = 0;
}

void pgraph_vk_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Clear last surface shape to force recreation of buffers at next draw
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    unbind_surface(d, true);
    unbind_surface(d, false);

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        if (s->draw_dirty) {
            nsprof_event(NSPROF_EV_SDOWN_FLUSH);
        }
        pgraph_vk_surface_download_if_dirty(d, s);
        invalidate_surface(d, s);
    }
    /*
     * Flush means "free everything now" (scale change, snapshot,
     * renderer teardown): drain every in-flight slot so the
     * evict_submit_seq gate in prune_invalid_surfaces is satisfied
     * for all entries rather than deferring their destruction.
     */
    for (int slot = 0; slot < NUM_FLIGHT_SLOTS; slot++) {
        pgraph_vk_wait_slot_fence(pg, slot);
    }
    prune_invalid_surfaces(r, 0);

    pgraph_vk_reload_surface_scale_factor(pg);
}
