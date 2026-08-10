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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <volk.h>
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>

#include "debug.h"
#include "constants.h"
#include "glsl.h"

#if defined(__APPLE__)
#define HAVE_EXTERNAL_MEMORY 0
#define HAVE_IOSURFACE_SHARING 1
#else
#define HAVE_EXTERNAL_MEMORY 1
#define HAVE_IOSURFACE_SHARING 0
#endif

typedef struct QueueFamilyIndices {
    int queue_family;
} QueueFamilyIndices;

typedef struct MemorySyncRequirement {
    hwaddr addr, size;
} MemorySyncRequirement;

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
} RenderPassState;

typedef struct RenderPass {
    RenderPassState state;
    VkRenderPass render_pass;
} RenderPass;

typedef struct PipelineKey {
    bool clear;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    /*
     * 5 pipeline-affecting regs (CONTROL_0/1/2, BLEND, SETUPRASTER).
     * NV_PGRAPH_CONTROL_3 was previously included but its bits
     * (SHADEMODE, FOG_MODE, FOGENABLE, POINTPARAMSENABLE,
     * PROVOKING_VERTEX) are already captured by ShaderState or
     * consumed CPU-side — see init_pipeline_key / check_pipeline_dirty
     * in draw.c and pgraph_glsl_check_shader_state_dirty in
     * glsl/shaders.c.
     */
    uint32_t regs[5];
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

typedef struct PipelineBinding {
    LruNode node;
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkRenderPass render_pass;
    unsigned int draw_time;
    bool has_dynamic_line_width;
    bool has_dynamic_depth_bias;
} PipelineBinding;

enum Buffer {
    BUFFER_STAGING_DST,
    BUFFER_STAGING_SRC,
    BUFFER_COMPUTE_DST,
    BUFFER_COMPUTE_SRC,
    BUFFER_INDEX,
    BUFFER_INDEX_STAGING,
    BUFFER_VERTEX_RAM,
    BUFFER_VERTEX_INLINE,
    BUFFER_VERTEX_INLINE_STAGING,
    BUFFER_UNIFORM,
    BUFFER_UNIFORM_STAGING,
    BUFFER_COUNT
};

typedef struct StorageBuffer {
    VkBuffer buffer;
    VkBufferUsageFlags usage;
    VmaAllocationCreateInfo alloc_info;
    VmaAllocation allocation;
    VkMemoryPropertyFlags properties;
    bool is_coherent;
    size_t buffer_offset;
    size_t buffer_size;
    size_t buffer_limit;
    uint8_t *mapped;
} StorageBuffer;

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;

    /*
     * Highest submission index (r->submit_count numbering) that may
     * still reference this image, stamped at invalidation. The image
     * must not be destroyed until r->retired_submit_count reaches it
     * — destroying a VkImage referenced by a pending command buffer
     * is invalid on every driver, and with flight-slot pipelining the
     * previous main CB is routinely still executing. Reuse (migrate)
     * needs no such gate: see get_any_compatible_invalid_surface.
     */
    uint64_t evict_submit_seq;

    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    VkImage image;
    VkImageView image_view;
    VmaAllocation allocation;

    // Used for scaling
    VkImage image_scratch;
    VkImageLayout image_scratch_current_layout;
    VmaAllocation allocation_scratch;

    /*
     * Exported MTLTexture of a zeta image (opt-in real depth for
     * MetalFX temporal, XEMU_MFX_REAL_DEPTH=1; requires
     * metal_texture_export_enabled). Retained; NULL otherwise.
     */
    void *mtl_texture;

    bool initialized;

    /*
     * Cached index into r->surface_ranges[], -1 when not tracked.
     * Lets surface_ranges_remove skip the linear scan; kept in sync
     * by the insert/remove fixup loops.
     */
    int surface_range_slot;
} SurfaceBinding;

typedef struct ShaderModuleInfo {
    int refcnt;
    char *glsl;
    GByteArray *spirv;
    VkShaderModule module;
    SpvReflectShaderModule reflect_module;
    SpvReflectDescriptorSet **descriptor_sets;
    ShaderUniformLayout uniforms;
    ShaderUniformLayout push_constants;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
    } geom;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
    } psh;
} ShaderBinding;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    VkImage image;
    VkImageLayout current_layout;
    VkImageView image_view;
    VmaAllocation allocation;
    bool possibly_dirty;
    uint64_t hash;
    /*
     * Incremental content-hash cache. When non-NULL, `chunk_hashes` is an
     * array of `num_chunk_hashes` 64-bit XXH3 hashes, one per
     * TEXTURE_CHUNK_SIZE bytes of texture content. On dirty-bitmap fire
     * we re-hash only chunks whose backing pages are dirty; the aggregate
     * (`hash`) is the XOR of all chunk hashes plus `palette_hash`. Only
     * allocated for page-aligned textures >= TEXTURE_INCREMENTAL_HASH_MIN;
     * freed on eviction.
     */
    uint64_t *chunk_hashes;
    uint32_t num_chunk_hashes;
    uint64_t palette_hash;
    /*
     * Guest frame (pg->frame_time) in which this binding's content was
     * last verified against (or uploaded from) guest RAM. Re-binds in
     * the same frame skip the per-bind dirty-bitmap scan + hash unless
     * possibly_dirty was raised in between. See create_texture().
     */
    int verified_frame_time;
    unsigned int draw_time;
    uint32_t submit_time;
} TextureBinding;

