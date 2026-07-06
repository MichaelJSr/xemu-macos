/*
 * MetalFX integration for xemu on Apple Silicon.
 *
 * Provides spatial upscaling, temporal upscaling (with depth), and
 * frame interpolation (macOS 26+). MetalFX offloads upscaling and
 * interpolation to the GPU/ANE, freeing cycles for emulation.
 *
 * Copyright (c) 2026
 * License: LGPL v2+
 */

#ifdef __APPLE__

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <IOSurface/IOSurface.h>
#include "metalfx_upscale.h"
#include "ui/xemu-present.h"

#import <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef NDEBUG
#define METALFX_DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define METALFX_DPRINTF(fmt, ...) do {} while (0)
#endif

static os_unfair_lock g_metalfx_lock = OS_UNFAIR_LOCK_INIT;

/*
 * Phase 5 note: MTLResidencySet (macOS 15+) was evaluated but not
 * adopted here. Each MetalFX subsystem owns only 2-4 long-lived Metal
 * resources (scaler, output IOSurface texture, optional depth/motion
 * textures) which Metal already tracks via the command buffer's
 * automatic residency path. Residency sets are most effective for
 * argument-buffer-driven rendering with hundreds of resources; the
 * NV2A Vulkan renderer is the only code path that could benefit, but
 * that runs through MoltenVK and doesn't expose Metal residency sets.
 */

/*
 * In-flight command buffer counters per subsystem.
 *
 * The encode paths release g_metalfx_lock *before* calling
 * waitUntilCompleted so other threads can make progress. That leaves a
 * window where a concurrent destroy_locked() call could release Metal
 * objects the GPU is still referencing. We avoid it by attaching a
 * completion handler that decrements a per-subsystem counter and making
 * the destroy path spin-wait for the counter to drain.
 *
 * The handler runs on Metal's private completion queue, not the encoder
 * thread, so it is safe even when the encoder is still blocked in
 * waitUntilCompleted.
 */
static _Atomic int g_spatial_inflight  = 0;
static _Atomic int g_temporal_inflight = 0;
static _Atomic int g_interp_inflight   = 0;

static void metalfx_wait_inflight(_Atomic int *counter)
{
    /* Caller must not hold g_metalfx_lock (completion handlers run off-thread). */
    int spins = 0;
    while (atomic_load_explicit(counter, memory_order_acquire) > 0) {
        if (++spins < 1000) {
            /* Tight spin for the common case (<1ms) */
        } else {
            usleep(100);
        }
    }
}

#pragma mark - Shared Metal Device

static id<MTLDevice> g_shared_device = nil;
static id<MTLCommandQueue> g_shared_queue = nil;
static int g_shared_refcount = 0;

/*
 * Async presentation event (Metal presentation backend only).
 *
 * Each MetalFX command buffer signals g_present_event with a
 * monotonically increasing value instead of blocking the PFIFO thread
 * in waitUntilCompleted. The UI present pass encodes a GPU-side wait
 * on the frame's value before sampling MetalFX output, so the 1-5 ms
 * per-frame stall documented in the README is removed without losing
 * ordering. Under the OpenGL presentation backend the synchronous
 * wait is retained (GL cannot wait on MTLSharedEvent).
 */
static id<MTLSharedEvent> g_present_event = nil;
static _Atomic uint64_t g_present_event_value = 0;

static bool metalfx_async_mode(void)
{
    return xemu_present_is_metal() && g_present_event != nil;
}

void *metalfx_present_event(void)
{
    return (void *)g_present_event;
}

uint64_t metalfx_present_event_last_value(void)
{
    return atomic_load_explicit(&g_present_event_value,
                                memory_order_acquire);
}

/*
 * Commit a MetalFX command buffer. Async mode: signal the present
 * event and return immediately. Sync mode (GL presentation): commit
 * now; the caller must call metalfx_submit_wait(cb) after dropping
 * g_metalfx_lock.
 */
static void metalfx_submit(id<MTLCommandBuffer> cb)
{
    if (metalfx_async_mode()) {
        uint64_t v = atomic_fetch_add_explicit(&g_present_event_value, 1,
                                               memory_order_acq_rel) + 1;
        [cb encodeSignalEvent:g_present_event value:v];
    }
    [cb commit];
}

static void metalfx_submit_wait(id<MTLCommandBuffer> cb)
{
    if (!metalfx_async_mode()) {
        [cb waitUntilCompleted];
    }
}

/*
 * Input-side ordering against the async compositor submit (the
 * MoltenVK compositor pass signals an exported MTLSharedEvent).
 * Encoded at the head of every MetalFX command buffer; waits on
 * already-signaled values are free.
 */
static id<MTLSharedEvent> g_input_wait_event = nil;
static uint64_t g_input_wait_value = 0;

void metalfx_set_input_wait(void *event, uint64_t value)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (g_input_wait_event != (id<MTLSharedEvent>)event) {
        [g_input_wait_event release];
        g_input_wait_event = [(id<MTLSharedEvent>)event retain];
    }
    g_input_wait_value = value;
    os_unfair_lock_unlock(&g_metalfx_lock);
}

/* Caller holds g_metalfx_lock. */
static void metalfx_encode_input_wait(id<MTLCommandBuffer> cb)
{
    if (g_input_wait_event) {
        [cb encodeWaitForEvent:g_input_wait_event value:g_input_wait_value];
    }
}

static bool shared_metal_acquire(id<MTLDevice> *out_device,
                                 id<MTLCommandQueue> *out_queue)
{
    if (!g_shared_device) {
        g_shared_device = MTLCreateSystemDefaultDevice();
        if (!g_shared_device) return false;
        g_shared_queue = [g_shared_device newCommandQueue];
        if (!g_shared_queue) {
            /* Don't leak the device if queue allocation fails. */
            [g_shared_device release];
            g_shared_device = nil;
            return false;
        }
        /* Sticky: created once per process (see shared_metal_release) */
        if (xemu_present_is_metal() && !g_present_event) {
            g_present_event = [g_shared_device newSharedEvent];
        }
    }
    g_shared_refcount++;
    *out_device = g_shared_device;
    *out_queue = g_shared_queue;
    return true;
}

static void shared_metal_release(void)
{
    if (--g_shared_refcount <= 0) {
        /*
         * This translation unit is built without ARC (verified in
         * hw/xbox/nv2a/pgraph/vk/meson.build), so assigning nil does
         * not release the previous object. Explicit release is required
         * or we leak both MTLDevice and MTLCommandQueue for the process
         * lifetime.
         *
         * g_present_event is deliberately NOT released here: the UI
         * thread holds borrowed pointers to it across frames (in
         * NV2APresentFrame and encoded GPU waits), and subsystem
         * re-initialization can momentarily drop the refcount to zero.
         * One MTLSharedEvent for the process lifetime is the safe
         * contract; its monotonic value survives device re-acquisition.
         */
        [g_shared_queue release];
        g_shared_queue = nil;
        [g_shared_device release];
        g_shared_device = nil;
        g_shared_refcount = 0;
    }
}

#pragma mark - Helpers

/*
 * macOS 26 has an IOSurface bug where surfaces wider than ~2560 pixels get
 * an internal bytesPerRow of width*2 instead of width*4 for BGRA8, causing
 * Metal texture validation failures. Cap width to avoid this.
 */
#define METALFX_MAX_IOSURFACE_WIDTH 1920

