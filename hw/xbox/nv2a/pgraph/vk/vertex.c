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

#include "renderer.h"
#include "hw/xbox/nv2a/nsprof.h"

/*
 * Compare the currently-populated vertex attribute / binding
 * descriptions against the last-bound snapshot and update the
 * snapshot + vertex_state_dirty flag accordingly. Replaces a pair
 * of fast_hash calls with short-circuit memcmp: no multiplier chain
 * per check, and a mismatch in the count alone skips the memcmp
 * entirely. Shared between the indexed / raw attribute path and the
 * inline-buffer path.
 */
static void update_vertex_layout_dirty(PGRAPHVkState *r)
{
    size_t attr_bytes = r->num_active_vertex_attribute_descriptions *
                        sizeof(r->vertex_attribute_descriptions[0]);
    size_t bind_bytes = r->num_active_vertex_binding_descriptions *
                        sizeof(r->vertex_binding_descriptions[0]);

    bool changed = !r->vertex_layout_snapshot_valid ||
                   r->prev_num_active_vertex_attribute_descriptions !=
                       r->num_active_vertex_attribute_descriptions ||
                   r->prev_num_active_vertex_binding_descriptions !=
                       r->num_active_vertex_binding_descriptions ||
                   memcmp(r->prev_vertex_attribute_descriptions,
                          r->vertex_attribute_descriptions,
                          attr_bytes) != 0 ||
                   memcmp(r->prev_vertex_binding_descriptions,
                          r->vertex_binding_descriptions,
                          bind_bytes) != 0;

    if (changed) {
        memcpy(r->prev_vertex_attribute_descriptions,
               r->vertex_attribute_descriptions, attr_bytes);
        memcpy(r->prev_vertex_binding_descriptions,
               r->vertex_binding_descriptions, bind_bytes);
        r->prev_num_active_vertex_attribute_descriptions =
            r->num_active_vertex_attribute_descriptions;
        r->prev_num_active_vertex_binding_descriptions =
            r->num_active_vertex_binding_descriptions;
        r->vertex_layout_snapshot_valid = true;
    }
    r->vertex_state_dirty = changed;
}

VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    int64_t nsprof_t0 = nsprof_begin();
    VkDeviceSize res = pgraph_vk_append_to_buffer(pg, BUFFER_INDEX_STAGING,
                                                  &data, &size, 1, 1);
    nsprof_end(NSPROF_GEOM_UPDATE, nsprof_t0);
    return res;
}

VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
    int64_t nsprof_t0 = nsprof_begin();
    VkDeviceSize res = pgraph_vk_append_to_buffer(
        pg, BUFFER_VERTEX_INLINE_STAGING, data, sizes, count, 1);
    nsprof_end(NSPROF_GEOM_UPDATE, nsprof_t0);
    return res;
}

/*
 * Exact-conflict refinement (XEMU_VTX_EXACT=0 disables): guest dirty
 * bits are page-granular, so vertex-stream sync writes arrive padded
 * to page boundaries and consecutive writes false-share their
 * boundary page — the padded bytes re-copy identical content. A
 * conflict with the recording command buffer is only real if the
 * incoming bytes DIFFER from the mirror somewhere inside a span the
 * recording window actually wrote (recorded draws can only have
 * consumed those spans from this window's uploads; mirror content
 * over a recorded span cannot change value within a window — any
 * differing write triggers the finish, and identical writes are
 * no-ops). Returns true when a differing byte exists (finish
 * required), false when every conflicting byte is identical.
 *
 * Measured (Azurik attract demo): forced mid-frame finishes drop
 * from 4-5.6 to ~0.9 per flip.
 */
static bool uploaded_span_content_differs(PGRAPHVkState *r, int slot,
                                          hwaddr offset, VkDeviceSize size,
                                          const uint8_t *data,
                                          size_t start_bit, size_t end_bit)
{
    static int exact = -1;
    if (exact < 0) {
        const char *e = getenv("XEMU_VTX_EXACT");
        exact = !(e && e[0] == '0');
    }
    if (!exact) {
        return true; /* legacy: every page-granular conflict finishes */
    }

    const unsigned long *bm = r->flight[slot].uploaded_bitmap;
    const uint16_t *smin = r->flight[slot].page_span_min;
    const uint16_t *smax = r->flight[slot].page_span_max;
    const uint8_t *mirror = r->storage_buffers[BUFFER_VERTEX_RAM].mapped;

    size_t bit = start_bit;
    for (;;) {
        bit = find_next_bit(bm, end_bit, bit);
        if (bit >= end_bit) {
            return false;
        }
        hwaddr page_base = (hwaddr)bit * TARGET_PAGE_SIZE;
        hwaddr in_lo = MAX(offset, page_base);
        hwaddr in_hi = MIN(offset + size, page_base + TARGET_PAGE_SIZE);
        hwaddr sp_lo = page_base + smin[bit];
        hwaddr sp_hi = page_base + smax[bit];
        hwaddr lo = MAX(in_lo, sp_lo);
        hwaddr hi = MIN(in_hi, sp_hi);
        if (lo < hi &&
            memcmp(mirror + lo, data + (lo - offset), hi - lo) != 0) {
            return true;
        }
        bit++;
    }
}