typedef struct SamplerKey {
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
    int color_format;
    int dimensionality;
    int levels;
    int min_mipmap_level;
    int max_mipmap_level;
    bool linear;
    bool custom_border_color_enabled;
} SamplerKey;

typedef struct SamplerCacheEntry {
    LruNode node;
    SamplerKey key;
    VkSampler sampler;
    /* Same retirement gate as TextureBinding.submit_time: the sampler
     * may be referenced by descriptor sets of in-flight submissions. */
    uint32_t submit_time;
} SamplerCacheEntry;

typedef struct QueryReport QueryReport;
typedef QSIMPLEQ_HEAD(QueryReportQueue, QueryReport) QueryReportQueue;

struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
};

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

#if HAVE_IOSURFACE_SHARING
/*
 * One published present step in the push-model ring (XEMU_PUSH_PRESENT).
 * A flip publishes 1 entry with interpolation off, or `mode` entries with
 * interpolation on (the interp midpoint step(s) then the real frame), each
 * carrying its intended on-screen hold. Exactly one of mtl_texture /
 * iosurface is set; the entry owns a CFRetain on it until the ring slot is
 * overwritten or invalidated.
 */
typedef struct PushPresentEntry {
    void *mtl_texture;   // id<MTLTexture>, retained by the entry (or NULL)
    void *iosurface;     // IOSurfaceRef, retained by the entry (or NULL)
    void *event;         // id<MTLSharedEvent>, borrowed (sticky)
    uint64_t event_value;
    uint64_t frame_seq;
    uint64_t duration_ns;
    uint64_t generation; // schedule generation this entry belongs to
    int width, height;
    bool is_real;        // final step of its flip's schedule (not a midpoint)
} PushPresentEntry;

/* Ring depth: sized so the consumer's bounded-debt catch-up (see
 * push_present_max_debt: worst allowed leftover = 4x mode + 2 = 6) plus
 * one full atomically-published 4x schedule (4 steps) fits without
 * touching an unread slot — 6 + 4 = 10, +2 slack. At 8 (the original
 * "two 4x flips" sizing) a 4x burst arriving on a full-debt ring forced
 * one unread drop per burst at extreme step rates. A deeper UI stall
 * still drops oldest entries (bounded latency), matching the pull
 * path's drop-stale-when-behind. */
#define PUSH_PRESENT_RING_CAP 12
#endif

