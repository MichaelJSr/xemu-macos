/*
 * xemu: gdbstub extensions for automation harnesses
 *
 * Qxemu.stepover:1 (or "xemu.stepover+" in the client's qSupported): a
 * resume from a halt at an armed software breakpoint first executes the
 * instruction under the PC, without reporting that breakpoint again and
 * without lifting it. Clients otherwise lift it, single-step and re-arm
 * (z0/s/Z0/c), and in this fork every Z0 and z0 invalidates the code on the
 * breakpoint's page (xemu_gdb_bp_invalidate). The stub does the same step
 * itself: it single-steps the vCPU with the client's single-step flags (by
 * default no IRQs and no timers, exactly as 's') and, when that step's
 * debug exception arrives, lets the vCPU run on instead of stopping the
 * VM. Off by default and for each new connection.
 *
 * qxemu.read:addr,len;addr,len;...: many 'm' reads in one round trip. The
 * reply holds one field per region, ';'-separated, each exactly what 'm'
 * would answer: the bytes in hex, the readable prefix when the region runs
 * into an unmapped page, or E14 when none of it is readable.
 *
 * Memory packets (m, M, x, qxemu.read, and Qqemu.PhyMemMode:1) reach RAM
 * and ROM only: device MMIO, or an address nothing backs, fails as an
 * unmapped page does, without touching the device. Always on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/ctype.h"
#include "qemu/cutils.h"
#include "exec/cpu-common.h"
#include "exec/gdbstub.h"
#include "exec/replay-core.h"
#include "exec/target_page.h"
#include "gdbstub/commands.h"
#include "gdbstub/xemu.h"
#include "hw/core/cpu.h"
#include "qemu/rcu.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "internals.h"

static bool stepover_on;
/* vCPUs (by cpu_index) in an internal step over a breakpoint. BQL. */
static uint64_t stepover_pending;

static bool stepover_test_and_clear(CPUState *cpu)
{
    uint64_t bit;

    if (cpu->cpu_index >= 64) {
        return false;
    }
    bit = 1ull << cpu->cpu_index;
    if (!(stepover_pending & bit)) {
        return false;
    }
    stepover_pending &= ~bit;
    return true;
}

bool xemu_gdb_stepover_done(CPUState *cpu)
{
    if (!stepover_test_and_clear(cpu)) {
        return false;
    }
    /* A watchpoint hit in the stepped instruction is reported as usual. */
    if (!cpu->singlestep_enabled || cpu->watchpoint_hit) {
        return false;
    }
    cpu_single_step(cpu, 0);
    return true;
}

void xemu_gdb_stepover_cancel(CPUState *cpu)
{
    if (stepover_test_and_clear(cpu)) {
        cpu_single_step(cpu, 0);
    }
}

/*
 * Every resume, whoever asks for it (c, vCont;c, QMP cont or
 * xemu-frame-advance): a vCPU halted on one of the client's breakpoints
 * steps over it first. 's' already sets single-step, which runs the
 * instruction under a breakpoint, so it is left alone. A stop before the
 * step finished undoes it; the next resume starts it again.
 */
static void xemu_gdb_vm_state_change(void *opaque, bool running,
                                     RunState state)
{
    CPUState *cpu;

    if (!running) {
        CPU_FOREACH(cpu) {
            xemu_gdb_stepover_cancel(cpu);
        }
        return;
    }
    if (!stepover_on || replay_mode != REPLAY_MODE_NONE || !tcg_enabled()) {
        return;
    }
    CPU_FOREACH(cpu) {
        if (cpu->cpu_index >= 64 || cpu->singlestep_enabled ||
            !cpu->cc->get_pc ||
            !cpu_breakpoint_test(cpu, cpu->cc->get_pc(cpu), BP_GDB)) {
            continue;
        }
        stepover_pending |= 1ull << cpu->cpu_index;
        cpu_single_step(cpu, gdbserver_state.sstep_flags | SSTEP_ENABLE);
    }
}

void xemu_gdb_session_reset(void)
{
    stepover_on = false;
}

void xemu_gdb_client_features(const char *features)
{
    g_auto(GStrv) f = g_strsplit(features, ";", -1);

    if (g_strv_contains((const gchar * const *)f, "xemu.stepover+")) {
        stepover_on = true;
    }
}

static void handle_set_xemu_stepover(GArray *params, void *user_ctx)
{
    unsigned long v = params->len ? gdb_get_cmd_param(params, 0)->val_ul : 2;

    if (v > 1) {
        gdb_put_packet("E22");
        return;
    }
    stepover_on = v;
    gdb_put_packet("OK");
}

static void handle_query_xemu_stepover(GArray *params, void *user_ctx)
{
    gdb_put_packet(stepover_on ? "1" : "0");
}

static const GdbCmdParseEntry xemu_set_stepover = {
    .handler = handle_set_xemu_stepover,
    .cmd = "xemu.stepover:",
    .cmd_startswith = true,
    .schema = "l0",
};

static const GdbCmdParseEntry xemu_query_stepover = {
    .handler = handle_query_xemu_stepover,
    .cmd = "xemu.stepover",
};

typedef struct XemuReadRegion {
    uint64_t addr, len;
} XemuReadRegion;

