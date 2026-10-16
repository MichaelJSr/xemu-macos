/*
 * xemu: which kinds of breakpoint a vCPU has armed
 *
 * A gdb breakpoint (BP_GDB) is a guest PC hook site whose TB raises the
 * stop however it is entered (accel/tcg/tcg-accel-ops.c), so fast paths
 * that enter TBs without check_for_breakpoints() (the return memo, the
 * icount step cut, cross-page links) need to step aside only for DR7
 * breakpoints (BP_CPU), which still run their page one instruction per
 * TB.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_XEMU_BREAKPOINTS_H
#define EXEC_XEMU_BREAKPOINTS_H

#include "hw/core/cpu.h"

/* True when @cpu has a DR7 (BP_CPU) breakpoint armed. */
static inline bool xemu_cpu_breakpoints(CPUState *cpu)
{
    CPUBreakpoint *bp;

    if (likely(QTAILQ_EMPTY(&cpu->breakpoints))) {
        return false;
    }
    QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
        if (bp->flags & BP_CPU) {
            return true;
        }
    }
    return false;
}

#endif