typedef struct PGRAPHVkDisplayState {
    ShaderModuleInfo *display_frag;

    // Cached uniform locations (resolved once after shader load)
    int uloc_display_size;
    int uloc_line_offset;
    int uloc_pvideo_enable;
    int uloc_pvideo_color_key_enable;
    int uloc_pvideo_color_key;
    int uloc_pvideo_in_pos;
    int uloc_pvideo_pos;
    int uloc_pvideo_scale;
    bool ulocs_resolved;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_set;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkRenderPass render_pass;
    VkFramebuffer framebuffer;

    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkSampler sampler;

    struct {
        PvideoState state;
        PvideoState last_uploaded_state;
        int width, height;
        VkImage image;
        VkImageView image_view;
        VmaAllocation allocation;
        VkSampler sampler;
    } pvideo;

    int width, height;
    VkFormat format;
    int draw_time;

    // OpenGL Interop
#ifdef WIN32
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_memory_obj;
    GLuint gl_texture_id;
#if HAVE_IOSURFACE_SHARING
    void *iosurface; // IOSurfaceRef

    // Frame interpolation: deferred generation across sync calls
    void *interp_prev_surface;    // IOSurfaceRef - previous frame
    void *interp_cur_surface;     // IOSurfaceRef - current frame
    int interp_remaining;         // frames left to generate this cycle
    int interp_width, interp_height;

    // Host-monotonic capture timestamps of the two input frames, fed to
    // MTLFXFrameInterpolator.deltaTime (which expects wall-clock seconds
    // between the two input frames — not a unitless frame-sequence ratio).
    uint64_t interp_prev_surface_ns;
    uint64_t interp_cur_surface_ns;

    /*
     * IOSurface rebind cache. Keyed on IOSurfaceID (kernel-assigned
     * identifier that remains stable while the surface exists, unlike
     * the pointer which can be reused across create/release). Matches
     * the cache-key pattern used in metalfx_upscale.m. Shared by the
     * CGL rebind path and the Metal present-handoff path.
     */
    uint32_t last_cgl_surface_id;
    int last_cgl_width, last_cgl_height;

    /*
     * Metal-native presentation handoff: the surface most recently
     * presented by render_display (base compositor output, MetalFX
     * upscale output, or frame-interpolation output). Holds its own
     * CFRetain; consumed (borrowed) by pgraph_vk_get_present_frame
     * under the sync handshake.
     */
    void *present_iosurface; // IOSurfaceRef, retained
    /*
     * When MetalFX produced this frame in Metal-native mode, the
     * output is a private MTLTexture (ring entry) instead of an
     * IOSurface; exactly one of present_iosurface /
     * present_mtl_texture is set. Retained (CFRetain on the ObjC
     * object).
     */
    void *present_mtl_texture; // id<MTLTexture>, retained
    int present_width, present_height;
    /*
     * GPU-side ordering for the published frame: the UI present pass
     * encodes a wait for `present_event` (id<MTLSharedEvent>,
     * borrowed) to reach `present_event_value` before sampling.
     * MetalFX-produced frames use the MetalFX present event; base
     * compositor frames use the exported compositor timeline event
     * (async submit). NULL event / value 0 = no wait required.
     */
    void *present_event;
    uint64_t present_event_value;

    /*
     * Presentation pacing: present_frame_seq increments on every
     * publish (bumped by the display_set_present_* helpers);
     * present_duration_ns is the intended on-screen hold for paced
     * interpolation steps (0 = unpaced). See NV2APresentFrame.
     */
    uint64_t present_frame_seq;
    uint64_t present_duration_ns;

    /*
     * Frame-interpolation presentation queue (Metal backend). An
     * interpolated frame lies temporally BETWEEN prev and cur, so it
     * must be shown BEFORE the new real frame; the real frame is held
     * here until the interpolated step(s) have been presented, paced
     * at frame_period / interp_mode. Presenting the real frame first
     * (the old behavior) made motion run forward-backward-forward —
     * visible as frames arriving out of order.
     */
    void *pending_real_texture; // id<MTLTexture>, retained
    uint64_t pending_real_event_value;
    uint64_t last_present_step_ns; // host time of last published step
    uint64_t interp_step_ns;       // pacing interval between steps

    /*
     * Cached midpoint frame for the current interpolation cycle.
     * MTLFXFrameInterpolator has no phase parameter, so all interp
     * steps of a 4x cycle show the same midpoint image — and the
     * interpolator's history contract requires prevColorTexture to
     * equal the previous encode's colorTexture, which repeated
     * same-pair encodes violate. Generate once per cycle, re-present
     * the cached output for the remaining steps.
     */
    void *interp_midpoint_texture; // id<MTLTexture>, retained
    uint64_t interp_midpoint_event_value;

    /*
     * Hitch guard: EMA of real-frame capture gaps. When a gap spikes
     * (load hitch / scene cut), interpolation is skipped for that
     * cycle and the interpolator history is reset, instead of
     * blending across a content jump.
     */
    uint64_t interp_avg_gap_ns;
#endif

    /*
     * Exported MTLTexture of the compositor VkImage (Metal backend,
     * texture-export mode — no IOSurface exists). Retained; consumed
     * directly by MetalFX and the UI present pass.
     */
    void *mtl_texture;

    SurfaceBinding *last_descriptor_surface;
    bool last_descriptor_pvideo;

#if HAVE_IOSURFACE_SHARING
    /*
     * Push-model present handoff (XEMU_PUSH_PRESENT). At flip the PFIFO
     * thread publishes this flip's present schedule — 1 step (interp off)
     * or `mode` steps (interp on) — into present_ring
     * (pgraph_vk_present_schedule_publish); the UI present thread consumes
     * one step per frame (pgraph_vk_present_slot_read) with no cross-thread
     * sync round trip, and paces the steps GPU-side via
     * presentDrawable:afterMinimumDuration:.
     *
     * present_slot_lock is a plain mutex, deliberately NOT a seqlock: the
     * handles are reference counted, and a lock-free reader cannot CFRetain
     * a pointer the writer may be concurrently CFReleasing
     * (retain-after-free). The lock bounds the writer's ring push and the
     * reader's consume into mutually-exclusive O(mode<=4) critical sections
     * (a few CFRetain/CFRelease + field copies, no GPU work, no nested
     * blocking wait), so it removes the round trip without reintroducing
     * one, and keeps the lock a leaf. Each ring entry owns its own CFRetain
     * on its handle (dropped when the slot is overwritten or invalidated);
     * the reader CFRetains under the lock so its handle provably survives
     * past the next publish.
     *
     * The reader hands out steps in frame_seq order, consuming the oldest
     * entry with seq > present_ring_consumed (a monotonic cursor, so a
     * consumed step is never resurrected). When the ring overflows the
     * oldest entry is dropped, so the reader can skip stale steps when it
     * falls behind — the same content the pull path drops when a new flip
     * lands before the UI reads. present_ring_generation is bumped by
     * invalidate (resize / teardown / hitch-guard reset) to fence off a
     * stale schedule.
     */
    QemuMutex present_slot_lock;
    PushPresentEntry present_ring[PUSH_PRESENT_RING_CAP];
    uint32_t present_ring_head;       // next write index (mod CAP)
    uint32_t present_ring_count;      // valid entries, <= CAP
    uint64_t present_ring_consumed;   // highest frame_seq handed to reader
    uint64_t present_ring_pub_seq;    // highest frame_seq pushed (dedup)
    uint64_t present_ring_generation; // bumped on invalidate
#endif
} PGRAPHVkDisplayState;