static void handle_query_xemu_read(GArray *params, void *user_ctx)
{
    const char *p = params->len ? gdb_get_cmd_param(params, 0)->data : "";
    g_autoptr(GArray) regions = g_array_new(false, false,
                                            sizeof(XemuReadRegion));
    GString *out = gdbserver_state.str_buf;
    uint64_t total = 0;

    /* Parse everything first: a malformed request gets E22 alone. */
    do {
        XemuReadRegion r;

        if (!qemu_isxdigit(p[0]) || qemu_strtou64(p, &p, 16, &r.addr) ||
            *p != ',' || !qemu_isxdigit(p[1]) ||
            qemu_strtou64(p + 1, &p, 16, &r.len) || r.len == 0 ||
            r.len > XEMU_GDB_MAX_READ - total || (*p && *p != ';')) {
            gdb_put_packet("E22");
            return;
        }
        total += r.len;
        g_array_append_val(regions, r);
        if (*p == ';') {
            p++;
        }
    } while (*p);

    /* Each field exactly as 'm' answers it (handle_read_mem). */
    g_string_truncate(out, 0);
    for (guint i = 0; i < regions->len; i++) {
        XemuReadRegion *r = &g_array_index(regions, XemuReadRegion, i);
        size_t done = gdb_read_mem_prefix(r->addr, r->len);

        if (i) {
            g_string_append_c(out, ';');
        }
        if (done) {
            gdb_memtohex(out, gdbserver_state.mem_buf->data, done);
            g_string_truncate(out, out->len - 1);   /* its trailing NUL */
        } else {
            g_string_append(out, "E14");
        }
    }
    gdb_put_strbuf();
}

static const GdbCmdParseEntry xemu_query_read = {
    .handler = handle_query_xemu_read,
    .cmd = "xemu.read:",
    .cmd_startswith = true,
    .schema = "s0",
};

/*
 * True when RAM or a ROM fully backs [addr, addr + len) of @as, so a
 * debugger access is a plain copy. Device MMIO has side effects (reads that
 * clear status, FIFO doorbells) and many of xemu's device models assert on
 * accesses a guest never makes; an address nothing backs counts as MMIO.
 */
static bool phys_range_is_memory(AddressSpace *as, hwaddr addr, hwaddr len,
                                 MemTxAttrs attrs, bool is_write)
{
    RCU_READ_LOCK_GUARD();
    while (len) {
        hwaddr xlat, l = len;
        MemoryRegion *mr = address_space_translate(as, addr, &xlat, &l,
                                                   is_write, attrs);

        if (!l || !memory_region_supports_direct_access(mr)) {
            return false;
        }
        addr += l;
        len -= l;
    }
    return true;
}

/*
 * As cpu_memory_rw_debug(), page by page through @cpu's page tables, but
 * only RAM or ROM may back each page's range. That check and the copy
 * share one walk of the page tables. With no @buf, only checks.
 */
static int virt_rw(CPUState *cpu, vaddr addr, uint8_t *buf, size_t len,
                   bool is_write)
{
    size_t page = qemu_target_page_size();

    cpu_synchronize_state(cpu);
    while (len) {
        vaddr base = addr & ~(vaddr)(page - 1);
        size_t l = MIN(len, base + page - addr);
        MemTxAttrs attrs;
        hwaddr pa = cpu_get_phys_page_attrs_debug(cpu, base, &attrs);
        AddressSpace *as;

        if (pa == -1) {
            return -1;
        }
        as = cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu, attrs));
        pa += addr - base;
        if (!phys_range_is_memory(as, pa, l, attrs, is_write) ||
            (buf && address_space_rw(as, pa, attrs, buf, l, is_write) !=
                    MEMTX_OK)) {
            return -1;
        }
        if (buf) {
            buf += l;
        }
        addr += l;
        len -= l;
    }
    return 0;
}

int xemu_gdb_memory_rw_debug(CPUState *cpu, vaddr addr, uint8_t *buf,
                             size_t len, bool phys, bool is_write)
{
    if (phys) {
        if (!phys_range_is_memory(&address_space_memory, addr, len,
                                  MEMTXATTRS_UNSPECIFIED, is_write)) {
            return -1;
        }
        if (is_write) {
            cpu_physical_memory_write(addr, buf, len);
        } else {
            cpu_physical_memory_read(addr, buf, len);
        }
        return 0;
    }
    if (cpu->cc->memory_rw_debug) {
        /* The target's own debug access decides. */
        return cpu->cc->memory_rw_debug(cpu, addr, buf, len, is_write);
    }
    /* A write is checked whole first, so none of it lands unless all can. */
    if (is_write && virt_rw(cpu, addr, NULL, len, true)) {
        return -1;
    }
    return virt_rw(cpu, addr, buf, len, is_write);
}

void xemu_gdb_init(void)
{
    g_autoptr(GPtrArray) queries = g_ptr_array_new();
    g_autoptr(GPtrArray) sets = g_ptr_array_new();

    g_ptr_array_add(queries, (gpointer)&xemu_query_stepover);
    g_ptr_array_add(queries, (gpointer)&xemu_query_read);
    g_ptr_array_add(sets, (gpointer)&xemu_set_stepover);
    gdb_extend_query_table(queries);
    gdb_extend_set_table(sets);
    gdb_extend_qsupported_features((char *)";Qxemu.stepover+;qxemu.read+");
    qemu_add_vm_change_state_handler(xemu_gdb_vm_state_change, NULL);
}