static IOSurfaceRef create_iosurface_bgra(int w, int h)
{
    if (w > METALFX_MAX_IOSURFACE_WIDTH) {
        METALFX_DPRINTF(
                "MetalFX: Skipping IOSurface %dx%d (width > %d limit)\n",
                w, h, METALFX_MAX_IOSURFACE_WIDTH);
        return NULL;
    }

    NSDictionary *props = @{
        (id)kIOSurfaceWidth: @(w),
        (id)kIOSurfaceHeight: @(h),
        (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfaceBytesPerRow: @(w * 4),
        (id)kIOSurfaceAllocSize: @((NSUInteger)w * h * 4),
        (id)kIOSurfacePixelFormat: @(0x42475241u), /* 'BGRA' */
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

static id<MTLTexture> texture_from_iosurface(id<MTLDevice> device,
                                             IOSurfaceRef surface,
                                             MTLPixelFormat fmt,
                                             int w, int h,
                                             MTLTextureUsage usage)
{
    size_t bpr = IOSurfaceGetBytesPerRow(surface);
    size_t bpe = IOSurfaceGetBytesPerElement(surface);
    if (bpe == 0) bpe = 4;
    size_t min_bpr = (size_t)w * bpe;
    if (bpr < min_bpr) {
        METALFX_DPRINTF(
                "MetalFX: IOSurface %dx%d bytesPerRow=%zu < %zu, "
                "skipping Metal texture\n", w, h, bpr, min_bpr);
        return nil;
    }

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    desc.usage = usage;
    desc.storageMode = MTLStorageModeShared;
    return [device newTextureWithDescriptor:desc iosurface:surface plane:0];
}

static id<MTLTexture> create_metal_texture(id<MTLDevice> device,
                                           MTLPixelFormat fmt,
                                           int w, int h,
                                           MTLTextureUsage usage)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    desc.usage = usage;
    desc.storageMode = MTLStorageModePrivate;
    return [device newTextureWithDescriptor:desc];
}

/*
 * Metal-native output rings. With async submission the UI may still
 * be sampling frame N while the PFIFO thread encodes frame N+1, so
 * outputs rotate through a small ring of private MTLTextures instead
 * of reusing one IOSurface-backed texture. Private textures also have
 * no IOSurface involvement, which is what lifts the macOS 26
 * 1920px-width IOSurface limitation.
 */
#define METALFX_RING_DEPTH 3

typedef struct MetalFXOutputRing {
    id<MTLTexture> tex[METALFX_RING_DEPTH];
    int next;
    int last; /* index of most recently written entry, -1 = none */
} MetalFXOutputRing;

static bool ring_init(MetalFXOutputRing *ring, id<MTLDevice> device,
                      int w, int h, MTLTextureUsage usage)
{
    for (int i = 0; i < METALFX_RING_DEPTH; i++) {
        ring->tex[i] = create_metal_texture(device, MTLPixelFormatBGRA8Unorm,
                                            w, h, usage);
        if (!ring->tex[i]) {
            for (int j = 0; j < i; j++) {
                [ring->tex[j] release];
                ring->tex[j] = nil;
            }
            return false;
        }
    }
    ring->next = 0;
    ring->last = -1;
    return true;
}

static void ring_destroy(MetalFXOutputRing *ring)
{
    for (int i = 0; i < METALFX_RING_DEPTH; i++) {
        [ring->tex[i] release];
        ring->tex[i] = nil;
    }
    ring->next = 0;
    ring->last = -1;
}

static id<MTLTexture> ring_advance(MetalFXOutputRing *ring)
{
    if (!ring->tex[0]) {
        return nil;
    }
    id<MTLTexture> t = ring->tex[ring->next];
    ring->last = ring->next;
    ring->next = (ring->next + 1) % METALFX_RING_DEPTH;
    return t;
}

static void *ring_last_retained(MetalFXOutputRing *ring)
{
    if (ring->last < 0 || !ring->tex[ring->last]) {
        return NULL;
    }
    return (void *)CFRetain(ring->tex[ring->last]);
}

void metalfx_drain_inflight(void)
{
    metalfx_wait_inflight(&g_spatial_inflight);
    metalfx_wait_inflight(&g_temporal_inflight);
    metalfx_wait_inflight(&g_interp_inflight);
}

void *metalfx_wrap_iosurface_texture(IOSurfaceRef surface)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (!g_shared_device || !surface) {
        os_unfair_lock_unlock(&g_metalfx_lock);
        return NULL;
    }
    id<MTLTexture> tex = texture_from_iosurface(
        g_shared_device, surface, MTLPixelFormatBGRA8Unorm,
        (int)IOSurfaceGetWidth(surface), (int)IOSurfaceGetHeight(surface),
        MTLTextureUsageShaderRead);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return (void *)tex; /* +1 from newTexture */
}

void metalfx_texture_dims(void *texture, int *w, int *h)
{
    id<MTLTexture> t = (id<MTLTexture>)texture;
    if (w) *w = t ? (int)t.width : 0;
    if (h) *h = t ? (int)t.height : 0;
}


static NSString *const kSyntheticDepthKernel =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void syntheticDepth(\n"
     "    texture2d<float, access::read>  color [[texture(0)]],\n"
     "    texture2d<float, access::write> depth [[texture(1)]],\n"
     "    uint2 gid [[thread_position_in_grid]])\n"
     "{\n"
     "    if (gid.x >= color.get_width() || gid.y >= color.get_height()) return;\n"
     "    float4 c = color.read(gid);\n"
     "    float lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));\n"
     "    depth.write(float4(lum, 0, 0, 0), gid);\n"
     "}\n";

#pragma mark - Spatial Upscaler

/*
 * Synchronous spatial upscale: commit and waitUntilCompleted on the
 * display thread. Simple, race-free, and trades ~1-5 ms of
 * display-thread blocking per frame for guaranteed IOSurface coherence
 * with the GL texture-bound consumer.
 *
 * An earlier pass (Phase C2) attempted double-buffered async via a
 * dispatch_semaphore + two IOSurfaces, but the cross-API sync between
 * Metal (writer) and OpenGL (reader via CGLTexImageIOSurface2D) turned
 * out to be insufficient: SDL_GL_SwapWindow + vsync flush GL commands
 * to the driver and block until vblank, but do not fence GL's
 * IOSurface-texture sampling to completion, so a subsequent Metal
 * encode to the same slot could race the previous frame's still-active
 * GL read. That manifested as ghosting/jitter even with the correct
 * double-buffer index. Left in as a documented TODO for a later pass
 * that adds an explicit cross-API fence (glWaitSync on a Metal-created
 * MTLSharedEvent, or triple-buffering with a looser timing budget).
 */
typedef struct MetalFXSpatialState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLFXSpatialScaler> scaler;
    id<MTLTexture> inputTexture;
    id<MTLTexture> outputTexture;
    IOSurfaceRef outputSurface;
    MetalFXOutputRing ring; /* Metal-native mode */
    IOSurfaceID cachedInputSurfaceID;
    int inputWidth, inputHeight;
    int outputWidth, outputHeight;
    bool initialized;
} MetalFXSpatialState;

static MetalFXSpatialState g_spatial = { 0 };

static void metalfx_destroy_locked(void)
{
    [g_spatial.scaler release];           g_spatial.scaler = nil;
    [g_spatial.inputTexture release];     g_spatial.inputTexture = nil;
    [g_spatial.outputTexture release];    g_spatial.outputTexture = nil;
    ring_destroy(&g_spatial.ring);
    g_spatial.cachedInputSurfaceID = 0;
    if (g_spatial.outputSurface) {
        CFRelease(g_spatial.outputSurface);
        g_spatial.outputSurface = NULL;
    }
    if (g_spatial.device) {
        /*
         * device/commandQueue are owned by shared_metal_acquire; we only
         * release our reference to them via shared_metal_release.
         */
        g_spatial.commandQueue = nil;
        g_spatial.device = nil;
        shared_metal_release();
    }
    g_spatial.initialized = false;
}

bool metalfx_init(int input_w, int input_h, int output_w, int output_h)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (g_spatial.initialized) {
        if (g_spatial.inputWidth == input_w &&
            g_spatial.inputHeight == input_h &&
            g_spatial.outputWidth == output_w &&
            g_spatial.outputHeight == output_h) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return true;
        }
        /* Drain outstanding GPU work before releasing resources. */
        os_unfair_lock_unlock(&g_metalfx_lock);
        metalfx_wait_inflight(&g_spatial_inflight);
        os_unfair_lock_lock(&g_metalfx_lock);
        metalfx_destroy_locked();
    }

    @autoreleasepool {
        if (!shared_metal_acquire(&g_spatial.device, &g_spatial.commandQueue)) {
            os_unfair_lock_unlock(&g_metalfx_lock); return false;
        }
        if (![MTLFXSpatialScalerDescriptor supportsDevice:g_spatial.device]) {
            shared_metal_release();
            g_spatial.device = nil;
            g_spatial.commandQueue = nil;
            os_unfair_lock_unlock(&g_metalfx_lock); return false;
        }

        MTLFXSpatialScalerDescriptor *desc =
            [[MTLFXSpatialScalerDescriptor alloc] init];
        desc.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.inputWidth = input_w;
        desc.inputHeight = input_h;
        desc.outputWidth = output_w;
        desc.outputHeight = output_h;
        desc.colorProcessingMode =
            MTLFXSpatialScalerColorProcessingModePerceptual;

        g_spatial.scaler =
            [desc newSpatialScalerWithDevice:g_spatial.device];
        [desc release];
        if (!g_spatial.scaler) {
            metalfx_destroy_locked();
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

        /*
         * Metal-native mode: ring of private textures (no IOSurface,
         * no macOS 26 width cap). GL mode: single IOSurface-backed
         * output texture for the CGL consumer.
         */
        if (xemu_present_is_metal() &&
            ring_init(&g_spatial.ring, g_spatial.device, output_w, output_h,
                      MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead)) {
            /* outputs rotate through the ring */
        } else {
            g_spatial.outputSurface = create_iosurface_bgra(output_w, output_h);
            if (!g_spatial.outputSurface) {
                metalfx_destroy_locked();
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }

            g_spatial.outputTexture = texture_from_iosurface(
                g_spatial.device, g_spatial.outputSurface,
                MTLPixelFormatBGRA8Unorm, output_w, output_h,
                MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
            if (!g_spatial.outputTexture) {
                metalfx_destroy_locked();
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }
        }

        g_spatial.inputWidth = input_w;
        g_spatial.inputHeight = input_h;
        g_spatial.outputWidth = output_w;
        g_spatial.outputHeight = output_h;
        g_spatial.initialized = true;

        METALFX_DPRINTF("MetalFX: Spatial upscaler initialized %dx%d -> %dx%d\n",
                input_w, input_h, output_w, output_h);
        os_unfair_lock_unlock(&g_metalfx_lock);
        return true;
    }
}

/* Caller must CFRelease the returned IOSurfaceRef. */
IOSurfaceRef metalfx_get_output_surface(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    IOSurfaceRef s = g_spatial.outputSurface;
    if (s) CFRetain(s);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return s;
}

/* Caller must CFRelease the returned texture handle. */
void *metalfx_get_output_texture(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    void *t = ring_last_retained(&g_spatial.ring);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return t;
}

bool metalfx_upscale(IOSurfaceRef inputSurface)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (!g_spatial.initialized || !inputSurface) {
        os_unfair_lock_unlock(&g_metalfx_lock);
        return false;
    }

    @autoreleasepool {
        IOSurfaceID inputID = IOSurfaceGetID(inputSurface);
        if (inputID != g_spatial.cachedInputSurfaceID) {
            id<MTLTexture> tex = texture_from_iosurface(
                g_spatial.device, inputSurface, MTLPixelFormatBGRA8Unorm,
                g_spatial.inputWidth, g_spatial.inputHeight,
                MTLTextureUsageShaderRead);
            if (tex) {
                [g_spatial.inputTexture release];
                g_spatial.inputTexture = tex;
                g_spatial.cachedInputSurfaceID = inputID;
            }
        }
        if (!g_spatial.inputTexture) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

        id<MTLTexture> output = ring_advance(&g_spatial.ring);
        if (!output) {
            output = g_spatial.outputTexture;
        }
        g_spatial.scaler.colorTexture = g_spatial.inputTexture;
        g_spatial.scaler.outputTexture = output;

        id<MTLCommandBuffer> cb = [g_spatial.commandQueue commandBuffer];
        metalfx_encode_input_wait(cb);
        [g_spatial.scaler encodeToCommandBuffer:cb];
        atomic_fetch_add_explicit(&g_spatial_inflight, 1, memory_order_acq_rel);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull _) {
            (void)_;
            atomic_fetch_sub_explicit(&g_spatial_inflight, 1,
                                      memory_order_acq_rel);
        }];
        metalfx_submit(cb);
        os_unfair_lock_unlock(&g_metalfx_lock);
        metalfx_submit_wait(cb);
        return true;
    }
}