typedef struct ComputePipelineKey {
    VkFormat host_fmt;
    bool pack;
    int workgroup_size;
} ComputePipelineKey;

typedef struct ComputePipeline {
    LruNode node;
    ComputePipelineKey key;
    VkPipeline pipeline;
} ComputePipeline;

typedef struct PGRAPHVkComputeState {
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[8192];
    int descriptor_set_index;
    VkPipelineLayout pipeline_layout;
    Lru pipeline_cache;
    ComputePipeline *pipeline_cache_entries;

    VkPipeline unswizzle_pipeline;
    VkPipeline unswizzle_2bpp_pipeline;
    VkPipeline yuv_to_rgba_pipeline;
} PGRAPHVkComputeState;

typedef struct PGRAPHVkState {
    uint32_t vk_api_version;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;

    // TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
    bool portability_enumeration_extension_enabled;

    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;
#if HAVE_IOSURFACE_SHARING
    bool metal_objects_extension_enabled;
#endif

    // TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
    bool supports_geometry_shaders;
    bool supports_geom_point_size;

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

    VkQueue queue;
#define NUM_FLIGHT_SLOTS 2

    VkCommandPool command_pool;

    struct {
        VkCommandBuffer main_cb;
        VkCommandBuffer aux_cb;
        VkFence fence;
        VkSemaphore semaphore;
        VkFramebuffer framebuffers[50];
        int framebuffer_index;
        int descriptor_set_base;
        int descriptor_set_limit;
        int ubo_descriptor_set_base;
        int ubo_descriptor_set_limit;
        int compute_descriptor_set_base;
        int compute_descriptor_set_limit;
        VkDeviceSize staging_buffer_base;
        VkDeviceSize staging_buffer_limit;
        VkDeviceSize index_staging_base;
        VkDeviceSize index_staging_limit;
        VkDeviceSize vertex_inline_staging_base;
        VkDeviceSize vertex_inline_staging_limit;
        VkDeviceSize uniform_staging_base;
        VkDeviceSize uniform_staging_limit;
        unsigned long *uploaded_bitmap;
        /*
         * Min/max dirty page indices tracked as bits are set. Lets
         * flush_memory_buffer() and aux_has_work() skip find_first_bit/
         * find_last_bit scans over the whole VRAM page bitmap. `min` is
         * ULONG_MAX when no dirty pages.
         */
        unsigned long uploaded_first_dirty_bit;
        unsigned long uploaded_last_dirty_bit;
        /*
         * Page-relative byte span [min,max) written to each uploaded
         * page during this slot's recording window. Valid only where
         * the corresponding uploaded_bitmap bit is set (the bit
         * guards validity, so reset needs no clearing). Used by the
         * exact-conflict refinement in update_vertex_ram_buffer:
         * guest dirty bits are page-granular, so vertex-stream sync
         * writes arrive page-padded and consecutive writes
         * false-share their boundary page; comparing the incoming
         * bytes against the mirror over just this span proves most
         * such conflicts change nothing the recorded draws could
         * have read.
         */
        uint16_t *page_span_min;
        uint16_t *page_span_max;
        bool submitted;
        /* r->submit_count value of this slot's last submission; feeds
         * retired_submit_count when the slot fence is observed. */
        uint64_t submit_index;
        /*
         * Occlusion queries recorded into this slot's last submission
         * plus the guest reports awaiting their results. Drained when
         * the slot fence is reaped (slot reclaim) instead of stalling
         * the CPU on the just-submitted command buffer at finish time.
         */
        int query_count;
        QueryReportQueue report_queue;
    } flight[NUM_FLIGHT_SLOTS];
    int current_flight;

    VkCommandBuffer command_buffer;
    VkSemaphore command_buffer_semaphore;
    VkFence command_buffer_fence;
    unsigned int command_buffer_start_time;
    bool in_command_buffer;
    uint32_t submit_count;
    /*
     * Highest submit_count whose fence has been observed signaled.
     * Same-queue fences retire in submission order, so this is a
     * watermark: every submission <= it has fully completed.
     */
    uint64_t retired_submit_count;

    VkCommandBuffer aux_command_buffer;
    VkFence aux_fence;
    bool in_aux_command_buffer;

    /*
     * Dedicated command-buffer ring for the async compositor pass
     * (Metal backend). Previously the compositor shared the per-slot
     * aux CB + single aux_fence, so the next aux use (texture/surface
     * uploads, staging sync) reclaimed the compositor fence on the
     * PFIFO thread — measured 1.4-1.9 ms per flip in heavy scenes.
     * With its own 2-deep ring, an entry is only reclaimed when its
     * slot comes around again (two sync intervals later, virtually
     * always signaled) and upload paths never wait on compositor work.
     */
#define COMPOSITOR_CB_RING 2
    VkCommandBuffer compositor_cbs[COMPOSITOR_CB_RING];
    VkFence compositor_fences[COMPOSITOR_CB_RING];
    bool compositor_pending[COMPOSITOR_CB_RING];
    int compositor_cb_index;

    bool timeline_semaphore_enabled;
    /*
     * Async compositor handoff (Metal backend): a timeline VkSemaphore
     * whose backing MTLSharedEvent is exported via
     * VK_EXT_metal_objects. Each async compositor submit signals
     * ++present_timeline_value; MetalFX command buffers and the UI
     * present pass encode GPU-side waits on the exported event instead
     * of the PFIFO thread blocking in vkWaitForFences.
     */
    VkSemaphore present_timeline;
    uint64_t present_timeline_value;
    void *present_timeline_event; /* id<MTLSharedEvent>, retained */

    /*
     * Direct MTLTexture export of the compositor image (Metal
     * backend): removes the IOSurface from the present chain entirely
     * — which also removes the macOS 26 >1920px BGRA IOSurface
     * constraint from the MetalFX *input* side, so upscaling engages
     * at surface_scale=4. Probed once at display init.
     */
    bool metal_texture_export_enabled;
    /* PFN_vkExportMetalObjectsEXT, stored untyped for portability */
    void *export_metal_objects_fn;

    /*
     * Flip-time zeta snapshot (XEMU_MFX_REAL_DEPTH=2): a dedicated,
     * exportable image blitted from the bound zeta at FLIP_STALL, so
     * the MetalFX temporal scaler samples depth that matches the
     * *presented* frame rather than the next in-progress one the single
     * live guest zeta already holds by present time. Recreated on zeta
     * dims/format change; mtl_texture retained. Fields are declared
     * unconditionally (harmless pointers/handles) but only ever touched
     * under HAVE_IOSURFACE_SHARING.
     */
    VkImage zeta_snapshot_image;
    VmaAllocation zeta_snapshot_allocation;
    void *zeta_snapshot_mtl_texture; /* retained id<MTLTexture>, or NULL */
    VkImageLayout zeta_snapshot_layout;
    VkFormat zeta_snapshot_format;
    int zeta_snapshot_width, zeta_snapshot_height;
    bool zeta_snapshot_valid; /* captured content is current for this flip */

    int framebuffer_index;
    bool framebuffer_dirty;
    bool render_pass_state_dirty;

    VkRenderPass render_pass;
    GArray *render_passes; // RenderPass
    bool in_render_pass;
    bool in_draw;
    bool nop_draw;

    Lru pipeline_cache;
    VkPipelineCache vk_pipeline_cache;
    PipelineBinding *pipeline_cache_entries;
    PipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;

    /*
     * Periodic VkPipelineCache persistence. App quit does not reliably
     * reach pgraph_destroy()/finalize, so the cache is also flushed to
     * disk from the PFIFO thread at flip boundaries once new pipelines
     * have accumulated and a minimum interval has elapsed.
     */
    uint32_t pipeline_cache_unsaved;
    int64_t pipeline_cache_last_save_ns;

    /*
     * Per-command-buffer dynamic-state cache. Vulkan dynamic state is
     * scoped to the command buffer, so these are reset in
     * pgraph_vk_begin_command_buffer. Skipping a redundant vkCmdSet*
     * saves a few host instructions per draw; at ~20-50k draws/frame
     * this aggregates visibly.
     */
    bool dynstate_cache_valid;
    VkViewport cached_viewport;
    VkRect2D cached_scissor;
    float cached_blend_constants[4];
    float cached_depth_bias_constant;
    float cached_depth_bias_slope;
    float cached_line_width;

    /*
     * Cache of pg->surface_binding_dim.{width,height} scaled by
     * pg->surface_scale_factor. Used by begin_draw's viewport and
     * begin_render_pass's render area. Invalidated at each write
     * of surface_binding_dim (update_surface_part in surface.c) and
     * at pgraph_vk_reload_surface_scale_factor. Invalid state is
     * encoded by cached_scaled_binding_dim_valid == false; lazy-
     * populated on first consumer miss after invalidation.
     */
    bool cached_scaled_binding_dim_valid;
    uint32_t cached_scaled_binding_dim_w;
    uint32_t cached_scaled_binding_dim_h;

    /*
     * Per-command-buffer cache of the last vkCmdBindVertexBuffers
     * arguments. Reset in pgraph_vk_begin_command_buffer. memcmp'd
     * against the would-be arguments in bind_vertex_buffer; on a
     * match the Vulkan call is skipped.
     */
    bool last_vertex_bind_valid;
    uint32_t last_vertex_bind_count;
    VkBuffer last_vertex_bind_buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize last_vertex_bind_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    /*
     * Per-command-buffer cache of the last vkCmdBindIndexBuffer call.
     * Mirror of last_vertex_bind_*: indexed-draw bursts (glyph / sprite
     * batches, repeated meshes during material swaps) can bind the
     * same index buffer back-to-back. Reset in
     * pgraph_vk_begin_command_buffer.
     */
    bool last_index_bind_valid;
    VkBuffer last_index_bind_buffer;
    VkDeviceSize last_index_bind_offset;
    VkIndexType last_index_bind_type;

    /*
     * Per-command-buffer + per-layout cache of the last
     * vkCmdPushConstants payload. Push constants are CB-scoped AND
     * owned by a specific VkPipelineLayout; the layout handle is
     * part of the skip fingerprint, so a pipeline rebind to a
     * different layout implicitly busts the cache without needing
     * a separate invalidation hook.
     */
    bool last_push_constants_valid;
    VkPipelineLayout last_push_constants_layout;
    int last_push_constants_num_attrs;
    float last_push_constants_values[NV2A_VERTEXSHADER_ATTRIBUTES][4];

    VkDescriptorPool descriptor_pool;
    /*
     * Split descriptor sets: set 0 holds UBOs (VSH + PSH uniforms),
     * set 1 holds textures (NV2A_MAX_TEXTURES combined image samplers).
     * Each set has its own pool/array/index so a draw that only changes
     * textures doesn't force re-writing UBOs, and vice versa. The
     * unprefixed `descriptor_*` fields refer to the texture set (set 1);
     * `ubo_descriptor_*` are the UBO set (set 0) counterparts. UBOs
     * advance less often so their pool is smaller.
     */
    VkDescriptorSetLayout ubo_descriptor_set_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet ubo_descriptor_sets[2048];
    VkDescriptorSet descriptor_sets[8192];
    int ubo_descriptor_set_index;
    int descriptor_set_index;
    int last_bound_ubo_descriptor_set_index;
    int last_bound_descriptor_set_index;

    StorageBuffer storage_buffers[BUFFER_COUNT];

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    size_t bitmap_size;

    VkVertexInputAttributeDescription vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;

    VkVertexInputBindingDescription vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_binding_descriptions;
    bool vertex_state_dirty;

    /*
     * Snapshot of the last-bound vertex layout. Compared with memcmp
     * to decide whether pg->vertex_state_dirty should flip — replaces
     * a pair of fast_hash passes (attr + binding) that used to fold
     * into vertex_layout_hash. memcmp has no multiplier chain and
     * short-circuits on first mismatch; the stored snapshot is the
     * ground truth for subsequent compares.
     */
    VkVertexInputAttributeDescription prev_vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputBindingDescription prev_vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int prev_num_active_vertex_attribute_descriptions;
    int prev_num_active_vertex_binding_descriptions;
    bool vertex_layout_snapshot_valid;

    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    int invalid_surface_count;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    TextureBinding dummy_texture;
    bool texture_bindings_changed;
    VkFormatProperties *texture_format_properties;
    GThreadPool *decode_thread_pool;

    /*
     * Spatial index over VRAM byte ranges covered by active texture
     * cache entries. Each bucket holds the TextureBinding pointers
     * whose key.texture_* or key.palette_* range overlaps the
     * bucket's byte range. pgraph_vk_mark_textures_possibly_dirty
     * scans only the buckets the dirty range actually touches instead
     * of walking every active LRU entry. Lazily initialized on first
     * call (when we first know vram size); entries maintained via
     * tex_dirty_buckets_insert / _remove hooks at cache-miss /
     * eviction sites.
     */
    GPtrArray **tex_dirty_buckets;
    uint32_t tex_dirty_num_buckets;

    /*
     * High-water-mark scratch buffers for the per-upload guest-VRAM
     * snapshot (see create_texture()). Grown via g_realloc and
     * released in pgraph_vk_finalize_textures.
     */
    uint8_t *texture_snapshot_buf;
    size_t texture_snapshot_buf_capacity;
    uint8_t *palette_snapshot_buf;
    size_t palette_snapshot_buf_capacity;

    /*
     * Reusable TextureLayout scratch (~5 KiB) for get_texture_layout();
     * exactly one layout is alive at a time so a single buffer
     * replaces a g_malloc0/g_free pair per texture upload. Type is
     * private to texture.c, hence void*.
     */
    void *texture_layout_scratch;

    Lru sampler_cache;
    SamplerCacheEntry *sampler_cache_entries;
    SamplerCacheEntry *sampler_bindings[NV2A_MAX_TEXTURES];
    SamplerCacheEntry dummy_sampler;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    ShaderModuleInfo *quad_vert_module, *solid_frag_module;
    bool shader_bindings_changed;
    bool use_push_constants_for_uniform_attrs;

    /*
     * Cached hash of shader_binding->state. Refreshed when
     * shader_bindings_changed fires; combined (XOR) with the hashes of
     * the smaller PipelineKey sub-fields to form the full pipeline cache
     * hash. Avoids re-hashing the ShaderState bytes (by far the largest
     * part of PipelineKey) on every dirty-pipeline draw call when only
     * non-shader state (blend/depth/vertex desc) changed.
     */
    uint64_t cached_shader_state_hash;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    size_t uniform_buffer_offsets[2];
    bool uniforms_changed;

    VkQueryPool query_pool;
    int max_queries_in_flight;
    int num_queries_in_flight;
    bool new_query_needed;
    /* First wall-clock instant the FIFO was seen idle with reports
     * pending; 0 when not idle / none pending. Drives the deferred-
     * report fallback submit (see process_pending_reports). */
    int64_t reports_idle_since_ns;
    uint64_t *query_results_buf;
    QueryReport *report_pool;
    int report_pool_next;

    uint32_t *emulated_indices_buf;
    size_t emulated_indices_buf_size;
    bool query_in_flight;
    uint32_t zpass_pixel_count_result;
    QueryReportQueue report_queue; // Reports for the current recording

    SurfaceFormatInfo kelvin_surface_zeta_vk_map[3];

    uint32_t clear_parameter;

    PGRAPHVkDisplayState display;
    PGRAPHVkComputeState compute;

    GHashTable *surface_lookup;
    GHashTable *render_pass_lookup;

    struct {
        hwaddr start;
        hwaddr end;
        SurfaceBinding *surface;
    } *surface_ranges;
    int surface_range_count;
    int surface_range_capacity;

    int64_t last_expire_ns;
} PGRAPHVkState;

