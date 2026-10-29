/*
 * XEMU_GUEST_BUGCHECK=1: name the guest's bugchecks and dead halts.
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A diagnostic for harness soaks that meet a boot idle in the kernel's
 * halt with every I/O counter frozen (the Azurik harness's V22): the 4627
 * kernel's KeBugCheckEx (export 96; KeBugCheck, 95, calls it) stores its
 * code and parameters and ends in a "cli; hlt; jmp" loop, as its double
 * fault handler does without them. With the variable set:
 *  - xemu finds KeBugCheckEx from the export table of the kernel image
 *    at 0x80010000 and hooks it (xemu-hooks.h): at each load, before the
 *    VM runs on (the loaded RAM is complete), and at each vblank until it
 *    is hooked, where two vblanks in a row must agree, as a booting
 *    kernel may still be loading (a fixed frame under icount, so replays
 *    report alike); each call prints
 *    "xemu: guest bugcheck 0x<code> (<p1>, <p2>, <p3>, <p4>) from <ret>"
 *    and a line of the registers, and emits XEMU_GUEST_BUGCHECK with
 *    those and the stack (STACK_BYTES from ESP, which holds a 0x1E's
 *    trap frame: guest data, so the event alone carries it);
 *  - a HLT with interrupts disabled (it can only end in a reset or an
 *    NMI) prints "xemu: guest halted with interrupts disabled at <eip>"
 *    and emits XEMU_GUEST_HALTED (helper_hlt).
 * Both only read guest state: the hook site is a PC no normal run
 * executes, so runs keep their instructions and RAM.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "qapi/qapi-events-xemu.h"
#include "qapi/qapi-types-xemu.h"
#include "tcg/xemu-hooks.h"
#include "tcg/xemu-bugcheck.h"

#define KERNEL_BASE 0x80010000u
#define KEBUGCHECKEX_ORDINAL 96
#define REPORT_MAX 16
#define STACK_BYTES 4096

static uint32_t cand_pc;
static uint8_t cand_bytes[16];
static uint32_t hooked_pc;
static int hook_id = -1;
static unsigned bugchecks, halts;

bool xemu_guest_bugcheck_enabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("XEMU_GUEST_BUGCHECK");
        on = e && e[0] == '1';
    }
    return on;
}

static bool rd(uint32_t va, void *buf, int len)
{
    return first_cpu &&
           cpu_memory_rw_debug(first_cpu, va, buf, len, false) == 0;
}

static uint32_t u32(const uint8_t *p)
{
    return ldl_le_p(p);
}

/* KeBugCheckEx's linear address from the kernel's export table, or 0. */
static uint32_t find_kebugcheckex(void)
{
    uint8_t h[0x200], ed[40], fn[4];
    uint32_t pe, opt, base, size, exp_rva, exp_size, ord_base, nfun, afun;
    uint32_t idx, rva;

    if (!rd(KERNEL_BASE, h, sizeof(h)) || h[0] != 'M' || h[1] != 'Z') {
        return 0;
    }
    pe = u32(h + 0x3c);
    if (pe > sizeof(h) - 0x80 || memcmp(h + pe, "PE\0\0", 4)) {
        return 0;
    }
    opt = pe + 24;
    if (lduw_le_p(h + opt) != 0x10b) {      /* PE32 */
        return 0;
    }
    base = u32(h + opt + 28);
    size = u32(h + opt + 56);
    exp_rva = u32(h + opt + 96);
    exp_size = u32(h + opt + 100);
    if (base != KERNEL_BASE || !exp_rva || exp_size < sizeof(ed) ||
        exp_rva >= size || !rd(base + exp_rva, ed, sizeof(ed))) {
        return 0;
    }
    ord_base = u32(ed + 16);
    nfun = u32(ed + 20);
    afun = u32(ed + 28);
    idx = KEBUGCHECKEX_ORDINAL - ord_base;
    if (ord_base > KEBUGCHECKEX_ORDINAL || idx >= nfun || afun >= size ||
        !rd(base + afun + idx * 4, fn, 4)) {
        return 0;
    }
    rva = u32(fn);
    return rva && rva < size ? base + rva : 0;
}

