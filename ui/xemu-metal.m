/*
 * xemu Metal-Native Presenter (implementation)
 *
 * Manual retain/release (no ARC), matching the conventions of
 * hw/xbox/nv2a/pgraph/vk/metalfx_upscale.m which shares the build's
 * Objective-C configuration.
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
#ifdef __APPLE__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h>
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>

#include "xemu-metal.h"
#include "hw/xbox/nv2a/nsprof.h"

static SDL_Window *g_window;
static SDL_MetalView g_metal_view;
static CAMetalLayer *g_layer;
static id<MTLDevice> g_device;
static id<MTLCommandQueue> g_queue;
static char g_device_name[256];

/* Per-frame state, valid between begin_frame / end_frame. */
static id<CAMetalDrawable> g_drawable;
static id<MTLCommandBuffer> g_cmdbuf;
static id<MTLRenderCommandEncoder> g_encoder;
static MTLRenderPassDescriptor *g_pass_desc;
static int g_drawable_w, g_drawable_h;

/*
 * Frame-spanning autorelease pool. The render loop runs on the main
 * thread *outside* any Cocoa run-loop pool, so without this every
 * autoreleased object created during a frame (command buffers,
 * encoders, pass descriptors, ImGui backend internals) accumulates
 * for the lifetime of the process — unbounded growth and allocator
 * churn. Pushed in begin_frame, popped in end_frame.
 */
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
static void *g_frame_pool;

/* IOSurface wrap cache (single entry; the present surface changes
 * rarely — same keying discipline as the CGL rebind cache). */
static id<MTLTexture> g_iosurface_tex;
static uint32_t g_iosurface_id;
static int g_iosurface_w, g_iosurface_h;

/* VGA fallback upload texture */
static id<MTLTexture> g_vga_tex;

/* Layer pixel size snapshot, readable from any thread (the PFIFO
 * thread uses it to pick MetalFX output resolution). */
static _Atomic int g_layer_pw, g_layer_ph;

static void update_drawable_size(void);

bool xemu_metal_init(SDL_Window *window)
{
    g_window = window;

    g_metal_view = SDL_Metal_CreateView(window);
    if (!g_metal_view) {
        fprintf(stderr, "xemu-metal: SDL_Metal_CreateView failed: %s\n",
                SDL_GetError());
        return false;
    }

    g_layer = (CAMetalLayer *)SDL_Metal_GetLayer(g_metal_view);
    if (!g_layer) {
        fprintf(stderr, "xemu-metal: SDL_Metal_GetLayer failed\n");
        return false;
    }

    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) {
        fprintf(stderr, "xemu-metal: no Metal device available\n");
        return false;
    }
    g_queue = [g_device newCommandQueue];
    if (!g_queue) {
        fprintf(stderr, "xemu-metal: failed to create command queue\n");
        [g_device release];
        g_device = nil;
        return false;
    }
    [g_queue setLabel:@"xemu present queue"];

    snprintf(g_device_name, sizeof(g_device_name), "%s",
             [[g_device name] UTF8String]);

    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = YES;
    g_layer.maximumDrawableCount = 3;
    update_drawable_size();

    /* Pass descriptor reused every frame; only the attachment texture
     * changes. */
    g_pass_desc = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
    g_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    g_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_pass_desc.colorAttachments[0].clearColor =
        MTLClearColorMake(0, 0, 0, 1);

    fprintf(stderr, "Metal presenter initialized on %s\n", g_device_name);
    return true;
}