// renderer.c
void pgraph_vk_check_memory_budget(PGRAPHState *pg);

// debug.c
#define RGBA_RED     (float[4]){1,0,0,1}
#define RGBA_YELLOW  (float[4]){1,1,0,1}
#define RGBA_GREEN   (float[4]){0,1,0,1}
#define RGBA_BLUE    (float[4]){0,0,1,1}
#define RGBA_PINK    (float[4]){1,0,1,1}
#define RGBA_DEFAULT (float[4]){0,0,0,0}

void pgraph_vk_debug_init(void);
void pgraph_vk_insert_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                   float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_end_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd);

// instance.c
void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_vk_finalize_instance(PGRAPHState *pg);
QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device);
uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties);

// glsl.c
void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);
GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source);
VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r,
                                                       GByteArray *spv);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl);
void pgraph_vk_ref_shader_module(ShaderModuleInfo *info);
void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);
void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);

// buffer.c
void pgraph_vk_init_buffers(NV2AState *d);
void pgraph_vk_finalize_buffers(NV2AState *d);
bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size, size_t count,
                                    VkDeviceAddress alignment);
VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment);

// command.c
void pgraph_vk_init_command_buffers(PGRAPHState *pg);
void pgraph_vk_finalize_command_buffers(PGRAPHState *pg);
VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg);
void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd);
/*
 * Compositor CB ring (Metal backend async present chain): submit
 * without a synchronous fence wait, signaling `timeline` at `value`.
 * The ring entry's fence is reclaimed when the entry is reused, two
 * sync intervals later.
 */