/*
 * Texture-input spatial upscale: the input arrives as the exported
 * compositor MTLTexture (no IOSurface wrap or caching needed).
 */
bool metalfx_upscale_tex(void *inputTexture)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (!g_spatial.initialized || !inputTexture) {
        os_unfair_lock_unlock(&g_metalfx_lock);
        return false;
    }

    @autoreleasepool {
        id<MTLTexture> output = ring_advance(&g_spatial.ring);
        if (!output) {
            output = g_spatial.outputTexture;
        }
        g_spatial.scaler.colorTexture = (id<MTLTexture>)inputTexture;
        g_spatial.scaler.outputTexture = output;

        id<MTLCommandBuffer> cb = [g_spatial.commandQueue commandBuffer];
        metalfx_encode_input_wait(cb);
        [g_spatial.scaler encodeToCommandBuffer:cb];
        atomic_fetch_add_explicit(&g_spatial_inflight, 1, memory_order_acq_rel);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull _) {
            (void)_;
            atomic_fetch_sub_explicit(&g_spatial_inflight, 1,
                                      memory_order_acq_rel);
        }];
        metalfx_submit(cb);
        os_unfair_lock_unlock(&g_metalfx_lock);
        metalfx_submit_wait(cb);
        return true;
    }
}

void metalfx_destroy(void)
{
    /*
     * Drain any in-flight GPU work before releasing Metal objects.
     * Must be done without holding g_metalfx_lock so completion handlers
     * (which run off-thread) can decrement the counter without contention.
     */
    metalfx_wait_inflight(&g_spatial_inflight);
    os_unfair_lock_lock(&g_metalfx_lock);
    metalfx_destroy_locked();
    os_unfair_lock_unlock(&g_metalfx_lock);
}

#pragma mark - Temporal Upscaler

typedef struct MetalFXTemporalState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLFXTemporalScaler> scaler;
    id<MTLTexture> colorTexture;
    id<MTLTexture> depthTexture;
    IOSurfaceID cachedDepthSurfaceID;
    id<MTLTexture> motionTexture;
    id<MTLTexture> outputTexture;
    id<MTLTexture> outputSharedTexture;
    IOSurfaceRef outputSurface;
    MetalFXOutputRing ring; /* Metal-native mode */
    IOSurfaceID cachedColorSurfaceID;
    int inputWidth, inputHeight;
    int outputWidth, outputHeight;
    int requestedOutputW, requestedOutputH;
    bool initialized;
    bool needsReset;
    uint32_t frameIndex;
    int failedInputW, failedInputH;
    int failedOutputW, failedOutputH;
    id<MTLComputePipelineState> syntheticDepthPipeline;
    id<MTLTexture> syntheticDepthTexture;
    /* Real zeta depth mode (XEMU_MFX_REAL_DEPTH): scaler created for
     * the exported zeta texture's pixel format, standard-Z. */
    bool realDepth;
    MTLPixelFormat depthFormat;
} MetalFXTemporalState;

static MetalFXTemporalState g_temporal = { 0 };

bool metalfx_temporal_is_supported(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { cached = 0; return false; }
    cached = [MTLFXTemporalScalerDescriptor supportsDevice:device] ? 1 : 0;
    [device release];
    return cached;
}

static void metalfx_temporal_destroy_locked(void)
{
    [g_temporal.scaler release];                g_temporal.scaler = nil;
    [g_temporal.colorTexture release];          g_temporal.colorTexture = nil;
    [g_temporal.depthTexture release];          g_temporal.depthTexture = nil;
    g_temporal.cachedDepthSurfaceID = 0;
    [g_temporal.motionTexture release];         g_temporal.motionTexture = nil;
    [g_temporal.syntheticDepthPipeline release];
    g_temporal.syntheticDepthPipeline = nil;
    [g_temporal.syntheticDepthTexture release];
    g_temporal.syntheticDepthTexture = nil;
    [g_temporal.outputTexture release];         g_temporal.outputTexture = nil;
    [g_temporal.outputSharedTexture release];   g_temporal.outputSharedTexture = nil;
    ring_destroy(&g_temporal.ring);
    g_temporal.cachedColorSurfaceID = 0;
    if (g_temporal.outputSurface) {
        CFRelease(g_temporal.outputSurface);
        g_temporal.outputSurface = NULL;
    }
    if (g_temporal.device) {
        g_temporal.commandQueue = nil;
        g_temporal.device = nil;
        shared_metal_release();
    }
    g_temporal.initialized = false;
    g_temporal.failedInputW = 0;
    g_temporal.failedInputH = 0;
    g_temporal.failedOutputW = 0;
    g_temporal.failedOutputH = 0;
    g_temporal.realDepth = false;
    g_temporal.depthFormat = MTLPixelFormatR32Float;
}

