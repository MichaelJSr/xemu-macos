/*
 * QEMU TCG vCPU common functionality
 *
 * Functionality common to all TCG vCPU variants: mttcg, rr and icount.
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "system/tcg.h"
#include "system/replay.h"
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "exec/cputlb.h"
#include "exec/hwaddr.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#if defined(XBOX)
#include "exec/target_page.h"
#include "system/memory.h"
#include "qemu/rcu.h"
#endif
#include "exec/watchpoint.h"
#include "gdbstub/enums.h"

#include "hw/core/cpu.h"

#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"
#include "tcg-accel-ops-rr.h"
#include "tcg-accel-ops-icount.h"

/* common functionality among all TCG variants */

void tcg_cpu_init_cflags(CPUState *cpu, bool parallel)
{
    uint32_t cflags;

    /*
     * Include the cluster number in the hash we use to look up TBs.
     * This is important because a TB that is valid for one cluster at
     * a given physical address and set of CPU flags is not necessarily
     * valid for another:
     * the two clusters may have different views of physical memory, or
     * may have different CPU features (eg FPU present or absent).
     */
    cflags = cpu->cluster_index << CF_CLUSTER_SHIFT;

    cflags |= parallel ? CF_PARALLEL : 0;
    cflags |= icount_enabled() ? CF_USE_ICOUNT : 0;
    tcg_cflags_set(cpu, cflags);
}

void tcg_cpu_destroy(CPUState *cpu)
{
    cpu_thread_signal_destroyed(cpu);
}

int tcg_cpu_exec(CPUState *cpu)
{
    int ret;
    assert(tcg_enabled());
    cpu_exec_start(cpu);
    ret = cpu_exec(cpu);
    cpu_exec_end(cpu);

    return ret;
}

static void tcg_cpu_reset_hold(CPUState *cpu)
{
    tcg_flush_jmp_cache(cpu);

    tlb_flush_all_cpus_synced(cpu);
}

/* mask must never be zero, except for A20 change call */
void tcg_handle_interrupt(CPUState *cpu, int mask)
{
    cpu_set_interrupt(cpu, mask);

    /*
     * If called from iothread context, wake the target cpu in
     * case its halted.
     */
    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    } else {
        qatomic_set(&cpu->neg.icount_decr.u16.high, -1);
    }
}

static bool tcg_supports_guest_debug(void)
{
    return true;
}

/* Translate GDB watchpoint type to a flags value for cpu_watchpoint_* */
static inline int xlat_gdb_type(CPUState *cpu, int gdbtype)
{
    static const int xlat[] = {
        [GDB_WATCHPOINT_WRITE]  = BP_GDB | BP_MEM_WRITE,
        [GDB_WATCHPOINT_READ]   = BP_GDB | BP_MEM_READ,
        [GDB_WATCHPOINT_ACCESS] = BP_GDB | BP_MEM_ACCESS,
    };

    int cputype = xlat[gdbtype];

    if (cpu->cc->gdb_stop_before_watchpoint) {
        cputype |= BP_STOP_BEFORE_ACCESS;
    }
    return cputype;
}

#if defined(XBOX)
/*
 * xemu: a gdb breakpoint must invalidate the code it sits in. Upstream
 * checks breakpoints only where a TB is looked up and gets away without
 * invalidating because goto_tb never leaves a page, so every arrival in a
 * page passes a lookup. This fork enters a page without one through
 * cross-page goto_tb chains (xpage), the inline jump-cache probe and the
 * inline return memo, so a breakpoint on warm code could take hundreds of
 * frames to fire. Invalidating the TBs on the breakpoint's physical page
 * (tb_invalidate_phys_range is page-wide in this fork; the byte alone
 * would do) unlinks the chains into them and fails both probes (CF_INVALID,
 * and under CF_PCREL the jump cache and return memo are cleared). Removal
 * does the same, dropping links made to single-instruction BP-page TBs
 * while it was armed. A page with no mapping now could still hold TBs from
 * an earlier mapping: flush everything then. gdb re-inserts breakpoints on
 * each resume, so each step pays one page invalidation.
 */
