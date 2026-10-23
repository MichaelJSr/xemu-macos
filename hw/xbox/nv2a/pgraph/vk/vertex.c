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
 * Whether the incoming bytes differ from the mirror anywhere a draw
 * recorded in @slot's window reads (its read spans). When they don't, no
 * draw of that slot still on the GPU reads a byte the copy changes, so
 * the copy need neither finish nor wait for it. Guest dirty bits are
 * page-granular and a draw's upload of a page also carries the bytes the
 * guest has written next to an earlier draw's since: each streamed batch
 * after the last, which the deterministic pusher uploads before the guest
 * writes the next (Azurik's death level: ~30 such conflicts a flip, each
 * a finish and a GPU round trip).
 */
static bool read_span_content_differs(PGRAPHVkState *r, int slot,
                                      hwaddr offset, VkDeviceSize size,
                                      const uint8_t *data,
                                      size_t start_bit, size_t end_bit)
{
    const unsigned long *rb = r->flight[slot].read_bitmap;
    const uint16_t *smin = r->flight[slot].page_span_min;
    const uint16_t *smax = r->flight[slot].page_span_max;
    const uint8_t *count = r->flight[slot].page_span_count;
    const uint8_t *mirror = r->storage_buffers[BUFFER_VERTEX_RAM].mapped;

    for (size_t bit = start_bit;; bit++) {
        bit = find_next_bit(rb, end_bit, bit);
        if (bit >= end_bit) {
            return false;
        }
        hwaddr page_base = (hwaddr)bit * TARGET_PAGE_SIZE;
        size_t first = bit * VTX_PAGE_SPANS;
        for (size_t k = first; k < first + count[bit]; k++) {
            hwaddr lo = MAX(offset, page_base + smin[k]);
            hwaddr hi = MIN(offset + size, page_base + smax[k]);
            if (lo < hi &&
                memcmp(mirror + lo, data + (lo - offset), hi - lo) != 0) {
                return true;
            }
        }
    }
}

/*
 * XEMU_VTX_SPAN_REFUTE=1: checks the read spans against an independent
 * record of what each slot's draws read of the mirror, taken from the
 * vertex buffer bindings themselves (offset, stride, element size and the
 * draw's vertex count) rather than from the syncs that feed the spans.
 * Every finish or fence wait a copy skips is checked: a recorded read
 * whose bytes the copy changes is a violation (a GPU reader the spans
 * miss). Counts print at exit.
 */
typedef struct VtxRefuteRead {
    hwaddr lo, hi;
} VtxRefuteRead;

static uint64_t vtx_refute_checks, vtx_refute_violations;

static void vtx_refute_dump(void)
{
    fprintf(stderr,
            "xemu: vtx-span refute checks=%" PRIu64 " violations=%" PRIu64
            "\n", vtx_refute_checks, vtx_refute_violations);
}

void pgraph_vk_init_vtx_span_refute(PGRAPHVkState *r)
{
    static bool registered;
    const char *e = getenv("XEMU_VTX_SPAN_REFUTE");

    r->vtx_span_refute = e && *e && strcmp(e, "0");
    if (!r->vtx_span_refute) {
        return;
    }
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        r->flight[i].refute_reads =
            g_array_new(FALSE, FALSE, sizeof(VtxRefuteRead));
    }
    if (!registered) {
        registered = true;
        atexit(vtx_refute_dump);
    }
}

void pgraph_vk_finalize_vtx_span_refute(PGRAPHVkState *r)
{
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        if (r->flight[i].refute_reads) {
            g_array_free(r->flight[i].refute_reads, TRUE);
            r->flight[i].refute_reads = NULL;
        }
    }
}

void pgraph_vk_refute_forget(PGRAPHVkState *r, int slot)
{
    if (r->flight[slot].refute_reads) {
        g_array_set_size(r->flight[slot].refute_reads, 0);
    }
}

/* A binding of the draw being recorded reads the mirror at @offset. */
void pgraph_vk_refute_note_read(PGRAPHVkState *r, hwaddr offset,
                                uint32_t stride, uint32_t element_size)
{
    GArray *a = r->flight[r->current_flight].refute_reads;
    hwaddr vram_end = (hwaddr)r->bitmap_size * TARGET_PAGE_SIZE;
    uint32_t n = r->refute_draw_vertices;
    /* A path that binds the mirror without a vertex count: assume all. */
    hwaddr hi = n ? offset + (hwaddr)(n - 1) * stride + element_size :
                    vram_end;
    VtxRefuteRead rr = { .lo = offset, .hi = MIN(hi, vram_end) };

    if (!a || rr.lo >= rr.hi) {
        return;
    }
    if (a->len) {
        VtxRefuteRead *last = &g_array_index(a, VtxRefuteRead, a->len - 1);
        if (last->lo == rr.lo && last->hi == rr.hi) {
            return;
        }
    }
    g_array_append_val(a, rr);
}

