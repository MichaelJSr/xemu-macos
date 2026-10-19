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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "renderer.h"
#include "hw/xbox/nv2a/nsprof.h"
#include "ui/xemu-settings.h"
#include <glib/gstdio.h>
#include <math.h>

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    r->nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (r->nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

/* A guest triangle fan or filled polygon: one fan of triangles. */
static bool is_fan_draw(int primitive_mode, int polygon_mode)
{
    return primitive_mode == PRIM_TYPE_TRIANGLE_FAN ||
           (primitive_mode == PRIM_TYPE_POLYGON &&
            polygon_mode == POLY_MODE_FILL);
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int polygon_mode = r->shader_binding->state.geom.polygon_front_mode;
    int primitive_mode = r->shader_binding->state.geom.primitive_mode;

    // TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
    if (!r->supports_geometry_shaders) {
        switch (primitive_mode) {
        case PRIM_TYPE_LINE_LOOP:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PRIM_TYPE_QUADS:
        case PRIM_TYPE_QUAD_STRIP:
        case PRIM_TYPE_TRIANGLES:
            if (polygon_mode == POLY_MODE_FILL) {
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            } else if (polygon_mode == POLY_MODE_LINE) {
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            } else {
                return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            }
        case PRIM_TYPE_POLYGON:
            if (polygon_mode == POLY_MODE_LINE) {
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            }
            break;
        default:
            break;
        }
    }

    if (r->emulate_triangle_fans && is_fan_draw(primitive_mode, polygon_mode)) {
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    // FIXME: Replace with LUT
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINE_LOOP:
        // FIXME: line strips, except that the first and last vertices are also used as a line
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME
        } else if (polygon_mode == POLY_MODE_FILL) {
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        nv2a_vk_assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        nv2a_vk_assert(!"Invalid primitive_mode");
        return 0;
    }
}

/* nsprof: one per Vulkan draw of a guest triangle fan or filled polygon
 * (native TRIANGLE_FAN, or its CPU triangle list). */
static void count_fan_draw(PGRAPHState *pg)
{
    if (nsprof_enabled() &&
        is_fan_draw(pg->primitive_mode,
                    GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                             NV_PGRAPH_SETUPRASTER_FRONTFACEMODE))) {
        nsprof_event(NSPROF_EV_VK_FAN_DRAW);
    }
}

static bool draw_needs_primitive_emulation(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Read primitive and polygon modes directly from hardware registers instead
    // of relying on r->shader_binding. This prevents stale shader state from
    // causing assertions or crashes (SIGSEGV) when binding shaders mid-draw,
    // particularly noticeable on macOS/MoltenVK.
    int polygon_mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    int primitive_mode = pg->primitive_mode;

    if (r->emulate_triangle_fans && is_fan_draw(primitive_mode, polygon_mode)) {
        return true;
    }

    if (r->supports_geometry_shaders) {
        return false;
    }

    switch (primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
        return true;
    case PRIM_TYPE_TRIANGLES: {
        // MoltenVK lacks reliable support for VK_EXT_provoking_vertex.
        // We detect the expected provoking vertex behavior from hardware
        // registers to emulate the vertex rotation on the CPU later if needed.
        bool first_vertex = (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3) &
                             NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                            NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
        return first_vertex;
    }
    case PRIM_TYPE_POLYGON:
        return polygon_mode == POLY_MODE_LINE;
    default:
        return false;
    }
}

static size_t get_max_emulated_index_count(PGRAPHState *pg, uint32_t in_count)
{
    int polygon_mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    int primitive_mode = pg->primitive_mode;

    size_t result = 0;
    switch (primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
        result = in_count > 1 ? in_count * 2 : 0;
        break;
    case PRIM_TYPE_QUADS: {
        size_t quads = in_count / 4;
        if (polygon_mode == POLY_MODE_FILL) {
            result = quads * 6;
        } else if (polygon_mode == POLY_MODE_LINE) {
            result = quads * 8;
        } else {
            result = quads * 4;
        }
        break;
    }
    case PRIM_TYPE_QUAD_STRIP: {
        size_t quads = in_count >= 4 ? (in_count - 2) / 2 : 0;
        if (polygon_mode == POLY_MODE_FILL) {
            result = quads * 6;
        } else if (polygon_mode == POLY_MODE_LINE) {
            result = quads * 8;
        } else {
            result = quads * 4;
        }
        break;
    }
    case PRIM_TYPE_TRIANGLES:
        result = in_count;
        break;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_FILL) {
            result = in_count >= 3 ? (in_count - 2) * 3 : 0;
        } else {
            result = (polygon_mode == POLY_MODE_LINE && in_count > 1) ?
                         in_count * 2 :
                         0;
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
        result = in_count >= 3 ? (in_count - 2) * 3 : 0;
        break;
    default:
        result = 0;
        break;
    }

    return result;
}

static uint32_t *get_emulated_indices_buf(PGRAPHVkState *r, size_t count)
{
    if (count > r->emulated_indices_buf_size) {
        g_free(r->emulated_indices_buf);
        r->emulated_indices_buf_size = MAX(count, r->emulated_indices_buf_size * 2);
        r->emulated_indices_buf =
            g_malloc_n(r->emulated_indices_buf_size, sizeof(uint32_t));
    }
    return r->emulated_indices_buf;
}

static size_t build_emulated_indices_from_array(PGRAPHState *pg, uint32_t start,
                                                uint32_t count, uint32_t *out)
{
    int polygon_mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    int primitive_mode = pg->primitive_mode;
    size_t j = 0;

#define EMIT2(a, b)     \
    do {                \
        out[j++] = (a); \
        out[j++] = (b); \
    } while (0)
#define EMIT3(a, b, c)  \
    do {                \
        out[j++] = (a); \
        out[j++] = (b); \
        out[j++] = (c); \
    } while (0)
#define EMIT4(a, b, c, d) \
    do {                  \
        out[j++] = (a);   \
        out[j++] = (b);   \
        out[j++] = (c);   \
        out[j++] = (d);   \
    } while (0)

    if (is_fan_draw(primitive_mode, polygon_mode)) {
        /* Fan triangle i is (v[i+1], v[i+2], v0), the Vulkan order:
         * same winding, same provoking vertex. */
        for (uint32_t i = 0; i + 2 < count; i++) {
            EMIT3(start + i + 1, start + i + 2, start);
        }
        goto out;
    }

    switch (primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_POLYGON:
        if (count <= 1) {
            break;
        }
        for (uint32_t i = 0; i < count - 1; i++) {
            EMIT2(start + i, start + i + 1);
        }
        EMIT2(start + count - 1, start);
        break;
    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            uint32_t v0 = start + i;
            uint32_t v1 = start + i + 1;
            uint32_t v2 = start + i + 2;
            uint32_t v3 = start + i + 3;
            if (polygon_mode == POLY_MODE_FILL) {
                EMIT3(v0, v1, v3);
                EMIT3(v1, v2, v3);
            } else if (polygon_mode == POLY_MODE_LINE) {
                EMIT2(v0, v1);
                EMIT2(v1, v2);
                EMIT2(v2, v3);
                EMIT2(v3, v0);
            } else {
                EMIT4(v0, v1, v2, v3);
            }
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            uint32_t v0 = start + i;
            uint32_t v1 = start + i + 1;
            uint32_t v2 = start + i + 2;
            uint32_t v3 = start + i + 3;
            if (polygon_mode == POLY_MODE_FILL) {
                EMIT3(v0, v1, v3);
                EMIT3(v0, v3, v2);
            } else if (polygon_mode == POLY_MODE_LINE) {
                EMIT2(v0, v1);
                EMIT2(v1, v3);
                EMIT2(v3, v2);
                EMIT2(v2, v0);
            } else {
                EMIT4(v0, v1, v2, v3);
            }
        }
        break;
    case PRIM_TYPE_TRIANGLES: {
        bool first_vertex = (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3) &
                             NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                            NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
        if (first_vertex && polygon_mode == POLY_MODE_FILL) {
            // Emulate provoking vertex = first (required by Xbox) on macOS by
            // rotating the indices manually since Apple's Metal API lacks
            // provoking vertex control.
            for (uint32_t i = 0; i + 2 < count; i += 3) {
                uint32_t v0 = start + i;
                uint32_t v1 = start + i + 1;
                uint32_t v2 = start + i + 2;
                EMIT3(v1, v2, v0);
            }
        }
        break;
    }
    default:
        break;
    }

out:
#undef EMIT4
#undef EMIT3
#undef EMIT2
    return j;
}