void xemu_metal_finalize(void)
{
    /* Drain any in-flight work before tearing the queue down. */
    if (g_queue) {
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
    [g_iosurface_tex release];
    g_iosurface_tex = nil;
    [g_vga_tex release];
    g_vga_tex = nil;
    [g_pass_desc release];
    g_pass_desc = nil;
    [g_queue release];
    g_queue = nil;
    [g_device release];
    g_device = nil;
    if (g_metal_view) {
        SDL_Metal_DestroyView(g_metal_view);
        g_metal_view = NULL;
    }
    g_layer = nil;
}

void xemu_metal_set_vsync(bool enabled)
{
    if (g_layer) {
        g_layer.displaySyncEnabled = enabled ? YES : NO;
    }
}

const char *xemu_metal_device_name(void)
{
    return g_device_name;
}

static void update_drawable_size(void)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (pw <= 0 || ph <= 0) {
        return;
    }
    CGSize cur = g_layer.drawableSize;
    if ((int)cur.width != pw || (int)cur.height != ph) {
        g_layer.drawableSize = CGSizeMake(pw, ph);
    }
    atomic_store_explicit(&g_layer_pw, pw, memory_order_release);
    atomic_store_explicit(&g_layer_ph, ph, memory_order_release);
}

void xemu_metal_layer_pixel_size(int *w, int *h)
{
    if (w) {
        *w = atomic_load_explicit(&g_layer_pw, memory_order_acquire);
    }
    if (h) {
        *h = atomic_load_explicit(&g_layer_ph, memory_order_acquire);
    }
}

/*
 * Drawable-first present split (XEMU_PRESENT_DRAWABLE_FIRST): acquire the
 * drawable without opening a pass, so the caller can choose what to present
 * after the nextDrawable block. Idempotent within a frame; a frame that is
 * dropped after acquiring gives the drawable back through end_frame. The
 * matching declaration lives in ui/xemu.c — xemu-metal.h stays the
 * begin_frame/end_frame lifecycle.
 */
bool xemu_metal_acquire_drawable(void);

bool xemu_metal_acquire_drawable(void)
{
    if (g_drawable) {
        return true;
    }

    g_frame_pool = objc_autoreleasePoolPush();

    @autoreleasepool {
        update_drawable_size();

        int64_t nsprof_t0 = nsprof_begin();
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        nsprof_end(NSPROF_DRAWABLE_ACQUIRE, nsprof_t0);
        if (!drawable) {
            objc_autoreleasePoolPop(g_frame_pool);
            g_frame_pool = NULL;
            return false;
        }
        g_drawable = [drawable retain];
    }

    g_drawable_w = (int)g_drawable.texture.width;
    g_drawable_h = (int)g_drawable.texture.height;
    return true;
}

bool xemu_metal_begin_frame(void *wait_event, uint64_t wait_value)
{
    if (!xemu_metal_acquire_drawable()) {
        return false;
    }

    g_cmdbuf = [[g_queue commandBuffer] retain];
    [g_cmdbuf setLabel:@"xemu present"];

    /*
     * Order against async MetalFX output: GPU-side wait, encoded
     * before the render pass that samples the frame. MTLSharedEvent
     * waits work across queues and devices, so this is safe even
     * though MetalFX submits on its own shared queue.
     */
    if (wait_event && wait_value) {
        [g_cmdbuf encodeWaitForEvent:(id<MTLSharedEvent>)wait_event
                               value:wait_value];
    }

    g_pass_desc.colorAttachments[0].texture = g_drawable.texture;
    g_encoder =
        [[g_cmdbuf renderCommandEncoderWithDescriptor:g_pass_desc] retain];
    [g_encoder setLabel:@"xemu present pass"];
    return true;
}

static uint64_t g_present_duration_ns;

void xemu_metal_set_present_duration(uint64_t duration_ns)
{
    g_present_duration_ns = duration_ns;
}

