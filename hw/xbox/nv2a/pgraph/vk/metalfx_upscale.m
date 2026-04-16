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

#import <os/lock.h>

#ifndef NDEBUG
#define METALFX_DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define METALFX_DPRINTF(fmt, ...) do {} while (0)
#endif

static os_unfair_lock g_metalfx_lock = OS_UNFAIR_LOCK_INIT;

#pragma mark - Shared Metal Device

static id<MTLDevice> g_shared_device = nil;
static id<MTLCommandQueue> g_shared_queue = nil;
static int g_shared_refcount = 0;

static bool shared_metal_acquire(id<MTLDevice> *out_device,
                                 id<MTLCommandQueue> *out_queue)
{
    if (!g_shared_device) {
        g_shared_device = MTLCreateSystemDefaultDevice();
        if (!g_shared_device) return false;
        g_shared_queue = [g_shared_device newCommandQueue];
    }
    g_shared_refcount++;
    *out_device = g_shared_device;
    *out_queue = g_shared_queue;
    return true;
}

static void shared_metal_release(void)
{
    if (--g_shared_refcount <= 0) {
        g_shared_queue = nil;
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

typedef struct MetalFXSpatialState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLFXSpatialScaler> scaler;
    id<MTLTexture> inputTexture;
    id<MTLTexture> outputTexture;
    IOSurfaceRef outputSurface;
    IOSurfaceID cachedInputSurfaceID;
    int inputWidth, inputHeight;
    int outputWidth, outputHeight;
    bool initialized;
} MetalFXSpatialState;

static MetalFXSpatialState g_spatial = { 0 };

bool metalfx_is_supported(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { cached = 0; return false; }
    cached = [MTLFXSpatialScalerDescriptor supportsDevice:device] ? 1 : 0;
    return cached;
}

static void metalfx_destroy_locked(void)
{
    g_spatial.scaler = nil;
    g_spatial.inputTexture = nil;
    g_spatial.outputTexture = nil;
    g_spatial.cachedInputSurfaceID = 0;
    if (g_spatial.outputSurface) {
        CFRelease(g_spatial.outputSurface);
        g_spatial.outputSurface = NULL;
    }
    if (g_spatial.device) {
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
        if (!g_spatial.scaler) {
            metalfx_destroy_locked();
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

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
                g_spatial.inputTexture = tex;
                g_spatial.cachedInputSurfaceID = inputID;
            }
        }
        if (!g_spatial.inputTexture) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return false;
        }

        g_spatial.scaler.colorTexture = g_spatial.inputTexture;
        g_spatial.scaler.outputTexture = g_spatial.outputTexture;

        id<MTLCommandBuffer> cb = [g_spatial.commandQueue commandBuffer];
        [g_spatial.scaler encodeToCommandBuffer:cb];
        [cb commit];
        os_unfair_lock_unlock(&g_metalfx_lock);
        [cb waitUntilCompleted];
        return true;
    }
}

void metalfx_destroy(void)
{
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
} MetalFXTemporalState;

static MetalFXTemporalState g_temporal = { 0 };

bool metalfx_temporal_is_supported(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { cached = 0; return false; }
    cached = [MTLFXTemporalScalerDescriptor supportsDevice:device] ? 1 : 0;
    return cached;
}

static void metalfx_temporal_destroy_locked(void)
{
    g_temporal.scaler = nil;
    g_temporal.colorTexture = nil;
    g_temporal.depthTexture = nil;
    g_temporal.cachedDepthSurfaceID = 0;
    g_temporal.motionTexture = nil;
    g_temporal.syntheticDepthPipeline = nil;
    g_temporal.syntheticDepthTexture = nil;
    g_temporal.outputTexture = nil;
    g_temporal.outputSharedTexture = nil;
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
}

