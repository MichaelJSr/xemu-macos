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
    buffer->properties = mem_props.memoryTypes[ai.memoryType].propertyFlags;
    buffer->is_coherent =
        (buffer->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

/*
 * Create `buffer` with its backing VkDeviceMemory imported from an
 * external host pointer (VK_EXT_external_memory_host). Used for
 * BUFFER_VERTEX_RAM so the buffer is a live view onto QEMU's guest
 * VRAM instead of a Vulkan-owned copy. Returns true on success; if
 * any step fails we fall back to the VMA-allocated path so the
 * renderer still boots.
 */
static bool create_host_imported_buffer(PGRAPHState *pg, StorageBuffer *buffer,
                                        void *host_ptr)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkExternalMemoryBufferCreateInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ext_info,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer vk_buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(r->device, &buffer_create_info, NULL, &vk_buffer)
            != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(r->device, vk_buffer, &buf_req);

    VkMemoryHostPointerPropertiesEXT host_ptr_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    if (vkGetMemoryHostPointerPropertiesEXT(
            r->device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            host_ptr, &host_ptr_props) != VK_SUCCESS) {
        vkDestroyBuffer(r->device, vk_buffer, NULL);
        return false;
    }

    /*
     * Memory type must be both HOST_VISIBLE (we need to read/write from
     * the guest side) and usable by the buffer (type_bits intersection).
     * We require HOST_COHERENT so downstream code that treats BUFFER_VERTEX_RAM
     * as coherent (see flush_memory_buffer in draw.c) keeps working.
     */
    uint32_t type_bits =
        buf_req.memoryTypeBits & host_ptr_props.memoryTypeBits;
    uint32_t mem_type = pgraph_vk_get_memory_type(
        pg, type_bits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == 0xFFFFFFFF) {
        vkDestroyBuffer(r->device, vk_buffer, NULL);
        return false;
    }

    VkImportMemoryHostPointerInfoEXT import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = host_ptr,
    };
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        /* Allocation size must be a multiple of the buffer's required size
         * and is commonly the full VRAM region size. */
        .allocationSize = buffer->buffer_size,
        .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(r->device, &alloc_info, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(r->device, vk_buffer, NULL);
        return false;
    }
    if (vkBindBufferMemory(r->device, vk_buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(r->device, memory, NULL);
        vkDestroyBuffer(r->device, vk_buffer, NULL);
        return false;
    }

    buffer->buffer = vk_buffer;
    buffer->allocation = VK_NULL_HANDLE; /* VMA does not own this */
    buffer->host_imported = true;
    buffer->host_imported_memory = memory;
    buffer->mapped = (uint8_t *)host_ptr;
    buffer->properties =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    buffer->is_coherent = true;
    return true;
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (buffer->host_imported) {
        if (buffer->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(r->device, buffer->buffer, NULL);
            buffer->buffer = VK_NULL_HANDLE;
        }
        if (buffer->host_imported_memory != VK_NULL_HANDLE) {
            vkFreeMemory(r->device, buffer->host_imported_memory, NULL);
            buffer->host_imported_memory = VK_NULL_HANDLE;
        }
        buffer->host_imported = false;
        buffer->mapped = NULL;
        return;
    }

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

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = 512 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_STAGING_DST].buffer_size,
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
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    /*
     * Decide whether we can host-import d->vram_ptr for BUFFER_VERTEX_RAM.
     * Requires the extension, plus the host pointer and the allocation
     * size to both be multiples of minImportedHostPointerAlignment. QEMU
     * MemoryRegions are host-page aligned via mmap, which satisfies
     * MoltenVK's typical 4 KiB requirement, but we gate defensively in
     * case the driver reports a larger alignment than the host page size.
     */
    {
        VkDeviceSize align = r->external_memory_host_min_alignment;
        bool ptr_ok = align > 0 &&
            ((uintptr_t)d->vram_ptr & (align - 1)) == 0;
        bool size_ok = align > 0 &&
            (memory_region_size(d->vram) & (align - 1)) == 0;
        r->external_memory_host_enabled =
            r->external_memory_host_extension_enabled && ptr_ok && size_ok;
    }

    /*
     * With VK_EXT_external_memory_host on, BUFFER_VERTEX_RAM is a live
     * view of guest VRAM, not a separate allocation; STORAGE_BUFFER is
     * also advertised so the compute unswizzle shader can bind it as
     * input. With the extension off we keep the legacy VMA allocation
     * (STORAGE_BUFFER is still advertised but unused).
     */
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        r->flight[i].uploaded_bitmap = bitmap_new(r->bitmap_size);
        bitmap_clear(r->flight[i].uploaded_bitmap, 0, r->bitmap_size);
        r->flight[i].uploaded_first_dirty_bit = ULONG_MAX;
        r->flight[i].uploaded_last_dirty_bit = 0;
    }

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = NV2A_VERTEXSHADER_ATTRIBUTES * NV2A_MAX_BATCH_LENGTH *
                       4 * sizeof(float) * 10,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        r->storage_buffers[i].buffer_limit = r->storage_buffers[i].buffer_size;

        /*
         * BUFFER_VERTEX_RAM gets the host-pointer import path when the
         * extension is usable; everything else (and the fallback) uses
         * the VMA allocator.
         */
        if (i == BUFFER_VERTEX_RAM && r->external_memory_host_enabled) {
            if (create_host_imported_buffer(pg, &r->storage_buffers[i],
                                             d->vram_ptr)) {
                fprintf(stderr,
                        "- BUFFER_VERTEX_RAM imported from d->vram_ptr "
                        "(VK_EXT_external_memory_host, %zu MiB)\n",
                        r->storage_buffers[i].buffer_size / (1024 * 1024));
                continue;
            }
            /*
             * Host import failed (e.g. driver bug). Fall through to the
             * VMA path so the renderer still boots; texture.c's direct-
             * VRAM compute path stays gated on host_imported.
             */
            r->external_memory_host_enabled = false;
            fprintf(stderr,
                    "- BUFFER_VERTEX_RAM host import failed; "
                    "falling back to VMA allocation\n");
        }

        create_buffer(pg, &r->storage_buffers[i]);
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
        /* Host-imported buffers already have `mapped` set to the host ptr. */
        if (b->host_imported) {
            continue;
        }
        VK_CHECK(vmaMapMemory(r->allocator, b->allocation,
                              (void **)&b->mapped));
    }
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        StorageBuffer *b = &r->storage_buffers[i];
        /*
         * Host-imported buffers never went through vmaMapMemory — skip
         * the corresponding unmap. destroy_buffer dispatches on the
         * host_imported flag to pick the right teardown.
         */
        if (b->mapped && !b->host_imported) {
            vmaUnmapMemory(r->allocator, b->allocation);
        }
        destroy_buffer(pg, b);
    }

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        g_free(r->flight[i].uploaded_bitmap);
        r->flight[i].uploaded_bitmap = NULL;
    }
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
