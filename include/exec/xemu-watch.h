/*
 * xemu: guest memory access log (QMP xemu-watch-start/-drain/-stop)
 *
 * Logs CPU accesses to watched ranges without stopping the VM. A watched
 * page gets TLB_WATCHPOINT in its TLB entries, as a gdb watchpoint's does,
 * so its accesses take the slow path, where cputlb.c calls
 * xemu_watch_check next to the MemAccessCallback check. Implementation:
 * accel/tcg/xemu-watch.c.
 *
 * Copyright (c) 2026 xemu-macos contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_WATCH_H
#define XEMU_WATCH_H

#if defined(XBOX)
#include "qemu/atomic.h"
#include "exec/hwaddr.h"
#include "exec/vaddr.h"

/* Set while a log runs; only the vCPU thread writes it. */
extern bool xemu_watch_active;

int xemu_watch_page_flags(CPUState *cpu, vaddr page, hwaddr ppage,
                          uintptr_t host_page);

/*
 * TLB refill of @page (physical @ppage, host @host_page or 0 for I/O):
 * BP_MEM_READ/BP_MEM_WRITE for the accesses a watched range wants to see.
 * Inline: one atomic load and branch per refill while no log runs (it
 * changes tlb_set_page_full's PGO profile hash until the next retrain).
 */
static inline int xemu_watch_tlb_flags(CPUState *cpu, vaddr page,
                                       hwaddr ppage, uintptr_t host_page)
{
    if (likely(!qatomic_read(&xemu_watch_active))) {
        return 0;
    }
    return xemu_watch_page_flags(cpu, page, ppage, host_page);
}

/*
 * An access to a TLB_WATCHPOINT page: @flags BP_MEM_READ and/or
 * BP_MEM_WRITE, @full and @tlb_flags its TLB entry, @host its host address
 * (meaningless for I/O), @ra the host return address into the TB (or 0).
 * Out of line and unconditional, so that the slow-path callers keep
 * their control flow (and their PGO profile).
 */
void xemu_watch_check(CPUState *cpu, vaddr addr, vaddr len, int flags,
                      CPUTLBEntryFull *full, int tlb_flags, void *host,
                      uintptr_t ra);
#endif

#endif