bool metalfx_temporal_init(int input_w, int input_h,
                           int output_w, int output_h,
                           void *depth_format_from)
{
    int req_output_w = output_w;
    int req_output_h = output_h;

    /*
     * Real-depth mode (XEMU_MFX_REAL_DEPTH): the scaler's depth input
     * is created with the exported zeta texture's pixel format and
     * NV2A standard-Z semantics. Only honored when the zeta texture
     * matches the scaler's input dimensions.
     */
    MTLPixelFormat want_depth_format = MTLPixelFormatR32Float;
    bool want_real_depth = false;
    if (depth_format_from) {
        id<MTLTexture> dt = (id<MTLTexture>)depth_format_from;
        if ((int)dt.width == input_w && (int)dt.height == input_h) {
            want_depth_format = dt.pixelFormat;
            want_real_depth = true;
        }
    }

    os_unfair_lock_lock(&g_metalfx_lock);
    if (g_temporal.initialized) {
        if (g_temporal.inputWidth == input_w &&
            g_temporal.inputHeight == input_h &&
            g_temporal.requestedOutputW == req_output_w &&
            g_temporal.requestedOutputH == req_output_h &&
            g_temporal.realDepth == want_real_depth &&
            g_temporal.depthFormat == want_depth_format) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return true;
        }
        os_unfair_lock_unlock(&g_metalfx_lock);
        metalfx_wait_inflight(&g_temporal_inflight);
        os_unfair_lock_lock(&g_metalfx_lock);
        metalfx_temporal_destroy_locked();
    }

    if (g_temporal.failedInputW == input_w &&
        g_temporal.failedInputH == input_h &&
        g_temporal.failedOutputW == req_output_w &&
        g_temporal.failedOutputH == req_output_h) {
        os_unfair_lock_unlock(&g_metalfx_lock);
        return false;
    }

    @autoreleasepool {
        if (!shared_metal_acquire(&g_temporal.device, &g_temporal.commandQueue)) {
            os_unfair_lock_unlock(&g_metalfx_lock); return false;
        }
        if (![MTLFXTemporalScalerDescriptor supportsDevice:g_temporal.device]) {
            shared_metal_release();
            g_temporal.device = nil;
            g_temporal.commandQueue = nil;
            os_unfair_lock_unlock(&g_metalfx_lock); return false;
        }

        float max_scale = [MTLFXTemporalScalerDescriptor
            supportedInputContentMaxScaleForDevice:g_temporal.device];
        float scale_x = (float)output_w / (float)input_w;
        float scale_y = (float)output_h / (float)input_h;
        float max_req = (scale_x > scale_y) ? scale_x : scale_y;

        if (max_scale > 0 && max_req > max_scale) {
            output_w = (int)(input_w * max_scale);
            output_h = (int)(input_h * max_scale);
            METALFX_DPRINTF(
                    "MetalFX: Temporal scale %.1fx exceeds max %.1fx, "
                    "clamping output to %dx%d\n",
                    max_req, max_scale, output_w, output_h);
        }

        MTLFXTemporalScalerDescriptor *desc =
            [[MTLFXTemporalScalerDescriptor alloc] init];
        desc.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.depthTextureFormat = want_depth_format;
        desc.motionTextureFormat = MTLPixelFormatRG16Float;
        desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.inputWidth = input_w;
        desc.inputHeight = input_h;
        desc.outputWidth = output_w;
        desc.outputHeight = output_h;
        desc.autoExposureEnabled = YES;

        g_temporal.scaler =
            [desc newTemporalScalerWithDevice:g_temporal.device];
        if (!g_temporal.scaler && want_real_depth) {
            /* Depth-stencil/zeta format rejected: retry with the
             * synthetic R32Float depth path. */
            METALFX_DPRINTF(
                    "MetalFX: Temporal scaler rejected depth format %u; "
                    "falling back to synthetic depth\n",
                    (unsigned)want_depth_format);
            want_real_depth = false;
            want_depth_format = MTLPixelFormatR32Float;
            desc.depthTextureFormat = want_depth_format;
            g_temporal.scaler =
                [desc newTemporalScalerWithDevice:g_temporal.device];
        }
        [desc release];
        if (!g_temporal.scaler) {
            METALFX_DPRINTF(
                    "MetalFX: Failed to create temporal scaler %dx%d -> %dx%d\n",
                    input_w, input_h, output_w, output_h);
            metalfx_temporal_destroy_locked();
            g_temporal.failedInputW = input_w;
            g_temporal.failedInputH = input_h;
            g_temporal.failedOutputW = req_output_w;
            g_temporal.failedOutputH = req_output_h;
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }
        g_temporal.realDepth = want_real_depth;
        g_temporal.depthFormat = want_depth_format;
        if (want_real_depth) {
            METALFX_DPRINTF(
                    "MetalFX: Temporal scaler using real zeta depth "
                    "(format %u)\n", (unsigned)want_depth_format);
        }

        if (xemu_present_is_metal() &&
            ring_init(&g_temporal.ring, g_temporal.device, output_w, output_h,
                      MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
                          MTLTextureUsageRenderTarget)) {
            /* Metal-native: outputs rotate through the private ring */
        } else {
            g_temporal.outputSurface = create_iosurface_bgra(output_w, output_h);
            if (g_temporal.outputSurface) {
                g_temporal.outputTexture = texture_from_iosurface(
                    g_temporal.device, g_temporal.outputSurface,
                    MTLPixelFormatBGRA8Unorm, output_w, output_h,
                    MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
                    MTLTextureUsageRenderTarget);
                g_temporal.outputSharedTexture = nil;
            }
            if (!g_temporal.outputTexture) {
                g_temporal.outputTexture = create_metal_texture(
                    g_temporal.device, MTLPixelFormatBGRA8Unorm,
                    output_w, output_h,
                    MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
                    MTLTextureUsageRenderTarget);
                if (!g_temporal.outputTexture) {
                    metalfx_temporal_destroy_locked();
                    os_unfair_lock_unlock(&g_metalfx_lock);
                    return false;
                }
                if (g_temporal.outputSurface) {
                    g_temporal.outputSharedTexture = texture_from_iosurface(
                        g_temporal.device, g_temporal.outputSurface,
                        MTLPixelFormatBGRA8Unorm, output_w, output_h,
                        MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
                }
            }
        }

        MTLTextureDescriptor *motionDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                        width:input_w
                                       height:input_h
                                    mipmapped:NO];
        motionDesc.usage = MTLTextureUsageShaderRead;
        motionDesc.storageMode = MTLStorageModeShared;
        g_temporal.motionTexture =
            [g_temporal.device newTextureWithDescriptor:motionDesc];

        MTLRegion region = MTLRegionMake2D(0, 0, input_w, input_h);
        size_t bpr = input_w * 4;
        void *zeros = calloc(input_h, bpr);
        [g_temporal.motionTexture replaceRegion:region
                                    mipmapLevel:0
                                      withBytes:zeros
                                    bytesPerRow:bpr];
        free(zeros);

        g_temporal.inputWidth = input_w;
        g_temporal.inputHeight = input_h;
        g_temporal.outputWidth = output_w;
        g_temporal.outputHeight = output_h;
        g_temporal.requestedOutputW = req_output_w;
        g_temporal.requestedOutputH = req_output_h;
        g_temporal.initialized = true;

        g_temporal.scaler.motionVectorScaleX = 1.0f;
        g_temporal.scaler.motionVectorScaleY = 1.0f;
        g_temporal.needsReset = true;
        g_temporal.frameIndex = 0;

        NSError *err = nil;
        id<MTLLibrary> lib = [g_temporal.device
            newLibraryWithSource:kSyntheticDepthKernel options:nil error:&err];
        if (lib) {
            id<MTLFunction> fn = [lib newFunctionWithName:@"syntheticDepth"];
            if (fn) {
                g_temporal.syntheticDepthPipeline =
                    [g_temporal.device newComputePipelineStateWithFunction:fn
                                                                    error:&err];
                [fn release];
            }
            [lib release];
        }
        if (g_temporal.syntheticDepthPipeline) {
            MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                            width:input_w
                                           height:input_h
                                        mipmapped:NO];
            depthDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            depthDesc.storageMode = MTLStorageModePrivate;
            g_temporal.syntheticDepthTexture =
                [g_temporal.device newTextureWithDescriptor:depthDesc];
            METALFX_DPRINTF("MetalFX: Synthetic depth enabled for temporal\n");
        }

        METALFX_DPRINTF(
                "MetalFX: Temporal upscaler initialized %dx%d -> %dx%d\n",
                input_w, input_h, output_w, output_h);
        os_unfair_lock_unlock(&g_metalfx_lock);
        return true;
    }
}

