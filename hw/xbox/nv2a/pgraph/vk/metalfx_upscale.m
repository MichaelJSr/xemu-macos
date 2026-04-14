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
        fprintf(stderr,
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
        fprintf(stderr,
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



#pragma mark - Spatial Upscaler

typedef struct MetalFXSpatialState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLFXSpatialScaler> scaler;
    id<MTLTexture> inputTexture;
    id<MTLTexture> outputTexture;
    IOSurfaceRef outputSurface;
    IOSurfaceRef cachedInputSurface;
    int inputWidth, inputHeight;
    int outputWidth, outputHeight;
    bool initialized;
} MetalFXSpatialState;

static MetalFXSpatialState g_spatial = { 0 };

bool metalfx_is_supported(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return false;
    return [MTLFXSpatialScalerDescriptor supportsDevice:device];
}

bool metalfx_init(int input_w, int input_h, int output_w, int output_h)
{
    if (g_spatial.initialized) {
        if (g_spatial.inputWidth == input_w &&
            g_spatial.inputHeight == input_h &&
            g_spatial.outputWidth == output_w &&
            g_spatial.outputHeight == output_h) {
            return true;
        }
        metalfx_destroy();
    }

    @autoreleasepool {
        g_spatial.device = MTLCreateSystemDefaultDevice();
        if (!g_spatial.device) return false;
        if (![MTLFXSpatialScalerDescriptor supportsDevice:g_spatial.device])
            return false;

        g_spatial.commandQueue = [g_spatial.device newCommandQueue];

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
        if (!g_spatial.scaler) return false;

        g_spatial.outputSurface = create_iosurface_bgra(output_w, output_h);
        if (!g_spatial.outputSurface) return false;

        g_spatial.outputTexture = texture_from_iosurface(
            g_spatial.device, g_spatial.outputSurface,
            MTLPixelFormatBGRA8Unorm, output_w, output_h,
            MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);

        g_spatial.inputWidth = input_w;
        g_spatial.inputHeight = input_h;
        g_spatial.outputWidth = output_w;
        g_spatial.outputHeight = output_h;
        g_spatial.initialized = true;

        fprintf(stderr, "MetalFX: Spatial upscaler initialized %dx%d -> %dx%d\n",
                input_w, input_h, output_w, output_h);
        return true;
    }
}

IOSurfaceRef metalfx_get_output_surface(void)
{
    return g_spatial.outputSurface;
}

bool metalfx_upscale(IOSurfaceRef inputSurface)
{
    if (!g_spatial.initialized || !inputSurface) return false;

    @autoreleasepool {
        if (inputSurface != g_spatial.cachedInputSurface) {
            g_spatial.inputTexture = texture_from_iosurface(
                g_spatial.device, inputSurface, MTLPixelFormatBGRA8Unorm,
                g_spatial.inputWidth, g_spatial.inputHeight,
                MTLTextureUsageShaderRead);
            g_spatial.cachedInputSurface = inputSurface;
        }
        if (!g_spatial.inputTexture) return false;

        g_spatial.scaler.colorTexture = g_spatial.inputTexture;
        g_spatial.scaler.outputTexture = g_spatial.outputTexture;

        id<MTLCommandBuffer> cb = [g_spatial.commandQueue commandBuffer];
        [g_spatial.scaler encodeToCommandBuffer:cb];
        [cb commit];
        [cb waitUntilScheduled];
        return true;
    }
}

void metalfx_destroy(void)
{
    g_spatial.scaler = nil;
    g_spatial.inputTexture = nil;
    g_spatial.outputTexture = nil;
    g_spatial.commandQueue = nil;
    g_spatial.device = nil;
    g_spatial.cachedInputSurface = NULL;
    if (g_spatial.outputSurface) {
        CFRelease(g_spatial.outputSurface);
        g_spatial.outputSurface = NULL;
    }
    g_spatial.initialized = false;
}

#pragma mark - Temporal Upscaler

typedef struct MetalFXTemporalState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLFXTemporalScaler> scaler;
    id<MTLTexture> colorTexture;
    id<MTLTexture> directDepthTexture;
    id<MTLTexture> motionTexture;
    id<MTLTexture> outputTexture;
    id<MTLTexture> outputSharedTexture;
    IOSurfaceRef outputSurface;
    IOSurfaceRef cachedColorSurface;
    int inputWidth, inputHeight;
    int outputWidth, outputHeight;
    int requestedOutputW, requestedOutputH;
    bool initialized;
    int failedInputW, failedInputH;
    int failedOutputW, failedOutputH;
} MetalFXTemporalState;

