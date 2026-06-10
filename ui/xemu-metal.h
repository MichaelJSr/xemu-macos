/*
 * xemu Metal-Native Presenter
 *
 * Owns the CAMetalLayer attached to the main SDL window, the shared
 * MTLDevice/MTLCommandQueue used for presentation, and the per-frame
 * drawable/command-buffer/encoder lifecycle. All functions are called
 * from the main/UI thread unless noted.
 *
 * Raw Metal object handles cross this C interface as void* (they are
 * id<MTLDevice>, id<MTLCommandBuffer>, etc. on the ObjC side).
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
#ifndef XEMU_METAL_H
#define XEMU_METAL_H

#include <stdbool.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Window SDL_Window;

/* Window/layer/device setup. Returns false on failure. */
bool xemu_metal_init(SDL_Window *window);
void xemu_metal_finalize(void);

/* CAMetalLayer.displaySyncEnabled (the Metal vsync analogue). */
void xemu_metal_set_vsync(bool enabled);

/* Marketing name of the MTLDevice, for logs / About page. */
const char *xemu_metal_device_name(void);

/*
 * Frame lifecycle. begin acquires the next drawable and opens a render
 * pass (cleared to black) on a fresh command buffer; end finishes
 * encoding, presents the drawable and commits. Returns false when no
 * drawable is available (e.g. window fully occluded) — skip the frame.
 *
 * wait_event/wait_value: optional GPU-side ordering against async
 * MetalFX work — an id<MTLSharedEvent> wait encoded before the render
 * pass. Pass NULL/0 for no wait.
 */
bool xemu_metal_begin_frame(void *wait_event, uint64_t wait_value);
void xemu_metal_end_frame(void);

/*
 * GPU-enforced minimum on-screen duration for the frame presented by
 * the next end_frame (presentDrawable:afterMinimumDuration:). One
 * shot; 0 = present normally. Used to pace interpolation steps
 * exactly instead of quantizing to the UI loop cadence.
 */
void xemu_metal_set_present_duration(uint64_t duration_ns);

/*
 * Current CAMetalLayer pixel size (thread-safe snapshot, updated on
 * every presented frame). 0x0 before the first frame.
 */
void xemu_metal_layer_pixel_size(int *w, int *h);

/* Handles for the compositor / ImGui bridge. Valid between
 * begin_frame and end_frame (except device/queue: always valid after
 * init). */
void *xemu_metal_get_device(void);          /* id<MTLDevice> */
void *xemu_metal_get_command_queue(void);   /* id<MTLCommandQueue> */
void *xemu_metal_get_command_buffer(void);  /* id<MTLCommandBuffer> */
void *xemu_metal_get_render_encoder(void);  /* id<MTLRenderCommandEncoder> */
void *xemu_metal_get_render_pass_desc(void); /* MTLRenderPassDescriptor* */
int xemu_metal_get_drawable_width(void);
int xemu_metal_get_drawable_height(void);

/*
 * Wrap an IOSurfaceRef into a cached BGRA id<MTLTexture> (returned as
 * a UI texture handle). Single-entry cache keyed on IOSurfaceID +
 * dimensions; the returned handle is valid until the next call. The
 * texture retains the IOSurface for GPU lifetime.
 */
void *xemu_metal_wrap_iosurface(void *iosurface);

/* Retain/release an ObjC handle (CFRetain/CFRelease) from C code. */
void xemu_metal_retain_handle(void *handle);
void xemu_metal_release_handle(void *handle);

/*
 * Upload a 32bpp pixman surface (BGRX/BGRA, top-origin rows) for the
 * VGA fallback path. Reuses/resizes one cached texture; the returned
 * handle is valid until the next call.
 */
void *xemu_metal_upload_vga_surface(const void *data, int w, int h,
                                    int stride_bytes);

#ifdef __cplusplus
}
#endif

#endif