/* A copy skipped @slot's finish or fence wait: check that's sound. */
static void vtx_refute_skip(PGRAPHVkState *r, int slot, hwaddr offset,
                            VkDeviceSize size, const uint8_t *data)
{
    GArray *a = r->flight[slot].refute_reads;
    const uint8_t *mirror = r->storage_buffers[BUFFER_VERTEX_RAM].mapped;
    hwaddr end = offset + size;
    bool overlaps = false;

    if (!a) {
        return;
    }
    for (guint k = 0; k < a->len; k++) {
        VtxRefuteRead *rr = &g_array_index(a, VtxRefuteRead, k);
        hwaddr lo = MAX(rr->lo, offset);
        hwaddr hi = MIN(rr->hi, end);
        if (lo >= hi) {
            continue;
        }
        overlaps = true;
        if (memcmp(mirror + lo, data + (lo - offset), hi - lo) != 0) {
            if (vtx_refute_violations < 8) {
                fprintf(stderr,
                        "xemu: vtx-span refute violation: slot %d%s copy "
                        "[0x%" HWADDR_PRIx ", +0x%" PRIx64 ") changes read "
                        "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")\n",
                        slot, slot == r->current_flight ? " (recording)" : "",
                        offset, (uint64_t)size, rr->lo, rr->hi);
            }
            vtx_refute_violations++;
            break;
        }
    }
    if (overlaps) {
        vtx_refute_checks++;
    }
}

/* @slot's GPU reads have retired (or it never recorded any). */
static void forget_slot_vertex_ram(PGRAPHVkState *r, int slot)
{
    bitmap_clear(r->flight[slot].uploaded_bitmap, 0, r->bitmap_size);
    bitmap_clear(r->flight[slot].read_bitmap, 0, r->bitmap_size);
    r->flight[slot].uploaded_first_dirty_bit = ULONG_MAX;
    r->flight[slot].uploaded_last_dirty_bit = 0;
    pgraph_vk_refute_forget(r, slot);
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
        if (i != r->current_flight) {
            /*
             * A submitted slot: its draws may still be on the GPU, and
             * read only the bytes in its read spans. The copy waits for
             * them only when it changes one of those bytes, on any page
             * they read, whether or not the slot's window uploaded it.
             * After the wait, the slot's tracking is cleared so further
             * writes this frame skip the (already signaled) fence.
             *
             * This used to wait whenever the slot had uploaded any of
             * the copy's pages (it already beat a full
             * pgraph_vk_finish, which also submitted and rotated the
             * current command buffer: measured in-game 3-6x per flip
             * on streamed vertex data, serializing the 2-slot pipeline).
             */
            if (read_span_content_differs(r, i, offset, size,
                                          (const uint8_t *)data,
                                          start_bit, end_bit)) {
                pgraph_vk_wait_slot_fence(pg, i);
                forget_slot_vertex_ram(r, i);
                continue;
            }
            if (r->vtx_span_refute) {
                vtx_refute_skip(r, i, offset, size, (const uint8_t *)data);
            }
            if (r->flight[i].uploaded_first_dirty_bit != ULONG_MAX &&
                find_next_bit(r->flight[i].uploaded_bitmap, end_bit,
                              start_bit) < end_bit) {
                nsprof_event(NSPROF_EV_VTX_WAIT_SKIP);
            }
            continue;
        }
        /*
         * The recording command buffer: a conflict is only real if the
         * copy changes a byte one of its draws reads (its read spans).
         * The page-wide check this replaced finished whenever an
         * uploaded page changed anywhere, ~34 times a flip in d2 under
         * icount, where every streamed batch's upload carried the next
         * batch's bytes.
         */
        if (!read_span_content_differs(r, i, offset, size,
                                       (const uint8_t *)data, start_bit,
                                       end_bit)) {
            if (r->vtx_span_refute) {
                vtx_refute_skip(r, i, offset, size, (const uint8_t *)data);
            }
            if (r->flight[i].uploaded_first_dirty_bit != ULONG_MAX &&
                find_next_bit(r->flight[i].uploaded_bitmap, end_bit,
                              start_bit) < end_bit) {
                nsprof_event(NSPROF_EV_VTX_EXACT_SKIP);
            }
            continue;
        }
        /*
         * It must be submitted before the mirror is overwritten, and the
         * submit's fence waited.
         *
         * pgraph_vk_finish alone no longer guarantees the second half:
         * with flight-slot pipelining it waits only the PREVIOUS slot's
         * fence and leaves the just-submitted CB executing (upstream's
         * single-slot finish really did drain). Without the explicit wait
         * below, the memcpy into the mirror races the submitted CB's
         * vertex fetches of the old content — torn geometry on any
         * driver, merely masked on macOS by MoltenVK's deferred encode
         * delay. Wait the slot we just submitted, then clear its tracking
         * (its reads have retired) so later writes this frame skip the
         * already-signaled fence entirely.
         */
        int submitted_slot = r->current_flight;
        pgraph_vk_finish(pg, VK_FINISH_REASON_VERTEX_BUFFER_DIRTY);
        pgraph_vk_wait_slot_fence(pg, submitted_slot);
        forget_slot_vertex_ram(r, submitted_slot);
        break;
    }

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    int64_t nsprof_t0 = nsprof_begin();
    memcpy(r->storage_buffers[BUFFER_VERTEX_RAM].mapped + offset, data, size);
    nsprof_end(NSPROF_GEOM_UPDATE, nsprof_t0);

    bitmap_set(r->flight[r->current_flight].uploaded_bitmap, start_bit, nbits);

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

