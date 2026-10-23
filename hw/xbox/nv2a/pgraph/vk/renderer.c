/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "exec/icount.h"
#include "system/runstate.h"
#include "renderer.h"

#include "gloffscreen.h"

#if HAVE_IOSURFACE_SHARING
#include "metalfx_upscale.h"
#endif
#include "hw/xbox/nv2a/nsprof.h"
#include "ui/xemu-present.h"

#if HAVE_EXTERNAL_MEMORY || HAVE_IOSURFACE_SHARING
static GloContext *g_gl_context;
#endif

static void early_context_init(void)
{
#if HAVE_EXTERNAL_MEMORY || HAVE_IOSURFACE_SHARING
    /*
     * The gloffscreen context only serves the GL presentation paths
     * (CGL IOSurface display, GL readbacks). Under the Metal backend
     * the present chain is pure Metal/Vulkan, so skip the hidden GL
     * window + context.
     */
    if (!xemu_present_is_metal()) {
        g_gl_context = glo_context_create();
    }
#endif
}

static void null_ram_setup(NV2AState *d);

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));

#if HAVE_EXTERNAL_MEMORY || HAVE_IOSURFACE_SHARING
    if (g_gl_context) {
        glo_set_current(g_gl_context);
    }
#endif

    pgraph_vk_debug_init();

    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }

    pgraph_vk_init_command_buffers(pg);
    pgraph_vk_init_buffers(d);
    pgraph_vk_init_surfaces(pg);
    pgraph_vk_init_shaders(pg);
    pgraph_vk_init_pipelines(pg);
    pgraph_vk_init_textures(pg);
    pgraph_vk_init_reports(pg);
    pgraph_vk_init_compute(pg);
    pgraph_vk_init_flight_partitions(pg);
    pgraph_vk_init_display(pg);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    pgraph_vk_determine_gpu_properties(d);

    PGRAPHVkState *r = pg->vk_renderer_state;
#if HAVE_IOSURFACE_SHARING
    if (r->metal_copy_present && xemu_present_is_metal()) {
        /* Frames reach Metal through an MTLHeap-exported buffer (display.c) */
        fprintf(stderr, "IOSurface sharing: enabled (copy-present)\n");
    } else {
        fprintf(stderr, "IOSurface sharing: %s (VK_EXT_metal_objects=%s)\n",
                r->metal_objects_extension_enabled ? "enabled" : "disabled",
                r->metal_objects_extension_enabled ? "yes" : "no");
    }
#elif HAVE_EXTERNAL_MEMORY
    fprintf(stderr, "External memory sharing: enabled\n");
#else
    fprintf(stderr, "Display sharing: none (CPU fallback)\n");
#endif
    (void)r;
    null_ram_setup(d);
}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_driver_stopped();
    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_finalize_instance(pg);

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (qatomic_xchg(&pg->flush_discard, false)) {
        /*
         * Deterministic mode, after a load: the previous session's GPU
         * surfaces, reports and queries belong to RAM that was just
         * replaced, so drop them without writing back (reports were
         * delivered when the load began, nv2a_vm_state_change; this only
         * drops strays). Renderer state the savestate lacks starts as in a
         * fresh process.
         */
        pgraph_vk_det_reports_discard(d);
        SurfaceBinding *s;
        QTAILQ_FOREACH(s, &r->surfaces, entry) {
            s->draw_dirty = false;
            s->download_pending = false;
        }
        r->last_expire_frame = pg->frame_time;
        r->expire_updates = 0;
    }

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_pipeline_cache_note_flush(pg);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static int64_t last_sync_time_ns;

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    /*
     * QEMU_CLOCK_REALTIME is CLOCK_MONOTONIC (qemu/timer.h get_clock());
     * QEMU_CLOCK_HOST is gettimeofday and "will reflect system time
     * changes the host may undergo (e.g. due to NTP)" — a backwards
     * jump would freeze the 8 ms sync gate until wall-clock overtakes
     * last_sync_time_ns again. An earlier pass had these two swapped;
     * all present-path timing (this gate, interpolation timestamps,
     * step pacing, surface expiry) uses the monotonic clock.
     */
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t elapsed = now - last_sync_time_ns;
    const int64_t min_sync_interval_ns = 8000000; /* ~8ms = 120Hz cap */

    bool has_interp_work = false;
#if HAVE_IOSURFACE_SHARING
    has_interp_work = r->display.interp_remaining > 0 ||
                      r->display.pending_real_texture != NULL;
