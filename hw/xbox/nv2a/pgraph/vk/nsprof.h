/*
 * NV2A Vulkan wall-time profiler (release-build friendly)
 *
 * The NV2A_PROF_* counters are stripped in release builds
 * (NV2A_STRIP_PROFILE_COUNTERS=1) and count events, not time. This is
 * a minimal nanosecond accumulator for the handful of PFIFO-thread
 * costs that drive optimization decisions: shader/pipeline creation
 * (compile stutter), texture hashing/uploads, and vertex RAM copies.
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
#ifndef HW_XBOX_NV2A_PGRAPH_VK_NSPROF_H
#define HW_XBOX_NV2A_PGRAPH_VK_NSPROF_H

#include <stdbool.h>
#include <stdint.h>

enum NsprofCounter {
    NSPROF_SHADER_GEN,   /* shader cache miss: GLSL gen + SPIR-V compile */
    NSPROF_PIPELINE_GEN, /* pipeline cache miss: vkCreateGraphicsPipelines */
    NSPROF_TEX_UPLOAD,   /* texture upload: layout + copy + unswizzle */
    NSPROF_TEX_HASH,     /* texture content hashing (dirty checks) */
    NSPROF_TEX_SNAPSHOT, /* guest VRAM snapshot memcpy before upload */
    NSPROF_GEOM_UPDATE,  /* vertex RAM / inline / index buffer copies */
    NSPROF__COUNT,
};

bool nsprof_enabled(void);

/* Returns a start timestamp, or -1 when disabled. */
int64_t nsprof_begin(void);
void nsprof_end(enum NsprofCounter c, int64_t t0);

/* Called once per guest flip (PFIFO thread); prints the periodic
 * summary when enabled. */
void nsprof_flip_tick(void);

#endif
