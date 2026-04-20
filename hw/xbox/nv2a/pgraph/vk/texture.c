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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"

/*
 * Incremental texture content hashing.
 *
 * TEXTURE_CHUNK_SIZE is the granularity at which fast_hash results are
 * cached per TextureBinding. When the VRAM dirty bitmap fires, only
 * chunks whose backing pages are actually dirty get re-hashed; stale
 * chunks reuse the cached hash. Combined via XOR into the texture's
 * aggregate content hash.
 *
 * 64 KiB keeps per-texture metadata small (a 4 MiB texture = 64 entries =
 * 512 B) while still making the common "one small update to a large
 * atlas" pattern cheap.
 *
 * TEXTURE_INCREMENTAL_HASH_MIN is the texture size below which we stay
 * on the single-shot full-buffer hash; for small textures the setup
 * cost of the chunked path exceeds the savings.
 */
#define TEXTURE_CHUNK_SIZE          (64 * 1024)
#define TEXTURE_INCREMENTAL_HASH_MIN (256 * 1024)

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);
static void sampler_cache_release_node_resources(PGRAPHVkState *r, SamplerCacheEntry *snode);

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    nv2a_vk_bounds_check(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
    bool gpu_unswizzle;
    unsigned int unswizzle_width, unswizzle_height;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        nv2a_vk_assert(false);
        __builtin_unreachable();
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

typedef struct DecodeTaskBatch {
    int remaining;
    QemuEvent done;
} DecodeTaskBatch;

typedef struct DecodeTask {
    void *src_data;
    void *palette_data;
    TextureShape shape;
    BasicColorFormatInfo fmt_info;
    unsigned int width, height, depth;
    bool is_compressed;
    bool is_3d;
    void *decoded_data;
    size_t decoded_size;
    DecodeTaskBatch *batch;
} DecodeTask;

static void decode_task_func(gpointer data, gpointer user_data)
{
    (void)user_data;
    DecodeTask *task = (DecodeTask *)data;

    if (task->is_compressed) {
        if (task->is_3d) {
            task->decoded_data = s3tc_decompress_3d(
                kelvin_format_to_s3tc_format(task->shape.color_format),
                task->src_data, task->width, task->height, task->depth);
        } else {
            task->decoded_data = s3tc_decompress_2d(
                kelvin_format_to_s3tc_format(task->shape.color_format),
                task->src_data, task->width, task->height);
        }
        task->decoded_size = task->width * task->height *
                             (task->is_3d ? task->depth : 1) * 4;
    } else {
        unsigned int pitch = task->width * task->fmt_info.bytes_per_pixel;
        if (task->is_3d) {
            unsigned int slice_pitch = pitch * task->height;
            size_t sz = slice_pitch * task->depth;
            uint8_t *unswizzled = g_malloc(sz);
            unswizzle_box(task->src_data, task->width, task->height,
                          task->depth, unswizzled, pitch, slice_pitch,
                          task->fmt_info.bytes_per_pixel);
            size_t conv_size;
            uint8_t *converted = pgraph_convert_texture_data(
                task->shape, unswizzled, task->palette_data,
                task->width, task->height, task->depth,
                pitch, slice_pitch, &conv_size);
            if (converted) {
                g_free(unswizzled);
                task->decoded_data = converted;
                task->decoded_size = conv_size;
            } else {
                task->decoded_data = unswizzled;
                task->decoded_size = sz;
            }
        } else {
            size_t sz = task->height * pitch;
            uint8_t *unswizzled = g_malloc(sz);
            unswizzle_rect(task->src_data, task->width, task->height,
                           unswizzled, pitch, task->fmt_info.bytes_per_pixel);
            size_t conv_size = sz;
            uint8_t *converted = pgraph_convert_texture_data(
                task->shape, unswizzled, task->palette_data,
                task->width, task->height, 1, pitch, 0, &conv_size);
            if (converted) {
                g_free(unswizzled);
                task->decoded_data = converted;
                task->decoded_size = conv_size;
            } else {
                task->decoded_data = unswizzled;
                task->decoded_size = sz;
            }
        }
    }

    if (qatomic_dec_fetch(&task->batch->remaining) == 0) {
        qemu_event_set(&task->batch->done);
    }
}

static void decode_batch_init(DecodeTaskBatch *batch, int count)
{
    qatomic_set(&batch->remaining, count);
    qemu_event_init(&batch->done, false);
}

static void decode_batch_wait(DecodeTaskBatch *batch)
{
    qemu_event_wait(&batch->done);
    qemu_event_destroy(&batch->done);
}

static bool texture_format_needs_data_conversion(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
        return true;
    default:
        return false;
    }
}