void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                        void *data, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_download_surfaces_in_range_if_dirty(pg, offset, size,
                                                  NSPROF_EV_SDOWN_VTXRAM);

    size_t start_bit = offset / TARGET_PAGE_SIZE;
    size_t end_bit = TARGET_PAGE_ALIGN(offset + size) / TARGET_PAGE_SIZE;
    size_t nbits = end_bit - start_bit;

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        if (!r->flight[i].uploaded_bitmap) {
            continue;
        }
        /*
         * Fast reject: if the query range doesn't intersect this slot's
         * tracked [first..last] dirty-page window, no overlap is
         * possible and we can skip the find_next_bit scan.
         */
        unsigned long slot_first = r->flight[i].uploaded_first_dirty_bit;
        if (slot_first == ULONG_MAX) {
            continue;
        }
        unsigned long slot_last = r->flight[i].uploaded_last_dirty_bit;
        if (start_bit > slot_last || end_bit <= slot_first) {
            continue;
        }
        if (find_next_bit(r->flight[i].uploaded_bitmap,
                          start_bit + nbits, start_bit) < end_bit) {
            if (i == r->current_flight) {
                if (!uploaded_span_content_differs(r, i, offset, size,
                                                   (const uint8_t *)data,
                                                   start_bit, end_bit)) {
                    /*
                     * Every conflicting byte is identical to what the
                     * recording window already uploaded — recorded
                     * draws observe the same data, so the finish (and
                     * its rotation + mid-frame reclaim wait) is a
                     * false positive from page padding.
                     */
                    nsprof_event(NSPROF_EV_VTX_EXACT_SKIP);
                    continue;
                }
                /*
                 * The currently recording command buffer references
                 * this range: it must be submitted before the mirror
                 * is overwritten, and the submit's fence waited.
                 */
                pgraph_vk_finish(pg, VK_FINISH_REASON_VERTEX_BUFFER_DIRTY);
                break;
            }
            /*
             * The conflicting GPU reads were already submitted with
             * slot i. Waiting that slot's fence is sufficient — the
             * previous behavior (a full pgraph_vk_finish) also
             * submitted and rotated the current command buffer,
             * costing a submit + cross-slot fence wait per conflict.
             * Measured in-game this fired 3-6x per flip on streamed
             * vertex data, serializing the 2-slot pipeline. After the
             * wait, the slot's upload tracking is cleared so further
             * writes this frame skip the (already signaled) fence.
             */
            pgraph_vk_wait_slot_fence(pg, i);
            bitmap_clear(r->flight[i].uploaded_bitmap, 0, r->bitmap_size);
            r->flight[i].uploaded_first_dirty_bit = ULONG_MAX;
            r->flight[i].uploaded_last_dirty_bit = 0;
        }
    }

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    int64_t nsprof_t0 = nsprof_begin();
    memcpy(r->storage_buffers[BUFFER_VERTEX_RAM].mapped + offset, data, size);
    nsprof_end(NSPROF_GEOM_UPDATE, nsprof_t0);

    {
        unsigned long *bm = r->flight[r->current_flight].uploaded_bitmap;
        uint16_t *smin = r->flight[r->current_flight].page_span_min;
        uint16_t *smax = r->flight[r->current_flight].page_span_max;
        hwaddr write_end = offset + size;
        for (size_t b = start_bit; b < end_bit; b++) {
            hwaddr page_base = (hwaddr)b * TARGET_PAGE_SIZE;
            uint16_t lo = offset > page_base ?
                              (uint16_t)(offset - page_base) : 0;
            uint16_t hi = write_end < page_base + TARGET_PAGE_SIZE ?
                              (uint16_t)(write_end - page_base) :
                              (uint16_t)TARGET_PAGE_SIZE;
            if (test_bit(b, bm)) {
                if (lo < smin[b]) {
                    smin[b] = lo;
                }
                if (hi > smax[b]) {
                    smax[b] = hi;
                }
            } else {
                set_bit(b, bm);
                smin[b] = lo;
                smax[b] = hi;
            }
        }
    }

    /*
     * Track dirty-page min/max so aux_has_work() and flush_memory_buffer()
     * can skip scanning the whole VRAM page bitmap when work is small.
     */
    if (nbits > 0) {
        unsigned long last_bit = start_bit + nbits - 1;
        if (start_bit <
            r->flight[r->current_flight].uploaded_first_dirty_bit) {
            r->flight[r->current_flight].uploaded_first_dirty_bit =
                start_bit;
        }
        if (last_bit >
            r->flight[r->current_flight].uploaded_last_dirty_bit) {
            r->flight[r->current_flight].uploaded_last_dirty_bit =
                last_bit;
        }
    }
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(r->vertex_ram_buffer_syncs));
    r->vertex_ram_buffer_syncs[r->num_vertex_ram_buffer_syncs++] =
        (MemorySyncRequirement){ .addr = addr, .size = size };
}

