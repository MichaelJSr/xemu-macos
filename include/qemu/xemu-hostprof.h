/*
 * xemu: host-time counters for profiling a run phase by phase
 *
 * Cumulative since launch; QMP query-xemu-host-profile returns them and
 * scripts/bench-det.py --timeline diffs them per boot phase. Each counter
 * has a single writer, the vCPU thread (one vCPU on the Xbox), so plain
 * adds suffice; a reader on another thread may see a value a moment old.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_XEMU_HOSTPROF_H
#define QEMU_XEMU_HOSTPROF_H

#include "qemu/timer.h"

typedef struct XemuHostProf {
    int64_t translate_ns;   /* host ns in tb_gen_code */
    int64_t translations;
    int64_t apu_ns;         /* host ns in the deterministic-mode APU tick */
    int64_t apu_ticks;
    int64_t idle_ns;        /* guest ns an idle vCPU warped past (icount) */
    int64_t idle_warps;
} XemuHostProf;

extern XemuHostProf xemu_hostprof;

/*
 * Times the enclosing scope into translate_ns:
 *     XEMU_HOSTPROF_SCOPE(xemu_hostprof_translate_end);
 * (a cleanup attribute, so every return path counts).
 */
static inline void xemu_hostprof_translate_end(int64_t *t0)
{
    xemu_hostprof.translate_ns += get_clock() - *t0;
    xemu_hostprof.translations++;
}

static inline void xemu_hostprof_apu_end(int64_t *t0)
{
    xemu_hostprof.apu_ns += get_clock() - *t0;
    xemu_hostprof.apu_ticks++;
}

#define XEMU_HOSTPROF_SCOPE(end) \
    int64_t xemu_hostprof_t0 __attribute__((cleanup(end))) = get_clock()

static inline void xemu_hostprof_idle_warp(int64_t guest_ns)
{
    xemu_hostprof.idle_ns += guest_ns;
    xemu_hostprof.idle_warps++;
}

#endif
