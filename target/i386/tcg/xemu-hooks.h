/*
 * xemu guest PC hooks: callbacks at guest instructions that run inside
 * the translated code and leave the VM running unless a hook stops it.
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * How it works (system emulation only):
 *  - A hooked PC is a "site". The translator starts a TB at every site
 *    (it ends the TB before one) and emits a call to helper_xemu_hook at
 *    the top of that TB, before the instruction. The helper finds the
 *    site's hooks in an RCU-published table and calls them in priority
 *    order. Code that isn't a site is translated exactly as before, so
 *    cross-page links, the jump-cache probe, the return memo and the
 *    icount step cuts all stay on: the call lives in the TB itself, so
 *    every way into it runs the hook. Only a one-instruction CF_NOIRQ
 *    TB, which re-executes an instruction that already started
 *    (cpu_io_recompile under icount, precise SMC, a watchpoint), has no
 *    call: the hooks ran for that arrival in the TB it started in.
 *    gdbstub breakpoints are separate (cpu->breakpoints) and keep
 *    working beside hooks.
 *  - A TB translated after a site exists holds its call, so only older
 *    TBs need invalidating: at each commit and each VM start, for every
 *    site with hooks, the TBs on the physical page its PC maps to then,
 *    once per page (a load may map the PC elsewhere), or all TBs when it
 *    is unmapped, as for a gdb breakpoint. Removing the last hook of a
 *    site keeps the site: a later hook at the same PC then costs no
 *    retranslation. A hookless site that still runs often (more than
 *    XEMU_HOOK_IDLE_PURGE arrivals) is dropped, and its pages
 *    invalidated, the next time the VM stops.
 *  - A hook reads registers with xemu_hook_reg() and guest memory with
 *    xemu_hook_read() (RAM only, through the guest page tables, never a
 *    device access), and may end the running xemu-frame-advance before the
 *    instruction with xemu_hooks_stop(). It must not change guest state
 *    except through documented helpers, so that under -icount every run
 *    with the same hooks executes the same instructions.
 *  - Hooks run on the vCPU thread without the BQL. Add, remove and commit
 *    them on the main thread with the BQL held, best with the VM stopped:
 *    a commit while it runs drops the older TBs only at the next VM start.
 *    An opaque pointer must stay valid until the VM has stopped (or an RCU
 *    grace period) after removal.
 *  - Invalidated TBs normally wait in the recycler, which revives one whose
 *    bytes still match; a site's pages bypass it
 *    (xemu_tb_invalidate_page_norecycle), as their bytes don't change.
 */

#ifndef TARGET_I386_XEMU_HOOKS_H
#define TARGET_I386_XEMU_HOOKS_H

#include "qemu/atomic.h"
#include "cpu.h"

typedef struct XemuHookCtx {
    CPUX86State *env;
    vaddr pc;           /* linear address of the hooked instruction */
    uintptr_t ra;       /* helper return address, for xemu_hooks_stop() */
    uint64_t call;      /* engine-wide arrival number (see xemu_hooks_calls) */
} XemuHookCtx;

typedef void (*XemuHookFn)(XemuHookCtx *ctx, void *opaque);

/* Hooks at one PC run in ascending priority, then in the order added. */
enum {
    XEMU_HOOK_PRIO_UNTIL = 0,       /* run-until checks: before any action */
    XEMU_HOOK_PRIO_ACTION = 100,    /* collectors and setters */
};

#define XEMU_HOOK_IDLE_PURGE 4096
#define XEMU_HOOK_MAX_SITES 256

/* Main thread, BQL held. Changes take effect at xemu_hooks_commit(). */
int xemu_hooks_add(vaddr pc, int prio, XemuHookFn fn, void *opaque);
void xemu_hooks_remove(int id);
void xemu_hooks_commit(void);

/* Arrivals at any site so far (the vCPU's count; read it while stopped). */
uint64_t xemu_hooks_calls(void);

/*
 * From a hook: end the running xemu-frame-advance with the VM stopped
 * before the hooked instruction (EIP = ctx->pc, nothing of it executed,
 * later hooks at this PC not run). Records the registers there (see
 * xemu_hooks_last_stop) and does not return. Returns false, and the VM
 * runs on, when no advance runs or it is already ending.
 */
bool xemu_hooks_stop(XemuHookCtx *ctx);

typedef struct XemuHookStop {
    vaddr pc;
    uint32_t regs[CPU_NB_REGS32];   /* R_EAX.. R_EDI */
    uint32_t eip, eflags;
} XemuHookStop;

/* The registers of the last successful xemu_hooks_stop() (BQL held). */
const XemuHookStop *xemu_hooks_last_stop(void);

/* Registers a hook or an expression names: R_EAX..R_EDI, then these. */
enum { XEMU_HREG_EIP = CPU_NB_REGS32, XEMU_HREG_EFLAGS };
uint32_t xemu_hook_reg(XemuHookCtx *ctx, int reg);

/* Guest memory at a linear address (RAM only). False when unreadable. */
bool xemu_hook_read(CPUX86State *env, vaddr va, void *buf, unsigned len);

/*
 * Address expressions: a register or a constant, then '+'/'-' constant
 * offsets, with [ ] reading the 32-bit pointer at what is inside, at most
 * XEMU_HOOK_EXPR_DEREFS deep. Examples: "esp+0x14", "0x2f1c40",
 * "[[ecx+0x20]]+0x15c". Registers: eax ecx edx ebx esp ebp esi edi eip.
 */
#define XEMU_HOOK_EXPR_DEREFS 4
#define XEMU_HOOK_EXPR_OPS 16

typedef struct XemuHookExpr {
    int8_t base_reg;            /* -1: base is a constant */
    uint8_t nops;
    uint32_t base;
    struct {
        bool deref;             /* read the u32 at the value */
        uint32_t add;           /* else add this (mod 2^32) */
    } op[XEMU_HOOK_EXPR_OPS];
} XemuHookExpr;

bool xemu_hook_expr_parse(const char *s, XemuHookExpr *e, Error **errp);
/* False when a dereference reads unmapped or non-RAM memory. */
bool xemu_hook_expr_eval(XemuHookCtx *ctx, const XemuHookExpr *e,
                         uint32_t *out);
/* True when the expression reads guest memory (has a dereference). */
bool xemu_hook_expr_reads_memory(const XemuHookExpr *e);

/* Translator side (vCPU thread, under RCU). */
extern unsigned xemu_hooks_nsites;
bool xemu_hooks_site_at_slow(vaddr pc);

static inline bool xemu_hooks_site_at(vaddr pc)
{
    return unlikely(qatomic_read(&xemu_hooks_nsites) != 0) &&
           xemu_hooks_site_at_slow(pc);
}

/* ui/xemu.c: the frame advance a hook can end (BQL held). */
bool xemu_frame_advance_hook_stop(void);
/*
 * ui/xemu.c: whether xemu-frame-advance would take these arguments in the
 * current run state (and no advance runs), so that a command built on it
 * refuses a call before it changes anything.
 */
struct XemuTraceRegionList;
bool xemu_frame_advance_check(int64_t frames, struct XemuTraceRegionList *trace,
                              bool has_trace_every, int64_t trace_every,
                              Error **errp);

#endif
