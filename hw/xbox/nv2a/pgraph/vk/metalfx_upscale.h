/*
 * MetalFX Upscaler - C interface header
 * Supports spatial upscaling, temporal upscaling, and frame interpolation.
 */
#ifndef METALFX_UPSCALE_H
#define METALFX_UPSCALE_H

#include <stdbool.h>

#ifdef __APPLE__
#include <IOSurface/IOSurfaceRef.h>

/* Spatial upscaler */
bool metalfx_is_supported(void);
bool metalfx_init(int input_w, int input_h, int output_w, int output_h);
IOSurfaceRef metalfx_get_output_surface(void);
bool metalfx_upscale(IOSurfaceRef inputSurface);
void metalfx_destroy(void);

/* Temporal upscaler (color + optional depth, zero-motion) */
bool metalfx_temporal_is_supported(void);
bool metalfx_temporal_init(int input_w, int input_h,
                           int output_w, int output_h);
IOSurfaceRef metalfx_temporal_get_output_surface(void);
void metalfx_temporal_set_depth_texture(void *mtl_texture);
bool metalfx_temporal_upscale(IOSurfaceRef colorSurface);
void metalfx_temporal_destroy(void);

/* Frame interpolation (macOS 26+) */
bool metalfx_interpolation_is_supported(void);
bool metalfx_interpolation_init(int width, int height);
IOSurfaceRef metalfx_interpolation_get_output_surface(void);
bool metalfx_interpolation_generate(IOSurfaceRef colorA,
                                    IOSurfaceRef colorB,
                                    IOSurfaceRef depthA,
                                    IOSurfaceRef depthB,
                                    float delta_time);
void metalfx_interpolation_destroy(void);

#endif /* __APPLE__ */
#endif /* METALFX_UPSCALE_H */