bool metalfx_temporal_init(int input_w, int input_h,
                           int output_w, int output_h)
{
    int req_output_w = output_w;
    int req_output_h = output_h;

    os_unfair_lock_lock(&g_metalfx_lock);
    if (g_temporal.initialized) {
        if (g_temporal.inputWidth == input_w &&
            g_temporal.inputHeight == input_h &&
            g_temporal.requestedOutputW == req_output_w &&
            g_temporal.requestedOutputH == req_output_h) {
            os_unfair_lock_unlock(&g_metalfx_lock);
            return true;
        }
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
        desc.depthTextureFormat = MTLPixelFormatR32Float;
        desc.motionTextureFormat = MTLPixelFormatRG16Float;
        desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.inputWidth = input_w;
        desc.inputHeight = input_h;
        desc.outputWidth = output_w;
        desc.outputHeight = output_h;
        desc.autoExposureEnabled = YES;

        g_temporal.scaler =
            [desc newTemporalScalerWithDevice:g_temporal.device];
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
            }
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
                    g_temporal.depthTexture = tex;
                    g_temporal.cachedDepthSurfaceID = depthID;
                }
            }
        } else {
            g_temporal.depthTexture = nil;
            g_temporal.cachedDepthSurfaceID = 0;
        }

        id<MTLCommandBuffer> cb = [g_temporal.commandQueue commandBuffer];

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

        g_temporal.scaler.colorTexture = g_temporal.colorTexture;
        g_temporal.scaler.outputTexture = g_temporal.outputTexture;
        g_temporal.scaler.motionTexture = g_temporal.motionTexture;
        if (g_temporal.depthTexture) {
            g_temporal.scaler.depthTexture = g_temporal.depthTexture;
        } else if (g_temporal.syntheticDepthTexture) {
            g_temporal.scaler.depthTexture = g_temporal.syntheticDepthTexture;
        }
        g_temporal.scaler.inputContentWidth = g_temporal.inputWidth;
        g_temporal.scaler.inputContentHeight = g_temporal.inputHeight;

        static const float halton_x[] = { 0.0f, -0.25f, 0.25f, -0.375f, 0.125f, -0.125f, 0.375f, -0.4375f };
        static const float halton_y[] = { 0.0f, -0.333f, 0.333f, -0.111f, 0.222f, -0.222f, 0.111f, 0.111f };
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

        [cb commit];
        os_unfair_lock_unlock(&g_metalfx_lock);
        [cb waitUntilCompleted];
        return true;
    }
}

void metalfx_temporal_destroy(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    metalfx_temporal_destroy_locked();
    os_unfair_lock_unlock(&g_metalfx_lock);
}

#pragma mark - Frame Interpolation (macOS 26+)

typedef struct MetalFXInterpolationState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id interpolator; /* id<MTLFXFrameInterpolator>, stored untyped for deployment target compat */
    id<MTLTexture> outputTexture;
    id<MTLTexture> outputSharedTexture;
    IOSurfaceRef outputSurface;
    id<MTLTexture> cachedColorCur;
    id<MTLTexture> cachedColorPrev;
    id<MTLTexture> cachedDepthCur;
    id<MTLTexture> cachedDepthPrev;
    id<MTLTexture> motionTexture;
    IOSurfaceID lastColorCurID;
    IOSurfaceID lastColorPrevID;
    IOSurfaceID lastDepthCurID;
    IOSurfaceID lastDepthPrevID;
    int width, height;
    bool initialized;
    bool firstFrame;
} MetalFXInterpolationState;

static MetalFXInterpolationState g_interp = { 0 };

bool metalfx_interpolation_is_supported(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    if (@available(macOS 26.0, *)) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { cached = 0; return false; }
        cached = [MTLFXFrameInterpolatorDescriptor supportsDevice:device] ? 1 : 0;
        return cached;
    }
    cached = 0;
    return false;
}