static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        nv2a_vk_assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        nv2a_vk_assert(s.dimensionality == 2);
        nv2a_vk_assert(!f.linear);
    }
    nv2a_vk_assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    void *texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        nv2a_vk_assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            nv2a_vk_assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        nv2a_vk_assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;

        /*
         * GPU unswizzle now supports any power-of-two dimensions via
         * CPU-computed swizzle masks (Phase 3.2). Xbox swizzled textures
         * are always POT by hardware constraint, so this condition is
         * generally true; checked here defensively.
         */
        bool use_gpu_unswizzle = !is_compressed &&
            (f.bytes_per_pixel == 2 || f.bytes_per_pixel == 4) &&
            !texture_format_needs_data_conversion(s.color_format) &&
            (adjusted_width  & (adjusted_width  - 1)) == 0 &&
            (adjusted_height & (adjusted_height - 1)) == 0;

        if (use_gpu_unswizzle) {
            for (int layer = 0; layer < num_layers; layer++) {
                unsigned int width = adjusted_width, height = adjusted_height;
                hwaddr layer_addr = texture_vram_offset + layer * layer_size;
                void *layer_ptr = (char *)d->vram_ptr + layer_addr;

                for (int level = 0; level < s.levels; level++) {
                    width = MAX(width, 1);
                    height = MAX(height, 1);

                    size_t sz = width * height * f.bytes_per_pixel;

                    void *raw_copy = g_malloc(sz);
                    memcpy(raw_copy, layer_ptr, sz);

                    unsigned int tex_w = width, tex_h = height;
                    if (s.cubemap && adjusted_width != s.width) {
                        tex_w = s.width;
                        tex_h = s.height;
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_w,
                        .height = tex_h,
                        .depth = 1,
                        .vram_addr = layer_addr,
                        .decoded_size = sz,
                        .decoded_data = raw_copy,
                        .gpu_unswizzle = true,
                        .unswizzle_width = width,
                        .unswizzle_height = height,
                    };

                    layer_ptr = (char *)layer_ptr + sz;
                    layer_addr += sz;
                    width /= 2;
                    height /= 2;
                }
            }
        } else {
        int total_tasks = num_layers * s.levels;

        DecodeTask *tasks = g_malloc0_n(total_tasks, sizeof(DecodeTask));
        DecodeTaskBatch batch;
        decode_batch_init(&batch, total_tasks);
        int task_idx = 0;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            void *layer_ptr = (char *)d->vram_ptr + texture_vram_offset +
                              layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                width = MAX(width, 1);
                height = MAX(height, 1);

                DecodeTask *t = &tasks[task_idx++];
                t->src_data = layer_ptr;
                t->palette_data = palette_data_ptr;
                t->shape = s;
                t->fmt_info = f;
                t->width = width;
                t->height = height;
                t->depth = 1;
                t->is_compressed = is_compressed;
                t->is_3d = false;
                t->batch = &batch;

                if (width <= 16 && height <= 16) {
                    decode_task_func(t, NULL);
                } else {
                    GError *err = NULL;
                    g_thread_pool_push(r->decode_thread_pool, t, &err);
                    if (err) {
                        fprintf(stderr, "nv2a: decode thread pool push failed: %s\n",
                                err->message);
                        g_error_free(err);
                        decode_task_func(t, NULL);
                    }
                }

                if (is_compressed) {
                    unsigned int pw = (width + 3) & ~3;
                    unsigned int ph = (height + 3) & ~3;
                    layer_ptr = (char *)layer_ptr + pw / 4 * ph / 4 * block_size;
                } else {
                    layer_ptr = (char *)layer_ptr +
                                width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }

        decode_batch_wait(&batch);

        task_idx = 0;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            for (int level = 0; level < s.levels; level++) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                DecodeTask *t = &tasks[task_idx++];
                unsigned int tex_w = width, tex_h = height;
                if (s.cubemap && adjusted_width != s.width) {
                    tex_w = s.width;
                    tex_h = s.height;
                }
                layout->layers[layer].levels[level] = (TextureLevel){
                    .width = tex_w,
                    .height = tex_h,
                    .depth = 1,
                    .decoded_size = t->decoded_size,
                    .decoded_data = t->decoded_data,
                };
                width /= 2;
                height /= 2;
            }
        }
        g_free(tasks);
        }
    } else if (s.dimensionality == 3) {
        nv2a_vk_assert(!f.linear);
        int total_tasks = s.levels;
        DecodeTask *tasks = g_malloc0_n(total_tasks, sizeof(DecodeTask));
        DecodeTaskBatch batch3d;
        decode_batch_init(&batch3d, total_tasks);

        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;
        for (int level = 0; level < s.levels; level++) {
            width = MAX(width, 1);
            height = MAX(height, 1);
            depth = MAX(depth, 1);

            DecodeTask *t = &tasks[level];
            t->src_data = texture_data_ptr;
            t->palette_data = palette_data_ptr;
            t->shape = s;
            t->fmt_info = f;
            t->width = width;
            t->height = height;
            t->depth = depth;
            t->is_compressed = is_compressed;
            t->is_3d = true;
            t->batch = &batch3d;

            if (width <= 16 && height <= 16 && depth <= 16) {
                decode_task_func(t, NULL);
            } else {
                GError *err = NULL;
                g_thread_pool_push(r->decode_thread_pool, t, &err);
                if (err) {
                    fprintf(stderr, "nv2a: decode thread pool push failed: %s\n",
                            err->message);
                    g_error_free(err);
                    decode_task_func(t, NULL);
                }
            }

            if (is_compressed) {
                unsigned int pw = (width + 3) & ~3;
                unsigned int ph = (height + 3) & ~3;
                texture_data_ptr += pw / 4 * ph / 4 * depth * block_size;
            } else {
                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }

        decode_batch_wait(&batch3d);

        for (int level = 0; level < s.levels; level++) {
            DecodeTask *t = &tasks[level];
            layout->layers[0].levels[level] = (TextureLevel){
                .width = t->width,
                .height = t->height,
                .depth = t->depth,
                .decoded_size = t->decoded_size,
                .decoded_data = t->decoded_data,
            };
        }
        g_free(tasks);
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

/*
 * VRAM-byte-range spatial index for the texture cache. Each bucket
 * covers TEX_DIRTY_BUCKET_SIZE bytes of guest VRAM and holds pointers
 * to every TextureBinding whose key.texture_* or key.palette_* range
 * overlaps that bucket. A dirty range of size S touches
 * ceil(S / TEX_DIRTY_BUCKET_SIZE) buckets, so dirty-scan cost scales
 * with the dirty range instead of with the cache population.
 *
 * 128 KiB buckets strike a balance: small enough that a typical
 * 64 KiB texture only lands in 1 bucket, large enough that a 2 MiB
 * font atlas lands in 16 (keeping the per-texture bookkeeping cheap
 * on insert / evict).
 */
#define TEX_DIRTY_BUCKET_SHIFT 17
#define TEX_DIRTY_BUCKET_SIZE  (1ULL << TEX_DIRTY_BUCKET_SHIFT)

static void tex_dirty_buckets_insert(PGRAPHVkState *r, TextureBinding *snode);

static void tex_dirty_buckets_seed_visitor(Lru *lru, LruNode *node, void *opaque)
{
    PGRAPHVkState *r = opaque;
    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->image == VK_NULL_HANDLE) {
        return;
    }
    tex_dirty_buckets_insert(r, tnode);
}

static void tex_dirty_buckets_ensure(PGRAPHVkState *r, hwaddr vram_size)
{
    if (r->tex_dirty_buckets != NULL) {
        return;
    }
    uint32_t n = (uint32_t)((vram_size + TEX_DIRTY_BUCKET_SIZE - 1) /
                            TEX_DIRTY_BUCKET_SIZE);
    r->tex_dirty_buckets = g_new0(GPtrArray *, n);
    r->tex_dirty_num_buckets = n;

    /*
     * On first-use init the buckets are empty but the LRU may already
     * contain cached TextureBindings from frames before any dirty
     * signal fired (cache-miss inserts no-op when buckets are NULL).
     * Seed the index by walking the active LRU once; all subsequent
     * inserts/removes keep the index in sync incrementally.
     */
    lru_visit_active(&r->texture_cache, tex_dirty_buckets_seed_visitor, r);
}

static inline uint32_t tex_dirty_bucket_idx(hwaddr addr)
{
    return (uint32_t)(addr >> TEX_DIRTY_BUCKET_SHIFT);
}

static void tex_dirty_buckets_insert_range(PGRAPHVkState *r,
                                           TextureBinding *snode,
                                           hwaddr range_addr,
                                           hwaddr range_len)
{
    if (range_len == 0 || r->tex_dirty_buckets == NULL) {
        return;
    }
    uint32_t first = tex_dirty_bucket_idx(range_addr);
    uint32_t last = tex_dirty_bucket_idx(range_addr + range_len - 1);
    if (last >= r->tex_dirty_num_buckets) {
        last = r->tex_dirty_num_buckets - 1;
    }
    for (uint32_t b = first; b <= last; b++) {
        if (r->tex_dirty_buckets[b] == NULL) {
            r->tex_dirty_buckets[b] = g_ptr_array_new();
        }
        g_ptr_array_add(r->tex_dirty_buckets[b], snode);
    }
}

static void tex_dirty_buckets_insert(PGRAPHVkState *r, TextureBinding *snode)
{
    tex_dirty_buckets_insert_range(r, snode,
                                   snode->key.texture_vram_offset,
                                   snode->key.texture_length);
    if (snode->key.palette_length > 0) {
        tex_dirty_buckets_insert_range(r, snode,
                                       snode->key.palette_vram_offset,
                                       snode->key.palette_length);
    }
}

static void tex_dirty_buckets_remove_range(PGRAPHVkState *r,
                                           TextureBinding *snode,
                                           hwaddr range_addr,
                                           hwaddr range_len)
{
    if (range_len == 0 || r->tex_dirty_buckets == NULL) {
        return;
    }
    uint32_t first = tex_dirty_bucket_idx(range_addr);
    uint32_t last = tex_dirty_bucket_idx(range_addr + range_len - 1);
    if (last >= r->tex_dirty_num_buckets) {
        last = r->tex_dirty_num_buckets - 1;
    }
    for (uint32_t b = first; b <= last; b++) {
        GPtrArray *arr = r->tex_dirty_buckets[b];
        if (arr == NULL) {
            continue;
        }
        /*
         * Array is small (typically a handful of entries per bucket);
         * g_ptr_array_remove_fast swaps with the last and pops, so
         * order doesn't matter and removal is O(k) with k == bucket
         * occupancy.
         */
        g_ptr_array_remove_fast(arr, snode);
    }
}

static void tex_dirty_buckets_remove(PGRAPHVkState *r, TextureBinding *snode)
{
    tex_dirty_buckets_remove_range(r, snode,
                                   snode->key.texture_vram_offset,
                                   snode->key.texture_length);
    if (snode->key.palette_length > 0) {
        tex_dirty_buckets_remove_range(r, snode,
                                       snode->key.palette_vram_offset,
                                       snode->key.palette_length);
    }
}

static void tex_dirty_buckets_finalize(PGRAPHVkState *r)
{
    if (r->tex_dirty_buckets == NULL) {
        return;
    }
    for (uint32_t b = 0; b < r->tex_dirty_num_buckets; b++) {
        if (r->tex_dirty_buckets[b] != NULL) {
            g_ptr_array_free(r->tex_dirty_buckets[b], TRUE);
        }
    }
    g_free(r->tex_dirty_buckets);
    r->tex_dirty_buckets = NULL;
    r->tex_dirty_num_buckets = 0;
}

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    hwaddr vram_size = memory_region_size(d->vram);
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    nv2a_vk_assert(end <= vram_size);

    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    tex_dirty_buckets_ensure(r, vram_size);

    uint32_t first = tex_dirty_bucket_idx(addr);
    uint32_t last = tex_dirty_bucket_idx(end);
    if (last >= r->tex_dirty_num_buckets) {
        last = r->tex_dirty_num_buckets - 1;
    }

    /*
     * Walk every bucket in the dirty range; for each TextureBinding in
     * those buckets, confirm byte-level overlap (bucketing is
     * conservative — a bucket can hold a texture that touches the
     * bucket but not our specific dirty range) and flip
     * possibly_dirty. Wide dirty ranges (or the flush path which
     * passes [0, vram_size)) still scale linearly with active-texture
     * count, but the common small-range case is now proportional to
     * touched buckets only.
     */
    for (uint32_t b = first; b <= last; b++) {
        GPtrArray *arr = r->tex_dirty_buckets[b];
        if (arr == NULL) {
            continue;
        }
        for (guint i = 0; i < arr->len; i++) {
            TextureBinding *tnode = arr->pdata[i];
            if (tnode->possibly_dirty) {
                continue;
            }

            hwaddr k_tex_addr = tnode->key.texture_vram_offset;
            hwaddr k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
            bool overlapping =
                !(addr > k_tex_end || k_tex_addr > end);

            if (!overlapping && tnode->key.palette_length > 0) {
                hwaddr k_pal_addr = tnode->key.palette_vram_offset;
                hwaddr k_pal_end =
                    k_pal_addr + tnode->key.palette_length - 1;
                overlapping =
                    !(addr > k_pal_end || k_pal_addr > end);
            }

            if (overlapping) {
                tnode->possibly_dirty = true;
            }
        }
    }
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    nv2a_vk_assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, texture_vram_offset, length);
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, palette_vram_offset,
                                            palette_length);
    }
    return possibly_dirty;
}