VkCommandBuffer pgraph_vk_begin_compositor_commands(PGRAPHState *pg);
void pgraph_vk_end_compositor_commands_async(PGRAPHState *pg,
                                             VkCommandBuffer cmd,
                                             VkSemaphore timeline,
                                             uint64_t value);
void pgraph_vk_drain_compositor_cbs(PGRAPHState *pg);
void pgraph_vk_wait_for_previous_flight(PGRAPHState *pg);
void pgraph_vk_wait_slot_fence(PGRAPHState *pg, int slot);
void pgraph_vk_select_flight_slot(PGRAPHState *pg);
void pgraph_vk_init_flight_partitions(PGRAPHState *pg);

// image.c
void pgraph_vk_transition_image_layout(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkImage image, VkFormat format,
                                       VkImageLayout oldLayout,
                                       VkImageLayout newLayout);

// vertex.c
void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element);
void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset, void *data,
                                    VkDeviceSize size);
VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count);

// surface.c
void pgraph_vk_init_surfaces(PGRAPHState *pg);
void pgraph_vk_finalize_surfaces(PGRAPHState *pg);
/* XEMU_MFX_REAL_DEPTH: 0 = synthetic (default), 1 = live zeta read,
 * 2 = flip-time zeta snapshot. Cached env parse; safe on all platforms. */