/* Caller must CFRelease the returned IOSurfaceRef. */
IOSurfaceRef metalfx_temporal_get_output_surface(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    IOSurfaceRef s = g_temporal.outputSurface;
    if (s) CFRetain(s);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return s;
}

/* Caller must CFRelease the returned texture handle. */
void *metalfx_temporal_get_output_texture(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    void *t = ring_last_retained(&g_temporal.ring);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return t;
}

void metalfx_temporal_reset(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    g_temporal.needsReset = true;
    os_unfair_lock_unlock(&g_metalfx_lock);
}

bool metalfx_temporal_upscale(IOSurfaceRef colorSurface,
                              IOSurfaceRef depthSurface)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (!g_temporal.initialized || !colorSurface) {
        os_unfair_lock_unlock(&g_metalfx_lock);
        return false;
    }

    @autoreleasepool {
        IOSurfaceID colorID = IOSurfaceGetID(colorSurface);
        if (colorID != g_temporal.cachedColorSurfaceID) {
            id<MTLTexture> tex = texture_from_iosurface(
                g_temporal.device, colorSurface, MTLPixelFormatBGRA8Unorm,
                g_temporal.inputWidth, g_temporal.inputHeight,
                MTLTextureUsageShaderRead);
            if (tex) {
                [g_temporal.colorTexture release];
                g_temporal.colorTexture = tex;
                g_temporal.cachedColorSurfaceID = colorID;
            }
        }
        if (!g_temporal.colorTexture) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

        if (depthSurface) {
            IOSurfaceID depthID = IOSurfaceGetID(depthSurface);
            if (depthID != g_temporal.cachedDepthSurfaceID) {
                id<MTLTexture> tex = texture_from_iosurface(
                    g_temporal.device, depthSurface, MTLPixelFormatR32Float,
                    g_temporal.inputWidth, g_temporal.inputHeight,
                    MTLTextureUsageShaderRead);
                if (tex) {
                    [g_temporal.depthTexture release];
                    g_temporal.depthTexture = tex;
                    g_temporal.cachedDepthSurfaceID = depthID;
                }
            }
        } else {
            [g_temporal.depthTexture release];
            g_temporal.depthTexture = nil;
            g_temporal.cachedDepthSurfaceID = 0;
        }

        id<MTLCommandBuffer> cb = [g_temporal.commandQueue commandBuffer];
        metalfx_encode_input_wait(cb);

        if (!g_temporal.depthTexture && g_temporal.syntheticDepthPipeline &&
            g_temporal.syntheticDepthTexture && g_temporal.colorTexture) {
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:g_temporal.syntheticDepthPipeline];
            [enc setTexture:g_temporal.colorTexture atIndex:0];
            [enc setTexture:g_temporal.syntheticDepthTexture atIndex:1];
            NSUInteger tw = g_temporal.syntheticDepthPipeline.threadExecutionWidth;
            NSUInteger th = g_temporal.syntheticDepthPipeline.maxTotalThreadsPerThreadgroup / tw;
            MTLSize tgSize = MTLSizeMake(tw, th, 1);
            MTLSize gridSize = MTLSizeMake(g_temporal.inputWidth,
                                           g_temporal.inputHeight, 1);
            [enc dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
            [enc endEncoding];
        }

        id<MTLTexture> output = ring_advance(&g_temporal.ring);
        if (!output) {
            output = g_temporal.outputTexture;
        }
        g_temporal.scaler.colorTexture = g_temporal.colorTexture;
        g_temporal.scaler.outputTexture = output;
        g_temporal.scaler.motionTexture = g_temporal.motionTexture;
        if (g_temporal.depthTexture) {
            g_temporal.scaler.depthTexture = g_temporal.depthTexture;
        } else if (g_temporal.syntheticDepthTexture) {
            g_temporal.scaler.depthTexture = g_temporal.syntheticDepthTexture;
        }
        g_temporal.scaler.inputContentWidth = g_temporal.inputWidth;
        g_temporal.scaler.inputContentHeight = g_temporal.inputHeight;

        static const float halton_x[] = { 0.0f, -0.25f, 0.25f, -0.375f, 0.125f, -0.125f, 0.375f, -0.4375f };
        static const float halton_y[] = { 0.0f, -0.333f, 0.333f, -0.111f, 0.222f, -0.222f, 0.111f, -0.444f };
        int jidx = g_temporal.frameIndex % 8;
        g_temporal.scaler.jitterOffsetX = halton_x[jidx];
        g_temporal.scaler.jitterOffsetY = halton_y[jidx];
        g_temporal.frameIndex++;

        g_temporal.scaler.reset = g_temporal.needsReset;
        g_temporal.needsReset = false;

        [g_temporal.scaler encodeToCommandBuffer:cb];

        if (g_temporal.outputSharedTexture) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:g_temporal.outputTexture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(g_temporal.outputWidth,
                                              g_temporal.outputHeight, 1)
                        toTexture:g_temporal.outputSharedTexture
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        }

        atomic_fetch_add_explicit(&g_temporal_inflight, 1, memory_order_acq_rel);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull _) {
            (void)_;
            atomic_fetch_sub_explicit(&g_temporal_inflight, 1,
                                      memory_order_acq_rel);
        }];
        metalfx_submit(cb);
        os_unfair_lock_unlock(&g_metalfx_lock);
        metalfx_submit_wait(cb);
        return true;
    }
}

/*
 * Texture-input temporal upscale: the input arrives as the exported
 * compositor MTLTexture (no IOSurface wrap or caching), with the
 * synthetic-depth kernel fed from the same texture.
 */