static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    const int num_layers = state->cubemap ? 6 : 1;

    bool has_gpu_unswizzle = layout->layers[0].levels[0].gpu_unswizzle;

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            nv2a_vk_assert(size);
            texture_data_size += size;
        }
    }

    StorageBuffer *staging = &r->storage_buffers[BUFFER_STAGING_SRC];

    if (has_gpu_unswizzle && pgraph_vk_compute_needs_finish(r)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    /*
     * Sub-allocate from BUFFER_STAGING_SRC using buffer_offset as a bump
     * allocator. Multiple texture uploads accumulate in the staging buffer
     * and their copies are batched on the main command buffer. If the
     * staging buffer is too full, flush first to reclaim space.
     */
    VkDeviceSize slot_limit = r->flight[r->current_flight].staging_buffer_limit;
    if (staging->buffer_offset + texture_data_size > slot_limit) {
        if (r->in_command_buffer) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        }
        staging->buffer_offset = r->flight[r->current_flight].staging_buffer_base;
    }

    nv2a_vk_assert(staging->buffer_offset + texture_data_size <= slot_limit);

    VkDeviceSize base_offset = staging->buffer_offset;

    uint8_t *mapped_memory_ptr = staging->mapped;

    int num_regions = num_layers * state->levels;
    /*
     * num_regions is bounded by TextureLayer.levels[16] ×
     * TextureLayout.layers[6] = 96 entries. Keep on the stack
     * (~5 KiB) instead of g_malloc0_n + g_autofree per upload; per-
     * upload heap churn shows up under texture streaming.
     */
    nv2a_vk_assert(num_regions <= (int)(ARRAY_SIZE(layout->layers) *
                                        ARRAY_SIZE(layout->layers[0].levels)));
    VkBufferImageCopy regions[ARRAY_SIZE(layout->layers) *
                              ARRAY_SIZE(layout->layers[0].levels)] = { 0 };

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = base_offset;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + buffer_offset, level->decoded_data,
                   level->decoded_size);
            *region = (VkBufferImageCopy){
                .bufferOffset = buffer_offset,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += level->decoded_size;
            region++;
        }
    }

    staging->buffer_offset = buffer_offset;

    if (!staging->is_coherent) {
        vmaFlushAllocation(r->allocator, staging->allocation,
                           base_offset, texture_data_size);
    }

    {
        VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
        pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

        VkBufferMemoryBarrier host_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging->buffer,
            .offset = base_offset,
            .size = texture_data_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &host_barrier, 0, NULL);

        pgraph_vk_transition_image_layout(pg, cmd, binding->image,
                                          vkf.vk_format,
                                          binding->current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        if (has_gpu_unswizzle) {
            BasicColorFormatInfo f_info =
                kelvin_color_format_info_map[state->color_format];
            VkDeviceSize level_offset = base_offset;

            for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
                for (int level_idx = 0; level_idx < state->levels;
                     level_idx++) {
                    TextureLevel *level =
                        &layout->layers[layer_idx].levels[level_idx];

                    VkBufferCopy swz_copy = {
                        .srcOffset = level_offset,
                        .dstOffset = 0,
                        .size = level->decoded_size,
                    };
                    vkCmdCopyBuffer(
                        cmd, staging->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer, 1,
                        &swz_copy);

                    VkBufferMemoryBarrier pre_compute = {
                        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .buffer =
                            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                        .size = level->decoded_size,
                    };
                    vkCmdPipelineBarrier(
                        cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                        1, &pre_compute, 0, NULL);

                    if (f_info.bytes_per_pixel == 2) {
                        pgraph_vk_dispatch_unswizzle_2bpp(
                            pg, cmd,
                            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                            level->unswizzle_width, level->unswizzle_height);
                    } else {
                        pgraph_vk_dispatch_unswizzle(
                            pg, cmd,
                            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                            level->unswizzle_width, level->unswizzle_height);
                    }

                    VkBufferMemoryBarrier post_compute = {
                        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .buffer =
                            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                        .size = level->decoded_size,
                    };
                    vkCmdPipelineBarrier(
                        cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                        &post_compute, 0, NULL);

                    VkBufferImageCopy image_region = {
                        .bufferOffset = 0,
                        .bufferRowLength = 0,
                        .bufferImageHeight = 0,
                        .imageSubresource.aspectMask =
                            VK_IMAGE_ASPECT_COLOR_BIT,
                        .imageSubresource.mipLevel = level_idx,
                        .imageSubresource.baseArrayLayer = layer_idx,
                        .imageSubresource.layerCount = 1,
                        .imageOffset = (VkOffset3D){ 0, 0, 0 },
                        .imageExtent = (VkExtent3D){ level->width,
                                                     level->height,
                                                     level->depth },
                    };
                    vkCmdCopyBufferToImage(
                        cmd,
                        r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                        binding->image, binding->current_layout, 1,
                        &image_region);

                    level_offset += level->decoded_size;
                }
            }
        } else {
            vkCmdCopyBufferToImage(cmd, staging->buffer,
                                   binding->image, binding->current_layout,
                                   num_regions, regions);
        }

        pgraph_vk_transition_image_layout(pg, cmd, binding->image,
                                          vkf.vk_format,
                                          binding->current_layout,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        pgraph_vk_end_debug_marker(r, cmd);
    }

    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
}

