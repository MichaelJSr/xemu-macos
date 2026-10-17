/*
 * xemu User Interface
 *
 * Copyright (C) 2020-2022 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdint>
#include <fpng.h>
#include <vector>

#include "xemu-present.h"
#include "xemu-snapshots.h"
#include "xui/gl-helpers.hh"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include "xui/metal-helpers.hh"
#endif

static uintptr_t display_tex = 0;
static bool display_flip = false;
static void *display_wait_event;
static uint64_t display_wait_value;

void xemu_snapshots_set_framebuffer_texture(uintptr_t tex, bool flip,
                                            void *wait_event,
                                            uint64_t wait_value)
{
#ifdef __APPLE__
    /*
     * Under Metal the handle is an id<MTLTexture> owned by the
     * renderer (replaced on later frames); keep our own reference so
     * a savestate thumbnail capture can't read a freed object.
     */
    if (xemu_present_is_metal() && tex != display_tex) {
        MetalRetainTexture(tex);
        if (display_tex) {
            MetalDestroyTexture(display_tex);
        }
    }
    /*
     * The compositor's timeline event (unlike MetalFX's) is released
     * when the Vulkan renderer is torn down, and a monitor savevm can
     * read this pair long after the frame that set it: keep our own
     * reference.
     */
    if (wait_event != display_wait_event) {
        if (wait_event) {
            CFRetain(wait_event);
        }
        if (display_wait_event) {
            CFRelease(display_wait_event);
        }
    }
#endif
    display_tex = tex;
    display_flip = flip;
    display_wait_event = wait_event;
    display_wait_value = wait_value;
}

uintptr_t xemu_snapshots_load_png_thumbnail(const void *buf, size_t size)
{
    std::vector<uint8_t> pixels;
    unsigned int width, height, channels;
    if (fpng::fpng_decode_memory(buf, size, pixels, width, height, channels,
                                 4) != fpng::FPNG_DECODE_SUCCESS) {
        return 0;
    }

    return CreateUiTextureFromRgba(pixels.data(), width, height);
}

void xemu_snapshots_free_thumbnail(uintptr_t tex)
{
    DestroyUiTexture(tex);
}

void *xemu_snapshots_create_framebuffer_thumbnail_png(size_t *size)
{
    /*
     * Avoids crashing if a snapshot is made on a thread with no GL context
     * Normally, this is not an issue, but it is better to fail safe than assert
     * here.
     * FIXME: Allow for dispatching a thumbnail request to the UI thread to
     * remove this altogether.
     */
    if (display_tex == 0 ||
        (!xemu_present_is_metal() && !SDL_GL_GetCurrentContext())) {
        return NULL;
    }

    std::vector<uint8_t> png;
    if (!RenderFramebufferToPng(display_tex, display_flip, png,
                                2 * XEMU_SNAPSHOT_THUMBNAIL_WIDTH,
                                2 * XEMU_SNAPSHOT_THUMBNAIL_HEIGHT,
                                display_wait_event, display_wait_value)) {
        return NULL;
    }

    void *buf = g_malloc(png.size());
    memcpy(buf, png.data(), png.size());
    *size = png.size();
    return buf;
}