#endif

    /*
     * Under icount a screendump (taken paused) shows the current frame.
     * While the VM runs with the push ring on, the window keeps the frames
     * flips publish: a render here would finish the frame's command buffer
     * at a point the UI thread picks, and report sums can depend on where
     * it splits (windowed icount azurik-heavy-w2 ended on a second RAM hash
     * in 2 of 16 paced runs).
     */
    bool keep_flip_frames = false;
#if HAVE_IOSURFACE_SHARING
    keep_flip_frames = icount_enabled() && runstate_is_running() &&
                       pgraph_vk_push_present_enabled(d);
#endif
    if (!keep_flip_frames &&
        (elapsed >= min_sync_interval_ns || has_interp_work ||
         icount_enabled())) {
        pgraph_vk_render_display(pg);
        if (elapsed >= min_sync_interval_ns) {
            last_sync_time_ns = now;
        }
    }

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending) ||
        qatomic_read(&r->det_reports.deliver)
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        pgraph_vk_det_reports_process(d);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_vk_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    /*
     * Flip-time zeta snapshot (XEMU_MFX_REAL_DEPTH=2): record the depth
     * blit into this frame's command buffer BEFORE the finish so the
     * copy rides this submission — capturing depth that matches the
     * frame about to be presented, before the next frame overwrites the
     * single guest zeta. No-op unless the knob is 2. See surface.c.
     */
    pgraph_vk_zeta_snapshot_capture(&d->pgraph);
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);
    pgraph_vk_debug_frame_terminator();
    pgraph_vk_maybe_save_pipeline_cache(&d->pgraph);
#if HAVE_IOSURFACE_SHARING
    if (pgraph_vk_push_present_enabled(d)) {
        /*
         * Push-model present: composite and publish this flip's frame
         * now, on the PFIFO thread, so the UI can present it with no
         * cross-thread sync round trip. render_display runs in its
         * normal lock context (FLIP_STALL executes under pgraph.lock,
         * exactly as the pull-model sync path does) and is gated to a
         * ~120 Hz ceiling so it never composites more often than the
         * pull path would. This does not touch guest vblank/flip timing
         * — waiting_for_flip and the vblank release are untouched below.
         *
         * The gate uses its OWN timestamp, not the pull path's
         * last_sync_time_ns: during the startup fallback the UI still
         * pulls (advancing last_sync_time_ns every ~8 ms), which would
         * otherwise keep this gate closed forever and the slot would
         * never publish — starving push out of ever engaging.
         */
        static int64_t last_push_publish_ns;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now - last_push_publish_ns >= 8000000) {
            pgraph_vk_render_display(&d->pgraph);
            pgraph_vk_present_schedule_publish(&d->pgraph);
            last_push_publish_ns = now;
        }
    }
#endif
    pgraph_vk_driver_flip();
    nsprof_flip_tick();
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);

    if (!icount_enabled()) {
        /* Not under icount: it would move surface expiry by host time. */
        surface->frame_time = pg->frame_time;
    }

#if HAVE_EXTERNAL_MEMORY
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);
    return r->display.gl_texture_id;
#elif HAVE_IOSURFACE_SHARING
    if (r->metal_objects_extension_enabled) {
        qemu_event_reset(&d->pgraph.sync_complete);
        qatomic_set(&pg->sync_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        int64_t present_wait_t0 = nsprof_begin();
        qemu_event_wait(&d->pgraph.sync_complete);
        nsprof_end(NSPROF_PRESENT_WAIT, present_wait_t0);
        return r->display.gl_texture_id;
    }
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_vk_wait_for_surface_download(surface);
    return 0;
#else
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_vk_wait_for_surface_download(surface);
    return 0;
#endif
}

/*
 * Metal-native presentation path: identical sync handshake to
 * pgraph_vk_get_framebuffer_surface, but hands the UI the present
 * IOSurface (stored by pgraph_vk_render_display) instead of a CGL
 * rect-texture name. The pointer is borrowed: the display state keeps
 * its own retain until the next render_display, and the UI's
 * MTLTexture wrap retains the surface for GPU lifetime.
 */
static bool pgraph_vk_get_present_frame(NV2AState *d, NV2APresentFrame *frame)
{
#if HAVE_IOSURFACE_SHARING
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color ||
        !pgraph_vk_metal_present_interop(r)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return false;
    }

    if (!icount_enabled()) {
        /* Not under icount: it would move surface expiry by host time. */
        surface->frame_time = pg->frame_time;
    }

    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    int64_t present_wait_t0 = nsprof_begin();
    qemu_event_wait(&d->pgraph.sync_complete);
    nsprof_end(NSPROF_PRESENT_WAIT, present_wait_t0);

    frame->iosurface = r->display.present_iosurface;
    frame->mtl_texture = r->display.present_mtl_texture;
    frame->event = r->display.present_event ? r->display.present_event
                                            : metalfx_present_event();
    frame->event_value = r->display.present_event_value;
    frame->frame_seq = r->display.present_frame_seq;
    frame->display_duration_ns = r->display.present_duration_ns;
    frame->width = r->display.present_width;
    frame->height = r->display.present_height;
    return frame->iosurface != NULL || frame->mtl_texture != NULL;
