/*
 * NV2A wall-time profiler (implementation)
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

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include <stdio.h>
#include <stdlib.h>

#include "nsprof.h"

static const char *const counter_names[NSPROF__COUNT] = {
    [NSPROF_SHADER_GEN] = "shader_gen",
    [NSPROF_PIPELINE_GEN] = "pipeline_gen",
    [NSPROF_TEX_UPLOAD] = "tex_upload",
    [NSPROF_TEX_HASH] = "tex_hash",
    [NSPROF_TEX_SNAPSHOT] = "tex_snapshot",
    [NSPROF_GEOM_UPDATE] = "geom_update",
    [NSPROF_FENCE_WAIT] = "fence_wait",
    [NSPROF_AUX_FENCE_WAIT] = "aux_fence",
    [NSPROF_MFX_DRAIN] = "mfx_drain",
    [NSPROF_SURF_DOWNLOAD] = "surf_down",
    [NSPROF_FLIP_IDLE] = "flip_idle",
    [NSPROF_PRESENT_WAIT] = "present_wait",
    [NSPROF_ZETA_SNAPSHOT] = "zeta_snap",
};

static const char *const event_names[NSPROF_EV__COUNT] = {
    [NSPROF_EV_FINISH_VERTEX_BUFFER_DIRTY] = "finish_vtx_dirty",
    [NSPROF_EV_FINISH_SURFACE_CREATE] = "finish_surf_create",
    [NSPROF_EV_FINISH_SURFACE_DOWN] = "finish_surf_down",
    [NSPROF_EV_FINISH_NEED_BUFFER_SPACE] = "finish_buf_space",
    [NSPROF_EV_FINISH_PRESENTING] = "finish_present",
    [NSPROF_EV_FINISH_FLIP_STALL] = "finish_flip_stall",
    [NSPROF_EV_FINISH_FLUSH] = "finish_flush",
    [NSPROF_EV_FINISH_STALLED] = "finish_stalled",
    [NSPROF_EV_FINISH_REPORTS_FULL] = "finish_reports_full",
    [NSPROF_EV_FINISH_REPORTS_SUBMIT] = "finish_reports_submit",
    [NSPROF_EV_DRAW] = "draws",
    [NSPROF_EV_SDOWN_ACCESS_R] = "sdown_access_r",
    [NSPROF_EV_SDOWN_ACCESS_W] = "sdown_access_w",
    [NSPROF_EV_SDOWN_VTXRAM] = "sdown_vtxram",
    [NSPROF_EV_SDOWN_TEXBIND] = "sdown_texbind",
    [NSPROF_EV_SDOWN_BLIT] = "sdown_blit",
    [NSPROF_EV_SDOWN_EVICT] = "sdown_evict",
    [NSPROF_EV_SDOWN_INCOMPAT] = "sdown_incompat",
    [NSPROF_EV_SDOWN_FLUSH] = "sdown_flush",
    [NSPROF_EV_SUPLOAD_COLOR] = "supload_color",
    [NSPROF_EV_SUPLOAD_ZETA] = "supload_zeta",
    [NSPROF_EV_TEXBIND_SKIP] = "texbind_skip",
    [NSPROF_EV_VTX_EXACT_SKIP] = "vtx_exact_skip",
    [NSPROF_EV_RENDERPASS] = "renderpass",
    [NSPROF_EV_PIPELINE_BIND] = "pipeline_bind",
    [NSPROF_EV_VK_DRAW_CALL] = "vk_draw_call",
    [NSPROF_EV_DRAW_ARRAYS_MULTI_SUBRANGE] = "da_multi_subrange",
    [NSPROF_EV_DRAW_MERGE_IDENTICAL] = "merge_identical",
    [NSPROF_EV_DRAW_MERGE_CANDIDATE] = "merge_candidate",
    [NSPROF_EV_DRAW_MERGE_CAND_UNIF_DIFF] = "merge_cand_udiff",
    [NSPROF_EV_DRAW_STATE_CHANGED] = "merge_state_changed",
    [NSPROF_EV_RENDERPASS_CAUSE_SURFACE] = "rpcause_surface",
    [NSPROF_EV_RENDERPASS_CAUSE_CLEAR] = "rpcause_clear",
    [NSPROF_EV_RENDERPASS_CAUSE_TEXUPLOAD] = "rpcause_texupload",
    [NSPROF_EV_RENDERPASS_CAUSE_OTHER] = "rpcause_other",
};

static struct {
    int64_t total_ns;
    int64_t max_ns;
    uint64_t events;
} counters[NSPROF__COUNT];

static uint64_t events[NSPROF_EV__COUNT];

static uint64_t flips;
static int64_t interval_start_ns;
static int64_t flip_wait_start_ns = -1;

bool nsprof_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("XEMU_NV2A_NSPROF");
        cached = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

int64_t nsprof_begin(void)
{
    if (!nsprof_enabled()) {
        return -1;
    }
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

void nsprof_end(enum NsprofCounter c, int64_t t0)
{
    if (t0 < 0) {
        return;
    }
    int64_t dt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    counters[c].total_ns += dt;
    counters[c].events++;
    if (dt > counters[c].max_ns) {
        counters[c].max_ns = dt;
    }
}

void nsprof_event(enum NsprofEvent e)
{
    if (!nsprof_enabled()) {
        return;
    }
    events[e]++;
}

void nsprof_flip_wait_begin(void)
{
    flip_wait_start_ns = nsprof_begin();
}

void nsprof_flip_wait_end(void)
{
    if (flip_wait_start_ns >= 0) {
        nsprof_end(NSPROF_FLIP_IDLE, flip_wait_start_ns);
        flip_wait_start_ns = -1;
    }
}

void nsprof_flip_tick(void)
{
    if (!nsprof_enabled()) {
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!interval_start_ns) {
        interval_start_ns = now;
    }
    flips++;

    if (now - interval_start_ns < 5000000000ll) {
        return;
    }

    double secs = (double)(now - interval_start_ns) / 1e9;
    fprintf(stderr, "nsprof: %.1fs interval, %llu flips (%.1f/s)\n", secs,
            (unsigned long long)flips, (double)flips / secs);
    for (int i = 0; i < NSPROF__COUNT; i++) {
        if (!counters[i].events) {
            continue;
        }
        fprintf(stderr,
                "nsprof:   %-12s ev=%-7llu total=%7.2fms "
                "per_flip=%7.1fus max=%7.1fus\n",
                counter_names[i], (unsigned long long)counters[i].events,
                (double)counters[i].total_ns / 1e6,
                (double)counters[i].total_ns / 1e3 / (double)flips,
                (double)counters[i].max_ns / 1e3);
        counters[i].total_ns = 0;
        counters[i].max_ns = 0;
        counters[i].events = 0;
    }
    for (int i = 0; i < NSPROF_EV__COUNT; i++) {
        if (!events[i]) {
            continue;
        }
        fprintf(stderr, "nsprof:   %-19s ev=%-8llu per_flip=%7.1f\n",
                event_names[i], (unsigned long long)events[i],
                (double)events[i] / (double)flips);
        events[i] = 0;
    }
    flips = 0;
    interval_start_ns = now;
}