static MetalFXTemporalState g_temporal = { 0 };

bool metalfx_temporal_is_supported(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return false;
    return [MTLFXTemporalScalerDescriptor supportsDevice:device];
}

bool metalfx_temporal_init(int input_w, int input_h,
                           int output_w, int output_h)
{
    int req_output_w = output_w;
    int req_output_h = output_h;

    if (g_temporal.initialized) {
        if (g_temporal.inputWidth == input_w &&
            g_temporal.inputHeight == input_h &&
            g_temporal.requestedOutputW == req_output_w &&
            g_temporal.requestedOutputH == req_output_h) {
            return true;
        }
        metalfx_temporal_destroy();
    }

    if (g_temporal.failedInputW == input_w &&
        g_temporal.failedInputH == input_h &&
        g_temporal.failedOutputW == req_output_w &&
        g_temporal.failedOutputH == req_output_h) {
        return false;
    }

    @autoreleasepool {
        g_temporal.device = MTLCreateSystemDefaultDevice();
        if (!g_temporal.device) return false;
        if (![MTLFXTemporalScalerDescriptor supportsDevice:g_temporal.device])
            return false;

        float max_scale = [MTLFXTemporalScalerDescriptor
            supportedInputContentMaxScaleForDevice:g_temporal.device];
        float scale_x = (float)output_w / (float)input_w;
        float scale_y = (float)output_h / (float)input_h;
        float max_req = (scale_x > scale_y) ? scale_x : scale_y;

        if (max_scale > 0 && max_req > max_scale) {
            output_w = (int)(input_w * max_scale);
            output_h = (int)(input_h * max_scale);
            fprintf(stderr,
                    "MetalFX: Temporal scale %.1fx exceeds max %.1fx, "
                    "clamping output to %dx%d\n",
                    max_req, max_scale, output_w, output_h);
        }

        g_temporal.commandQueue = [g_temporal.device newCommandQueue];

        MTLFXTemporalScalerDescriptor *desc =
            [[MTLFXTemporalScalerDescriptor alloc] init];
        desc.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.depthTextureFormat = MTLPixelFormatDepth32Float_Stencil8;
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
            fprintf(stderr,
                    "MetalFX: Failed to create temporal scaler %dx%d -> %dx%d\n",
                    input_w, input_h, output_w, output_h);
            g_temporal.failedInputW = input_w;
            g_temporal.failedInputH = input_h;
            g_temporal.failedOutputW = req_output_w;
            g_temporal.failedOutputH = req_output_h;
            return false;
        }

        g_temporal.outputTexture = create_metal_texture(
            g_temporal.device, MTLPixelFormatBGRA8Unorm,
            output_w, output_h,
            MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
            MTLTextureUsageRenderTarget);
        if (!g_temporal.outputTexture) return false;

        g_temporal.outputSurface = create_iosurface_bgra(output_w, output_h);
        if (g_temporal.outputSurface) {
            g_temporal.outputSharedTexture = texture_from_iosurface(
                g_temporal.device, g_temporal.outputSurface,
                MTLPixelFormatBGRA8Unorm, output_w, output_h,
                MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
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

        g_temporal.scaler.motionVectorScaleX = 0.0f;
        g_temporal.scaler.motionVectorScaleY = 0.0f;

        fprintf(stderr,
                "MetalFX: Temporal upscaler initialized %dx%d -> %dx%d\n",
                input_w, input_h, output_w, output_h);
        return true;
    }
}

IOSurfaceRef metalfx_temporal_get_output_surface(void)
{
    return g_temporal.outputSurface;
}

void metalfx_temporal_set_depth_texture(void *mtl_texture)
{
    g_temporal.directDepthTexture = (__bridge id<MTLTexture>)mtl_texture;
}

bool metalfx_temporal_upscale(IOSurfaceRef colorSurface)
{
    if (!g_temporal.initialized || !colorSurface) return false;

    @autoreleasepool {
        if (colorSurface != g_temporal.cachedColorSurface) {
            g_temporal.colorTexture = texture_from_iosurface(
                g_temporal.device, colorSurface, MTLPixelFormatBGRA8Unorm,
                g_temporal.inputWidth, g_temporal.inputHeight,
                MTLTextureUsageShaderRead);
            g_temporal.cachedColorSurface = colorSurface;
        }
        if (!g_temporal.colorTexture) return false;

        g_temporal.scaler.colorTexture = g_temporal.colorTexture;
        g_temporal.scaler.outputTexture = g_temporal.outputTexture;
        g_temporal.scaler.motionTexture = g_temporal.motionTexture;
        if (g_temporal.directDepthTexture) {
            g_temporal.scaler.depthTexture = g_temporal.directDepthTexture;
        }
        g_temporal.scaler.inputContentWidth = g_temporal.inputWidth;
        g_temporal.scaler.inputContentHeight = g_temporal.inputHeight;
        g_temporal.scaler.jitterOffsetX = 0.0f;
        g_temporal.scaler.jitterOffsetY = 0.0f;
        g_temporal.scaler.reset = false;

        id<MTLCommandBuffer> cb = [g_temporal.commandQueue commandBuffer];
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
        [cb waitUntilScheduled];
        return true;
    }
}

void metalfx_temporal_destroy(void)
{
    g_temporal.scaler = nil;
    g_temporal.colorTexture = nil;
    g_temporal.directDepthTexture = nil;
    g_temporal.motionTexture = nil;
    g_temporal.outputTexture = nil;
    g_temporal.outputSharedTexture = nil;
    g_temporal.commandQueue = nil;
    g_temporal.device = nil;
    g_temporal.cachedColorSurface = NULL;
    if (g_temporal.outputSurface) {
        CFRelease(g_temporal.outputSurface);
        g_temporal.outputSurface = NULL;
    }
    g_temporal.initialized = false;
    g_temporal.failedInputW = 0;
    g_temporal.failedInputH = 0;
    g_temporal.failedOutputW = 0;
    g_temporal.failedOutputH = 0;
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
    id<MTLTexture> cachedDepthTex;
    id<MTLTexture> motionTexture;
    IOSurfaceRef lastColorCur;
    IOSurfaceRef lastColorPrev;
    IOSurfaceRef lastDepth;
    int width, height;
    bool initialized;
    bool firstFrame;
} MetalFXInterpolationState;

static MetalFXInterpolationState g_interp = { 0 };

bool metalfx_interpolation_is_supported(void)
{
    if (@available(macOS 26.0, *)) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return false;
        return [MTLFXFrameInterpolatorDescriptor supportsDevice:device];
    }
    return false;
}