#else
    return false;
#endif
}

/*
 * Push-model present read: hand the UI the frame the PFIFO thread
 * published at the last flip, with no sync handshake. Returns false
 * (UI falls back to pgraph_vk_get_present_frame) when push is inactive
 * or nothing is published yet (startup, post-resize).
 */
static bool pgraph_vk_get_present_frame_pushed(NV2AState *d,
                                               NV2APresentFrame *frame)
{
#if HAVE_IOSURFACE_SHARING
    if (!pgraph_vk_push_present_enabled(d)) {
        return false;
    }
    return pgraph_vk_present_slot_read(&d->pgraph, frame);
#else
    return false;
#endif
}

/*
 * Push-model peek: like _pushed but leaves the consume cursor untouched,
 * so the refuter can inspect the step the UI's next read will consume
 * without stealing it. Debug-only (XEMU_PUSH_PRESENT_REFUTE).
 */
static bool pgraph_vk_get_present_frame_peek(NV2AState *d,
                                             NV2APresentFrame *frame)
{
#if HAVE_IOSURFACE_SHARING
    if (!pgraph_vk_push_present_enabled(d)) {
        return false;
    }
    return pgraph_vk_present_slot_peek(&d->pgraph, frame);
#else
    return false;
#endif
}

/*
 * XEMU_RENDER_NULL_RAM (xemu-macos 0.16.1): the Vulkan renderer draws, but
 * guest RAM stays exactly as the null renderer leaves it, so a run's RAM
 * digests are a null run's and pictures come from the same guest state.
 * The null renderer writes each zpass report as 0 at GET_REPORT, writes no
 * rendered pixels back to RAM and runs no blits; so does this mode (the
 * report is still queried, for the GPU counters' occlusion-samples).
 * "request" also skips draws, as the null renderer does, except in the VM
 * run after QMP xemu-render-next (pgraph.c), which starts drawing at the
 * next flip; a skip drops all renderer state, as a load does, and so does
 * the start of drawing. Both modes ("1" and "request") replace the op
 * table with the wrapped ops (null_ram_setup); runs without the variable
 * keep the plain table.
 */
static PGRAPHRenderer pgraph_vk_renderer;
static PGRAPHRenderer pgraph_vk_null_ram_renderer;

/* 0: off, 1: every frame drawn ("1"), 2: draws on request ("request") */
static int null_ram_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("XEMU_RENDER_NULL_RAM");
        if (e && !strcmp(e, "1")) {
            mode = 1;
        } else if (e && !strcmp(e, "request")) {
            mode = 2;
        } else {
            mode = 0;
            if (e && *e && strcmp(e, "0")) {
                fprintf(stderr, "xemu: ignoring XEMU_RENDER_NULL_RAM=%s "
                        "(use 1 or request)\n", e);
            }
        }
    }
    return mode;
}

/* Renderer state from before a skip, or kept while skipping, is stale. */
static void null_ram_drop_state(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(pg, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        pg->texture_dirty[i] = true;
    }
    pgraph_mark_all_state_dirty(pg);
}

/* Whether this op skips; a skip asked for takes effect at once. */
static bool null_ram_skip(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (unlikely(!r->render_skipping &&
                 qatomic_read(&d->pgraph.render_skip))) {
        r->render_skipping = true;
        null_ram_drop_state(d);
    }
    return r->render_skipping;
}