bool metalfx_temporal_upscale_tex(void *inputTexture, void *depthTexture)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    if (!g_temporal.initialized || !inputTexture) {
        os_unfair_lock_unlock(&g_metalfx_lock);
        return false;
    }

    @autoreleasepool {
        id<MTLTexture> color = (id<MTLTexture>)inputTexture;

        /* Real zeta depth: only when the scaler was created for this
         * format and the dims match (resize transients fall back). */
        id<MTLTexture> real_depth = nil;
        if (g_temporal.realDepth && depthTexture) {
            id<MTLTexture> dt = (id<MTLTexture>)depthTexture;
            if (dt.pixelFormat == g_temporal.depthFormat &&
                (int)dt.width == g_temporal.inputWidth &&
                (int)dt.height == g_temporal.inputHeight) {
                real_depth = dt;
            }
        }

        id<MTLCommandBuffer> cb = [g_temporal.commandQueue commandBuffer];
        metalfx_encode_input_wait(cb);

        if (!real_depth && g_temporal.syntheticDepthPipeline &&
            g_temporal.syntheticDepthTexture) {
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:g_temporal.syntheticDepthPipeline];
            [enc setTexture:color atIndex:0];
            [enc setTexture:g_temporal.syntheticDepthTexture atIndex:1];
            NSUInteger tw =
                g_temporal.syntheticDepthPipeline.threadExecutionWidth;
            NSUInteger th =
                g_temporal.syntheticDepthPipeline.maxTotalThreadsPerThreadgroup /
                tw;
            MTLSize tgSize = MTLSizeMake(tw, th, 1);
            MTLSize gridSize = MTLSizeMake(g_temporal.inputWidth,
                                           g_temporal.inputHeight, 1);
            [enc dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
            [enc endEncoding];
        }

        id<MTLTexture> output = ring_advance(&g_temporal.ring);
        if (!output) {
            output = g_temporal.outputTexture;
        }
        g_temporal.scaler.colorTexture = color;
        g_temporal.scaler.outputTexture = output;
        g_temporal.scaler.motionTexture = g_temporal.motionTexture;
        if (real_depth) {
            g_temporal.scaler.depthTexture = real_depth;
            /* NV2A renders standard Z (0 = near) */
            g_temporal.scaler.depthReversed = NO;
        } else if (g_temporal.syntheticDepthTexture) {
            g_temporal.scaler.depthTexture = g_temporal.syntheticDepthTexture;
            /* Luminance proxy: treat brighter as nearer (reversed) */
            g_temporal.scaler.depthReversed = YES;
        }
        g_temporal.scaler.inputContentWidth = g_temporal.inputWidth;
        g_temporal.scaler.inputContentHeight = g_temporal.inputHeight;

        static const float halton_x[] = { 0.0f, -0.25f, 0.25f, -0.375f, 0.125f, -0.125f, 0.375f, -0.4375f };
        static const float halton_y[] = { 0.0f, -0.333f, 0.333f, -0.111f, 0.222f, -0.222f, 0.111f, -0.444f };
        int jidx = g_temporal.frameIndex % 8;
        g_temporal.scaler.jitterOffsetX = halton_x[jidx];
        g_temporal.scaler.jitterOffsetY = halton_y[jidx];
        g_temporal.frameIndex++;

        g_temporal.scaler.reset = g_temporal.needsReset;
        g_temporal.needsReset = false;

        [g_temporal.scaler encodeToCommandBuffer:cb];

        if (g_temporal.outputSharedTexture && output == g_temporal.outputTexture) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:g_temporal.outputTexture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(g_temporal.outputWidth,
                                              g_temporal.outputHeight, 1)
                        toTexture:g_temporal.outputSharedTexture
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        }

        atomic_fetch_add_explicit(&g_temporal_inflight, 1, memory_order_acq_rel);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull _) {
            (void)_;
            atomic_fetch_sub_explicit(&g_temporal_inflight, 1,
                                      memory_order_acq_rel);
        }];
        metalfx_submit(cb);
        os_unfair_lock_unlock(&g_metalfx_lock);
        metalfx_submit_wait(cb);
        return true;
    }
}

void metalfx_temporal_destroy(void)
{
    metalfx_wait_inflight(&g_temporal_inflight);
    os_unfair_lock_lock(&g_metalfx_lock);
    metalfx_temporal_destroy_locked();
    os_unfair_lock_unlock(&g_metalfx_lock);
}

#pragma mark - Frame Interpolation (macOS 26+)

/*
 * MTLFXFrameInterpolator and its descriptor exist only in the
 * macOS 26 SDK. Runtime @available checks are not enough — the
 * type names must exist at compile time — so the whole
 * interpolation implementation is compiled out on older SDKs
 * (e.g. CI runners) and replaced by inert stubs below. Frame
 * interpolation then simply reports unsupported at runtime,
 * matching pre-macOS-26 hosts.
 */
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && \
    __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000

typedef struct MetalFXInterpolationState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id interpolator; /* id<MTLFXFrameInterpolator>, stored untyped for deployment target compat */
    id<MTLTexture> outputTexture;
    id<MTLTexture> outputSharedTexture;
    IOSurfaceRef outputSurface;
    MetalFXOutputRing ring; /* Metal-native mode */
    id<MTLTexture> cachedColorCur;
    id<MTLTexture> cachedColorPrev;
    id<MTLTexture> cachedDepthCur;   /* bound if caller provides depth */
    /*
     * Legacy zero-filled motion texture, created and bound only when
     * XEMU_MFX_INTERP_ZERO_MOTION is set. A zeroed motion texture
     * asserts "no pixel moved between the two frames", which makes
     * the interpolator ghost-blend moving objects between their two
     * positions (character flicker/blur against the background). The
     * motionTexture property is nullable: leaving it nil lets MetalFX
     * fall back to internal motion estimation, which handles game
     * content far better.
     */
    id<MTLTexture> motionTexture;
    /*
     * Synthetic depth for the interpolator (luminance proxy, same
     * kernel as the temporal scaler but at interpolation resolution).
     * Gives the interpolator object/background separation cues when
     * no real depth exists and no temporal scaler is linked.
     */
    id<MTLComputePipelineState> synthDepthPipeline;
    id<MTLTexture> synthDepthTexture;
    IOSurfaceID lastColorCurID;
    IOSurfaceID lastColorPrevID;
    IOSurfaceID lastDepthCurID;
    /*
     * MTLFXFrameInterpolator (macOS 26) takes color cur/prev but only
     * current-frame depth — no prev-depth binding. If a future SDK adds
     * it, reintroduce cachedDepthPrev + lastDepthPrevID here.
     */
    int width, height;
    bool initialized;
    bool firstFrame;
    /*
     * When the producer is the MetalFX temporal scaler, the
     * interpolator is linked to it through the descriptor's `scaler`
     * property (MTLFXTemporalScaler conforms to
     * MTLFXFrameInterpolatableScaler) so it inherits the scaler's
     * real motion/depth/history state — Apple's highest-quality
     * interpolation path. Tracked so a mode change recreates the
     * interpolator.
     */
    bool linked_to_temporal;
} MetalFXInterpolationState;

static MetalFXInterpolationState g_interp = { 0 };

static bool interp_zero_motion_requested(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("XEMU_MFX_INTERP_ZERO_MOTION");
        cached = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

bool metalfx_interpolation_is_supported(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    if (@available(macOS 26.0, *)) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { cached = 0; return false; }
        cached = [MTLFXFrameInterpolatorDescriptor supportsDevice:device] ? 1 : 0;
        [device release];
        return cached;
    }
    cached = 0;
    return false;
}

static void metalfx_interpolation_destroy_locked(void)
{
    [g_interp.interpolator release];           g_interp.interpolator = nil;
    [g_interp.outputTexture release];          g_interp.outputTexture = nil;
    [g_interp.outputSharedTexture release];    g_interp.outputSharedTexture = nil;
    ring_destroy(&g_interp.ring);
    [g_interp.motionTexture release];          g_interp.motionTexture = nil;
    [g_interp.synthDepthPipeline release];     g_interp.synthDepthPipeline = nil;
    [g_interp.synthDepthTexture release];      g_interp.synthDepthTexture = nil;
    [g_interp.cachedColorCur release];         g_interp.cachedColorCur = nil;
    [g_interp.cachedColorPrev release];        g_interp.cachedColorPrev = nil;
    [g_interp.cachedDepthCur release];         g_interp.cachedDepthCur = nil;
    g_interp.lastColorCurID = 0;
    g_interp.lastColorPrevID = 0;
    g_interp.lastDepthCurID = 0;
    g_interp.linked_to_temporal = false;
    if (g_interp.outputSurface) {
        CFRelease(g_interp.outputSurface);
        g_interp.outputSurface = NULL;
    }
    if (g_interp.device) {
        g_interp.commandQueue = nil;
        g_interp.device = nil;
        shared_metal_release();
    }
    g_interp.initialized = false;
}

