/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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
#include "hw/xbox/nv2a/nsprof.h"
#include "renderer.h"
#include "ui/xemu-events.h"

#include <math.h>

static void perform_blit(int operation, uint8_t *source, uint8_t *dest,
                         size_t width, size_t height, size_t width_bytes,
                         size_t source_pitch, size_t dest_pitch,
                         BetaState *beta)
{
    if (operation == NV09F_SET_OPERATION_SRCCOPY) {
        for (unsigned int y = 0; y < height; y++) {
            memmove(dest, source, width_bytes);
            source += source_pitch;
            dest += dest_pitch;
        }
    } else if (operation == NV09F_SET_OPERATION_BLEND_AND) {
        uint32_t max_beta_mult = 0x7f80;
        uint32_t beta_mult = beta->beta >> 16;
        uint32_t inv_beta_mult = max_beta_mult - beta_mult;

        for (unsigned int y = 0; y < height; y++) {
            uint8_t *s = source;
            uint8_t *d = dest;
            for (unsigned int x = 0; x < width; x++) {
                for (unsigned int ch = 0; ch < 3; ch++) {
                    uint32_t a = s[x * 4 + ch] * beta_mult;
                    uint32_t b = d[x * 4 + ch] * inv_beta_mult;
                    d[x * 4 + ch] = (a + b) / max_beta_mult;
                }
            }
            source += source_pitch;
            dest += dest_pitch;
        }
    } else {
        fprintf(stderr, "Unknown blit operation: 0x%x\n", operation);
        assert(!"Unknown blit operation");
    }
}

static void patch_alpha(uint8_t *dest, size_t width_pixels, size_t height,
                        size_t dest_pitch, uint8_t alpha_val)
{
    for (unsigned int y = 0; y < height; y++) {
        uint8_t *d = dest;
        for (unsigned int x = 0; x < width_pixels; x++) {
            d[x * 4 + 3] = alpha_val;
        }
        dest += dest_pitch;
    }
}

static bool surface_fully_overwritten(const SurfaceBinding *surface,
                                      hwaddr dest_addr, size_t dest_pitch,
                                      size_t row_bytes, size_t num_rows)
{
    if (surface->swizzle || !dest_pitch || surface->pitch != dest_pitch ||
        surface->vram_addr < dest_addr) {
        return false;
    }

    hwaddr offset = surface->vram_addr - dest_addr;
    hwaddr first_row = offset / dest_pitch;
    hwaddr first_col = offset % dest_pitch;
    hwaddr surface_row_bytes = surface->width * surface->fmt.bytes_per_pixel;

    return first_col + surface_row_bytes <= row_bytes &&
           first_row + surface->height <= num_rows;
}

static void discard_overwritten_surfaces(PGRAPHState *pg, hwaddr dest_addr,
                                         size_t dest_pitch, size_t row_bytes,
                                         size_t num_rows)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface_fully_overwritten(surface, dest_addr, dest_pitch, row_bytes,
                                      num_rows)) {
            surface->download_pending = false;
            surface->draw_dirty = false;
        }
    }
}

/*
 * Whether the NV09F blit may run as a GPU copy between two surfaces of
 * these sizes and pitches: a SRCCOPY within both, at the pitches the
 * surfaces were made with, no alpha forced by an X8 format (the CPU path
 * patches alpha there), and not cut by a GPU tile limit. The caller checks
 * the rest: two distinct, unswizzled colour surfaces of one format and of
 * the blit's pixel size.
 */
static bool pgraph_blit_gpu_fits(NV2AState *d, hwaddr dest_addr,
                                 unsigned int bytes_per_pixel,
                                 unsigned int src_width, unsigned int src_height,
                                 unsigned int src_pitch, unsigned int dst_width,
                                 unsigned int dst_height, unsigned int dst_pitch)
{
    ContextSurfaces2DState *cs = &d->pgraph.context_surfaces_2d;
    ImageBlitState *ib = &d->pgraph.image_blit;

    if (ib->operation != NV09F_SET_OPERATION_SRCCOPY ||
        cs->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ||
        cs->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8 ||
        ib->width == 0 || ib->height == 0 ||
        cs->source_pitch != src_pitch || cs->dest_pitch != dst_pitch ||
        ib->in_x + ib->width > src_width ||
        ib->in_y + ib->height > src_height ||
        ib->out_x + ib->width > dst_width ||
        ib->out_y + ib->height > dst_height) {
        return false;
    }

    hwaddr off = ib->out_y * cs->dest_pitch + ib->out_x * bytes_per_pixel;
    hwaddr size = (ib->height - 1) * cs->dest_pitch +
                  ib->width * bytes_per_pixel;
    return nv_clip_gpu_tile_blit(d, dest_addr + off, size) == size;
}