int pgraph_vk_mfx_real_depth_mode(void);
/* Flip-time zeta snapshot producer/teardown (XEMU_MFX_REAL_DEPTH=2).
 * Capture records a depth blit into the frame's command buffer and must
 * run before the FLIP_STALL finish so it rides that submission. Both are
 * no-ops off Apple / when the knob isn't 2. */
void pgraph_vk_zeta_snapshot_capture(PGRAPHState *pg);
void pgraph_vk_zeta_snapshot_destroy(PGRAPHState *pg);
void pgraph_vk_surface_flush(NV2AState *d);
void pgraph_vk_process_pending_downloads(NV2AState *d);
void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_vk_wait_for_surface_download(SurfaceBinding *e);
void pgraph_vk_download_dirty_surfaces(NV2AState *d);
void pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start,
                                                   hwaddr size, int nsprof_ev);
void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force);
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write);
SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr);
void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d);
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg);

// surface-compute.c
void pgraph_vk_init_compute(PGRAPHState *pg);
bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r);
void pgraph_vk_compute_finish_complete(PGRAPHVkState *r);
void pgraph_vk_finalize_compute(PGRAPHState *pg);
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale);
void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst);
void pgraph_vk_dispatch_unswizzle(PGRAPHState *pg, VkCommandBuffer cmd,
                                  VkBuffer src, VkBuffer dst,
                                  unsigned int width, unsigned int height);