static size_t build_emulated_indices_from_elements(PGRAPHState *pg,
                                                   const uint32_t *in,
                                                   uint32_t in_count,
                                                   uint32_t *out)
{
    int polygon_mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    int primitive_mode = pg->primitive_mode;
    size_t j = 0;

#define EMIT2(a, b)     \
    do {                \
        out[j++] = (a); \
        out[j++] = (b); \
    } while (0)
#define EMIT3(a, b, c)  \
    do {                \
        out[j++] = (a); \
        out[j++] = (b); \
        out[j++] = (c); \
    } while (0)
#define EMIT4(a, b, c, d) \
    do {                  \
        out[j++] = (a);   \
        out[j++] = (b);   \
        out[j++] = (c);   \
        out[j++] = (d);   \
    } while (0)

    if (is_fan_draw(primitive_mode, polygon_mode)) {
        /*
         * Fan triangle i is (v[i+1], v[i+2], v0), the Vulkan order. The
         * native path enabled primitive restart for fans, so a
         * 0xFFFFFFFF index starts a new fan here too.
         */
        uint32_t first = 0;
        while (first < in_count) {
            uint32_t end = first;
            while (end < in_count && in[end] != 0xFFFFFFFF) {
                end++;
            }
            for (uint32_t i = first; i + 2 < end; i++) {
                EMIT3(in[i + 1], in[i + 2], in[first]);
            }
            first = end + 1;
        }
        goto out;
    }

    switch (primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_POLYGON:
        if (in_count <= 1) {
            break;
        }
        for (uint32_t i = 0; i < in_count - 1; i++) {
            EMIT2(in[i], in[i + 1]);
        }
        EMIT2(in[in_count - 1], in[0]);
        break;
    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < in_count; i += 4) {
            uint32_t v0 = in[i];
            uint32_t v1 = in[i + 1];
            uint32_t v2 = in[i + 2];
            uint32_t v3 = in[i + 3];
            if (polygon_mode == POLY_MODE_FILL) {
                EMIT3(v0, v1, v3);
                EMIT3(v1, v2, v3);
            } else if (polygon_mode == POLY_MODE_LINE) {
                EMIT2(v0, v1);
                EMIT2(v1, v2);
                EMIT2(v2, v3);
                EMIT2(v3, v0);
            } else {
                EMIT4(v0, v1, v2, v3);
            }
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < in_count; i += 2) {
            uint32_t v0 = in[i];
            uint32_t v1 = in[i + 1];
            uint32_t v2 = in[i + 2];
            uint32_t v3 = in[i + 3];
            if (polygon_mode == POLY_MODE_FILL) {
                EMIT3(v0, v1, v3);
                EMIT3(v0, v3, v2);
            } else if (polygon_mode == POLY_MODE_LINE) {
                EMIT2(v0, v1);
                EMIT2(v1, v3);
                EMIT2(v3, v2);
                EMIT2(v2, v0);
            } else {
                EMIT4(v0, v1, v2, v3);
            }
        }
        break;
    case PRIM_TYPE_TRIANGLES: {
        bool first_vertex = (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3) &
                             NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                            NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
        if (first_vertex && polygon_mode == POLY_MODE_FILL) {
            // Emulate provoking vertex = first (required by Xbox) on macOS by
            // rotating the indices manually since Apple's Metal API lacks
            // provoking vertex control.
            for (uint32_t i = 0; i + 2 < in_count; i += 3) {
                EMIT3(in[i + 1], in[i + 2], in[i]);
            }
        }
        break;
    }
    default:
        break;
    }

out:
#undef EMIT4
#undef EMIT3
#undef EMIT2
    return j;
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    nv2a_vk_assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

/*
 * One file per driver: each rejects the other's blob (its header carries
 * the driver's pipelineCacheUUID), so a shared file would go cold at every
 * switch or fallback. MoltenVK keeps the name it always had.
 */
static char *get_pipeline_cache_path(PGRAPHVkState *r)
{
    const char *base = xemu_settings_get_base_path();
#ifdef __APPLE__
    if (r->driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP) {
        return g_strdup_printf("%spipeline_cache-kosmickrisp.bin", base);
    }
#endif
    return g_strdup_printf("%spipeline_cache.bin", base);
}

static void load_pipeline_cache_from_disk(PGRAPHVkState *r, void **data,
                                          size_t *size)
{
    *data = NULL;
    *size = 0;

    g_autofree char *path = get_pipeline_cache_path(r);
    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size > 0) {
        *data = g_malloc(file_size);
        if (fread(*data, 1, file_size, f) == file_size) {
            *size = file_size;
            fprintf(stderr, "Loaded pipeline cache (%zu bytes) from %s\n",
                    *size, path);
        } else {
            g_free(*data);
            *data = NULL;
        }
    }
    fclose(f);
}

static void save_pipeline_cache_to_disk(PGRAPHVkState *r)
{
    VkDevice device = r->device;
    VkPipelineCache cache = r->vk_pipeline_cache;
    int64_t t0 = g_get_monotonic_time();
    size_t size = 0;
    VkResult result = vkGetPipelineCacheData(device, cache, &size, NULL);
    if (result != VK_SUCCESS || size == 0) {
        return;
    }

    void *data = g_malloc(size);
    result = vkGetPipelineCacheData(device, cache, &size, data);
    if (result == VK_SUCCESS) {
        char *path = get_pipeline_cache_path(r);
        /* Write-then-rename so a kill mid-write can't leave a torn
         * cache file for the next boot to ingest. The temp name carries
         * the pid so instances sharing one cache (the benches run up to
         * four) can't interleave their writes; init_pipeline_cache
         * sweeps the orphans a kill leaves. g_rename replaces
         * an existing target on Windows too (plain rename() fails
         * there once the file exists). */
        char *tmp_path =
            g_strdup_printf("%s.%u.tmp", path, (unsigned)getpid());
        FILE *f = qemu_fopen(tmp_path, "wb");
        if (f) {
            bool ok = fwrite(data, 1, size, f) == size;
            ok &= fclose(f) == 0;
            if (ok && g_rename(tmp_path, path) == 0) {
                fprintf(stderr,
                        "Saved pipeline cache (%zu bytes, %.1f ms) to %s\n",
                        size, (g_get_monotonic_time() - t0) / 1000.0, path);
            } else {
                g_unlink(tmp_path);
            }
        }
        g_free(tmp_path);
        g_free(path);
    }
    g_free(data);
}

/*
 * Background save. The VkPipelineCache is not created externally
 * synchronized, so vkGetPipelineCacheData may run on this thread while
 * the PFIFO thread keeps creating pipelines through the same cache (the
 * driver locks it; MoltenVK's cache is about 11 MB on the heavy scene,
 * tens of ms to serialize and write, which used to land on a flip).
 * One save at a time; finalize joins it before destroying the cache.
 * It keeps the PFIFO thread's QoS: a creation that needs the cache waits
 * for the serialization, so a demoted saver would stall it longer.
 */
static void *pipeline_cache_save_thread(void *opaque)
{
    PGRAPHVkState *r = opaque;

    save_pipeline_cache_to_disk(r);
    qatomic_store_release(&r->pipeline_cache_save_busy, false);
    return NULL;
}

static void pipeline_cache_save_join(PGRAPHVkState *r)
{
    if (r->pipeline_cache_save_thread_live) {
        qemu_thread_join(&r->pipeline_cache_save_thread);
        r->pipeline_cache_save_thread_live = false;
    }
}

static bool pipeline_cache_save_start(PGRAPHVkState *r)
{
    if (qatomic_load_acquire(&r->pipeline_cache_save_busy)) {
        return false;
    }
    pipeline_cache_save_join(r);
    qatomic_set(&r->pipeline_cache_save_busy, true);
    qemu_thread_create(&r->pipeline_cache_save_thread, "vk.pcache_save",
                       pipeline_cache_save_thread, r, QEMU_THREAD_JOINABLE);
    r->pipeline_cache_save_thread_live = true;
    return true;
}

/*
 * Called from the PFIFO thread at flip boundaries. A crash or kill loses
 * every pipeline compiled since the last save, so save, in the
 * background, once PIPELINE_CACHE_SAVE_BATCH new cache entries have
 * accumulated or a loadvm's compile burst has settled, but only after
 * PIPELINE_CACHE_SAVE_QUIET_NS without a miss (so the save doesn't make
 * the rest of a burst wait on the cache) and never twice within
 * PIPELINE_CACHE_SAVE_MIN_NS; anything left over goes after
 * PIPELINE_CACHE_SAVE_INTERVAL_NS. A warm run (every stage a cache hit)
 * never saves, so it can't clobber a sibling's fuller cache either.
 */
#define PIPELINE_CACHE_SAVE_INTERVAL_NS (30ll * 1000000000ll)
#define PIPELINE_CACHE_SAVE_MIN_NS (5ll * 1000000000ll)
#define PIPELINE_CACHE_SAVE_SETTLE_NS (3ll * 1000000000ll)
#define PIPELINE_CACHE_SAVE_QUIET_NS (1ll * 1000000000ll)
#define PIPELINE_CACHE_SAVE_BATCH 16

void pgraph_vk_maybe_save_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->pipeline_cache_unsaved == 0) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t since = now - r->pipeline_cache_last_save_ns;
    bool settled = r->pipeline_cache_loadvm_ns &&
                   now - r->pipeline_cache_loadvm_ns >=
                       PIPELINE_CACHE_SAVE_SETTLE_NS;
    bool quiet = now - r->pipeline_cache_last_miss_ns >=
                 PIPELINE_CACHE_SAVE_QUIET_NS;
    if (since < PIPELINE_CACHE_SAVE_INTERVAL_NS &&
        (since < PIPELINE_CACHE_SAVE_MIN_NS || !quiet ||
         (r->pipeline_cache_unsaved < PIPELINE_CACHE_SAVE_BATCH &&
          !settled))) {
        return;
    }
    if (!pipeline_cache_save_start(r)) {
        return;
    }
    r->pipeline_cache_unsaved = 0;
    r->pipeline_cache_loadvm_ns = 0;
    r->pipeline_cache_last_save_ns = now;
}