void xemu_metal_end_frame(void)
{
    if (!g_cmdbuf) {
        /*
         * Drawable acquired but no pass ever opened — the drawable-first
         * caller dropped the frame after acquiring (deduped step). Give
         * the drawable back without presenting; the pool must still pop
         * on the thread that pushed it.
         */
        if (g_drawable) {
            [g_drawable release];
            g_drawable = nil;
        }
        if (g_frame_pool) {
            objc_autoreleasePoolPop(g_frame_pool);
            g_frame_pool = NULL;
        }
        return;
    }
    [g_encoder endEncoding];
    if (g_present_duration_ns > 0) {
        /*
         * Paced interpolation step: the previous frame must stay on
         * screen at least its intended hold time. Exact GPU-side
         * pacing — the renderer's CPU-side step gate is quantized to
         * UI sync arrivals (up to ~8 ms of jitter at 120 Hz).
         */
        [g_cmdbuf presentDrawable:g_drawable
             afterMinimumDuration:(double)g_present_duration_ns / 1e9];
        g_present_duration_ns = 0;
    } else {
        [g_cmdbuf presentDrawable:g_drawable];
    }
    [g_cmdbuf commit];

    [g_encoder release];
    g_encoder = nil;
    [g_cmdbuf release];
    g_cmdbuf = nil;
    g_pass_desc.colorAttachments[0].texture = nil;
    [g_drawable release];
    g_drawable = nil;

    if (g_frame_pool) {
        objc_autoreleasePoolPop(g_frame_pool);
        g_frame_pool = NULL;
    }
}

void *xemu_metal_get_device(void)
{
    return (void *)g_device;
}

void *xemu_metal_get_command_queue(void)
{
    return (void *)g_queue;
}

void *xemu_metal_get_command_buffer(void)
{
    return (void *)g_cmdbuf;
}

void *xemu_metal_get_render_encoder(void)
{
    return (void *)g_encoder;
}

void *xemu_metal_get_render_pass_desc(void)
{
    return (void *)g_pass_desc;
}

int xemu_metal_get_drawable_width(void)
{
    return g_drawable_w;
}

int xemu_metal_get_drawable_height(void)
{
    return g_drawable_h;
}

void xemu_metal_retain_handle(void *handle)
{
    if (handle) {
        CFRetain(handle);
    }
}

void xemu_metal_release_handle(void *handle)
{
    if (handle) {
        CFRelease(handle);
    }
}

void *xemu_metal_wrap_iosurface(void *iosurface)
{
    IOSurfaceRef surf = (IOSurfaceRef)iosurface;
    if (!surf || !g_device) {
        return NULL;
    }

    uint32_t sid = IOSurfaceGetID(surf);
    int w = (int)IOSurfaceGetWidth(surf);
    int h = (int)IOSurfaceGetHeight(surf);

    if (g_iosurface_tex && sid == g_iosurface_id && w == g_iosurface_w &&
        h == g_iosurface_h) {
        return (void *)g_iosurface_tex;
    }

    [g_iosurface_tex release];
    g_iosurface_tex = nil;

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:w
                                    height:h
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    g_iosurface_tex = [g_device newTextureWithDescriptor:td
                                               iosurface:surf
                                                   plane:0];
    if (!g_iosurface_tex) {
        fprintf(stderr, "xemu-metal: IOSurface wrap failed (%dx%d)\n", w, h);
        return NULL;
    }
    g_iosurface_id = sid;
    g_iosurface_w = w;
    g_iosurface_h = h;
    return (void *)g_iosurface_tex;
}

void *xemu_metal_upload_vga_surface(const void *data, int w, int h,
                                    int stride_bytes)
{
    if (!g_device || w <= 0 || h <= 0) {
        return NULL;
    }
    if (!g_vga_tex || (int)g_vga_tex.width != w ||
        (int)g_vga_tex.height != h) {
        [g_vga_tex release];
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:w
                                        height:h
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        g_vga_tex = [g_device newTextureWithDescriptor:td];
        if (!g_vga_tex) {
            return NULL;
        }
    }
    [g_vga_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                 mipmapLevel:0
                   withBytes:data
                 bytesPerRow:(NSUInteger)stride_bytes];
    return (void *)g_vga_tex;
}

#endif /* __APPLE__ */