static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    nv2a_vk_assert(!surface->color);

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    bool use_compute_to_convert_depth_stencil =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool compute_needs_finish = use_compute_to_convert_depth_stencil &&
                                pgraph_vk_compute_needs_finish(r);
    if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    size_t copied_image_size =
        scaled_width * scaled_height * surface->host_fmt.host_bytes_per_pixel;
    size_t stencil_buffer_offset = 0;
    size_t stencil_buffer_size = 0;

    int num_regions = 0;
    VkBufferImageCopy regions[2];
    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0, // Tightly packed
        .bufferImageHeight = 0, // Tightly packed
        .imageSubresource.aspectMask = surface->color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        stencil_buffer_offset =
            ROUND_UP(scaled_width * scaled_height * 4,
                     r->device_props.limits.minStorageBufferOffsetAlignment);
        stencil_buffer_size = scaled_width * scaled_height;
        copied_image_size += stencil_buffer_size;

        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = stencil_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){0, 0, 0},
            .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
        };
    }
    StorageBuffer *dst_storage_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    nv2a_vk_bounds_check(dst_storage_buffer->buffer_size >= copied_image_size);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    vkCmdCopyImageToBuffer(
        cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_storage_buffer->buffer,
        num_regions, regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkBuffer texture_source_buffer;

    if (use_compute_to_convert_depth_stencil) {
        size_t packed_image_size = scaled_width * scaled_height * 4;

        VkBufferMemoryBarrier pre_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = copied_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = packed_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(
            pg, surface, cmd,
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, false);

        VkBufferMemoryBarrier post_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = copied_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = packed_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_dst_barrier, 0, NULL);

        texture_source_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
    } else {
        VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = copied_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             1, &barrier, 0, NULL);

        texture_source_buffer = dst_storage_buffer->buffer;
    }

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    regions[0] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, texture_source_buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);

    VkBufferMemoryBarrier post_copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = texture_source_buffer,
        .size = copied_image_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