void pgraph_vk_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    BetaState *beta = &pg->beta;

    pgraph_vk_surface_update(d, false, true, true);

    assert(context_surfaces->object_instance == image_blit->context_surfaces);

    unsigned int bytes_per_pixel;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        bytes_per_pixel = 1;
        break;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        bytes_per_pixel = 2;
        break;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        bytes_per_pixel = 4;
        break;
    default:
        fprintf(stderr, "Unknown blit surface format: 0x%x\n",
                context_surfaces->color_format);
        assert(!"Unknown blit surface format");
        break;
    }

    hwaddr source_dma_len;
    uint8_t *source = (uint8_t *)nv_dma_map(
        d, context_surfaces->dma_image_source, &source_dma_len);
    assert(context_surfaces->source_offset < source_dma_len);
    source += context_surfaces->source_offset;
    hwaddr source_addr = source - d->vram_ptr;

    hwaddr dest_dma_len;
    uint8_t *dest = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_dest,
                                          &dest_dma_len);
    assert(context_surfaces->dest_offset < dest_dma_len);
    dest += context_surfaces->dest_offset;
    hwaddr dest_addr = dest - d->vram_ptr;

    /*
     * GPU boost: an NV09F SRCCOPY between two cached colour surfaces is a
     * GPU image copy, VRAM to VRAM as on the hardware, instead of a
     * download, memmove and upload. Anything the guards reject takes the
     * CPU path below.
     *
     * VkImageCopy cannot resample, so the fast path only covers integral
     * scales (its offsets and extents are integer multiples). Any other
     * factor falls through to the CPU path.
     *
     * The surfaces must share a guest colour format: an RGBA8-hosted
     * 16-bit surface holds decoded colours, which only a surface of the
     * same packing may take as they are.
     */
    hwaddr source_offset = image_blit->in_y * context_surfaces->source_pitch +
                           image_blit->in_x * bytes_per_pixel;
    hwaddr dest_offset = image_blit->out_y * context_surfaces->dest_pitch +
                         image_blit->out_x * bytes_per_pixel;

    float scale_factor = pg->surface_scale_factor;
    if (pgraph_gpu_boost() && scale_factor >= 1.0f &&
        scale_factor == floorf(scale_factor)) {
        SurfaceBinding *surf_src = pgraph_vk_surface_get(d, source_addr);
        SurfaceBinding *gdst = pgraph_vk_surface_get(d, dest_addr);

        if (surf_src != NULL && gdst != NULL && gdst != surf_src &&
            surf_src->color && gdst->color && !surf_src->swizzle &&
            !gdst->swizzle &&
            surf_src->host_fmt.vk_format == gdst->host_fmt.vk_format &&
            surf_src->shape.color_format == gdst->shape.color_format &&
            bytes_per_pixel == surf_src->fmt.bytes_per_pixel &&
            bytes_per_pixel == gdst->fmt.bytes_per_pixel &&
            pgraph_blit_gpu_fits(d, dest_addr, bytes_per_pixel,
                                 surf_src->width, surf_src->height,
                                 surf_src->pitch, gdst->width, gdst->height,
                                 gdst->pitch)) {
            /* The full rectangles: gpu_fits rules out a tile clip. */
            hwaddr gpu_src_size =
                (image_blit->height - 1) * context_surfaces->source_pitch +
                image_blit->width * bytes_per_pixel;
            hwaddr gpu_dst_size =
                (image_blit->height - 1) * context_surfaces->dest_pitch +
                image_blit->width * bytes_per_pixel;

            /* Reports the blit could clobber must reach RAM first. */
            pgraph_vk_det_reports_guard(d, source_addr + source_offset,
                                        gpu_src_size);
            pgraph_vk_det_reports_guard(d, dest_addr + dest_offset,
                                        gpu_dst_size);

            bool full_replace = image_blit->out_x == 0 &&
                                image_blit->out_y == 0 &&
                                image_blit->width == gdst->width &&
                                image_blit->height == gdst->height;
            if (surf_src->upload_pending) {
                pgraph_vk_upload_surface_data(d, surf_src, false);
            }
            if (gdst->upload_pending) {
                if (full_replace) {
                    gdst->upload_pending = false;
                } else {
                    pgraph_vk_upload_surface_data(d, gdst, false);
                }
            }

            unsigned int scale = (unsigned int)scale_factor;
            VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);

            pgraph_vk_transition_image_layout(
                pg, cmd, surf_src->image, surf_src->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            pgraph_vk_transition_image_layout(
                pg, cmd, gdst->image, gdst->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            VkImageCopy region = {
                .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .srcSubresource.layerCount = 1,
                .srcOffset = { (int32_t)(image_blit->in_x * scale),
                               (int32_t)(image_blit->in_y * scale), 0 },
                .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .dstSubresource.layerCount = 1,
                .dstOffset = { (int32_t)(image_blit->out_x * scale),
                               (int32_t)(image_blit->out_y * scale), 0 },
                .extent = { image_blit->width * scale,
                            image_blit->height * scale, 1 },
            };
            vkCmdCopyImage(cmd, surf_src->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gdst->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            pgraph_vk_transition_image_layout(
                pg, cmd, surf_src->image, surf_src->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            pgraph_vk_transition_image_layout(
                pg, cmd, gdst->image, gdst->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            pgraph_vk_end_nondraw_commands(pg, cmd);

            /*
             * RAM is behind the image now, as after a draw. download_pending
             * is left as it is: clearing it would lose a pending read-back
             * (the CPU path clears it only as its memmove refreshes RAM),
             * and setting it would read back every blit destination.
             */
            gdst->initialized = true;
            pg->draw_time++;
            gdst->draw_time = pg->draw_time;
            gdst->frame_time = pg->frame_time;
            gdst->draw_dirty = true;
            gdst->upload_pending = false;
            /* Date the picture as the CPU path does (query-xemu-display). */
            xemu_events_gpu_write(dest_addr + dest_offset, gpu_dst_size);
            return;
        }
    }

    source_addr += source_offset;
    dest_addr += dest_offset;

    size_t max_row_pixels =
        MIN(context_surfaces->source_pitch, context_surfaces->dest_pitch) /
        bytes_per_pixel;
    size_t row_pixels = MIN(max_row_pixels, image_blit->width);

    hwaddr dest_size = (image_blit->height - 1) * context_surfaces->dest_pitch +
                       image_blit->width * bytes_per_pixel;

    uint8_t *source_row = source + source_offset;
    uint8_t *dest_row = dest + dest_offset;
    size_t row_bytes = row_pixels * bytes_per_pixel;

    size_t adjusted_height = image_blit->height;
    size_t leftover_bytes = 0;

    hwaddr clipped_dest_size = nv_clip_gpu_tile_blit(d, dest_addr, dest_size);

    pgraph_vk_det_reports_guard(
        d, source_addr,
        (image_blit->height - 1) * context_surfaces->source_pitch + row_bytes);
    pgraph_vk_det_reports_guard(d, dest_addr, dest_size);

    if (clipped_dest_size < dest_size) {
        adjusted_height = clipped_dest_size / context_surfaces->dest_pitch;
        size_t consumed_bytes = adjusted_height * context_surfaces->dest_pitch;

        leftover_bytes = clipped_dest_size - consumed_bytes;
    }

    size_t total_rows = adjusted_height + (leftover_bytes > 0 ? 1 : 0);
    hwaddr source_size = total_rows ? (total_rows - 1) * context_surfaces->source_pitch +
                                       (leftover_bytes ? leftover_bytes : row_pixels * bytes_per_pixel) : 0;
    pgraph_vk_download_surfaces_in_range_if_dirty(pg, source_addr, source_size,
                                                  NSPROF_EV_SDOWN_BLIT);

    // Must happen after the source download so that surfaces overlapping both
    // regions are synchronized before being discarded.
    if (image_blit->operation == NV09F_SET_OPERATION_SRCCOPY) {
        discard_overwritten_surfaces(pg, dest_addr,
                                     context_surfaces->dest_pitch, row_bytes,
                                     adjusted_height);
    }
    pgraph_vk_download_surfaces_in_range_if_dirty(pg, dest_addr,
                                                  clipped_dest_size,
                                                  NSPROF_EV_SDOWN_BLIT);

    NV2A_DPRINTF("  blit 0x%tx -> 0x%tx (Size: %llu, Clipped Height: %zu)\n",
                 source_addr, dest_addr, dest_size, adjusted_height);

    if (adjusted_height > 0) {
        perform_blit(image_blit->operation, source_row, dest_row, row_pixels,
                     adjusted_height, row_bytes, context_surfaces->source_pitch,
                     context_surfaces->dest_pitch, beta);
    }

    if (leftover_bytes > 0) {
        uint8_t *src =
            source_row + adjusted_height * context_surfaces->source_pitch;
        uint8_t *dest =
            dest_row + adjusted_height * context_surfaces->dest_pitch;

        perform_blit(image_blit->operation, src, dest,
                     leftover_bytes / bytes_per_pixel, 1, leftover_bytes,
                     context_surfaces->source_pitch,
                     context_surfaces->dest_pitch, beta);
    }

    bool needs_alpha_patching;
    uint8_t alpha_override;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0xff;
        break;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0;
        break;
    default:
        needs_alpha_patching = false;
        alpha_override = 0;
    }

    if (needs_alpha_patching) {
        if (adjusted_height > 0) {
            patch_alpha(dest_row, row_pixels, adjusted_height,
                        context_surfaces->dest_pitch, alpha_override);
        }

        if (leftover_bytes > 0) {
            uint8_t *dest =
                dest_row + adjusted_height * context_surfaces->dest_pitch;
            patch_alpha(dest, leftover_bytes / 4, 1, 0, alpha_override);
        }
    }

    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_VGA);
    xemu_events_gpu_write(dest_addr, clipped_dest_size);
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_NV2A_TEX);
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_NV2A);

    if (pgraph_vk_mark_surfaces_in_range_for_upload(d, dest_addr,
                                                    clipped_dest_size)) {
        ++pg->draw_time;
    }
}