static void xemu_gdb_bp_invalidate(CPUState *cpu, vaddr pc)
{
    MemTxAttrs attrs;
    hwaddr page = cpu_get_phys_page_attrs_debug(cpu, pc & TARGET_PAGE_MASK,
                                                &attrs);
    ram_addr_t ra = RAM_ADDR_INVALID;

    if (page == -1) {
        queue_tb_flush(cpu);
        return;
    }
    WITH_RCU_READ_LOCK_GUARD() {
        AddressSpace *as =
            cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu, attrs));
        hwaddr xlat, len = 1;
        MemoryRegion *mr = address_space_translate(
            as, page | (pc & ~TARGET_PAGE_MASK), &xlat, &len, false, attrs);
        if (memory_region_is_ram(mr) || memory_region_is_romd(mr)) {
            ra = memory_region_get_ram_addr(mr) + xlat;
        }
    }
    /* MMIO code runs from one-shot TBs (phys_pc == -1): nothing cached. */
    if (ra != RAM_ADDR_INVALID) {
        tb_invalidate_phys_range(NULL, ra, ra);
    }
}
#endif

static int tcg_insert_breakpoint(CPUState *cs, int type, vaddr addr, vaddr len)
{
    CPUState *cpu;
    int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        CPU_FOREACH(cpu) {
            err = cpu_breakpoint_insert(cpu, addr, BP_GDB, NULL);
            if (err) {
                break;
            }
        }
#if defined(XBOX)
        if (!err) {
            xemu_gdb_bp_invalidate(cs, addr);
        }
#endif
        return err;
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_insert(cpu, addr, len,
                                        xlat_gdb_type(cpu, type), NULL);
            if (err) {
                break;
            }
        }
        return err;
    default:
        return -ENOSYS;
    }
}

static int tcg_remove_breakpoint(CPUState *cs, int type, vaddr addr, vaddr len)
{
    CPUState *cpu;
    int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        CPU_FOREACH(cpu) {
            err = cpu_breakpoint_remove(cpu, addr, BP_GDB);
            if (err) {
                break;
            }
        }
        #if defined(XBOX)
        if (!err) {
            xemu_gdb_bp_invalidate(cs, addr);
        }
#endif
        return err;
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_remove(cpu, addr, len,
                                        xlat_gdb_type(cpu, type));
            if (err) {
                break;
            }
        }
        return err;
    default:
        return -ENOSYS;
    }
}

static inline void tcg_remove_all_breakpoints(CPUState *cpu)
{
#if defined(XBOX)
    CPUBreakpoint *bp;
    QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
        if (bp->flags & BP_GDB) {
            xemu_gdb_bp_invalidate(cpu, bp->pc);
        }
    }
#endif
    cpu_breakpoint_remove_all(cpu, BP_GDB);
    cpu_watchpoint_remove_all(cpu, BP_GDB);
}

static void tcg_accel_ops_init(AccelClass *ac)
{
    AccelOpsClass *ops = ac->ops;

    if (qemu_tcg_mttcg_enabled()) {
        ops->create_vcpu_thread = mttcg_start_vcpu_thread;
        ops->kick_vcpu_thread = tcg_kick_vcpu_thread;
        ops->handle_interrupt = tcg_handle_interrupt;
    } else {
        ops->create_vcpu_thread = rr_start_vcpu_thread;
        ops->kick_vcpu_thread = rr_kick_vcpu_thread;

        if (icount_enabled()) {
            ops->handle_interrupt = icount_handle_interrupt;
            ops->get_virtual_clock = icount_get;
            ops->get_elapsed_ticks = icount_get;
        } else {
            ops->handle_interrupt = tcg_handle_interrupt;
        }
    }

    ops->cpu_reset_hold = tcg_cpu_reset_hold;
    ops->supports_guest_debug = tcg_supports_guest_debug;
    ops->insert_breakpoint = tcg_insert_breakpoint;
    ops->remove_breakpoint = tcg_remove_breakpoint;
    ops->remove_all_breakpoints = tcg_remove_all_breakpoints;
}

static void tcg_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->ops_init = tcg_accel_ops_init;
}

static const TypeInfo tcg_accel_ops_type = {
    .name = ACCEL_OPS_NAME("tcg"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = tcg_accel_ops_class_init,
    .abstract = true,
};
module_obj(ACCEL_OPS_NAME("tcg"));

static void tcg_accel_ops_register_types(void)
{
    type_register_static(&tcg_accel_ops_type);
}
type_init(tcg_accel_ops_register_types);