/* At KeBugCheckEx's first instruction: [esp] the caller, then the args. */
static void bugcheck_hook(XemuHookCtx *ctx, void *opaque)
{
    uint8_t s[24];
    uint32_t esp = xemu_hook_reg(ctx, R_ESP);
    uint32_t r[XEMU_HREG_COUNT];
    g_autofree uint8_t *stack = g_malloc(STACK_BYTES);
    g_autofree char *hex = NULL;
    unsigned n, len = 0;
    uint32List *params = NULL, **tail = &params;
    XemuRegs *regs;

    if (!xemu_hook_read(ctx, esp, s, sizeof(s))) {
        return;
    }
    n = qatomic_fetch_inc(&bugchecks) + 1;
    if (n > REPORT_MAX) {
        return;
    }
    for (int i = 0; i < XEMU_HREG_COUNT; i++) {
        r[i] = xemu_hook_reg(ctx, i);
    }
    /* A page at a time, up to the first that isn't readable RAM. */
    while (len < STACK_BYTES) {
        unsigned chunk = MIN(STACK_BYTES - len,
                             TARGET_PAGE_SIZE - ((esp + len) &
                                                 ~TARGET_PAGE_MASK));
        if (!xemu_hook_read(ctx, esp + len, stack + len, chunk)) {
            break;
        }
        len += chunk;
    }
    hex = xemu_hook_hex(stack, len);
    fprintf(stderr, "xemu: guest bugcheck 0x%08x (0x%08x, 0x%08x, 0x%08x, "
            "0x%08x) from 0x%08x%s\n", u32(s + 4), u32(s + 8), u32(s + 12),
            u32(s + 16), u32(s + 20), u32(s),
            n == REPORT_MAX ? " (further bugchecks not reported)" : "");
    fprintf(stderr, "xemu: guest bugcheck registers: eax=%08x ecx=%08x "
            "edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x "
            "eflags=%08x (stack: %u bytes in the event)\n", r[R_EAX],
            r[R_ECX], r[R_EDX], r[R_EBX], r[R_ESP], r[R_EBP], r[R_ESI],
            r[R_EDI], r[XEMU_HREG_EFLAGS], len);
    for (int i = 0; i < 4; i++) {
        QAPI_LIST_APPEND(tail, u32(s + 8 + i * 4));
    }
    regs = xemu_hook_regs_qapi(r);
    /* vCPU thread, no BQL: as XEMU_CPUID_FAULT, only the monitor locks. */
    qapi_event_send_xemu_guest_bugcheck(u32(s + 4), params, u32(s), n, regs,
                                        hex);
    qapi_free_uint32List(params);
    qapi_free_XemuRegs(regs);
}

void xemu_guest_dead_halt(CPUX86State *env)
{
    unsigned n = qatomic_fetch_inc(&halts) + 1;
    uint32_t eip = env->eip, esp = env->regs[R_ESP];

    if (n > REPORT_MAX) {
        return;
    }
    fprintf(stderr, "xemu: guest halted with interrupts disabled at 0x%08x "
            "(esp 0x%08x)%s\n", eip, esp,
            n == REPORT_MAX ? " (further halts not reported)" : "");
    qapi_event_send_xemu_guest_halted(eip, esp, n);
}

/* BQL held. Hook @pc, moving the hook if the kernel has changed. */
static void hook_at(uint32_t pc)
{
    if (hook_id >= 0) {
        if (pc == hooked_pc) {
            return;
        }
        xemu_hooks_remove(hook_id);
    }
    hook_id = xemu_hooks_add(pc, XEMU_HOOK_PRIO_UNTIL, bugcheck_hook, NULL);
    hooked_pc = pc;
    /* With the VM stopped this also drops older TBs at the site. */
    xemu_hooks_commit();
    fprintf(stderr, "xemu: guest bugcheck: KeBugCheckEx at 0x%08x hooked\n",
            pc);
}

/* ui/xemu.c, at each vblank while the VM runs (BQL held). */
void xemu_guest_bugcheck_frame(void)
{
    uint32_t pc;
    uint8_t bytes[16];

    if (hook_id >= 0 || !xemu_guest_bugcheck_enabled()) {
        return;
    }
    pc = find_kebugcheckex();
    if (!pc || !rd(pc, bytes, sizeof(bytes))) {
        cand_pc = 0;
        return;
    }
    if (pc == cand_pc && !memcmp(bytes, cand_bytes, sizeof(bytes))) {
        hook_at(pc);
        return;
    }
    cand_pc = pc;
    memcpy(cand_bytes, bytes, sizeof(bytes));
}

/*
 * ui/xemu.c, after each load (VM stopped, BQL held): the RAM is the
 * saved one, complete, so one read decides and the hook is in place
 * before the guest runs on.
 */
void xemu_guest_bugcheck_state_loaded(void)
{
    uint32_t pc;

    if (!xemu_guest_bugcheck_enabled()) {
        return;
    }
    cand_pc = 0;
    pc = find_kebugcheckex();
    if (pc) {
        hook_at(pc);
    }
}

/* ui/xemu.c, once the machine exists (BQL held). */
void xemu_guest_bugcheck_init(void)
{
    static bool done;

    if (!xemu_guest_bugcheck_enabled() || done) {
        return;
    }
    done = true;
    fprintf(stderr, "xemu: guest bugcheck: reporting KeBugCheckEx calls and "
            "halts with interrupts disabled (XEMU_GUEST_BUGCHECK)\n");
}
