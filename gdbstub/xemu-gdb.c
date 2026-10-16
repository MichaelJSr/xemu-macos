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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/ctype.h"
#include "qemu/cutils.h"
#include "exec/gdbstub.h"
#include "exec/replay-core.h"
#include "exec/target_page.h"
#include "gdbstub/commands.h"
#include "gdbstub/xemu.h"
#include "hw/core/cpu.h"
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
