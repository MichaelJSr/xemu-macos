/*
 * xemu: gdbstub extensions for automation harnesses (gdbstub/xemu-gdb.c)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GDBSTUB_XEMU_H
#define GDBSTUB_XEMU_H

#include "exec/vaddr.h"

/*
 * cpu_handle_guest_debug(): true when this debug exception ends an internal
 * step over the breakpoint under the PC (Qxemu.stepover), so the vCPU runs
 * on instead of stopping the VM.
 */
bool xemu_gdb_stepover_done(CPUState *cpu);

/*
 * tcg_insert_breakpoint() and friends: a gdb breakpoint (BP_GDB) at @pc
 * is a guest PC hook site whose hook stops the VM before the instruction
 * with EXCP_DEBUG (target/i386/tcg/system/xemu-hooks.c). Insert once per
 * cpu_breakpoint_insert round, remove once per removal; the first insert
 * at a PC invalidates its page, nothing else does. BQL held.
 */
void xemu_gdb_bp_site_insert(vaddr pc);
void xemu_gdb_bp_site_remove(vaddr pc);
void xemu_gdb_bp_site_remove_all(void);

/* The rest is for the system gdbstub only. */

/* Register the packets and the run-state handler (once, at server start). */
void xemu_gdb_init(void);

/* A client connected or went away: every extension is off again. */
void xemu_gdb_session_reset(void);

/* The client's qSupported feature list ("xemu.stepover+" turns it on). */
void xemu_gdb_client_features(const char *features);

/*
 * vCont left @cpu stopped or single-stepping on the client's behalf: it
 * takes no internal step over a breakpoint on this resume.
 */
void xemu_gdb_stepover_cancel(CPUState *cpu);

/*
 * gdb_target_memory_rw_debug(): the access as cpu_memory_rw_debug() (or,
 * with @phys, Qqemu.PhyMemMode:1, cpu_physical_memory_read/write()) makes
 * it, but -1, with no device access, when [addr, addr + len) touches
 * anything but RAM or ROM (device MMIO, nothing at all). A failed write
 * writes nothing.
 */
int xemu_gdb_memory_rw_debug(CPUState *cpu, vaddr addr, uint8_t *buf,
                             size_t len, bool phys, bool is_write);

#endif /* GDBSTUB_XEMU_H */