bool metalfx_interpolation_init(int width, int height)
{
    if (@available(macOS 26.0, *)) {
        if (g_interp.initialized) {
            if (g_interp.width == width && g_interp.height == height)
                return true;
            metalfx_interpolation_destroy();
        }

        @autoreleasepool {
            g_interp.device = MTLCreateSystemDefaultDevice();
            if (!g_interp.device) return false;

            g_interp.commandQueue = [g_interp.device newCommandQueue];

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
                fprintf(stderr,
                        "MetalFX: Failed to create frame interpolator\n");
                return false;
            }
            g_interp.interpolator = interp;

            g_interp.outputTexture = create_metal_texture(
                g_interp.device, MTLPixelFormatBGRA8Unorm,
                width, height,
                MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
                MTLTextureUsageRenderTarget);
            if (!g_interp.outputTexture) return false;

            g_interp.outputSurface = create_iosurface_bgra(width, height);
            if (g_interp.outputSurface) {
                g_interp.outputSharedTexture = texture_from_iosurface(
                    g_interp.device, g_interp.outputSurface,
                    MTLPixelFormatBGRA8Unorm, width, height,
                    MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
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

            fprintf(stderr,
                    "MetalFX: Frame interpolator initialized %dx%d\n",
                    width, height);
            return true;
        }
    }
    (void)width; (void)height;
    return false;
}

IOSurfaceRef metalfx_interpolation_get_output_surface(void)
{
    return g_interp.outputSurface;
}

bool metalfx_interpolation_generate(IOSurfaceRef colorA,
                                    IOSurfaceRef colorB,
                                    IOSurfaceRef depthA,
                                    IOSurfaceRef depthB,
                                    float delta_time)
{
    if (@available(macOS 26.0, *)) {
        if (!g_interp.initialized || !colorA || !colorB) return false;

        id<MTLFXFrameInterpolator> interp =
            (id<MTLFXFrameInterpolator>)g_interp.interpolator;

        @autoreleasepool {
            MTLTextureUsage readUsage = MTLTextureUsageShaderRead;

            /* colorB = current frame, colorA = previous frame */
            if (colorB != g_interp.lastColorCur) {
                g_interp.cachedColorCur = texture_from_iosurface(
                    g_interp.device, colorB, MTLPixelFormatBGRA8Unorm,
                    g_interp.width, g_interp.height, readUsage);
                g_interp.lastColorCur = colorB;
            }
            if (colorA != g_interp.lastColorPrev) {
                g_interp.cachedColorPrev = texture_from_iosurface(
                    g_interp.device, colorA, MTLPixelFormatBGRA8Unorm,
                    g_interp.width, g_interp.height, readUsage);
                g_interp.lastColorPrev = colorA;
            }
            if (!g_interp.cachedColorCur || !g_interp.cachedColorPrev)
                return false;

            if (depthB && depthB != g_interp.lastDepth) {
                g_interp.cachedDepthTex = texture_from_iosurface(
                    g_interp.device, depthB, MTLPixelFormatR32Float,
                    g_interp.width, g_interp.height, readUsage);
                g_interp.lastDepth = depthB;
            }

            interp.colorTexture = g_interp.cachedColorCur;
            interp.prevColorTexture = g_interp.cachedColorPrev;
            interp.outputTexture = g_interp.outputTexture;
            interp.motionTexture = g_interp.motionTexture;
            interp.motionVectorScaleX = 0.0f;
            interp.motionVectorScaleY = 0.0f;
            interp.deltaTime = delta_time;
            interp.nearPlane = 0.1f;
            interp.farPlane = 1000.0f;
            if (g_interp.cachedDepthTex) {
                interp.depthTexture = g_interp.cachedDepthTex;
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
            [cb waitUntilScheduled];
            return true;
        }
    }
    (void)colorA; (void)colorB; (void)depthA; (void)depthB;
    return false;
}

void metalfx_interpolation_destroy(void)
{
    g_interp.interpolator = nil;
    g_interp.outputTexture = nil;
    g_interp.outputSharedTexture = nil;
    g_interp.motionTexture = nil;
    g_interp.cachedColorCur = nil;
    g_interp.cachedColorPrev = nil;
    g_interp.cachedDepthTex = nil;
    g_interp.commandQueue = nil;
    g_interp.device = nil;
    g_interp.lastColorCur = NULL;
    g_interp.lastColorPrev = NULL;
    g_interp.lastDepth = NULL;
    if (g_interp.outputSurface) {
        CFRelease(g_interp.outputSurface);
        g_interp.outputSurface = NULL;
    }
    g_interp.initialized = false;
}

#endif /* __APPLE__ */