static void metalfx_interpolation_destroy_locked(void)
{
    g_interp.interpolator = nil;
    g_interp.outputTexture = nil;
    g_interp.outputSharedTexture = nil;
    g_interp.motionTexture = nil;
    g_interp.cachedColorCur = nil;
    g_interp.cachedColorPrev = nil;
    g_interp.cachedDepthCur = nil;
    g_interp.cachedDepthPrev = nil;
    g_interp.lastColorCurID = 0;
    g_interp.lastColorPrevID = 0;
    g_interp.lastDepthCurID = 0;
    g_interp.lastDepthPrevID = 0;
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

bool metalfx_interpolation_init(int width, int height)
{
    if (@available(macOS 26.0, *)) {
        os_unfair_lock_lock(&g_metalfx_lock);
        if (g_interp.initialized) {
            if (g_interp.width == width && g_interp.height == height) {
                os_unfair_lock_unlock(&g_metalfx_lock);
                return true;
            }
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

            id<MTLFXFrameInterpolator> interp =
                [desc newFrameInterpolatorWithDevice:g_interp.device];
            if (!interp) {
                METALFX_DPRINTF(
                        "MetalFX: Failed to create frame interpolator\n");
                metalfx_interpolation_destroy_locked();
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }
            g_interp.interpolator = interp;

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
                        MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
                }
            }

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

            g_interp.width = width;
            g_interp.height = height;
            g_interp.initialized = true;
            g_interp.firstFrame = true;

            METALFX_DPRINTF(
                    "MetalFX: Frame interpolator initialized %dx%d\n",
                    width, height);
            os_unfair_lock_unlock(&g_metalfx_lock);
            return true;
        }
    }
    (void)width; (void)height;
    return false;
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
                    g_interp.cachedColorPrev = tex;
                    g_interp.lastColorPrevID = colorAID;
                }
            }
            if (!g_interp.cachedColorCur || !g_interp.cachedColorPrev) {
                os_unfair_lock_unlock(&g_metalfx_lock);
                return false;
            }

            if (depthB) {
                IOSurfaceID depthBID = IOSurfaceGetID(depthB);
                if (depthBID != g_interp.lastDepthCurID) {
                    id<MTLTexture> tex = texture_from_iosurface(
                        g_interp.device, depthB, MTLPixelFormatR32Float,
                        g_interp.width, g_interp.height, readUsage);
                    if (tex) {
                        g_interp.cachedDepthCur = tex;
                        g_interp.lastDepthCurID = depthBID;
                    }
                }
            } else {
                g_interp.cachedDepthCur = nil;
                g_interp.lastDepthCurID = 0;
            }
            if (depthA) {
                IOSurfaceID depthAID = IOSurfaceGetID(depthA);
                if (depthAID != g_interp.lastDepthPrevID) {
                    id<MTLTexture> tex = texture_from_iosurface(
                        g_interp.device, depthA, MTLPixelFormatR32Float,
                        g_interp.width, g_interp.height, readUsage);
                    if (tex) {
                        g_interp.cachedDepthPrev = tex;
                        g_interp.lastDepthPrevID = depthAID;
                    }
                }
            } else {
                g_interp.cachedDepthPrev = nil;
                g_interp.lastDepthPrevID = 0;
            }

            interp.colorTexture = g_interp.cachedColorCur;
            interp.prevColorTexture = g_interp.cachedColorPrev;
            interp.outputTexture = g_interp.outputTexture;
            interp.motionTexture = g_interp.motionTexture;
            interp.motionVectorScaleX = 1.0f;
            interp.motionVectorScaleY = 1.0f;
            if (delta_time <= 0.0f || delta_time > 1.0f) delta_time = 0.5f;
            interp.deltaTime = delta_time;
            interp.nearPlane = 0.01f;
            interp.farPlane = 10000.0f;
            if (g_interp.cachedDepthCur) {
                interp.depthTexture = g_interp.cachedDepthCur;
            }
            interp.shouldResetHistory = g_interp.firstFrame;
            g_interp.firstFrame = false;

            id<MTLCommandBuffer> cb = [g_interp.commandQueue commandBuffer];
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

            [cb commit];
            os_unfair_lock_unlock(&g_metalfx_lock);
            [cb waitUntilCompleted];
            return true;
        }
    }
    (void)colorA; (void)colorB; (void)depthA; (void)depthB;
    return false;
}

void metalfx_interpolation_destroy(void)
{
    os_unfair_lock_lock(&g_metalfx_lock);
    metalfx_interpolation_destroy_locked();
    os_unfair_lock_unlock(&g_metalfx_lock);
}

#endif /* __APPLE__ */