/* A loadvm (or reset) flush: save once the new scene's pipelines are in. */
void pgraph_vk_pipeline_cache_note_flush(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->pipeline_cache_loadvm_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

__attribute__((noinline)) void
pgraph_vk_pipeline_cache_feedback_chain(PGRAPHVkState *r, const void **pnext,
                                        uint32_t stage_count,
                                        PipelineCacheFeedback *fb)
{
    if (!r->pipeline_creation_feedback_enabled ||
        stage_count > ARRAY_SIZE(fb->stages)) {
        return;
    }
    memset(fb, 0, sizeof(*fb));
    fb->info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
    fb->info.pNext = *pnext;
    fb->info.pPipelineCreationFeedback = &fb->pipeline;
    fb->info.pipelineStageCreationFeedbackCount = stage_count;
    fb->info.pPipelineStageCreationFeedbacks = fb->stages;
    *pnext = &fb->info;
}

/*
 * The cache gained an entry unless the pipeline was a cache hit, or, on
 * MoltenVK, which caches shader stages and never reports pipeline hits,
 * every stage was. Other drivers may cache whole pipelines behind stage
 * hits, so there stage hits alone still count as new. No feedback counts
 * as new, which was the rule before feedback. Every creation through
 * vk_pipeline_cache (draw, clear, display, surface compute) reports
 * here, or its misses would never be saved on an otherwise warm run.
 * Both helpers stay out of line so begin_pre_draw, which inlines the
 * creation paths, keeps its PGO profile.
 */
__attribute__((noinline)) void
pgraph_vk_pipeline_cache_note_created(PGRAPHVkState *r, const void *pnext,
                                      uint32_t stage_count,
                                      const PipelineCacheFeedback *fb)
{
    const VkPipelineCreationFeedbackFlags hit =
        VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;

    if (pnext == &fb->info) {
        if ((fb->pipeline.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
            (fb->pipeline.flags & hit)) {
            return;
        }
        bool all_hit = r->pipeline_cache_stage_hits_suffice && stage_count > 0;
        for (uint32_t i = 0; all_hit && i < stage_count; i++) {
            if (!(fb->stages[i].flags & hit)) {
                all_hit = false;
                break;
            }
        }
        if (all_hit) {
            return;
        }
    }
    r->pipeline_cache_unsaved++;
    r->pipeline_cache_last_miss_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

/*
 * A kill between the temp write and the rename in
 * save_pipeline_cache_to_disk leaves a pipeline_cache*.bin.<pid>.tmp that
 * no later run reads or overwrites (the pid differs), so sweep them once
 * at init, before this process writes its own, whichever driver wrote
 * them; the glob also takes the fixed pipeline_cache.bin.tmp of older
 * builds. Only files untouched for a minute go: a sibling sharing the
 * folder writes its temp file in well under a second, and a younger
 * orphan is swept by a later launch.
 */
static void sweep_stale_pipeline_cache_temp_files(void)
{
    const char *base = xemu_settings_get_base_path();
    GDir *d = g_dir_open(base, 0, NULL);
    if (!d) {
        return;
    }

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!g_str_has_prefix(name, "pipeline_cache") ||
            !strstr(name, ".bin.") || !g_str_has_suffix(name, ".tmp")) {
            continue;
        }
        char *path = g_build_filename(base, name, NULL);
        GStatBuf st;
        if (g_stat(path, &st) == 0 && now - (gint64)st.st_mtime > 60) {
            g_unlink(path);
        }
        g_free(path);
    }
    g_dir_close(d);
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    sweep_stale_pipeline_cache_temp_files();

    void *cache_data = NULL;
    size_t cache_size = 0;
    load_pipeline_cache_from_disk(r, &cache_data, &cache_size);

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = cache_size,
        .pInitialData = cache_data,
        .pNext = NULL,
    };
    VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                   &r->vk_pipeline_cache));

    g_free(cache_data);

    r->pipeline_cache_unsaved = 0;
    r->pipeline_cache_last_save_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    r->pipeline_cache_loadvm_ns = 0;
    r->pipeline_cache_last_miss_ns = 0;
    r->pipeline_cache_save_thread_live = false;
    r->pipeline_cache_save_busy = false;

    const size_t pipeline_cache_size = 4096;
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    nv2a_vk_assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pipeline_cache_save_join(r);
    if (r->pipeline_cache_unsaved) {
        save_pipeline_cache_to_disk(r);
    }

    lru_flush(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static inline gpointer render_pass_state_to_key(const RenderPassState *s)
{
    uint64_t v = (uint64_t)s->color_format | ((uint64_t)s->zeta_format << 32);
    return GSIZE_TO_POINTER((gsize)v);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
    r->render_pass_lookup = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_hash_table_destroy(r->render_pass_lookup);
    r->render_pass_lookup = NULL;
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

/*
 * The dynamic states of every frame pipeline, draw and clear alike. A
 * pipeline that left one of them static would replace the command
 * buffer's value at bind (Mesa drivers apply that; MoltenVK keeps the two
 * apart), and r->dyn would skip the set that restores it. Line width is
 * dynamic only with wideLines: without it the width is always 1.0.
 */
static void init_frame_dynamic_states(PGRAPHVkState *r)
{
    static const struct {
        VkDynamicState state;
        uint32_t bit;
    } states[] = {
        { VK_DYNAMIC_STATE_VIEWPORT, DYN_VIEWPORT },
        { VK_DYNAMIC_STATE_SCISSOR, DYN_SCISSOR },
        { VK_DYNAMIC_STATE_BLEND_CONSTANTS, DYN_BLEND_CONSTANTS },
        { VK_DYNAMIC_STATE_DEPTH_BIAS, DYN_DEPTH_BIAS },
        { VK_DYNAMIC_STATE_LINE_WIDTH, DYN_LINE_WIDTH },
    };
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(states) > ARRAY_SIZE(r->frame_dynamic_states));

    r->num_frame_dynamic_states = 0;
    r->frame_dynamic_mask = 0;
    for (int i = 0; i < ARRAY_SIZE(states); i++) {
        if (states[i].bit == DYN_LINE_WIDTH &&
            !r->enabled_physical_device_features.wideLines) {
            continue;
        }
        r->frame_dynamic_states[r->num_frame_dynamic_states++] =
            states[i].state;
        r->frame_dynamic_mask |= states[i].bit;
    }
}

/*
 * RasterState keeps these maps' values in uint8_t, and create_pipeline
 * builds from the key: an entry past 255 (an extension enum) would build
 * wrong pipelines, so it aborts here instead.
 */
static void check_raster_state_maps(void)
{
#define CHECK_U8_MAP(map)                                \
    for (int i = 0; i < ARRAY_SIZE(map); i++) {          \
        assert((uint64_t)(map)[i] <= UINT8_MAX);         \
    }
    CHECK_U8_MAP(pgraph_cull_face_vk_map);
    CHECK_U8_MAP(pgraph_depth_func_vk_map);
    CHECK_U8_MAP(pgraph_stencil_func_vk_map);
    CHECK_U8_MAP(pgraph_stencil_op_vk_map);
    CHECK_U8_MAP(pgraph_blend_factor_vk_map);
    CHECK_U8_MAP(pgraph_blend_equation_vk_map);
#undef CHECK_U8_MAP
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    check_raster_state_maps();
    init_frame_dynamic_states(pg->vk_renderer_state);
    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(pg->vk_renderer_state);
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(pg->vk_renderer_state);
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    g_hash_table_insert(r->render_pass_lookup,
                        render_pass_state_to_key(state),
                        GSIZE_TO_POINTER((gsize)new_pass.render_pass));
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    gpointer key = render_pass_state_to_key(state);
    gpointer val;
    if (g_hash_table_lookup_extended(r->render_pass_lookup, key, NULL, &val)) {
        return (VkRenderPass)(gsize)val;
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    nv2a_vk_assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->flight[0].framebuffers)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    int slot = r->current_flight;
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->flight[slot].framebuffers[r->framebuffer_index++]));
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.clear_parameter = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    int64_t nsprof_t0 = nsprof_begin();
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

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
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = r->num_frame_dynamic_states,
        .pDynamicStates = r->frame_dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    PipelineCacheFeedback feedback;
    pgraph_vk_pipeline_cache_feedback_chain(r, &pipeline_info.pNext,
                                            pipeline_info.stageCount,
                                            &feedback);

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));
    pgraph_vk_pipeline_cache_note_created(r, pipeline_info.pNext,
                                          pipeline_info.stageCount, &feedback);
    nsprof_end(NSPROF_PIPELINE_GEN, nsprof_t0);

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_vk_assert(r->pipeline_binding);
    return r->render_pass_state_dirty;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /*
     * texture_bindings_changed is deliberately NOT included here.
     * PipelineKey (renderer.h) contains no texture identity —
     * render_pass_state, shader_state, the raster state derived from
     * the 5 raster / blend regs, and the vertex binding / attribute
     * descriptions. Rebinding a different VkImage / VkSampler only
     * affects descriptor set 1, which is rewritten independently by
     * pgraph_vk_update_descriptor_sets (keyed off
     * texture_bindings_changed). If a texture change were to
     * require a pipeline rebuild (e.g. shader key depending on
     * bound sampler format), shader_bindings_changed fires and
     * that branch catches it.
     *
     * A bound clear pipeline (create_clear_pipeline) is always dirty: the
     * checks below compare against the previous draw, not against the
     * bound key, so a draw after a clear that changed none of them would
     * otherwise run on the clear pipeline and its empty layout.
     */
    if (!r->pipeline_binding || r->pipeline_binding->key.clear ||
        r->shader_bindings_changed || check_render_pass_dirty(pg)) {
        return true;
    }

    /*
     * NV_PGRAPH_CONTROL_3 is intentionally omitted: its bits
     * (SHADEMODE, FOG_MODE, FOGENABLE, POINTPARAMSENABLE) already
     * route through ShaderState (glsl/shaders.c:49 tracks CONTROL_3
     * dirty and bumps shader_bindings_changed), and PROVOKING_VERTEX
     * is consumed CPU-side (draw.c:145,293,398). Dropping it from
     * the fast-dirty check avoids spurious pipeline rehash +
     * cache lookups on every fog/shade-mode toggle.
     *
     * The key holds only what pgraph_vk_get_raster_state derives from
     * these registers, so a write that leaves it unchanged costs a
     * keyed lookup that finds the bound pipeline, and no rebind.
     */
    const unsigned int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_CONTROL_0,   NV_PGRAPH_CONTROL_1,
        NV_PGRAPH_CONTROL_2,   NV_PGRAPH_SETUPRASTER,
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    if (r->vertex_state_dirty) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