/* pgraph_vk_draw_begin's nop-draw test: nothing would be written. */
static bool null_ram_is_nop_draw(PGRAPHState *pg)
{
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool color_write = control_0 & (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
                                    NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                                    NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                                    NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    return !(color_write || depth_test || stencil_test);
}

static void null_ram_draw_begin(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (!null_ram_skip(d)) {
        pgraph_vk_draw_begin(d);
    } else {
        r->skip_nop_draw = null_ram_is_nop_draw(&d->pgraph);
    }
}

static void null_ram_draw_end(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (!null_ram_skip(d)) {
        pgraph_vk_draw_end(d);
    } else if (!r->skip_nop_draw) {
        /*
         * A skipped draw still carries its provoking vertex's oFog into the
         * next draw, as pgraph_vk_draw_end does after its flush, so the
         * first requested frame starts from an every-frame run's value.
         */
        pgraph_vsh_carryover_update(&d->pgraph);
    }
}

static void null_ram_flush_draw(NV2AState *d)
{
    if (!null_ram_skip(d)) {
        pgraph_vk_flush_draw(d);
    }
}

static void null_ram_clear_surface(NV2AState *d, uint32_t parameter)
{
    if (!null_ram_skip(d)) {
        pgraph_vk_clear_surface(d, parameter);
    }
}

static void null_ram_surface_update(NV2AState *d, bool upload,
                                    bool color_write, bool zeta_write)
{
    if (!null_ram_skip(d)) {
        pgraph_vk_surface_update(d, upload, color_write, zeta_write);
    }
}

static void null_ram_clear_report_value(NV2AState *d)
{
    if (!null_ram_skip(d)) {
        pgraph_vk_clear_report_value(d);
    }
}

static void null_ram_get_report(NV2AState *d, uint32_t parameter)
{
    /* As the null renderer: 0, in RAM at once. */
    pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
    if (!null_ram_skip(d)) {
        pgraph_vk_get_report(d, parameter);
    }
}

static void null_ram_image_blit(NV2AState *d)
{
    /* As the null renderer: no blit reaches RAM. */
}

static void pgraph_vk_flip_stall(NV2AState *d);

static void null_ram_flip_stall(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (!null_ram_skip(d)) {
        pgraph_vk_flip_stall(d);
        qatomic_inc(&d->pgraph.render_drawn_flips);
        return;
    }
    if (!qatomic_read(&d->pgraph.render_skip)) {
        /* xemu-render-next: draw from the next frame on, a whole frame. */
        r->render_skipping = false;
        null_ram_drop_state(d);
    }
    /*
     * A skipped flip still counts as one for what pgraph_vk_flip_stall
     * does besides drawing and presenting: the KosmicKrisp start window
     * closes after its 600 flips (or a crash hours later is a strike),
     * pipelines built in a short render window still get saved, and nsprof
     * counts the guest's frames.
     */
    pgraph_vk_maybe_save_pipeline_cache(&d->pgraph);
    pgraph_vk_driver_flip();
    nsprof_flip_tick();
}

static void null_ram_setup(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    int mode = null_ram_mode();

    r->null_ram = mode != 0;
    if (!mode) {
        return;
    }
    PGRAPHRenderer *nr = &pgraph_vk_null_ram_renderer;
    *nr = pgraph_vk_renderer;
    nr->ops.draw_begin = null_ram_draw_begin;
    nr->ops.draw_end = null_ram_draw_end;
    nr->ops.flush_draw = null_ram_flush_draw;
    nr->ops.clear_surface = null_ram_clear_surface;
    nr->ops.surface_update = null_ram_surface_update;
    nr->ops.clear_report_value = null_ram_clear_report_value;
    nr->ops.get_report = null_ram_get_report;
    nr->ops.image_blit = null_ram_image_blit;
    nr->ops.flip_stall = null_ram_flip_stall;
    pg->render_on_request = mode == 2;
    qatomic_set(&pg->render_skip, mode == 2);
    pg->renderer = nr;
    fprintf(stderr, "xemu: render null RAM: %s\n",
            mode == 2 ? "request" : "all");
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .request_report_delivery = pgraph_vk_request_report_delivery,
        .report_guard = pgraph_vk_det_reports_guard,
        .report_sync_read = pgraph_vk_det_reports_sync_read,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
        .get_present_frame = pgraph_vk_get_present_frame,
        .get_present_frame_pushed = pgraph_vk_get_present_frame_pushed,
        .get_present_frame_peek = pgraph_vk_get_present_frame_peek,
        .get_gpu_properties = pgraph_vk_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->memory_budget_extension_enabled) {
        return;
    }

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets =
        g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.95;
    const VkDeviceSize min_alloc_for_trim = 2048ULL * 1024 * 1024;
    bool near_budget = false;

    for (uint32_t i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        if (b->budget == 0) {
            continue;
        }
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %llu/%llu MiB (%.2f%%)", i,
                        (unsigned long long)(b->statistics.allocationBytes / (1024 * 1024)),
                        (unsigned long long)(b->budget / (1024 * 1024)),
                        use_to_budget_ratio * 100);
        near_budget |= (use_to_budget_ratio > budget_threshold &&
                        b->statistics.allocationBytes > min_alloc_for_trim);
    }

    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
}
