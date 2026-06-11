/*
 * NV2A Vulkan wall-time profiler (implementation)
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
};

static struct {
    int64_t total_ns;
    int64_t max_ns;
    uint64_t events;
} counters[NSPROF__COUNT];

static uint64_t flips;
static int64_t interval_start_ns;

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
    flips = 0;
    interval_start_ns = now;
}