/*
 * The draw pipeline's raster state from BLEND, CONTROL_0-2 and SETUPRASTER
 * (RasterState in renderer.h), which create_pipeline builds its create
 * info from: every field its enable turns off stays zero.
 */
static void pgraph_vk_get_raster_state(PGRAPHState *pg, RasterState *rs)
{
    memset(rs, 0, sizeof(*rs));

    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    rs->front_face = (setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                         VK_FRONT_FACE_COUNTER_CLOCKWISE :
                         VK_FRONT_FACE_CLOCKWISE;
    if (setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face =
            GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
        nv2a_vk_bounds_check(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rs->cull_mode = pgraph_cull_face_vk_map[cull_face];
    }
    uint32_t polygon_mode =
        GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    rs->depth_bias_enable =
        (polygon_mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL &&
         (setupraster & NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE)) ||
        (polygon_mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE &&
         (setupraster & NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE)) ||
        (polygon_mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT &&
         (setupraster & NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE));

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    if (control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) {
        rs->depth_test_enable = VK_TRUE;
        /* Without the test the spec disables depth writes anyway */
        rs->depth_write_enable =
            !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
        uint32_t depth_func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
        nv2a_vk_bounds_check(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
        rs->depth_compare_op = pgraph_depth_func_vk_map[depth_func];
    }

    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    if (control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) {
        uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
        uint32_t stencil_func =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t op_fail = GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
        nv2a_vk_bounds_check(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
        nv2a_vk_bounds_check(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        nv2a_vk_bounds_check(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        nv2a_vk_bounds_check(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

        rs->stencil_test_enable = VK_TRUE;
        rs->stencil_fail_op = pgraph_stencil_op_vk_map[op_fail];
        rs->stencil_pass_op = pgraph_stencil_op_vk_map[op_zpass];
        rs->stencil_depth_fail_op = pgraph_stencil_op_vk_map[op_zfail];
        rs->stencil_compare_op = pgraph_stencil_func_vk_map[stencil_func];
        rs->stencil_compare_mask =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        rs->stencil_write_mask =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        rs->stencil_reference =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
    }

    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        rs->color_write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        rs->color_write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        rs->color_write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        rs->color_write_mask |= VK_COLOR_COMPONENT_A_BIT;

    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    if (blend & NV_PGRAPH_BLEND_EN) {
        uint32_t sfactor = GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR);
        uint32_t equation = GET_MASK(blend, NV_PGRAPH_BLEND_EQN);
        nv2a_vk_bounds_check(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        nv2a_vk_bounds_check(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        nv2a_vk_bounds_check(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        rs->blend_enable = VK_TRUE;
        rs->blend_src_factor = pgraph_blend_factor_vk_map[sfactor];
        rs->blend_dst_factor = pgraph_blend_factor_vk_map[dfactor];
        rs->blend_op = pgraph_blend_equation_vk_map[equation];
    }
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, &key->render_pass_state);
    memcpy(&key->shader_state, &r->shader_binding->state, sizeof(ShaderState));
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

    // FIXME: Use more dynamic state updates
    pgraph_vk_get_raster_state(pg, &key->raster);
}

static void create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_bind_textures(d);
    pgraph_vk_bind_shaders(pg);

    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    r->vertex_state_dirty = false;
    r->render_pass_state_dirty = false;

    if (r->pipeline_binding && !pipeline_dirty) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = false;
        NV2A_VK_DGROUP_END();
        return;
    }

    PipelineKey key;
    init_pipeline_key(pg, &key);

    /*
     * Compute the pipeline cache hash as XOR of sub-hashes. ShaderState
     * is by far the largest field (~hundreds of bytes) and only changes
     * when shader bindings change; caching its hash avoids re-scanning
     * those bytes on every dirty-pipeline draw call where only blend /
     * depth / vertex-descriptor state changed. The LRU bucket uses the
     * hash plus a full memcmp for disambiguation, so XOR is safe.
     */
    if (r->cached_shader_state_hash == 0) {
        r->cached_shader_state_hash =
            fast_hash((const uint8_t *)&key.shader_state,
                      sizeof(key.shader_state));
        /* Guarantee non-zero sentinel; collision with 0 is astronomically
         * unlikely but still better to avoid re-hashing next frame. */
        if (r->cached_shader_state_hash == 0) {
            r->cached_shader_state_hash = 1;
        }
    }
    const size_t head_len =
        offsetof(PipelineKey, shader_state);
    const size_t tail_off =
        offsetof(PipelineKey, shader_state) + sizeof(key.shader_state);
    const size_t tail_len = sizeof(key) - tail_off;
    uint64_t hash = r->cached_shader_state_hash ^
                    fast_hash((const uint8_t *)&key, head_len) ^
                    fast_hash((const uint8_t *)&key + tail_off, tail_len);

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    int64_t nsprof_t0 = nsprof_begin();

    memcpy(&snode->key, &key, sizeof(key));

    /* Raster, depth-stencil and blend state come from the key only */
    const RasterState *rs = &key.raster;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vsh.module_info->module,
            .pName = "main",
        };
    if (r->shader_binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->psh.module_info->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    /*
     * NV2A supports an "index restart" marker in indexed draws (matching
     * Vulkan's 0xFFFF / 0xFFFFFFFF semantics). Enable primitive restart
     * for strip/fan topologies so titles that rely on the marker render
     * correctly. Vulkan only permits restart with list topologies when
     * VK_EXT_primitive_topology_list_restart is enabled; we don't, so
     * gate to strip/fan only.
     */
    VkPrimitiveTopology topology = get_primitive_topology(pg);
    bool restart_enable =
        topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
        topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
        topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topology,
        .primitiveRestartEnable = restart_enable ? VK_TRUE : VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable =
            r->enabled_physical_device_features.depthClamp ? VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = pgraph_polygon_mode_vk_map[r->shader_binding->state.geom
                                                      .polygon_front_mode],
        .lineWidth = 1.0f,
        .cullMode = rs->cull_mode,
        .frontFace = rs->front_face,
        .depthBiasEnable = rs->depth_bias_enable,
        .pNext = rasterizer_next_struct,
    };
    if (!r->enabled_physical_device_features.fillModeNonSolid &&
        rasterizer.polygonMode != VK_POLYGON_MODE_FILL) {
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkStencilOpState stencil = {
        .failOp = rs->stencil_fail_op,
        .passOp = rs->stencil_pass_op,
        .depthFailOp = rs->stencil_depth_fail_op,
        .compareOp = rs->stencil_compare_op,
        .compareMask = rs->stencil_compare_mask,
        .writeMask = rs->stencil_write_mask,
        .reference = rs->stencil_reference,
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test_enable,
        .depthWriteEnable = rs->depth_write_enable,
        .depthCompareOp = rs->depth_compare_op,
        .stencilTestEnable = rs->stencil_test_enable,
        .front = stencil,
        .back = stencil,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .blendEnable = rs->blend_enable,
        .srcColorBlendFactor = rs->blend_src_factor,
        .dstColorBlendFactor = rs->blend_dst_factor,
        .colorBlendOp = rs->blend_op,
        .srcAlphaBlendFactor = rs->blend_src_factor,
        .dstAlphaBlendFactor = rs->blend_dst_factor,
        .alphaBlendOp = rs->blend_op,
        .colorWriteMask = rs->color_write_mask,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = r->num_frame_dynamic_states,
        .pDynamicStates = r->frame_dynamic_states,
    };

    VkDescriptorSetLayout set_layouts[2] = {
        r->ubo_descriptor_set_layout,
        r->descriptor_set_layout,
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = set_layouts,
    };

    VkPushConstantRange push_constant_range;
    if (r->use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs);
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    PipelineCacheFeedback feedback;
    pgraph_vk_pipeline_cache_feedback_chain(r, &pipeline_create_info.pNext,
                                            pipeline_create_info.stageCount,
                                            &feedback);
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));
    pgraph_vk_pipeline_cache_note_created(r, pipeline_create_info.pNext,
                                          pipeline_create_info.stageCount,
                                          &feedback);
    nsprof_end(NSPROF_PIPELINE_GEN, nsprof_t0);

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

/*
 * Draw-merge attribution (XEMU_NV2A_NSPROF; GPU frame-cost campaign
 * Phase 1). Classifies each non-clear guest begin/end block against
 * the previous one to count how many consecutive blocks could fuse
 * into a single draw call: pipeline unchanged + same render pass +
 * no descriptor advance, split by whether the vertex binding is
 * bit-identical (already deduped) or same-buffers-new-offsets (the
 * cross-block merge candidate). Observation only — never changes
 * control flow; comparison work is gated on nsprof_enabled().
 */
static struct {
    bool prev_valid;
    void *prev_pipeline;    /* pipeline snode of previous block */
    uint64_t prev_pass_seq; /* render-pass generation at previous block */
    uint64_t pass_seq;      /* bumped at every begin_render_pass */
    bool desc_rebound;      /* a descriptor set advanced this block */
    bool push_changed;      /* push-constant payload changed this block */
    int vtx_rel;            /* 0 identical, 1 same-bufs-new-offsets, 2 other */
} nsprof_merge;

static void nsprof_merge_classify(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!nsprof_enabled() || pg->clearing) {
        return;
    }
    if (nsprof_merge.prev_valid &&
        r->pipeline_binding == nsprof_merge.prev_pipeline &&
        nsprof_merge.pass_seq == nsprof_merge.prev_pass_seq &&
        !nsprof_merge.desc_rebound) {
        if (nsprof_merge.vtx_rel == 0) {
            nsprof_event(NSPROF_EV_DRAW_MERGE_IDENTICAL);
        } else if (nsprof_merge.vtx_rel == 1) {
            nsprof_event(NSPROF_EV_DRAW_MERGE_CANDIDATE);
            if (nsprof_merge.push_changed) {
                nsprof_event(NSPROF_EV_DRAW_MERGE_CAND_UNIF_DIFF);
            }
        } else {
            nsprof_event(NSPROF_EV_DRAW_STATE_CHANGED);
        }
    } else {
        nsprof_event(NSPROF_EV_DRAW_STATE_CHANGED);
    }
    nsprof_merge.prev_valid = true;
    nsprof_merge.prev_pipeline = r->pipeline_binding;
    nsprof_merge.prev_pass_seq = nsprof_merge.pass_seq;
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->use_push_constants_for_uniform_attrs) {
        nsprof_merge.push_changed = false;
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        size_t bytes = (size_t)num_uniform_attrs * 4 * sizeof(float);
        VkPipelineLayout layout = r->pipeline_binding->layout;

        /*
         * Skip the push when the payload is bit-identical to the last
         * issued one on the same (CB, pipeline_layout) scope. Games
         * with fixed-function transforms often keep inline uniform
         * attrs constant across many consecutive draws. CB-scope is
         * handled by reset in pgraph_vk_begin_command_buffer; layout
         * scope is handled by including the layout handle in the
         * skip fingerprint (a pipeline rebind to a different layout
         * will not match and will force the push).
         */
        if (r->last_push_constants_valid &&
            r->last_push_constants_layout == layout &&
            r->last_push_constants_num_attrs == num_uniform_attrs &&
            !memcmp(r->last_push_constants_values, values, bytes)) {
            nsprof_merge.push_changed = false;
            return;
        }

        vkCmdPushConstants(r->command_buffer, layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           (uint32_t)bytes, &values);

        memcpy(r->last_push_constants_values, values, bytes);
        r->last_push_constants_layout = layout;
        r->last_push_constants_num_attrs = num_uniform_attrs;
        r->last_push_constants_valid = true;
        nsprof_merge.push_changed = true;
    } else {
        nsprof_merge.push_changed = false;
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_vk_assert(r->descriptor_set_index >= 1);
    nv2a_vk_assert(r->ubo_descriptor_set_index >= 1);

    /*
     * Two sets are bound independently: set 0 (UBOs) only when the UBO
     * index advances, set 1 (textures) only when the texture index
     * advances. Each "last-bound" tracker avoids redundant bind calls
     * when the corresponding set hasn't been rewritten.
     *
     * Vulkan pipeline-layout compatibility rules guarantee that the
     * last-bound sets remain valid across pipeline swaps as long as
     * the two pipelines' descriptor set layouts for each set index
     * match. All draw pipelines use the same (ubo_layout, tex_layout)
     * pair, so sets bound for pipeline A remain valid for pipeline B.
     * Clear pipelines use a zero-descriptor-set layout (nothing to
     * bind) and don't disturb the currently-bound sets for the next
     * draw pipeline.
     *
     * The skip was briefly reverted to an unconditional rebind while
     * diagnosing a pre-existing level-transition freeze; the
     * XEMU_PFIFO_HEARTBEAT diagnostic confirmed the freeze is not
     * pgraph-side, so the skip is restored.
     */
    bool need_bind_ubo =
        r->ubo_descriptor_set_index != r->last_bound_ubo_descriptor_set_index;
    bool need_bind_tex =
        r->descriptor_set_index != r->last_bound_descriptor_set_index;

    nsprof_merge.desc_rebound = need_bind_ubo || need_bind_tex;

    if (need_bind_ubo && need_bind_tex) {
        VkDescriptorSet sets[2] = {
            r->ubo_descriptor_sets[r->ubo_descriptor_set_index - 1],
            r->descriptor_sets[r->descriptor_set_index - 1],
        };
        vkCmdBindDescriptorSets(r->command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                r->pipeline_binding->layout, 0, 2, sets, 0,
                                NULL);
    } else if (need_bind_ubo) {
        vkCmdBindDescriptorSets(
            r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            r->pipeline_binding->layout, 0, 1,
            &r->ubo_descriptor_sets[r->ubo_descriptor_set_index - 1], 0, NULL);
    } else if (need_bind_tex) {
        vkCmdBindDescriptorSets(
            r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            r->pipeline_binding->layout, 1, 1,
            &r->descriptor_sets[r->descriptor_set_index - 1], 0, NULL);
    }

    r->last_bound_ubo_descriptor_set_index = r->ubo_descriptor_set_index;
    r->last_bound_descriptor_set_index = r->descriptor_set_index;
}

static void begin_query(PGRAPHVkState *r)
{
    nv2a_vk_assert(r->in_command_buffer);
    /* Queries begin inside the render pass so rotation never tears
     * it down; a query begun in a subpass must also end in it, which
     * end_render_pass() guarantees. The pool partition was reset in
     * bulk at command-buffer begin. */
    nv2a_vk_assert(r->in_render_pass);
    nv2a_vk_assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    nv2a_vk_assert(r->num_queries_in_flight < pgraph_vk_queries_per_slot(r));

    /* Query indices live in the current flight slot's pool partition. */
    int query_index = pgraph_vk_slot_query_base(r, r->current_flight) +
                      r->num_queries_in_flight;

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    VkQueryControlFlags query_flags = 0;
    if (r->enabled_physical_device_features.occlusionQueryPrecise) {
        query_flags |= VK_QUERY_CONTROL_PRECISE_BIT;
    }
    vkCmdBeginQuery(r->command_buffer, r->query_pool, query_index,
                    query_flags);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    nv2a_vk_assert(r->in_command_buffer);
    nv2a_vk_assert(r->in_render_pass);
    nv2a_vk_assert(r->query_in_flight);

    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  pgraph_vk_slot_query_base(r, r->current_flight) +
                      r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

static VkDeviceSize get_staging_slot_base(PGRAPHVkState *r, int index_src)
{
    int s = r->current_flight;
    switch (index_src) {
    case BUFFER_INDEX_STAGING:
        return r->flight[s].index_staging_base;
    case BUFFER_VERTEX_INLINE_STAGING:
        return r->flight[s].vertex_inline_staging_base;
    case BUFFER_UNIFORM_STAGING:
        return r->flight[s].uniform_staging_base;
    default:
        return 0;
    }
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    VkDeviceSize base = get_staging_slot_base(r, index_src);
    if (b_src->buffer_offset <= base) {
        return;
    }

    VkDeviceSize size = b_src->buffer_offset - base;
    VkBufferCopy copy_region = {
        .srcOffset = base,
        .dstOffset = base,
        .size = size,
    };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);

    VkAccessFlags dst_access_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        break;
    default:
        nv2a_vk_assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .offset = base,
        .size = size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *vram = &r->storage_buffers[BUFFER_VERTEX_RAM];

    /*
     * Tracked min/max dirty bits avoid scanning the full per-flight VRAM
     * page bitmap with find_first_bit/find_last_bit. ULONG_MAX sentinel
     * means no dirty pages; return early.
     */
    unsigned long first_dirty =
        r->flight[r->current_flight].uploaded_first_dirty_bit;
    if (first_dirty == ULONG_MAX) {
        return;
    }
    unsigned long last_dirty =
        r->flight[r->current_flight].uploaded_last_dirty_bit;

    VkDeviceSize offset = (VkDeviceSize)first_dirty * TARGET_PAGE_SIZE;
    VkDeviceSize end = (VkDeviceSize)(last_dirty + 1) * TARGET_PAGE_SIZE;
    if (end > vram->buffer_size) {
        end = vram->buffer_size;
    }
    VkDeviceSize size = end - offset;

    if (!vram->is_coherent) {
        VK_CHECK(vmaFlushAllocation(r->allocator, vram->allocation,
                                    offset, size));
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = vram->buffer,
        .offset = offset,
        .size = size,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

/*
 * Lazy-populate (width, height) = surface_binding_dim.{width,height} *
 * surface_scale_factor. Cached in PGRAPHVkState and invalidated at each
 * surface_binding_dim / scale-factor write. Shared by begin_render_pass
 * (render area) and begin_draw (viewport).
 */
static void get_scaled_binding_dim(PGRAPHState *pg,
                                   unsigned int *width,
                                   unsigned int *height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->cached_scaled_binding_dim_valid) {
        unsigned int w = pg->surface_binding_dim.width;
        unsigned int h = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &w, &h);
        r->cached_scaled_binding_dim_w = w;
        r->cached_scaled_binding_dim_h = h;
        r->cached_scaled_binding_dim_valid = true;
    }
    *width = r->cached_scaled_binding_dim_w;
    *height = r->cached_scaled_binding_dim_h;
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_vk_assert(r->in_command_buffer);
    nv2a_vk_assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width, vp_height;
    get_scaled_binding_dim(pg, &vp_width, &vp_height);

    nv2a_vk_assert(r->framebuffer_index > 0);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->flight[r->current_flight].framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    nsprof_event(NSPROF_EV_RENDERPASS);
    nsprof_merge.pass_seq++;
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;
}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        /* A query begun in this pass must end inside it. The next
         * zpass draw begins a fresh one; report sums span queries. */
        if (r->query_in_flight) {
            end_query(r);
        }
        vkCmdEndRenderPass(r->command_buffer);
        r->in_render_pass = false;
    }
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
    [VK_FINISH_REASON_REPORTS_FULL] = NV2A_PROF_FINISH_REPORTS_FULL,
    [VK_FINISH_REASON_REPORTS_SUBMIT] = NV2A_PROF_FINISH_REPORTS_SUBMIT,
    [VK_FINISH_REASON_QUERIES_FULL] = NV2A_PROF_FINISH_QUERIES_FULL,
};
/* pgraph_vk_finish offsets NSPROF_EV_FINISH_BASE by the reason. */
QEMU_BUILD_BUG_ON(NSPROF_EV_FINISH_QUERIES_FULL - NSPROF_EV_FINISH_BASE !=
                  VK_FINISH_REASON_QUERIES_FULL);

static void destroy_flight_framebuffers(PGRAPHState *pg, int slot)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->flight[slot].framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->flight[slot].framebuffers[i], NULL);
        r->flight[slot].framebuffers[i] = VK_NULL_HANDLE;
    }
    r->flight[slot].framebuffer_index = 0;
}

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_vk_assert(!r->in_draw);
    nv2a_vk_assert(r->debug_depth == 0);

    if (r->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);
        nsprof_event(NSPROF_EV_FINISH_BASE + finish_reason);

        if (r->in_render_pass) {
            end_render_pass(r); /* also ends any in-pass query */
        }
        VK_CHECK(vkEndCommandBuffer(r->command_buffer));

        bool aux_has_work =
            r->storage_buffers[BUFFER_INDEX_STAGING].buffer_offset >
                get_staging_slot_base(r, BUFFER_INDEX_STAGING) ||
            r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING].buffer_offset >
                get_staging_slot_base(r, BUFFER_VERTEX_INLINE_STAGING) ||
            r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset >
                get_staging_slot_base(r, BUFFER_UNIFORM_STAGING) ||
            r->flight[r->current_flight].uploaded_first_dirty_bit !=
                ULONG_MAX;

        int slot = r->current_flight;

        if (aux_has_work) {
            VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
            sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
            sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                                    BUFFER_VERTEX_INLINE);
            sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING,
                                    BUFFER_UNIFORM);
            flush_memory_buffer(pg, cmd);
            VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
            r->in_aux_command_buffer = false;

            VkPipelineStageFlags wait_stage =
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit_infos[] = {
                {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &r->aux_command_buffer,
                    .signalSemaphoreCount = 1,
                    .pSignalSemaphores = &r->command_buffer_semaphore,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &r->command_buffer,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &r->command_buffer_semaphore,
                    .pWaitDstStageMask = &wait_stage,
                }
            };
            nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
            vkResetFences(r->device, 1, &r->command_buffer_fence);
            VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos),
                                   submit_infos, r->command_buffer_fence));
        } else {
            if (r->in_aux_command_buffer) {
                VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
                r->in_aux_command_buffer = false;
            }

            VkSubmitInfo submit_info = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->command_buffer,
            };
            nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
            vkResetFences(r->device, 1, &r->command_buffer_fence);
            VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info,
                                   r->command_buffer_fence));
        }
        r->flight[slot].submitted = true;
        r->flight[slot].framebuffer_index = r->framebuffer_index;
        r->submit_count += 1;
        r->flight[slot].submit_index = r->submit_count;

        /*
         * Hand this submission's occlusion queries and pending guest
         * reports to the flight slot. They are drained when the slot
         * fence is reaped (pgraph_vk_wait_for_previous_flight) instead
         * of stalling here on work the GPU just started.
         */
        r->flight[slot].query_count = r->num_queries_in_flight;
        QSIMPLEQ_CONCAT(&r->flight[slot].report_queue, &r->report_queue);
        r->num_queries_in_flight = 0;
        r->report_pool_next = 0;

        bool check_budget = false;
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            check_budget = true;
        }

        /* Advance to the next flight slot */
        r->current_flight = (r->current_flight + 1) % NUM_FLIGHT_SLOTS;
        pgraph_vk_wait_for_previous_flight(pg);
        pgraph_vk_select_flight_slot(pg);

        int next = r->current_flight;
        r->descriptor_set_index = r->flight[next].descriptor_set_base;
        r->ubo_descriptor_set_index = r->flight[next].ubo_descriptor_set_base;
        r->last_bound_descriptor_set_index = -1;
        r->last_bound_ubo_descriptor_set_index = -1;
        r->in_command_buffer = false;
        destroy_flight_framebuffers(pg, next);
        r->framebuffer_index = 0;

        if (check_budget) {
            pgraph_vk_check_memory_budget(pg);
        }

    }

    /*
     * Most finish reasons leave report delivery deferred to slot
     * reclaim. Reasons where the guest (or a renderer transition)
     * needs the values now drain synchronously.
     */
    if (finish_reason == VK_FINISH_REASON_STALLED ||
        finish_reason == VK_FINISH_REASON_REPORTS_FULL ||
        finish_reason == VK_FINISH_REASON_QUERIES_FULL ||
        finish_reason == VK_FINISH_REASON_FLUSH) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_drain_all_pending_reports(d);
    }

    pgraph_vk_compute_finish_complete(r);

    int cur = r->current_flight;
    r->storage_buffers[BUFFER_STAGING_SRC].buffer_offset =
        r->flight[cur].staging_buffer_base;
    r->storage_buffers[BUFFER_STAGING_SRC].buffer_limit =
        r->flight[cur].staging_buffer_limit;
    r->storage_buffers[BUFFER_INDEX_STAGING].buffer_offset =
        r->flight[cur].index_staging_base;
    r->storage_buffers[BUFFER_INDEX_STAGING].buffer_limit =
        r->flight[cur].index_staging_limit;
    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING].buffer_offset =
        r->flight[cur].vertex_inline_staging_base;
    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING].buffer_limit =
        r->flight[cur].vertex_inline_staging_limit;
    r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset =
        r->flight[cur].uniform_staging_base;
    r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_limit =
        r->flight[cur].uniform_staging_limit;
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_vk_assert(!r->in_command_buffer);

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;

    /*
     * Reset this slot's whole occlusion-query partition in one
     * command, outside any render pass. The per-query
     * vkCmdResetQueryPool in begin_query() was the reason query
     * rotation had to tear down the render pass (resets are illegal
     * inside one) — measured at ~376 render passes per flip (about
     * one per draw) in zpass-heavy scenes, a full tile load/store
     * cycle per draw on Apple GPUs. With the partition pre-reset,
     * queries begin and end inside the pass (native Metal
     * visibility-buffer path under MoltenVK). Safe: the slot's
     * previous submission was fence-reaped before reuse, and other
     * slots own disjoint index ranges.
     */
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        pgraph_vk_slot_query_base(r, r->current_flight),
                        pgraph_vk_queries_per_slot(r));

    /*
     * Prime MoltenVK's per-command-buffer visibility flag with an
     * empty occlusion query, begun and ended outside any render
     * pass (legal, counts zero samples, folded harmlessly into
     * report sums). MoltenVK attaches the Metal visibility result
     * buffer to a render encoder only when
     * MVKCommandBuffer::_needsVisibilityResultMTLBuffer is already
     * set at pass-begin, and only vkCmdBeginQuery sets it — under
     * immediate prefill encoding
     * (MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2) our in-pass
     * query begins come after the first pass's encoder exists, so
     * a frame whose FIRST pass contains the first zpass draw got
     * an encoder with a nil visibility buffer and crashed in AGX
     * setVisibilityResultMode (user-reported, real gameplay).
     * MoltenVK only: other drivers have no such flag.
     */
    if (r->driver_is_moltenvk) {
        int primer_index =
            pgraph_vk_slot_query_base(r, r->current_flight) +
            r->num_queries_in_flight;
        nv2a_vk_assert(r->num_queries_in_flight <
                       pgraph_vk_queries_per_slot(r));
        vkCmdBeginQuery(r->command_buffer, r->query_pool, primer_index, 0);
        vkCmdEndQuery(r->command_buffer, r->query_pool, primer_index);
        r->num_queries_in_flight++;
    }

    /*
     * Dynamic state, vertex and index buffer binds and push constants
     * are command-buffer scoped: a new command buffer holds none of them.
     * This is the only place r->dyn resets, so the first draw sets every
     * frame dynamic state (begin_draw).
     */
    r->dyn.valid = 0;
    r->last_vertex_bind_valid = false;
    r->last_index_bind_valid = false;
    r->last_push_constants_valid = false;
    nsprof_merge.prev_valid = false;
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass(r); /* also ends any in-pass query */
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    nv2a_vk_assert(cmd == pg->vk_renderer_state->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static void begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_vk_assert(r->color_binding || r->zeta_binding);
    nv2a_vk_assert(!r->color_binding || r->color_binding->initialized);
    nv2a_vk_assert(!r->zeta_binding || r->zeta_binding->initialized);

    /*
     * Query-partition capacity guard. Deferred report delivery means a
     * command buffer can span a whole frame, and in-pass query rotation
     * allocates an index per rotation, so a heavy zpass scene can exhaust
     * the slot's partition mid-recording (overrunning it crashed inside
     * MoltenVK's visibility-buffer encode). Submit and start a fresh
     * recording before begin_draw allocates the next index.
     *
     * This must run before anything below builds per-draw state: the
     * finish destroys the slot's framebuffers, moves to the next flight
     * slot's descriptor sets and resets the staging offsets, and the code
     * below then creates all of that for the new slot. (It used to run in
     * begin_draw, after this function, which left framebuffer_index at 0
     * and the descriptor index at the new slot's base: begin_render_pass
     * then read framebuffers[-1], the slot's VkSemaphore, and bound
     * descriptor_sets[base - 1].) in_draw is false and no debug marker is
     * open yet, so finishing is legal. The -1 keeps one index for the
     * primer query the next command buffer begins with.
     */
    if (!pg->clearing && pg->zpass_pixel_count_enable &&
        r->in_command_buffer &&
        r->num_queries_in_flight >= pgraph_vk_queries_per_slot(r) - 1) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_QUERIES_FULL);
    }

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        create_pipeline(pg);
    }

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        if (r->in_render_pass) {
            nsprof_event(NSPROF_EV_RENDERPASS_CAUSE_SURFACE);
        }
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (!pg->clearing) {
        pgraph_vk_update_descriptor_sets(pg);
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

/*
 * The only setters of r->command_buffer's dynamic state. Each skips a
 * value the command buffer already holds (r->dyn), so a value set any
 * other way would leave the shadow stale.
 */
static void dyn_set_viewport(PGRAPHVkState *r, const VkViewport *viewport)
{
    if ((r->dyn.valid & DYN_VIEWPORT) &&
        !memcmp(viewport, &r->dyn.viewport, sizeof(*viewport))) {
        return;
    }
    vkCmdSetViewport(r->command_buffer, 0, 1, viewport);
    r->dyn.viewport = *viewport;
    r->dyn.valid |= DYN_VIEWPORT;
}

static void dyn_set_scissor(PGRAPHVkState *r, const VkRect2D *scissor)
{
    if ((r->dyn.valid & DYN_SCISSOR) &&
        !memcmp(scissor, &r->dyn.scissor, sizeof(*scissor))) {
        return;
    }
    vkCmdSetScissor(r->command_buffer, 0, 1, scissor);
    r->dyn.scissor = *scissor;
    r->dyn.valid |= DYN_SCISSOR;
}

static void dyn_set_blend_constants(PGRAPHVkState *r, const float constants[4])
{
    if ((r->dyn.valid & DYN_BLEND_CONSTANTS) &&
        !memcmp(constants, r->dyn.blend_constants,
                sizeof(r->dyn.blend_constants))) {
        return;
    }
    vkCmdSetBlendConstants(r->command_buffer, constants);
    memcpy(r->dyn.blend_constants, constants, sizeof(r->dyn.blend_constants));
    r->dyn.valid |= DYN_BLEND_CONSTANTS;
}

static void dyn_set_depth_bias(PGRAPHVkState *r, float constant, float slope)
{
    if ((r->dyn.valid & DYN_DEPTH_BIAS) &&
        !memcmp(&constant, &r->dyn.depth_bias_constant, sizeof(constant)) &&
        !memcmp(&slope, &r->dyn.depth_bias_slope, sizeof(slope))) {
        return;
    }
    vkCmdSetDepthBias(r->command_buffer, constant, 0.0f, slope);
    r->dyn.depth_bias_constant = constant;
    r->dyn.depth_bias_slope = slope;
    r->dyn.valid |= DYN_DEPTH_BIAS;
}

static void dyn_set_line_width(PGRAPHVkState *r, float width)
{
    if ((r->dyn.valid & DYN_LINE_WIDTH) &&
        !memcmp(&width, &r->dyn.line_width, sizeof(width))) {
        return;
    }
    vkCmdSetLineWidth(r->command_buffer, width);
    r->dyn.line_width = width;
    r->dyn.valid |= DYN_LINE_WIDTH;
}

/*
 * A guest draw's depth bias factors and blend constants, from the
 * registers. The bias goes in whether or not the pipeline enables it:
 * a disabled bias ignores the factors.
 */
static void dyn_set_guest_draw_state(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t zbias_reg = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
    uint32_t zfactor_reg = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
    float depth_bias_constant, depth_bias_slope;
    memcpy(&depth_bias_constant, &zbias_reg, sizeof(depth_bias_constant));
    memcpy(&depth_bias_slope, &zfactor_reg, sizeof(depth_bias_slope));
    dyn_set_depth_bias(r, depth_bias_constant, depth_bias_slope);

    float blend_constants[4] = { 0, 0, 0, 0 };
    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constants);
    }
    dyn_set_blend_constants(r, blend_constants);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_vk_assert(r->in_command_buffer);

    /*
     * The command buffer that records this draw is fixed from here on
     * (finishes for buffer space or queries came before): note what the
     * draw reads of the vertex RAM mirror in its slot.
     */
    pgraph_vk_note_vertex_ram_reads(pg);
    r->num_vertex_ram_buffer_syncs = 0;

    /*
     * Visibility testing: rotate / stop occlusion queries inside the
     * render pass. Ending or beginning a query no longer ends the
     * pass (see the bulk vkCmdResetQueryPool at command-buffer
     * begin); the pass now only ends for clears, render-target
     * changes, non-draw commands and submits.
     */
    if (r->query_in_flight &&
        (pg->clearing || !pg->zpass_pixel_count_enable ||
         r->new_query_needed)) {
        end_query(r);
    }

    if (pg->clearing) {
        if (r->in_render_pass) {
            nsprof_event(NSPROF_EV_RENDERPASS_CAUSE_CLEAR);
        }
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }

    if (!pg->clearing && pg->zpass_pixel_count_enable &&
        !r->query_in_flight) {
        begin_query(r);
    }

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        nsprof_event(NSPROF_EV_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width, vp_height;
        get_scaled_binding_dim(pg, &vp_width, &vp_height);

        VkViewport viewport = {
            .width = vp_width,
            .height = vp_height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        dyn_set_viewport(r, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        dyn_set_scissor(r, &scissor);

        if (r->frame_dynamic_mask & DYN_LINE_WIDTH) {
            dyn_set_line_width(r, clamp_line_width_to_device_limits(
                                      pg, pg->surface_scale_factor));
        }
    }

    if (!pg->clearing) {
        dyn_set_guest_draw_state(pg);
        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
    } else if (r->dyn.valid != r->frame_dynamic_mask) {
        /*
         * A clear opened this command buffer. Set the rest now: a partial
         * clear draws, and older validation layers want every dynamic
         * state the pipeline declares set before a draw, whatever the
         * enables.
         */
        dyn_set_guest_draw_state(pg);
    }
    nv2a_vk_assert(r->dyn.valid == r->frame_dynamic_mask);

    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_vk_assert(r->in_command_buffer);
    nv2a_vk_assert(r->in_render_pass);

    nsprof_merge_classify(pg);

    if (pg->clearing) {
        if (r->in_render_pass) {
            nsprof_event(NSPROF_EV_RENDERPASS_CAUSE_CLEAR);
        }
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nsprof_event(NSPROF_EV_DRAW);

    if (r->nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

    pgraph_vk_flush_draw(d);

    pg->draw_time++;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool color_write =
        (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    if (r->color_binding && color_write) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && (depth_test || stencil_test)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static void insertion_sort_syncs(MemorySyncRequirement *arr, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        MemorySyncRequirement key = arr[i];
        size_t j = i;
        while (j > 0 && arr[j - 1].addr > key.addr) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %llu bytes"
                          " -> %08" HWADDR_PRIx " %llu bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        (unsigned long long)r->vertex_ram_buffer_syncs[i].size,
                        start_addr,
                        (unsigned long long)(end_addr - start_addr));

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    insertion_sort_syncs(r->vertex_ram_buffer_syncs,
                         r->num_vertex_ram_buffer_syncs);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %llu bytes", i, addr, (unsigned long long)size);

        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            pgraph_vk_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                               size);
        }
    }

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            /* Through r->dyn, so the next draw restores both */
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            dyn_set_scissor(r, &clear_rect.rect);
            dyn_set_blend_constants(r, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        nsprof_merge.vtx_rel = 2;
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    uint32_t count = r->num_active_vertex_binding_descriptions;
    for (uint32_t i = 0; i < count; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
        if (r->vtx_span_refute && buffer_idx == BUFFER_VERTEX_RAM) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            pgraph_vk_refute_note_read(r, offsets[i],
                                       r->vertex_binding_descriptions[i].stride,
                                       attr->size * attr->count);
        }
    }
    r->refute_draw_vertices = 0;

    if (nsprof_enabled()) {
        /* Merge attribution only; duplicates the skip compare below so
         * the default path stays untouched. */
        if (r->last_vertex_bind_valid && r->last_vertex_bind_count == count &&
            !memcmp(r->last_vertex_bind_buffers, buffers,
                    count * sizeof(buffers[0]))) {
            nsprof_merge.vtx_rel =
                memcmp(r->last_vertex_bind_offsets, offsets,
                       count * sizeof(offsets[0])) ? 1 : 0;
        } else {
            nsprof_merge.vtx_rel = 2;
        }
    }

    /*
     * Skip the Vulkan call when the would-be binding is bit-identical
     * to the last one issued on this command buffer. Common pattern:
     * back-to-back flushes drawing the same mesh with different
     * uniforms / materials re-enter this path with unchanged buffers
     * and offsets. Cache is reset in pgraph_vk_begin_command_buffer.
     */
    if (r->last_vertex_bind_valid &&
        r->last_vertex_bind_count == count &&
        !memcmp(r->last_vertex_bind_buffers, buffers,
                count * sizeof(buffers[0])) &&
        !memcmp(r->last_vertex_bind_offsets, offsets,
                count * sizeof(offsets[0]))) {
        return;
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0, count, buffers, offsets);

    memcpy(r->last_vertex_bind_buffers, buffers, count * sizeof(buffers[0]));
    memcpy(r->last_vertex_bind_offsets, offsets, count * sizeof(offsets[0]));
    r->last_vertex_bind_count = count;
    r->last_vertex_bind_valid = true;
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

/*
 * Skip vkCmdBindIndexBuffer when the would-be bind is identical to the
 * last one issued on this command buffer. Mirrors bind_vertex_buffer's
 * dedup. Cache is reset in pgraph_vk_begin_command_buffer.
 */
static void bind_index_buffer(PGRAPHState *pg, VkBuffer buffer,
                              VkDeviceSize offset, VkIndexType type)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->last_index_bind_valid &&
        r->last_index_bind_buffer == buffer &&
        r->last_index_bind_offset == offset &&
        r->last_index_bind_type == type) {
        return;
    }

    vkCmdBindIndexBuffer(r->command_buffer, buffer, offset, type);

    r->last_index_bind_buffer = buffer;
    r->last_index_bind_offset = offset;
    r->last_index_bind_type = type;
    r->last_index_bind_valid = true;
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1, 1)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    nv2a_vk_assert(fmt < ARRAY_SIZE(table));
    nv2a_vk_assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    if (!remap.attributes) {
        return;
    }

    nv2a_vk_assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 1, 16));

    // FIXME: Caching
    /*
     * start_vertex is retained for API symmetry with the GL renderer, but
     * the VRAM base in r->vertex_attribute_offsets[attr_id] has already
     * been shifted by min_element * stride in
     * pgraph_vk_bind_vertex_attributes, so reads here start at vertex
     * min_element and copy num_vertices of them. Callers must pass
     * start_vertex == 0 to avoid double-shifting.
     */
    nv2a_vk_assert(start_vertex == 0);
    nv2a_vk_assert(buffer->mapped);

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id];
        VkDeviceSize new_stride = remap.map[attr_id].new_stride;
        VkDeviceSize old_stride = remap.map[attr_id].old_stride;

        if (new_stride == old_stride) {
            memcpy(out_ptr, in_ptr, new_stride * num_vertices);
        } else {
            for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
                memcpy(out_ptr, in_ptr, new_stride);
                out_ptr += new_stride;
                in_ptr += old_stride;
            }
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return;
    }

    r->num_vertex_ram_buffer_syncs = 0;

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        nv2a_vk_assert(pg->inline_elements_length == 0);
        nv2a_vk_assert(pg->inline_buffer_length == 0);
        nv2a_vk_assert(pg->inline_array_length == 0);

        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        /*
         * Narrow the remapped-attribute staging allocation + copy to
         * [min_start..max_element). bind_vertex_attributes has already
         * shifted r->vertex_attribute_offsets[] by min_start*stride, so
         * draws rebase via firstVertex = start - min_start
         * (or vertexOffset = -min_start for the emulated-primitives
         * indexed path).
         */
        uint32_t min_start = pg->draw_arrays_min_start;
        uint32_t range_vertices = max_element - min_start;
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, range_vertices);

        // TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
        bool emulate_primitives = draw_needs_primitive_emulation(pg);
        size_t total_emulated_index_count = 0;
        if (emulate_primitives) {
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                total_emulated_index_count += get_max_emulated_index_count(pg, pg->draw_arrays_count[i]);
            }
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                   total_emulated_index_count * sizeof(uint32_t));
        }

        begin_pre_draw(pg);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, range_vertices);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        r->refute_draw_vertices = range_vertices;
        bind_vertex_buffer(pg, remap.attributes, 0);
        if (emulate_primitives) {
            uint32_t *all_indices = get_emulated_indices_buf(r, total_emulated_index_count);
            size_t all_offset = 0;

            for (int i = 0; i < pg->draw_arrays_length; i++) {
                uint32_t start = pg->draw_arrays_start[i];
                uint32_t count = pg->draw_arrays_count[i];
                size_t max_index_count =
                    get_max_emulated_index_count(pg, count);
                if (max_index_count == 0) {
                    continue;
                }
                size_t index_count = build_emulated_indices_from_array(
                    pg, start, count, all_indices + all_offset);
                all_offset += index_count;
            }

            if (all_offset > 0) {
                VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                    pg, all_indices, all_offset * sizeof(uint32_t));
                bind_index_buffer(pg,
                                  r->storage_buffers[BUFFER_INDEX].buffer,
                                  buffer_offset, VK_INDEX_TYPE_UINT32);
                nsprof_event(NSPROF_EV_VK_DRAW_CALL);
                count_fan_draw(pg);
                vkCmdDrawIndexed(r->command_buffer, all_offset, 1, 0,
                                 -(int32_t)min_start, 0);
            }
        } else {
            if (pg->draw_arrays_length > 1) {
                nsprof_event(NSPROF_EV_DRAW_ARRAYS_MULTI_SUBRANGE);
            }
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                uint32_t start = pg->draw_arrays_start[i],
                         count = pg->draw_arrays_count[i];
                NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
                nsprof_event(NSPROF_EV_VK_DRAW_CALL);
                count_fan_draw(pg);
                vkCmdDraw(r->command_buffer, count, 1, start - min_start, 0);
            }
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        nv2a_vk_assert(pg->inline_buffer_length == 0);
        nv2a_vk_assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        bool emulate_primitives = draw_needs_primitive_emulation(pg);
        size_t index_count = pg->inline_elements_length;
        if (emulate_primitives) {
            index_count =
                get_max_emulated_index_count(pg, pg->inline_elements_length);
        }
        size_t index_data_size = index_count * sizeof(pg->inline_elements[0]);

        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
        sync_vertex_ram_buffer(pg);
        /*
         * Narrow remapped-attribute staging to [min_element..max_element].
         * bind_vertex_attributes shifted r->vertex_attribute_offsets[] by
         * min_element*stride above, so vkCmdDrawIndexed rebases the
         * absolute indices via vertexOffset = -min_element.
         */
        uint32_t range_vertices = max_element - min_element + 1;
        VertexBufferRemap remap = remap_unaligned_attributes(pg, range_vertices);

        begin_pre_draw(pg);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, range_vertices);
        const uint32_t *indices = pg->inline_elements;
        uint32_t *emulated_indices = NULL;
        if (emulate_primitives) {
            emulated_indices =
                get_emulated_indices_buf(r, index_count);
            index_count = build_emulated_indices_from_elements(
                pg, pg->inline_elements, pg->inline_elements_length,
                emulated_indices);
            indices = emulated_indices;
        }
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE, "Inline Elements");
        begin_draw(pg);
        r->refute_draw_vertices = range_vertices;
        bind_vertex_buffer(pg, remap.attributes, 0);
        if (index_count > 0) {
            VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                pg, (void *)indices, index_count * sizeof(indices[0]));
            bind_index_buffer(pg,
                              r->storage_buffers[BUFFER_INDEX].buffer,
                              buffer_offset, VK_INDEX_TYPE_UINT32);
            nsprof_event(NSPROF_EV_VK_DRAW_CALL);
            count_fan_draw(pg);
            vkCmdDrawIndexed(r->command_buffer, index_count, 1, 0,
                             -(int32_t)min_element, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        nv2a_vk_assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        VkDeviceSize sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        bool emulate_primitives = draw_needs_primitive_emulation(pg);
        size_t emulated_index_count = 0;
        if (emulate_primitives) {
            emulated_index_count =
                get_max_emulated_index_count(pg, pg->inline_buffer_length);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                emulated_index_count * sizeof(uint32_t));
        }
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);

        begin_pre_draw(pg);
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        if (emulate_primitives) {
            uint32_t *indices =
                get_emulated_indices_buf(r, emulated_index_count);
            size_t index_count = build_emulated_indices_from_array(
                pg, 0, pg->inline_buffer_length, indices);
            if (index_count > 0) {
                VkDeviceSize index_offset = pgraph_vk_update_index_buffer(
                    pg, indices, index_count * sizeof(indices[0]));
                bind_index_buffer(pg,
                                  r->storage_buffers[BUFFER_INDEX].buffer,
                                  index_offset, VK_INDEX_TYPE_UINT32);
                nsprof_event(NSPROF_EV_VK_DRAW_CALL);
                count_fan_draw(pg);
                vkCmdDrawIndexed(r->command_buffer, index_count, 1, 0, 0, 0);
            }
        } else {
            nsprof_event(NSPROF_EV_VK_DRAW_CALL);
            count_fan_draw(pg);
            vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);
        bool emulate_primitives = draw_needs_primitive_emulation(pg);
        size_t emulated_index_count = 0;
        if (emulate_primitives) {
            emulated_index_count =
                get_max_emulated_index_count(pg, index_count);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                emulated_index_count * sizeof(uint32_t));
        }

        begin_pre_draw(pg);
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        if (emulate_primitives) {
            uint32_t *indices =
                get_emulated_indices_buf(r, emulated_index_count);
            size_t actual_index_count =
                build_emulated_indices_from_array(pg, 0, index_count, indices);
            if (actual_index_count > 0) {
                VkDeviceSize index_offset = pgraph_vk_update_index_buffer(
                    pg, indices, actual_index_count * sizeof(indices[0]));
                bind_index_buffer(pg,
                                  r->storage_buffers[BUFFER_INDEX].buffer,
                                  index_offset, VK_INDEX_TYPE_UINT32);
                nsprof_event(NSPROF_EV_VK_DRAW_CALL);
                count_fan_draw(pg);
                vkCmdDrawIndexed(r->command_buffer, actual_index_count, 1, 0, 0,
                                 0);
            }
        } else {
            nsprof_event(NSPROF_EV_VK_DRAW_CALL);
            count_fan_draw(pg);
            vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
    }
}
