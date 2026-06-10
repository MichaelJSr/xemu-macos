/*
 * MetalFX Upscaler - C interface header
 * Supports spatial upscaling, temporal upscaling, and frame interpolation.
 */
#ifndef METALFX_UPSCALE_H
#define METALFX_UPSCALE_H

#include <stdbool.h>

#ifdef __APPLE__
#include <IOSurface/IOSurfaceRef.h>

/*
 * Metal-native presentation support.
 *
 * Under the Metal presentation backend, MetalFX outputs are rings of
 * private MTLTextures (no IOSurface => no macOS 26 width cap) and
 * command buffers signal a shared MTLSharedEvent instead of blocking
 * in waitUntilCompleted. The UI present pass encodes a GPU-side wait
 * on the frame's event value before sampling.
 *
 * Texture handles cross this interface as void* holding a retained
 * id<MTLTexture>; release with CFRelease.
 */
void *metalfx_present_event(void); /* id<MTLSharedEvent>, borrowed */
uint64_t metalfx_present_event_last_value(void);
void *metalfx_wrap_iosurface_texture(IOSurfaceRef surface); /* retained */
void metalfx_texture_dims(void *texture, int *w, int *h);

/*
 * Wait (CPU-side) for all in-flight MetalFX command buffers to
 * complete. Called before the MoltenVK compositor re-renders into the
 * display IOSurface in async mode: a still-running upscale reads that
 * surface, and overwriting it mid-read produces torn frames. The
 * previous frame's MetalFX work is almost always long finished by the
 * next sync (>=8 ms later), so this rarely blocks.
 */
void metalfx_drain_inflight(void);

/*
 * Input-side GPU ordering: subsequent MetalFX command buffers encode
 * a wait for `event` (id<MTLSharedEvent>) to reach `value` before
 * running. Used with the async compositor submit so the upscaler
 * never samples the compositor image before MoltenVK finished
 * writing it. Persistent until replaced; already-signaled values are
 * no-ops.
 */
void metalfx_set_input_wait(void *event, uint64_t value);

/* Spatial upscaler */
bool metalfx_is_supported(void);
bool metalfx_init(int input_w, int input_h, int output_w, int output_h);
IOSurfaceRef metalfx_get_output_surface(void);
void *metalfx_get_output_texture(void); /* retained; Metal-native mode */
bool metalfx_upscale(IOSurfaceRef inputSurface);
void metalfx_destroy(void);

/* Temporal upscaler (color + optional depth IOSurface, zero-motion) */
bool metalfx_temporal_is_supported(void);
bool metalfx_temporal_init(int input_w, int input_h,
                           int output_w, int output_h);
IOSurfaceRef metalfx_temporal_get_output_surface(void);
void *metalfx_temporal_get_output_texture(void); /* retained; Metal-native */
void metalfx_temporal_reset(void);
bool metalfx_temporal_upscale(IOSurfaceRef colorSurface,
                              IOSurfaceRef depthSurface);
void metalfx_temporal_destroy(void);

/* Frame interpolation (macOS 26+) */
bool metalfx_interpolation_is_supported(void);
/*
 * link_temporal_scaler: when true and the temporal scaler is active at
 * matching output dimensions, the interpolator is linked to it
 * (MTLFXFrameInterpolatableScaler) and inherits its real motion/depth/
 * history state — the highest-quality interpolation path.
 */
bool metalfx_interpolation_init(int width, int height,
                                bool link_temporal_scaler);
/* Invalidate interpolator history (scene cut / frame-time hitch). */
void metalfx_interpolation_reset(void);
IOSurfaceRef metalfx_interpolation_get_output_surface(void);
void *metalfx_interpolation_get_output_texture(void); /* retained; Metal-native */
bool metalfx_interpolation_generate(IOSurfaceRef colorA,
                                    IOSurfaceRef colorB,
                                    IOSurfaceRef depthA,
                                    IOSurfaceRef depthB,
                                    float delta_time);
/* Metal-native variant: prev/cur are retained id<MTLTexture> handles */
bool metalfx_interpolation_generate_tex(void *prevTexture,
                                        void *curTexture,
                                        float delta_time);
void metalfx_interpolation_destroy(void);

#endif /* __APPLE__ */
#endif /* METALFX_UPSCALE_H */
