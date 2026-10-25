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

#include "renderer.h"
#include "qemu/error-report.h"
#include <limits.h>

static void create_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vmaCreateBuffer(r->allocator, &buffer_create_info,
                             &buffer->alloc_info, &buffer->buffer,
                             &buffer->allocation, NULL));

    VmaAllocationInfo ai;
    vmaGetAllocationInfo(r->allocator, buffer->allocation, &ai);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &mem_props);
    VkMemoryPropertyFlags mem_flags =
        mem_props.memoryTypes[ai.memoryType].propertyFlags;
    buffer->properties = mem_flags;
    buffer->is_coherent =
        (mem_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

void pgraph_vk_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Profile buffer sizes

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    /* Surface downloads; grows on demand (pgraph_vk_reserve_staging_dst) */
    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = 16 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = 512 * 1024 * 1024,
    };

    /*
     * Compute buffers: depth-stencil pack/unpack and surface unswizzle.
     * Worst case at 10x scale of 640x480: 6400*4800*5 ~ 147 MiB.
     */
    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = 256 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .buffer_size = sizeof(pg->inline_elements) * 100,
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    /*
     * The mirror covers all of guest RAM, also above the NV2A's 128 MiB
     * (an experimental -m): a vertex array's offset is added after the
     * DMA base mask (nv_dma_map, nv2a_dma_addr_mask), so a draw can name
     * any RAM address, and the per-draw paths index the mirror and its
     * bitmaps unchecked.
     */
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    pgraph_vk_init_vtx_span_refute(r);

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        r->flight[i].uploaded_bitmap = bitmap_new(r->bitmap_size);
        bitmap_clear(r->flight[i].uploaded_bitmap, 0, r->bitmap_size);
        r->flight[i].uploaded_first_dirty_bit = ULONG_MAX;
        r->flight[i].uploaded_last_dirty_bit = 0;
        r->flight[i].read_bitmap = bitmap_new(r->bitmap_size);
        r->flight[i].whole_bitmap = bitmap_new(r->bitmap_size);
        r->flight[i].page_span_min =
            g_malloc_n(r->bitmap_size * VTX_PAGE_SPANS, sizeof(uint16_t));
        r->flight[i].page_span_max =
            g_malloc_n(r->bitmap_size * VTX_PAGE_SPANS, sizeof(uint16_t));
        r->flight[i].page_span_count = g_malloc_n(r->bitmap_size, 1);
    }

    /*
     * A slot holds the largest inline buffer one batch can make (every
     * attribute, a full batch of float4s); a larger request, only a remap
     * of a huge vertex range, grows the buffer (pgraph_vk_grow_staging).
     */
    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = NV2A_VERTEXSHADER_ATTRIBUTES *
                       (NV2A_MAX_BATCH_LENGTH + 1) * 4 * sizeof(float) *
                       NUM_FLIGHT_SLOTS,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size,
    };

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = 8 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    /*
     * Index, inline-vertex and uniform data are written into staging and
     * copied at each finish into a device-local twin that draws bind.
     * Where the staging memory is device-local and coherent (unified
     * memory: MoltenVK's shared type, KosmicKrisp), the twins would be
     * second copies in the same RAM, so they alias their staging buffers
     * instead: the slot discipline already keeps the CPU off a slot's
     * partition until its fence retires, and host writes before
     * vkQueueSubmit are visible to it. With nothing to copy, a finish then
     * submits one command buffer, not an extra one for the copies.
     */
    static const int twins[][2] = {
        { BUFFER_INDEX, BUFFER_INDEX_STAGING },
        { BUFFER_VERTEX_INLINE, BUFFER_VERTEX_INLINE_STAGING },
        { BUFFER_UNIFORM, BUFFER_UNIFORM_STAGING },
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        r->storage_buffers[i].buffer_limit = r->storage_buffers[i].buffer_size;
        if (i != BUFFER_INDEX && i != BUFFER_VERTEX_INLINE &&
            i != BUFFER_UNIFORM) {
            create_buffer(pg, &r->storage_buffers[i]);
        }
    }

    r->direct_staging = true;
    for (int i = 0; i < ARRAY_SIZE(twins); i++) {
        StorageBuffer *s = &r->storage_buffers[twins[i][1]];
        if (!(s->properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
            !s->is_coherent) {
            r->direct_staging = false;
        }
    }
    for (int i = 0; i < ARRAY_SIZE(twins); i++) {
        StorageBuffer *t = &r->storage_buffers[twins[i][0]];
        if (r->direct_staging) {
            t->buffer = r->storage_buffers[twins[i][1]].buffer;
            t->allocation = VK_NULL_HANDLE;   /* an alias: never destroyed */
        } else {
            create_buffer(pg, t);
        }
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_VERTEX_RAM,
                             BUFFER_INDEX_STAGING,
                             BUFFER_VERTEX_INLINE_STAGING,
                             BUFFER_UNIFORM_STAGING,
                             BUFFER_STAGING_SRC,
                             BUFFER_STAGING_DST };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        StorageBuffer *b = &r->storage_buffers[buffers_to_map[i]];
        VK_CHECK(vmaMapMemory(r->allocator, b->allocation,
                              (void **)&b->mapped));
    }
}