// FIXME: Should be able to skip the copy and sample the original surface image
static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = surface->host_fmt.aspect,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                   texture->current_layout, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static unsigned int vk_format_texel_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:   return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:     return 2;
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:   return 2;
    case VK_FORMAT_R16_UNORM:               return 2;
    case VK_FORMAT_R8G8B8_SNORM:            return 3;
    case VK_FORMAT_B8G8R8A8_UNORM:          return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return 4;
    case VK_FORMAT_R32_UINT:                return 4;
    default:                                return 0;
    }
}

static bool check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height ||
        shape->cubemap ||
        shape->levels > 1) {
        return false;
    }

    if (!surface->color) {
        return true;
    }

    VkColorFormatInfo tex_vkf = kelvin_color_format_vk_map[shape->color_format];
    return tex_vkf.vk_format &&
           surface->host_fmt.host_bytes_per_pixel == vk_format_texel_size(tex_vkf.vk_format);
}

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
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

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

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

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to persistently mapped staging buffer
    uint8_t *mapped_memory_ptr = r->storage_buffers[BUFFER_STAGING_SRC].mapped;
    assert(mapped_memory_ptr);
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    memset(mapped_memory_ptr, 0xff, texture_data_size);

    if (!r->storage_buffers[BUFFER_STAGING_SRC].is_coherent) {
        vmaFlushAllocation(r->allocator,
                           r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                           texture_data_size);
    }

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
    };

    r->dummy_sampler = (SamplerCacheEntry){
        .sampler = texture_sampler,
    };
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
    sampler_cache_release_node_resources(r, &r->dummy_sampler);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, texture->allocation, label);
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx); // FIXME: Check for pad issues
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter =
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx*4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;

    SamplerKey sampler_key;
    memset(&sampler_key, 0, sizeof(sampler_key));
    sampler_key.filter = filter;
    sampler_key.address = address;
    sampler_key.border_color = border_color_pack32;
    sampler_key.max_anisotropy = max_anisotropy;
    sampler_key.color_format = state.color_format;
    sampler_key.dimensionality = state.dimensionality;
    sampler_key.levels = state.levels;
    sampler_key.min_mipmap_level = state.min_mipmap_level;
    sampler_key.max_mipmap_level = state.max_mipmap_level;
    sampler_key.linear = f_basic.linear;
    sampler_key.custom_border_color_enabled = r->custom_border_color_extension_enabled;

    bool possibly_dirty = false;
    bool surface_to_texture = false;

    // Check active surfaces to see if this texture was a render target
    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        surface_to_texture =
            check_surface_to_texture_compatiblity(surface, &state);

        if (!surface_to_texture && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format,
                state.color_format);
        }

        if (surface_to_texture && surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    }

    if (!surface_to_texture) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        pgraph_vk_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length);
    }

    if (surface_to_texture && pg->surface_scale_factor > 1) {
        key.scale = pg->surface_scale_factor;
    }

    // --- Sampler cache lookup ---
    uint64_t sampler_hash = fast_hash((void *)&sampler_key, sizeof(sampler_key));
    LruNode *sampler_node = lru_lookup(&r->sampler_cache, sampler_hash, &sampler_key);
    SamplerCacheEntry *sampler_entry = container_of(sampler_node, SamplerCacheEntry, node);
    bool sampler_found = sampler_entry->sampler != VK_NULL_HANDLE;

    if (!sampler_found) {
        memcpy(&sampler_entry->key, &sampler_key, sizeof(sampler_key));

        VkColorFormatInfo vkf_s = kelvin_color_format_vk_map[state.color_format];
        void *sampler_next_struct = NULL;

        VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
        VkBorderColor vk_border_color;

        bool is_integer_type = vkf_s.vk_format == VK_FORMAT_R32_UINT;

        if (r->custom_border_color_extension_enabled) {
            vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                                VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            custom_border_color_create_info =
                (VkSamplerCustomBorderColorCreateInfoEXT){
                    .sType =
                        VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                    .format = vkf_s.vk_format,
                    .pNext = sampler_next_struct
                };
            if (is_integer_type) {
                float rgba[4];
                pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
                for (int i = 0; i < 4; i++) {
                    custom_border_color_create_info.customBorderColor.uint32[i] =
                        (uint32_t)((double)rgba[i] * (double)0xffffffff);
                }
            } else {
                pgraph_argb_pack32_to_rgba_float(
                    border_color_pack32,
                    custom_border_color_create_info.customBorderColor.float32);
            }
            sampler_next_struct = &custom_border_color_create_info;
        } else {
            if (is_integer_type) {
                vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
            } else if (border_color_pack32 == 0x00000000) {
                vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            } else if (border_color_pack32 == 0xff000000) {
                vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            } else {
                vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            }
        }

        if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
            NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
        if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
            NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
        if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
            NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
        if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
            NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

        VkFilter vk_min_filter, vk_mag_filter;
        unsigned int mag_filter_val = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
        nv2a_vk_bounds_check(mag_filter_val < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

        unsigned int min_filter_val = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
        nv2a_vk_bounds_check(min_filter_val < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

        if (is_linear_filter_supported_for_format(r, state.color_format)) {
            vk_mag_filter = pgraph_texture_mag_filter_vk_map[mag_filter_val];
            vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter_val];
        } else {
            vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
        }

        unsigned int mip_levels = f_basic.linear ? 1 : state.levels;

        bool mipmap_en =
            !f_basic.linear &&
            !(min_filter_val == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
              min_filter_val == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
              min_filter_val == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

        bool mipmap_nearest =
            f_basic.linear || mip_levels == 1 ||
            min_filter_val == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
            min_filter_val == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

        float lod_bias = pgraph_convert_lod_bias_to_float(
            GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
        if (lod_bias > r->device_props.limits.maxSamplerLodBias) {
            lod_bias = r->device_props.limits.maxSamplerLodBias;
        } else if (lod_bias < -r->device_props.limits.maxSamplerLodBias) {
            lod_bias = -r->device_props.limits.maxSamplerLodBias;
        }
        uint32_t sampler_max_anisotropy =
            MIN(r->device_props.limits.maxSamplerAnisotropy, max_anisotropy);

        VkSamplerCreateInfo sampler_create_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = vk_mag_filter,
            .minFilter = vk_min_filter,
            .addressModeU = lookup_texture_address_mode(
                GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
            .addressModeV = lookup_texture_address_mode(
                GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
            .addressModeW = (state.dimensionality > 2) ? lookup_texture_address_mode(
                GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) : 0,
            .anisotropyEnable =
                r->enabled_physical_device_features.samplerAnisotropy &&
                sampler_max_anisotropy > 1,
            .maxAnisotropy = sampler_max_anisotropy,
            .borderColor = vk_border_color,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                           VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .minLod = mipmap_en ? MIN(state.min_mipmap_level, state.levels - 1) : 0.0,
            .maxLod = mipmap_en ? MIN(state.max_mipmap_level, state.levels - 1) : 0.0,
            .mipLodBias = lod_bias,
            .pNext = sampler_next_struct,
        };

        VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                                 &sampler_entry->sampler));
    }

    r->sampler_bindings[texture_idx] = sampler_entry;

    // --- Texture image cache lookup ---
    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->image != VK_NULL_HANDLE;

    if (binding_found) {
        NV2A_VK_DPRINTF("Texture cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
    } else {
        possibly_dirty = true;
    }

    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    uint64_t content_hash = 0;

    if (!surface_to_texture) {
        /*
         * Chunked path requires that each chunk covers a distinct,
         * non-overlapping set of dirty-bitmap pages so per-chunk
         * test_and_clear_dirty can't steal another chunk's signal.
         *
         * Instead of demanding page-aligned texture extents, we reshape
         * chunks to be page-aligned at both ends by construction:
         *
         *   head fragment (optional): [tex_off, next_page_boundary)
         *     - up to (TARGET_PAGE_SIZE - 1) bytes
         *     - owns exactly the one page containing tex_off
         *     - skipped when tex_off is already page-aligned
         *
         *   body chunks: TEXTURE_CHUNK_SIZE-sized (64 KiB) starting
         *     at the first page boundary. Each covers exactly 16
         *     contiguous pages that no other chunk touches.
         *
         *   last body chunk: clipped to tex_end. Its trailing partial
         *     page (if any) is owned exclusively by this chunk.
         *
         * With head + body disjoint by page, test_and_clear_dirty per
         * chunk is safe for any alignment of texture_vram_offset.
         */
        bool use_chunks = texture_length >= TEXTURE_INCREMENTAL_HASH_MIN;
        if (!use_chunks) {
            /*
             * Small-texture path: single-shot full-buffer hash. The
             * chunked path's per-call overhead exceeds its savings
             * below ~256 KiB.
             */
            possibly_dirty |= check_texture_possibly_dirty(
                d, texture_vram_offset, texture_length,
                texture_palette_vram_offset, texture_palette_data_size);
            if (possibly_dirty) {
                content_hash = fast_hash(texture_data, texture_length);
                if (is_indexed) {
                    content_hash ^= fast_hash(palette_data,
                                              texture_palette_data_size);
                }
            }
        } else {
            /*
             * Incremental-hash path. See the layout comment above for
             * how head + body chunks divide up both the byte data and
             * the host-page set without overlap.
             */
            hwaddr anchor = TARGET_PAGE_ALIGN(texture_vram_offset);
            size_t head_len = anchor - texture_vram_offset;
            size_t body_len = (head_len < texture_length)
                                  ? texture_length - head_len
                                  : 0;
            bool has_head = head_len > 0 && head_len < texture_length;
            uint32_t body_chunks = (uint32_t)(
                (body_len + TEXTURE_CHUNK_SIZE - 1) / TEXTURE_CHUNK_SIZE);
            uint32_t expected_chunks =
                (has_head ? 1 : 0) + body_chunks;
            /*
             * use_chunks gate guarantees texture_length >= 256 KiB
             * which is far larger than a single host page, so the
             * degenerate "entire texture fits in one partial page"
             * case can't arise; expected_chunks >= 1 always.
             */
            nv2a_vk_assert(expected_chunks >= 1);

            bool need_alloc = (snode->chunk_hashes == NULL
                               || snode->num_chunk_hashes != expected_chunks);
            if (need_alloc) {
                g_free(snode->chunk_hashes);
                snode->chunk_hashes = g_new(uint64_t, expected_chunks);
                snode->num_chunk_hashes = expected_chunks;
                snode->palette_hash = 0; /* force re-hash below */
            }

            /*
             * `externally_marked` captures the case where another
             * texture's upload called pgraph_vk_mark_textures_possibly_dirty
             * on a VRAM range that overlaps ours. That neighbor's
             * test_and_clear_dirty already cleared the shared host
             * pages' dirty bits, so OUR local dirty-detection below
             * will return false even if the guest did modify our
             * data in those pages. In that state the cached
             * chunk_hashes can't be trusted: we must re-hash all
             * chunks to catch updates that we can't see as local
             * dirty events. This matches the pre-chunked single-shot
             * full-hash behavior, which always recomputed the whole
             * texture hash whenever possibly_dirty was set from any
             * source. Without this, shared-page textures (frequently
             * the case for HUD / translucent / LOD-streamed content)
             * show stale frames intermittently.
             */
            bool externally_marked = binding_found && snode->possibly_dirty;

            bool any_chunk_dirty = false;
            for (uint32_t i = 0; i < expected_chunks; i++) {
                hwaddr byte_start; /* relative to texture_data */
                size_t chunk_size;
                hwaddr page_start, page_end; /* absolute VRAM */

                if (has_head && i == 0) {
                    byte_start = 0;
                    chunk_size = head_len;
                    page_start = texture_vram_offset & TARGET_PAGE_MASK;
                    page_end = anchor;
                } else {
                    uint32_t body_i = has_head ? (i - 1) : i;
                    byte_start = head_len +
                        (hwaddr)body_i * TEXTURE_CHUNK_SIZE;
                    hwaddr byte_end = MIN(
                        byte_start + (hwaddr)TEXTURE_CHUNK_SIZE,
                        (hwaddr)texture_length);
                    chunk_size = byte_end - byte_start;
                    page_start = texture_vram_offset + byte_start;
                    /*
                     * Non-last body chunks end on a page boundary by
                     * construction. The last chunk may finish
                     * mid-page — round that final page up so we own
                     * it exclusively (no chunk follows).
                     */
                    hwaddr vram_end = texture_vram_offset + byte_end;
                    page_end = (byte_end == texture_length)
                                   ? TARGET_PAGE_ALIGN(vram_end)
                                   : vram_end;
                }

                nv2a_vk_assert(page_end <= memory_region_size(d->vram));
                bool dirty = memory_region_test_and_clear_dirty(
                    d->vram, page_start, page_end - page_start,
                    DIRTY_MEMORY_NV2A_TEX);
                if (dirty) {
                    any_chunk_dirty = true;
                }
                if (need_alloc || dirty || externally_marked) {
                    snode->chunk_hashes[i] = fast_hash(
                        (const uint8_t *)texture_data + byte_start,
                        chunk_size);
                }
            }

            bool palette_dirty = false;
            if (texture_palette_data_size
                && check_texture_dirty(d, texture_palette_vram_offset,
                                       texture_palette_data_size)) {
                palette_dirty = true;
            }

            if (any_chunk_dirty) {
                possibly_dirty = true;
                pgraph_vk_mark_textures_possibly_dirty(
                    d, texture_vram_offset, texture_length);
            }
            if (palette_dirty) {
                possibly_dirty = true;
                pgraph_vk_mark_textures_possibly_dirty(
                    d, texture_palette_vram_offset,
                    texture_palette_data_size);
            }

            if (possibly_dirty) {
                for (uint32_t i = 0; i < expected_chunks; i++) {
                    content_hash ^= snode->chunk_hashes[i];
                }
                if (is_indexed) {
                    /*
                     * Force palette re-hash on externally_marked for
                     * the same reason as the chunk loop above: a
                     * neighbor texture's test_and_clear may have
                     * consumed our palette page's dirty bit.
                     */
                    if (palette_dirty || snode->palette_hash == 0
                        || externally_marked) {
                        snode->palette_hash = fast_hash(
                            palette_data, texture_palette_data_size);
                    }
                    content_hash ^= snode->palette_hash;
                }
            }
        }
    }

    if (binding_found) {
        if (surface_to_texture) {
            if (surface->draw_time != snode->draw_time) {
                copy_surface_to_texture(pg, surface, snode);
            }
        } else {
            if (possibly_dirty && content_hash != snode->hash) {
                upload_texture_image(pg, texture_idx, snode);
                snode->hash = content_hash;
            }
        }

        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Texture cache miss");

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->possibly_dirty = false;
    snode->hash = content_hash;

    /*
     * Register the binding in the VRAM spatial index so subsequent
     * dirty-signals only scan the touched buckets. Safe to call before
     * tex_dirty_buckets_ensure() if it's never been run: the insert
     * no-ops when the buckets array is NULL, and the first dirty call
     * will ensure() + populate from scratch. Current callers call
     * pgraph_vk_mark_textures_possibly_dirty via update_surfaces /
     * flush_memory_buffer routinely, so ensure() runs well before any
     * texture-miss path.
     */
    tex_dirty_buckets_insert(r, snode);

    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    nv2a_vk_bounds_check(vkf.vk_format != 0);
    nv2a_vk_bounds_check(0 < state.dimensionality);
    nv2a_vk_bounds_check(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    nv2a_vk_bounds_check(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = state.width,
        .extent.height = state.height,
        .extent.depth = state.depth,
        .mipLevels = f_basic.linear ? 1 : state.levels,
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = vkf.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if (surface_to_texture) {
        pgraph_apply_scaling_factor(pg, &image_create_info.extent.width,
                                        &image_create_info.extent.height);
    }

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &snode->image,
                            &snode->allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = snode->image,
        .viewType = state.cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state.dimensionality],
        .format = vkf.vk_format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = vkf.component_map,
    };

    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &snode->image_view));

    set_texture_label(pg, snode);

    r->texture_bindings[texture_idx] = snode;

    if (surface_to_texture) {
        copy_surface_to_texture(pg, surface, snode);
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
    }

    NV2A_VK_DGROUP_END();
}

static bool check_textures_dirty_and_update_timestamps(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool dirty = false;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i]) {
            dirty = true;
        } else {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
    return dirty;
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    if (!check_textures_dirty_and_update_timestamps(pg)) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->texture_bindings[i] = &r->dummy_texture;
            r->sampler_bindings[i] = &r->dummy_sampler;
            continue;
        }

        create_texture(pg, i);

        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
    }

    r->texture_bindings_changed = true;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->chunk_hashes = NULL;
    snode->num_chunk_hashes = 0;
    snode->palette_hash = 0;
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    /*
     * Remove from the VRAM spatial index while snode->key is still
     * intact. Post-release, a future cache-miss may reuse this slot
     * with a different key and the old bucket entries would become
     * stale pointers if not pulled out here. No-op when the index
     * hasn't been allocated yet.
     */
    tex_dirty_buckets_remove(r, snode);

    vkDestroyImageView(r->device, snode->image_view, NULL);
    snode->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, snode->image, snode->allocation);
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;

    g_free(snode->chunk_hashes);
    snode->chunk_hashes = NULL;
    snode->num_chunk_hashes = 0;
}