/*
 * A draw's attribute array: sync [addr, addr + size) with guest RAM, and
 * note that the draw reads [addr, addr + read_size) of the mirror.
 */
static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size,
                                 hwaddr read_size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(r->vertex_ram_buffer_syncs));
    r->vertex_ram_buffer_reads[r->num_vertex_ram_buffer_syncs] =
        (MemorySyncRequirement){ .addr = addr, .size = read_size };
    r->vertex_ram_buffer_syncs[r->num_vertex_ram_buffer_syncs++] =
        (MemorySyncRequirement){ .addr = addr, .size = size };
}

/*
 * As a draw is recorded (begin_draw): note the mirror bytes it reads in
 * the current slot's window (read_bitmap and the page spans), for the
 * copies in pgraph_vk_update_vertex_ram_buffer.
 */
void pgraph_vk_note_vertex_ram_reads(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int slot = r->current_flight;
    unsigned long *rb = r->flight[slot].read_bitmap;
    uint16_t *smin = r->flight[slot].page_span_min;
    uint16_t *smax = r->flight[slot].page_span_max;
    uint8_t *count = r->flight[slot].page_span_count;
    hwaddr vram_end = (hwaddr)r->bitmap_size * TARGET_PAGE_SIZE;

    for (size_t i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        hwaddr lo = r->vertex_ram_buffer_reads[i].addr;
        hwaddr hi = MIN(lo + r->vertex_ram_buffer_reads[i].size, vram_end);

        for (hwaddr page = lo & TARGET_PAGE_MASK; page < hi;
             page += TARGET_PAGE_SIZE) {
            size_t b = page / TARGET_PAGE_SIZE;
            uint16_t plo = lo > page ? (uint16_t)(lo - page) : 0;
            uint16_t phi = hi < page + TARGET_PAGE_SIZE ?
                               (uint16_t)(hi - page) :
                               (uint16_t)TARGET_PAGE_SIZE;

            uint16_t *mn = &smin[b * VTX_PAGE_SPANS];
            uint16_t *mx = &smax[b * VTX_PAGE_SPANS];

            if (!test_bit(b, rb)) {
                set_bit(b, rb);
                mn[0] = plo;
                mx[0] = phi;
                count[b] = 1;
                continue;
            }
            /*
             * Join a span this read overlaps or touches; else take a
             * free one; else widen the nearest (a superset either way).
             * Separate spans keep a gap between two draws' bytes on a
             * page out: Azurik's next batch is often written there.
             */
            int c = count[b], best = 0;
            unsigned best_gap = UINT_MAX;
            for (int k = 0; k < c; k++) {
                unsigned gap = phi < mn[k] ? mn[k] - phi :
                               plo > mx[k] ? plo - mx[k] : 0;
                if (gap < best_gap) {
                    best_gap = gap;
                    best = k;
                }
            }
            if (best_gap > 0 && c < VTX_PAGE_SPANS) {
                mn[c] = plo;
                mx[c] = phi;
                count[b] = c + 1;
            } else {
                mn[best] = MIN(mn[best], plo);
                mx[best] = MAX(mx[best], phi);
            }
        }
    }
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

#if DEBUG_VK /* only NV2A_VK_DGROUP_BEGIN reads it */
static char const * const vertex_data_array_format_to_str[] = {
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D] = "UB_D3D",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL] = "UB_OGL",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1] = "S1",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F] = "F",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K] = "S32K",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP] = "CMP",
};
#endif

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
            /* The last element may reach past its stride. */
            update_memory_buffer(
                d, start, num_elements * stride,
                (hwaddr)(num_elements - 1) * stride +
                    MAX(stride, (size_t)attr->size * attr->count));
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