void pgraph_vk_dispatch_unswizzle_2bpp(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkBuffer src, VkBuffer dst,
                                       unsigned int width,
                                       unsigned int height);
void pgraph_vk_dispatch_yuv_to_rgba(PGRAPHState *pg, VkCommandBuffer cmd,
                                    VkBuffer src, VkBuffer dst,
                                    unsigned int width, unsigned int height);

// display.c
void pgraph_vk_init_display(PGRAPHState *pg);
void pgraph_vk_finalize_display(PGRAPHState *pg);
void pgraph_vk_render_display(PGRAPHState *pg);
#if HAVE_IOSURFACE_SHARING
/* Push-model present (XEMU_PUSH_PRESENT); Metal backend only. */
bool pgraph_vk_push_present_enabled(NV2AState *d);
void pgraph_vk_present_schedule_publish(PGRAPHState *pg);
bool pgraph_vk_present_slot_read(PGRAPHState *pg, struct NV2APresentFrame *frame);
bool pgraph_vk_present_slot_peek(PGRAPHState *pg, struct NV2APresentFrame *frame);
void pgraph_vk_present_slot_invalidate(PGRAPHState *pg);
#endif

// texture.c
void pgraph_vk_init_textures(PGRAPHState *pg);
void pgraph_vk_finalize_textures(PGRAPHState *pg);
void pgraph_vk_bind_textures(NV2AState *d);
void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                            hwaddr size);
void pgraph_vk_trim_texture_cache(PGRAPHState *pg);

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
void pgraph_vk_bind_shaders(PGRAPHState *pg);

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_drain_slot_reports(NV2AState *d, int slot);
void pgraph_vk_drain_all_pending_reports(NV2AState *d);

/*
 * The occlusion query pool is partitioned per flight slot so a slot's
 * queries can stay in flight (and be drained at slot reclaim) without
 * colliding with the indices used by the next recording.
 */
static inline int pgraph_vk_queries_per_slot(PGRAPHVkState *r)
{
    return r->max_queries_in_flight / NUM_FLIGHT_SLOTS;
}

static inline int pgraph_vk_slot_query_base(PGRAPHVkState *r, int slot)
{
    return slot * pgraph_vk_queries_per_slot(r);
}

typedef enum FinishReason {
    VK_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    VK_FINISH_REASON_SURFACE_CREATE,
    VK_FINISH_REASON_SURFACE_DOWN,
    VK_FINISH_REASON_NEED_BUFFER_SPACE,
    VK_FINISH_REASON_PRESENTING,
    VK_FINISH_REASON_FLIP_STALL,
    VK_FINISH_REASON_FLUSH,
    VK_FINISH_REASON_STALLED,
    VK_FINISH_REASON_REPORTS_FULL,
    /* Submit so pending occlusion queries can complete, but do NOT
     * synchronously drain: the pfifo idle loop polls the slot fence
     * (pgraph_vk_process_pending_reports) and delivers report values
     * when the GPU signals. Replaces the STALLED full-sync for the
     * guest poll path. */
    VK_FINISH_REASON_REPORTS_SUBMIT,
} FinishReason;

// draw.c
void pgraph_vk_init_pipelines(PGRAPHState *pg);
void pgraph_vk_finalize_pipelines(PGRAPHState *pg);
void pgraph_vk_maybe_save_pipeline_cache(PGRAPHState *pg);
void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_vk_draw_begin(NV2AState *d);
void pgraph_vk_draw_end(NV2AState *d);
void pgraph_vk_finish(PGRAPHState *pg, FinishReason why);
void pgraph_vk_flush_draw(NV2AState *d);
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

// gpuprops.c
void pgraph_vk_determine_gpu_properties(NV2AState *d);
GPUProperties *pgraph_vk_get_gpu_properties(void);

#endif