bool metalfx_interpolation_init(int width, int height,
                                bool link_temporal_scaler)
{
    if (@available(macOS 26.0, *)) {
        os_unfair_lock_lock(&g_metalfx_lock);

        /* Only link when the temporal scaler actually produces frames
         * at the interpolator's input size. */
        bool can_link = link_temporal_scaler && g_temporal.initialized &&
                        g_temporal.scaler != nil &&
                        g_temporal.outputWidth == width &&
                        g_temporal.outputHeight == height;

        if (g_interp.initialized) {
            if (g_interp.width == width && g_interp.height == height &&
                g_interp.linked_to_temporal == can_link) {
                os_unfair_lock_unlock(&g_metalfx_lock);
                return true;
            }
            os_unfair_lock_unlock(&g_metalfx_lock);
            metalfx_wait_inflight(&g_interp_inflight);
            os_unfair_lock_lock(&g_metalfx_lock);
            metalfx_interpolation_destroy_locked();
        }

        @autoreleasepool {
            if (!shared_metal_acquire(&g_interp.device, &g_interp.commandQueue)) {
                os_unfair_lock_unlock(&g_metalfx_lock); return false;
            }

            MTLFXFrameInterpolatorDescriptor *desc =
                [[MTLFXFrameInterpolatorDescriptor alloc] init];
            desc.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
            desc.depthTextureFormat = MTLPixelFormatR32Float;
            desc.motionTextureFormat = MTLPixelFormatRG16Float;
            desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
            desc.inputWidth = width;
            desc.inputHeight = height;
            desc.outputWidth = width;
            desc.outputHeight = height;
            if (can_link) {
                /*
                 * MTLFXTemporalScaler conforms to
                 * MTLFXFrameInterpolatableScaler: linking hands the
                 * interpolator the scaler's real motion/depth/history
                 * state instead of relying on optical-flow estimation.
                 */
                desc.scaler =
                    (id<MTLFXFrameInterpolatableScaler>)g_temporal.scaler;
            }

            id<MTLFXFrameInterpolator> interp =
                [desc newFrameInterpolatorWithDevice:g_interp.device];
            if (!interp && can_link) {
                /* Linked creation unsupported on this OS/device: retry
                 * standalone. */
                METALFX_DPRINTF(
                        "MetalFX: Linked frame interpolator creation "
                        "failed; retrying standalone\n");
                can_link = false;
                desc.scaler = nil;
                interp = [desc newFrameInterpolatorWithDevice:g_interp.device];
            }
            [desc release];
            if (!interp) {
                METALFX_DPRINTF(
                        "MetalFX: Failed to create frame interpolator\n");
                metalfx_interpolation_destroy_locked();
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }
            g_interp.interpolator = interp;
            g_interp.linked_to_temporal = can_link;

            if (xemu_present_is_metal() &&
                ring_init(&g_interp.ring, g_interp.device, width, height,
                          MTLTextureUsageShaderWrite |
                              MTLTextureUsageShaderRead |
                              MTLTextureUsageRenderTarget)) {
                /* Metal-native: outputs rotate through the private ring */
            } else {
                g_interp.outputSurface = create_iosurface_bgra(width, height);
                if (g_interp.outputSurface) {
                    g_interp.outputTexture = texture_from_iosurface(
                        g_interp.device, g_interp.outputSurface,
                        MTLPixelFormatBGRA8Unorm, width, height,
                        MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
                        MTLTextureUsageRenderTarget);
                    g_interp.outputSharedTexture = nil;
                }
                if (!g_interp.outputTexture) {
                    g_interp.outputTexture = create_metal_texture(
                        g_interp.device, MTLPixelFormatBGRA8Unorm,
                        width, height,
                        MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
                        MTLTextureUsageRenderTarget);
                    if (!g_interp.outputTexture) {
                        metalfx_interpolation_destroy_locked();
                        os_unfair_lock_unlock(&g_metalfx_lock);
                        return false;
                    }
                    if (g_interp.outputSurface) {
                        g_interp.outputSharedTexture = texture_from_iosurface(
                            g_interp.device, g_interp.outputSurface,
                            MTLPixelFormatBGRA8Unorm, width, height,
                            MTLTextureUsageShaderWrite |
                                MTLTextureUsageShaderRead);
                    }
                }
            }

            /*
             * Motion texture: only when explicitly requested via
             * XEMU_MFX_INTERP_ZERO_MOTION (legacy/debug). The default
             * is nil — internal motion estimation (see state struct
             * comment).
             */
            if (interp_zero_motion_requested()) {
                MTLTextureDescriptor *motionDesc = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                width:width
                                               height:height
                                            mipmapped:NO];
                motionDesc.usage = MTLTextureUsageShaderRead;
                motionDesc.storageMode = MTLStorageModeShared;
                g_interp.motionTexture =
                    [g_interp.device newTextureWithDescriptor:motionDesc];
                MTLRegion region = MTLRegionMake2D(0, 0, width, height);
                size_t bpr = width * 4;
                void *zeros = calloc(height, bpr);
                [g_interp.motionTexture replaceRegion:region
                                          mipmapLevel:0
                                            withBytes:zeros
                                          bytesPerRow:bpr];
                free(zeros);
            }

            /*
             * Synthetic luminance depth at interpolation resolution
             * (standalone interpolator only; a linked temporal scaler
             * supplies its own internal data).
             */
            if (!can_link) {
                NSError *err = nil;
                id<MTLLibrary> lib = [g_interp.device
                    newLibraryWithSource:kSyntheticDepthKernel
                                 options:nil
                                   error:&err];
                if (lib) {
                    id<MTLFunction> fn =
                        [lib newFunctionWithName:@"syntheticDepth"];
                    if (fn) {
                        g_interp.synthDepthPipeline = [g_interp.device
                            newComputePipelineStateWithFunction:fn
                                                          error:&err];
                        [fn release];
                    }
                    [lib release];
                }
                if (g_interp.synthDepthPipeline) {
                    MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                    width:width
                                                   height:height
                                                mipmapped:NO];
                    depthDesc.usage = MTLTextureUsageShaderRead |
                                      MTLTextureUsageShaderWrite;
                    depthDesc.storageMode = MTLStorageModePrivate;
                    g_interp.synthDepthTexture =
                        [g_interp.device newTextureWithDescriptor:depthDesc];
                }
            }

            g_interp.width = width;
            g_interp.height = height;
            g_interp.initialized = true;
            g_interp.firstFrame = true;

            METALFX_DPRINTF(
                    "MetalFX: Frame interpolator initialized %dx%d%s\n",
                    width, height,
                    can_link ? " (linked to temporal scaler)" : "");
            os_unfair_lock_unlock(&g_metalfx_lock);
            return true;
        }
    }
    (void)width; (void)height; (void)link_temporal_scaler;
    return false;
}

/* Invalidate interpolator history (scene cut / frame-time hitch). */
void metalfx_interpolation_reset(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    g_interp.firstFrame = true;
    os_unfair_lock_unlock(&g_metalfx_lock);
}

/* Caller must CFRelease the returned IOSurfaceRef. */
IOSurfaceRef metalfx_interpolation_get_output_surface(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    IOSurfaceRef s = g_interp.outputSurface;
    if (s) CFRetain(s);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return s;
}

/* Caller must CFRelease the returned texture handle. */
void *metalfx_interpolation_get_output_texture(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    void *t = ring_last_retained(&g_interp.ring);
    os_unfair_lock_unlock(&g_metalfx_lock);
    return t;
}

/*
 * Common interpolator configuration (camera parameters, motion mode,
 * pacing, history). Caller holds g_metalfx_lock.
 */
API_AVAILABLE(macos(26.0))
static void interp_configure_common(id<MTLFXFrameInterpolator> interp,
                                    float delta_time)
{
    /* nil by default: MetalFX internal motion estimation. The zeroed
     * texture (legacy/debug env) asserts "nothing moved" and ghosts
     * moving objects. */
    interp.motionTexture = g_interp.motionTexture;
    interp.motionVectorScaleX = 1.0f;
    interp.motionVectorScaleY = 1.0f;

    if (delta_time < 0.001f || delta_time > 0.1f) {
        delta_time = 1.0f / 60.0f;
    }
    interp.deltaTime = delta_time;

    /* Plausible camera parameters for the reprojection math; the
     * previous code left fieldOfView/aspectRatio at defaults. Xbox
     * titles overwhelmingly use a perspective projection in this
     * range, and approximate values beat uninitialized ones. */
    interp.nearPlane = 0.01f;
    interp.farPlane = 10000.0f;
    interp.fieldOfView = 60.0f;
    interp.aspectRatio = (float)g_interp.width / (float)g_interp.height;
    /* Synthetic luminance depth: treat brighter as nearer (reversed-Z
     * semantics, matching the property default — set explicitly). */
    interp.depthReversed = YES;

    interp.shouldResetHistory = g_interp.firstFrame;
    g_interp.firstFrame = false;
}

/*
 * Encode the synthetic-depth pass (luminance proxy of the current
 * color frame) ahead of the interpolator in the same command buffer.
 * Returns the depth texture to bind, or nil. Caller holds the lock.
 */
static id<MTLTexture> interp_encode_synth_depth(id<MTLCommandBuffer> cb,
                                                id<MTLTexture> color)
{
    if (!g_interp.synthDepthPipeline || !g_interp.synthDepthTexture ||
        !color) {
        return nil;
    }

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:g_interp.synthDepthPipeline];
    [enc setTexture:color atIndex:0];
    [enc setTexture:g_interp.synthDepthTexture atIndex:1];
    NSUInteger tw = g_interp.synthDepthPipeline.threadExecutionWidth;
    NSUInteger th =
        g_interp.synthDepthPipeline.maxTotalThreadsPerThreadgroup / tw;
    MTLSize tgSize = MTLSizeMake(tw, th, 1);
    MTLSize gridSize = MTLSizeMake(g_interp.width, g_interp.height, 1);
    [enc dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
    [enc endEncoding];
    return g_interp.synthDepthTexture;
}

