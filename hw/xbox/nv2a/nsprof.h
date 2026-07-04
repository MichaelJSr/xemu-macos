/*
 * NV2A wall-time profiler (release-build friendly)
 *
 * The NV2A_PROF_* counters are stripped in release builds
 * (NV2A_STRIP_PROFILE_COUNTERS=1) and count events, not time. This is
 * a minimal nanosecond accumulator for the PFIFO-thread costs that
 * drive optimization decisions: shader/pipeline creation (compile
 * stutter), texture hashing/uploads, vertex RAM copies, GPU fence
 * waits, surface readbacks, and the guest flip -> vblank idle gap.
 *
 * Enable with XEMU_NV2A_NSPROF=1; a summary (total / avg-per-flip /
 * max single event) prints to stderr every ~5 s. All instrumented
 * paths run on the PFIFO thread, so accumulation is unsynchronized.
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
#ifndef HW_XBOX_NV2A_NSPROF_H
#define HW_XBOX_NV2A_NSPROF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum NsprofCounter {
    NSPROF_SHADER_GEN,    /* shader cache miss: GLSL gen + SPIR-V compile */
    NSPROF_PIPELINE_GEN,  /* pipeline cache miss: vkCreateGraphicsPipelines */
    NSPROF_TEX_UPLOAD,    /* texture upload: layout + copy + unswizzle */
    NSPROF_TEX_HASH,      /* texture content hashing (dirty checks) */
    NSPROF_TEX_SNAPSHOT,  /* guest VRAM snapshot memcpy before upload */
    NSPROF_GEOM_UPDATE,   /* vertex RAM / inline / index buffer copies */
    NSPROF_FENCE_WAIT,    /* vkWaitForFences reclaiming a flight slot */
    NSPROF_AUX_FENCE_WAIT,/* aux CB fence: sync end + lazy async reclaim */
    NSPROF_MFX_DRAIN,     /* metalfx_drain_inflight CPU spin (PFIFO) */
    NSPROF_SURF_DOWNLOAD, /* GPU->CPU surface readback */
    NSPROF_FLIP_IDLE,     /* FLIP_STALL -> guest vblank release */
    NSPROF__COUNT,
};

/*
 * Plain event counts (no timing). The FINISH_* entries mirror
 * FinishReason in pgraph/vk/renderer.h by value so call sites can
 * offset from NSPROF_EV_FINISH_BASE.
 */
enum NsprofEvent {
    NSPROF_EV_FINISH_VERTEX_BUFFER_DIRTY,
    NSPROF_EV_FINISH_SURFACE_CREATE,
    NSPROF_EV_FINISH_SURFACE_DOWN,
    NSPROF_EV_FINISH_NEED_BUFFER_SPACE,
    NSPROF_EV_FINISH_PRESENTING,
    NSPROF_EV_FINISH_FLIP_STALL,
    NSPROF_EV_FINISH_FLUSH,
    NSPROF_EV_FINISH_STALLED,
    NSPROF_EV_FINISH_REPORTS_FULL,
    NSPROF_EV_DRAW, /* draw_end (guest begin/end pairs) */
    /* surface download trigger sites */
    NSPROF_EV_SDOWN_ACCESS_R, /* vCPU read of dirty surface VRAM */
    NSPROF_EV_SDOWN_ACCESS_W, /* vCPU write of dirty surface VRAM */
    NSPROF_EV_SDOWN_VTXRAM,   /* vertex RAM sync overlaps surface */
    NSPROF_EV_SDOWN_TEXBIND,  /* texture bind indexes surface VRAM */
    NSPROF_EV_SDOWN_BLIT,     /* NV097 image blit src/dst readback */
    NSPROF_EV_SDOWN_EVICT,    /* surface eviction / invalidation */
    NSPROF_EV_SDOWN_INCOMPAT, /* surface shape change: evict + readback */
    NSPROF_EV_SDOWN_FLUSH,    /* renderer flush / savestate readback */
    NSPROF_EV_SUPLOAD_COLOR,  /* RAM -> color surface upload */
    NSPROF_EV_SUPLOAD_ZETA,   /* RAM -> zeta surface upload */
    NSPROF_EV_TEXBIND_SKIP,   /* once-per-frame verified-bind fast path */
    NSPROF_EV_VTX_EXACT_SKIP, /* byte-identical vertex conflict, finish skipped */
    NSPROF_EV__COUNT,
};
#define NSPROF_EV_FINISH_BASE NSPROF_EV_FINISH_VERTEX_BUFFER_DIRTY

bool nsprof_enabled(void);

/* Returns a start timestamp, or -1 when disabled. */
int64_t nsprof_begin(void);
void nsprof_end(enum NsprofCounter c, int64_t t0);

void nsprof_event(enum NsprofEvent e);

/* FLIP_STALL -> vblank-release idle gap (both on the PFIFO thread). */
void nsprof_flip_wait_begin(void);
void nsprof_flip_wait_end(void);

/* Called once per guest flip (PFIFO thread); prints the periodic
 * summary when enabled. */
void nsprof_flip_tick(void);

#ifdef __cplusplus
}
#endif

#endif
