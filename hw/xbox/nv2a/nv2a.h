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
void nv2a_sync_ram(void);
void nv2a_det_reports_sync_read(uint64_t addr, uint64_t len);
void nv2a_save_quiesce_paused(void);
void nv2a_save_done(void);
/*
 * Loads refused for their NV2A address mask (XEMU_NV2A_WIDE) after they
 * had loaded over the machine, counted per process. BQL.
 */
unsigned nv2a_load_refusals(void);

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
 * Push-model present (Metal backend): read the frame
 * the renderer published at the last flip with NO cross-thread sync
 * round trip. Returns true and (like nv2a_get_present_frame) takes the
 * framebuffer_in_use handshake — pair with nv2a_release_framebuffer_-
 * surface(). Returns false without taking the handshake when push is
 * inactive or nothing is published yet; the caller then falls back to
 * nv2a_get_present_frame().
 */
bool nv2a_get_present_frame_pushed(NV2APresentFrame *frame);

/*
 * Non-consuming peek of the push ring: returns the frame the next
 * nv2a_get_present_frame_pushed() call will consume, WITHOUT advancing the
 * consume cursor. Takes/needs the same framebuffer_in_use handshake — pair
 * with nv2a_release_framebuffer_surface(). Debug-only (the push-present
 * refuter); returns false without taking the handshake when unavailable.
 */
bool nv2a_get_present_frame_peek(NV2APresentFrame *frame);

void nv2a_release_framebuffer_surface(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
/*
 * The Vulkan driver in use and why, e.g. "MoltenVK, 1.4.3 (auto)",
 * "KosmicKrisp, Mesa 26.3.0 (kosmickrisp)" or "MoltenVK, 1.4.3 (needs
 * macOS 26)"; NULL before the Vulkan renderer first initializes. Vulkan
 * builds only.
 */
const char *nv2a_vk_driver_desc(void);
/*
 * display.vulkan.driver as the Vulkan renderer read it at its first init
 * (CONFIG_DISPLAY_VULKAN_DRIVER_*; a change applies at the next launch),
 * or -1 before then. Vulkan builds only.
 */
int nv2a_vk_driver_setting(void);
/*
 * macOS: why this process won't try KosmicKrisp whatever the setting says
 * ("needs macOS 26", "pinned: deterministic", ...), or NULL.
 */
const char *nv2a_vk_kk_ineligible_reason(void);
/*
 * Settings picked KosmicKrisp: remove a strike marker (unless a running
 * xemu's start wrote it), so the next launch tries KosmicKrisp once more.
 * True if one was removed. Vulkan builds only.
 */
bool nv2a_vk_driver_retry(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);
uint32_t nv2a_get_crtc_start(void); /* BQL held; 0 before the device exists */
/* A renderer that draws (not the null one) runs; false before the device. */
bool nv2a_renderer_draws(void);
/* A full redraw of the VGA surface from guest RAM before reading it (BQL held). */
void nv2a_vga_refresh(void);

/*
 * xemu-frame-advance's gpu-counters (pgraph/gpu_counters.c), BQL held:
 * start a counting run owned by the advance, then stop it and return the
 * counts (a QAPI XemuGpuCounters).
 */
struct XemuGpuCounters;
bool nv2a_gpu_counters_advance_begin(bool per_flip, Error **errp);
struct XemuGpuCounters *nv2a_gpu_counters_advance_end(void);

#endif
