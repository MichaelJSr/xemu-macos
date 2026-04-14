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

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    vkResetFences(r->device, 1, &r->aux_fence);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, r->aux_fence));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    VK_CHECK(vkWaitForFences(r->device, 1, &r->aux_fence,
                             VK_TRUE, UINT64_MAX));

    r->in_aux_command_buffer = false;
}

void pgraph_vk_wait_for_previous_flight(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int slot = r->current_flight;

    if (r->flight[slot].submitted) {
        VK_CHECK(vkWaitForFences(r->device, 1, &r->flight[slot].fence,
                                 VK_TRUE, UINT64_MAX));
        r->flight[slot].submitted = false;
    }
}

void pgraph_vk_init_flight_partitions(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int ds_per_slot = ARRAY_SIZE(r->descriptor_sets) / NUM_FLIGHT_SLOTS;
    int cds_per_slot = ARRAY_SIZE(r->compute.descriptor_sets) / NUM_FLIGHT_SLOTS;
    VkDeviceSize staging_size = r->storage_buffers[BUFFER_STAGING_SRC].buffer_size;
    VkDeviceSize staging_per_slot = staging_size / NUM_FLIGHT_SLOTS;

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        r->flight[i].descriptor_set_base = i * ds_per_slot;
        r->flight[i].descriptor_set_limit = (i + 1) * ds_per_slot;
        r->flight[i].compute_descriptor_set_base = i * cds_per_slot;
        r->flight[i].compute_descriptor_set_limit = (i + 1) * cds_per_slot;
        r->flight[i].staging_buffer_base = i * staging_per_slot;
        r->flight[i].staging_buffer_limit = (i + 1) * staging_per_slot;
    }
}

void pgraph_vk_select_flight_slot(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int slot = r->current_flight;

    r->command_buffer = r->flight[slot].main_cb;
    r->aux_command_buffer = r->flight[slot].aux_cb;
    r->command_buffer_fence = r->flight[slot].fence;
    r->command_buffer_semaphore = r->flight[slot].semaphore;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    create_command_pool(pg);

    int total_cbs = NUM_FLIGHT_SLOTS * 2;
    VkCommandBuffer all_cbs[NUM_FLIGHT_SLOTS * 2];
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = total_cbs,
    };
    VK_CHECK(vkAllocateCommandBuffers(r->device, &alloc_info, all_cbs));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        r->flight[i].main_cb = all_cbs[i * 2];
        r->flight[i].aux_cb = all_cbs[i * 2 + 1];
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL,
                                &r->flight[i].fence));
        VK_CHECK(vkCreateSemaphore(r->device, &sem_info, NULL,
                                    &r->flight[i].semaphore));
        r->flight[i].submitted = false;
        r->flight[i].framebuffer_index = 0;
    }

    r->current_flight = 0;
    pgraph_vk_select_flight_slot(pg);

    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &r->aux_fence));
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDeviceWaitIdle(r->device);

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        vkDestroyFence(r->device, r->flight[i].fence, NULL);
        vkDestroySemaphore(r->device, r->flight[i].semaphore, NULL);
    }
    vkDestroyFence(r->device, r->aux_fence, NULL);

    int total_cbs = NUM_FLIGHT_SLOTS * 2;
    VkCommandBuffer all_cbs[NUM_FLIGHT_SLOTS * 2];
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        all_cbs[i * 2] = r->flight[i].main_cb;
        all_cbs[i * 2 + 1] = r->flight[i].aux_cb;
    }
    vkFreeCommandBuffers(r->device, r->command_pool, total_cbs, all_cbs);

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;

    destroy_command_pool(pg);
}