static void sampler_cache_release_node_resources(PGRAPHVkState *r, SamplerCacheEntry *snode)
{
    vkDestroySampler(r->device, snode->sampler, NULL);
    snode->sampler = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // FIXME: Simplify. We don't really need to check bindings


    // Currently bound
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    // Used in command buffer
    if (r->in_command_buffer && snode->submit_time == r->submit_count) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size = 8192;
    lru_init(&r->texture_cache);
    r->texture_cache_entries = g_malloc_n(texture_cache_size, sizeof(TextureBinding));
    nv2a_vk_assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    tex_dirty_buckets_finalize(r);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

static void sampler_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    SamplerCacheEntry *snode = container_of(node, SamplerCacheEntry, node);
    snode->sampler = VK_NULL_HANDLE;
}

static bool sampler_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, sampler_cache);
    SamplerCacheEntry *snode = container_of(node, SamplerCacheEntry, node);

    for (int i = 0; i < ARRAY_SIZE(r->sampler_bindings); i++) {
        if (r->sampler_bindings[i] == snode) {
            return false;
        }
    }

    return true;
}

static void sampler_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, sampler_cache);
    SamplerCacheEntry *snode = container_of(node, SamplerCacheEntry, node);
    sampler_cache_release_node_resources(r, snode);
}

