/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
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

#ifndef HW_NV2A_H
#define HW_NV2A_H

void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_context_init(void);
int nv2a_get_framebuffer_surface(void);

/*
 * Metal-native presentation: pull the current present frame as an
 * IOSurface instead of a GL texture name. The returned pointer is
 * borrowed — valid until nv2a_release_framebuffer_surface() (which
 * must be called after either acquisition function, regardless of
 * the result).
 */
typedef struct NV2APresentFrame {
    void *iosurface;   /* IOSurfaceRef (base compositor output) */
    void *mtl_texture; /* id<MTLTexture> (MetalFX ring output);
                          takes precedence over iosurface */
    /* GPU-side ordering: wait for `event` (id<MTLSharedEvent>) to
     * reach `event_value` before sampling. event_value 0 = no wait. */
    void *event;
    uint64_t event_value;
    /*
     * Presentation pacing. frame_seq increments on every published
     * frame (content change); display_duration_ns is the intended
     * on-screen hold time for paced interpolation steps (0 = unpaced).
     * The UI presents new sequence numbers with a GPU-enforced
     * minimum duration and skips re-presenting duplicates during
     * gameplay, so step timing is exact instead of quantized to the
     * UI loop.
     */
    uint64_t frame_seq;
    uint64_t display_duration_ns;
    uint32_t width;
    uint32_t height;
} NV2APresentFrame;
bool nv2a_get_present_frame(NV2APresentFrame *frame);

/*
 * Push-model present (XEMU_PUSH_PRESENT, Metal backend): read the frame
 * the renderer published at the last flip with NO cross-thread sync
 * round trip. Returns true and (like nv2a_get_present_frame) takes the
 * framebuffer_in_use handshake — pair with nv2a_release_framebuffer_-
 * surface(). Returns false without taking the handshake when push is
 * inactive or nothing is published yet; the caller then falls back to
 * nv2a_get_present_frame().
 */
bool nv2a_get_present_frame_pushed(NV2APresentFrame *frame);

void nv2a_release_framebuffer_surface(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);

#endif