bool metalfx_interpolation_generate(IOSurfaceRef colorA,
                                    IOSurfaceRef colorB,
                                    IOSurfaceRef depthA,
                                    IOSurfaceRef depthB,
                                    float delta_time)
{
    if (@available(macOS 26.0, *)) {
        os_unfair_lock_lock(&g_metalfx_lock);
        if (!g_interp.initialized || !colorA || !colorB) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

        id<MTLFXFrameInterpolator> interp =
            (id<MTLFXFrameInterpolator>)g_interp.interpolator;

        @autoreleasepool {
            MTLTextureUsage readUsage = MTLTextureUsageShaderRead;

            /* colorB = current frame, colorA = previous frame */
            IOSurfaceID colorBID = IOSurfaceGetID(colorB);
            if (colorBID != g_interp.lastColorCurID) {
                id<MTLTexture> tex = texture_from_iosurface(
                    g_interp.device, colorB, MTLPixelFormatBGRA8Unorm,
                    g_interp.width, g_interp.height, readUsage);
                if (tex) {
                    [g_interp.cachedColorCur release];
                    g_interp.cachedColorCur = tex;
                    g_interp.lastColorCurID = colorBID;
                }
            }
            IOSurfaceID colorAID = IOSurfaceGetID(colorA);
            if (colorAID != g_interp.lastColorPrevID) {
                id<MTLTexture> tex = texture_from_iosurface(
                    g_interp.device, colorA, MTLPixelFormatBGRA8Unorm,
                    g_interp.width, g_interp.height, readUsage);
                if (tex) {
                    [g_interp.cachedColorPrev release];
                    g_interp.cachedColorPrev = tex;
                    g_interp.lastColorPrevID = colorAID;
                }
            }
            if (!g_interp.cachedColorCur || !g_interp.cachedColorPrev) {
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }

            /*
             * Only current-frame depth is bindable on MTLFXFrameInterpolator
             * in macOS 26 (depthA / previous-depth is kept as an unused
             * forward-compat parameter; see state struct comment).
             */
            if (depthB) {
                IOSurfaceID depthBID = IOSurfaceGetID(depthB);
                if (depthBID != g_interp.lastDepthCurID) {
                    id<MTLTexture> tex = texture_from_iosurface(
                        g_interp.device, depthB, MTLPixelFormatR32Float,
                        g_interp.width, g_interp.height, readUsage);
                    if (tex) {
                        [g_interp.cachedDepthCur release];
                        g_interp.cachedDepthCur = tex;
                        g_interp.lastDepthCurID = depthBID;
                    }
                }
            } else if (g_interp.cachedDepthCur) {
                [g_interp.cachedDepthCur release];
                g_interp.cachedDepthCur = nil;
                g_interp.lastDepthCurID = 0;
            }
            (void)depthA;  /* reserved for prev-depth when Apple adds it */

            id<MTLTexture> output = ring_advance(&g_interp.ring);
            if (!output) {
                output = g_interp.outputTexture;
            }
            interp.colorTexture = g_interp.cachedColorCur;
            interp.prevColorTexture = g_interp.cachedColorPrev;
            interp.outputTexture = output;
            interp_configure_common(interp, delta_time);

            id<MTLCommandBuffer> cb = [g_interp.commandQueue commandBuffer];
            metalfx_encode_input_wait(cb);
            id<MTLTexture> depth = g_interp.cachedDepthCur;
            if (!depth && !g_interp.linked_to_temporal) {
                depth = interp_encode_synth_depth(cb, g_interp.cachedColorCur);
            }
            interp.depthTexture = depth; /* nil is valid */
            [interp encodeToCommandBuffer:cb];

            if (g_interp.outputSharedTexture) {
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                [blit copyFromTexture:g_interp.outputTexture
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(g_interp.width,
                                                  g_interp.height, 1)
                            toTexture:g_interp.outputSharedTexture
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
            }

            atomic_fetch_add_explicit(&g_interp_inflight, 1, memory_order_acq_rel);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull _) {
                (void)_;
                atomic_fetch_sub_explicit(&g_interp_inflight, 1,
                                          memory_order_acq_rel);
            }];
            metalfx_submit(cb);
            os_unfair_lock_unlock(&g_metalfx_lock);
            metalfx_submit_wait(cb);
            return true;
        }
    }
    (void)colorA; (void)colorB; (void)depthA; (void)depthB;
    return false;
}

/*
 * Metal-native variant: prev/cur arrive as retained id<MTLTexture>
 * handles (MetalFX ring outputs or wrapped base IOSurfaces) rather
 * than IOSurfaces, so no per-call wrap/caching is needed.
 */
bool metalfx_interpolation_generate_tex(void *prevTexture,
                                        void *curTexture,
                                        float delta_time)
{
    if (@available(macOS 26.0, *)) {
        os_unfair_lock_lock(&g_metalfx_lock);
        if (!g_interp.initialized || !prevTexture || !curTexture) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

        id<MTLFXFrameInterpolator> interp =
            (id<MTLFXFrameInterpolator>)g_interp.interpolator;

        @autoreleasepool {
            id<MTLTexture> output = ring_advance(&g_interp.ring);
            if (!output) {
                output = g_interp.outputTexture;
            }
            if (!output) {
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }

            interp.colorTexture = (id<MTLTexture>)curTexture;
            interp.prevColorTexture = (id<MTLTexture>)prevTexture;
            interp.outputTexture = output;
            interp_configure_common(interp, delta_time);

            id<MTLCommandBuffer> cb = [g_interp.commandQueue commandBuffer];
            metalfx_encode_input_wait(cb);
            id<MTLTexture> depth = nil;
            if (!g_interp.linked_to_temporal) {
                depth = interp_encode_synth_depth(
                    cb, (id<MTLTexture>)curTexture);
            }
            interp.depthTexture = depth; /* nil is valid */
            [interp encodeToCommandBuffer:cb];

            atomic_fetch_add_explicit(&g_interp_inflight, 1,
                                      memory_order_acq_rel);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull _) {
                (void)_;
                atomic_fetch_sub_explicit(&g_interp_inflight, 1,
                                          memory_order_acq_rel);
            }];
            metalfx_submit(cb);
            os_unfair_lock_unlock(&g_metalfx_lock);
            metalfx_submit_wait(cb);
            return true;
        }
    }
    (void)prevTexture; (void)curTexture; (void)delta_time;
    return false;
}

void metalfx_interpolation_destroy(void)
{
    metalfx_wait_inflight(&g_interp_inflight);
    os_unfair_lock_lock(&g_metalfx_lock);
    metalfx_interpolation_destroy_locked();
    os_unfair_lock_unlock(&g_metalfx_lock);
}

#else /* __MAC_OS_X_VERSION_MAX_ALLOWED < 260000 */

bool metalfx_interpolation_is_supported(void)
{
    return false;
}

bool metalfx_interpolation_init(int width, int height,
                                bool link_temporal_scaler)
{
    (void)width;
    (void)height;
    (void)link_temporal_scaler;
    return false;
}

void metalfx_interpolation_reset(void)
{
}

IOSurfaceRef metalfx_interpolation_get_output_surface(void)
{
    return NULL;
}

void *metalfx_interpolation_get_output_texture(void)
{
    return NULL;
}

bool metalfx_interpolation_generate(IOSurfaceRef colorA, IOSurfaceRef colorB,
                                    IOSurfaceRef depthA, IOSurfaceRef depthB,
                                    float delta_time)
{
    (void)colorA;
    (void)colorB;
    (void)depthA;
    (void)depthB;
    (void)delta_time;
    return false;
}

bool metalfx_interpolation_generate_tex(void *prevTexture, void *curTexture,
                                        float delta_time)
{
    (void)prevTexture;
    (void)curTexture;
    (void)delta_time;
    return false;
}

void metalfx_interpolation_destroy(void)
{
}

#endif /* __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000 */

#endif /* __APPLE__ */