static const VkFormat float_to_count[] = {
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,
};

static const VkFormat ub_to_count[] = {
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_R8G8B8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
};

static const VkFormat s1_to_count[] = {
    VK_FORMAT_R16_SNORM,
    VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16B16_SNORM,
    VK_FORMAT_R16G16B16A16_SNORM,
};

static const VkFormat s32k_to_count[] = {
    VK_FORMAT_R16_SSCALED,
    VK_FORMAT_R16G16_SSCALED,
    VK_FORMAT_R16G16B16_SSCALED,
    VK_FORMAT_R16G16B16A16_SSCALED,
};

static char const * const vertex_data_array_format_to_str[] = {
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D] = "UB_D3D",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL] = "UB_OGL",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1] = "S1",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F] = "F",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K] = "S32K",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP] = "CMP",
};

void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int num_elements = max_element - min_element + 1;

    if (inline_data) {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        NV2A_VK_DGROUP_BEGIN("[attr %02d] format=%s, count=%d, stride=%d", i,
                             vertex_data_array_format_to_str[attr->format],
                             attr->count, attr->stride);
        r->vertex_attribute_to_description_location[i] = -1;
        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        VkFormat vk_format;
        bool needs_conversion = false;
        bool d3d_swizzle = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            assert(attr->count == 4);
            d3d_swizzle = true;
            /* fallthru */
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            assert(attr->count <= ARRAY_SIZE(ub_to_count));
            vk_format = ub_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            assert(attr->count <= ARRAY_SIZE(s1_to_count));
            vk_format = s1_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            assert(attr->count <= ARRAY_SIZE(float_to_count));
            vk_format = float_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            assert(attr->count <= ARRAY_SIZE(s32k_to_count));
            vk_format = s32k_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            vk_format =
                VK_FORMAT_R32_SINT; // VK_FORMAT_B10G11R11_UFLOAT_PACK32 ??
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;

        hwaddr start = 0;
        if (inline_data) {
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            start = attrib_data_addr + min_element * stride;
            update_memory_buffer(d, start, num_elements * stride);
        }

        uint32_t provoking_element_index =
            (provoking_element >= min_element) ?
                provoking_element - min_element : 0;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry =
                (uint8_t *)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pg->uniform_attrs |= 1 << i;
            pgraph_update_inline_value(pg, attr, last_entry);
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        NV2A_VK_DPRINTF("offset = %08" HWADDR_PRIx, attrib_data_addr);
        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(pg, attr, last_entry);

        r->vertex_attribute_to_description_location[i] =
            r->num_active_vertex_binding_descriptions;

        r->vertex_binding_descriptions
            [r->num_active_vertex_binding_descriptions++] =
            (VkVertexInputBindingDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .stride = stride,
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };

        r->vertex_attribute_descriptions
            [r->num_active_vertex_attribute_descriptions++] =
            (VkVertexInputAttributeDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .location = i,
                .format = vk_format,
            };

        /*
         * Store the offset of vertex `min_element`, not vertex 0, so that
         * the binding base lines up with the narrowed attribute copy in
         * copy_remapped_attributes_to_inline_buffer. Draws rebase via
         * firstVertex = absolute_index - min_element (or vertexOffset =
         * -min_element for indexed draws). inline_data callers always pass
         * min_element == 0, so the shift is a no-op for that path.
         */
        r->vertex_attribute_offsets[i] =
            attrib_data_addr + (hwaddr)min_element * stride;

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }
        if (d3d_swizzle) {
            pg->swizzle_attrs |= (1 << i);
        }

        NV2A_VK_DGROUP_END();
    }

    update_vertex_layout_dirty(r);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->inline_buffer_populated) {
            r->vertex_attribute_to_description_location[i] =
                r->num_active_vertex_binding_descriptions;
            r->vertex_binding_descriptions
                [r->num_active_vertex_binding_descriptions++] =
                (VkVertexInputBindingDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .stride = 4 * sizeof(float),
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                };
            r->vertex_attribute_descriptions
                [r->num_active_vertex_attribute_descriptions++] =
                (VkVertexInputAttributeDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .location = i,
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                };
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            r->vertex_attribute_to_description_location[i] = -1;
            pg->uniform_attrs |= 1 << i;
        }
    }

    update_vertex_layout_dirty(r);
}