/*
 * A request that no empty slot of index or inline-vertex staging holds:
 * wait for the GPU to drain, then recreate the buffer (and its device twin,
 * unless aliased) with slots big enough. Only a remap of a huge vertex range
 * or an emulated draw of a huge one gets here; a stall beats overrunning
 * the slot. Call with no command buffer open, before the slot is written.
 */
void pgraph_vk_grow_staging(PGRAPHState *pg, int index, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int twin = index == BUFFER_INDEX_STAGING ? BUFFER_INDEX :
                                               BUFFER_VERTEX_INLINE;
    StorageBuffer *s = &r->storage_buffers[index];
    StorageBuffer *t = &r->storage_buffers[twin];

    nv2a_vk_assert(index == BUFFER_INDEX_STAGING ||
                   index == BUFFER_VERTEX_INLINE_STAGING);
    nv2a_vk_assert(!r->in_command_buffer);

    VkDeviceSize per_slot = s->buffer_size / NUM_FLIGHT_SLOTS;
    while (per_slot < size) {
        per_slot *= 2;
    }
    nv2a_vk_bounds_check(per_slot <= 2048ull * 1024 * 1024);
    warn_report("nv2a: a draw needs %" PRIu64 " MiB of %s staging; growing "
                "it to %" PRIu64 " MiB a slot", (uint64_t)(size >> 20),
                index == BUFFER_INDEX_STAGING ? "index" : "inline vertex",
                (uint64_t)(per_slot >> 20));

    for (int slot = 0; slot < NUM_FLIGHT_SLOTS; slot++) {
        pgraph_vk_wait_slot_fence(pg, slot);
    }

    vmaUnmapMemory(r->allocator, s->allocation);
    destroy_buffer(pg, s);
    if (t->allocation) {
        destroy_buffer(pg, t);
    }
    s->buffer_size = t->buffer_size = per_slot * NUM_FLIGHT_SLOTS;
    s->buffer_limit = t->buffer_limit = s->buffer_size;
    create_buffer(pg, s);
    VK_CHECK(vmaMapMemory(r->allocator, s->allocation, (void **)&s->mapped));
    if (r->direct_staging) {
        t->buffer = s->buffer;
    } else {
        create_buffer(pg, t);
    }

    /* The current slot is empty: every offset goes back to its base. */
    pgraph_vk_init_flight_partitions(pg);
}

/*
 * Surface downloads' host staging (BUFFER_STAGING_DST) holds at least size
 * bytes. Its only user records into a single-time command buffer and waits
 * for it, so the buffer is idle whenever that user starts one.
 */
void pgraph_vk_reserve_staging_dst(PGRAPHState *pg, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[BUFFER_STAGING_DST];

    if (size <= b->buffer_size) {
        return;
    }
    VkDeviceSize n = b->buffer_size;
    while (n < size) {
        n *= 2;
    }
    nv2a_vk_bounds_check(n <= 2048ull * 1024 * 1024);

    vmaUnmapMemory(r->allocator, b->allocation);
    destroy_buffer(pg, b);
    b->buffer_size = b->buffer_limit = n;
    create_buffer(pg, b);
    VK_CHECK(vmaMapMemory(r->allocator, b->allocation, (void **)&b->mapped));
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        StorageBuffer *b = &r->storage_buffers[i];
        if (!b->allocation) {
            b->buffer = VK_NULL_HANDLE;   /* a direct_staging alias */
            continue;
        }
        if (b->mapped) {
            vmaUnmapMemory(r->allocator, b->allocation);
        }
        destroy_buffer(pg, b);
    }

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        g_free(r->flight[i].uploaded_bitmap);
        r->flight[i].uploaded_bitmap = NULL;
        g_free(r->flight[i].read_bitmap);
        r->flight[i].read_bitmap = NULL;
        g_free(r->flight[i].whole_bitmap);
        r->flight[i].whole_bitmap = NULL;
        g_free(r->flight[i].page_span_min);
        r->flight[i].page_span_min = NULL;
        g_free(r->flight[i].page_span_max);
        r->flight[i].page_span_max = NULL;
        g_free(r->flight[i].page_span_count);
        r->flight[i].page_span_count = NULL;
    }
    pgraph_vk_finalize_vtx_span_refute(r);
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size, size_t count,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize worst_case_padding =
        (count > 1) ? (VkDeviceSize)(count - 1) * (alignment - 1) : 0;
    return (ROUND_UP(b->buffer_offset, alignment) + size + worst_case_padding)
           <= b->buffer_limit;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, count,
                                          alignment));

    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}