static bool sampler_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    SamplerCacheEntry *snode = container_of(node, SamplerCacheEntry, node);
    return memcmp(&snode->key, key, sizeof(SamplerKey));
}

static void sampler_cache_init(PGRAPHVkState *r)
{
    const size_t sampler_cache_size = 256;
    lru_init(&r->sampler_cache);
    r->sampler_cache_entries = g_malloc_n(sampler_cache_size, sizeof(SamplerCacheEntry));
    nv2a_vk_assert(r->sampler_cache_entries != NULL);
    for (int i = 0; i < sampler_cache_size; i++) {
        lru_add_free(&r->sampler_cache, &r->sampler_cache_entries[i].node);
    }
    r->sampler_cache.init_node = sampler_cache_entry_init;
    r->sampler_cache.compare_nodes = sampler_cache_entry_compare;
    r->sampler_cache.pre_node_evict = sampler_cache_entry_pre_evict;
    r->sampler_cache.post_node_evict = sampler_cache_entry_post_evict;
}

static void sampler_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->sampler_cache);
    g_free(r->sampler_cache_entries);
    r->sampler_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted, r->texture_cache.num_used);
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    sampler_cache_init(r);
    create_dummy_texture(pg);

    r->texture_format_properties = g_malloc0_n(
        ARRAY_SIZE(kelvin_color_format_vk_map), sizeof(VkFormatProperties));
    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, kelvin_color_format_vk_map[i].vk_format,
            &r->texture_format_properties[i]);
    }

    r->decode_thread_pool = g_thread_pool_new(
        decode_task_func, NULL, MIN((int)g_get_num_processors(), 4), FALSE, NULL);
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_vk_assert(!r->in_command_buffer);

    if (r->decode_thread_pool) {
        g_thread_pool_free(r->decode_thread_pool, FALSE, TRUE);
        r->decode_thread_pool = NULL;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
        r->sampler_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    sampler_cache_finalize(r);
    texture_cache_finalize(r);

    nv2a_vk_assert(r->texture_cache.num_used == 0);
    nv2a_vk_assert(r->sampler_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}
