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
 *    gdbstub breakpoints (BP_GDB) are sites too, with a hook that runs
 *    first and raises EXCP_DEBUG (xemu_gdb_bp_site_insert); the lookup's
 *    exact-PC check (check_for_breakpoints_slow) still fires before a
 *    TB, and DR7 breakpoints (BP_CPU) keep the one-instruction TBs.
 *  - A TB translated after a site exists holds its call, so only older
 *    TBs need invalidating: at each commit and each VM start, for every
 *    site with hooks, the TBs on the physical page its PC maps to then,
 *    once per page (a load may map the PC elsewhere), or all TBs when it
 *    is unmapped, as for a gdb breakpoint; and a mapping that appears
 *    while the VM runs (paging is off after a reset) at the first TB
 *    lookup by physical address on the site's page, which covers the
 *    page it found before it looks (xemu_hooks_lookup_cover, from
 *    tb_htable_lookup on the vCPU thread). A CF_PCREL TB runs at every
 *    mapping of its code, so the translator also cuts and calls at each
 *    address whose RAM address a site's PC has mapped to (those pages);
 *    the call reads the running PC, so elsewhere it finds no site and a
 *    TB translated at an alias still runs the hooks at the site's PC.
 *    Removing the last hook of a
 *    site keeps the site: a later hook at the same PC then costs no
 *    retranslation. A hookless site that still runs often (more than
 *    XEMU_HOOK_IDLE_PURGE arrivals) is dropped, and its pages
 *    invalidated, the next time the VM stops.
 *  - A hook reads registers with xemu_hook_reg() and guest memory with
 *    xemu_hook_read() (RAM only, through the guest page tables, never a
 *    device access; the GPU reports it would see are delivered first, so
 *    deterministic offscreen runs keep lazy reports), and may end the
 *    running xemu-frame-advance before the instruction with
 *    xemu_hooks_stop(). It changes guest state only
 *    through xemu_hook_write() (RAM only, as the gdbstub writes it), so
 *    that under -icount every run with the same hooks and the same writes
 *    executes the same instructions. A write on a page of the hooked TB's
 *    code (which drops every TB there, that one included) makes the
 *    engine leave the TB once the hook returns and run the instruction
 *    again from fresh code, the hooks up to the writer skipped, as a
 *    gdbstub write at a stop there would.
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
#include "exec/xemu-hook-expr.h"
#include "cpu.h"

typedef struct XemuHookCtx {
    CPUX86State *env;
    vaddr pc;           /* linear address of the hooked instruction */
    uintptr_t ra;       /* helper return address, for xemu_hooks_stop() */
    uint64_t call;      /* engine-wide arrival number (see xemu_hooks_calls) */
    int prio, id;       /* the running hook's (engine) */
    bool resumed;       /* the re-run xemu_hooks_stop_rerun() asked for */
    bool own_code;      /* xemu_hook_write() hit this TB's pages (engine) */
    bool raw;           /* xemu_hook_read(): RAM as it is (trace records) */
} XemuHookCtx;

typedef void (*XemuHookFn)(XemuHookCtx *ctx, void *opaque);

/* Hooks at one PC run in ascending priority, then in the order added. */
enum {
    XEMU_HOOK_PRIO_GDB = -1,        /* a gdb breakpoint: before any hook */
    XEMU_HOOK_PRIO_UNTIL = 0,       /* run-until checks: before any action */
    XEMU_HOOK_PRIO_ACTION = 100,    /* collectors and setters */
};

#define XEMU_HOOK_IDLE_PURGE 4096
#define XEMU_HOOK_MAX_SITES 256

/* Main thread, BQL held: before reading guest memory outside a hook. */
void xemu_hooks_init(void);

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
 * runs on, when no advance runs or it is already ending. When the
 * instruction runs again on resume, as the vCPU's next arrival, the
 * hooks that ran for it before the stop (this one included) are skipped,
 * so each hook sees each arrival once.
 */
bool xemu_hooks_stop(XemuHookCtx *ctx);

/*
 * As xemu_hooks_stop(), but when the instruction runs again on resume the
 * engine calls this hook once more for that arrival, with ctx->resumed
 * set (the hooks before it are still skipped), so that it can finish what
 * the stop held back. A load in between drops the re-run.
 */
bool xemu_hooks_stop_rerun(XemuHookCtx *ctx);

/* ui/xemu.c, after a load: a stopped arrival is no longer resumed. */
void xemu_hooks_state_loaded(void);

/* Registers a hook or an expression names: R_EAX..R_EDI, then these. */
enum { XEMU_HREG_EIP = CPU_NB_REGS32, XEMU_HREG_EFLAGS, XEMU_HREG_COUNT };
uint32_t xemu_hook_reg(XemuHookCtx *ctx, int reg);

typedef struct XemuHookStop {
    vaddr pc;
    uint32_t regs[XEMU_HREG_COUNT];     /* by xemu_hook_reg() number */
} XemuHookStop;

/* The registers of the last successful xemu_hooks_stop() (BQL held). */
const XemuHookStop *xemu_hooks_last_stop(void);

/*
 * xemu-hooks-qmp.c, for the hook commands' replies (main thread): the
 * QAPI XemuRegs of registers by xemu_hook_reg() number, and lowercase hex.
 */
struct XemuRegs;
struct XemuRegs *xemu_hook_regs_qapi(const uint32_t *regs);
char *xemu_hook_hex(const uint8_t *p, size_t n);

/*
 * Guest memory at a linear address (RAM only). False when unreadable.
 * Under icount a GPU report still undelivered in the bytes reaches RAM
 * first, as a guest read would trap and deliver it (unless ctx->raw).
 */
bool xemu_hook_read(XemuHookCtx *ctx, vaddr va, void *buf, unsigned len);

/*
 * Write guest memory at a linear address, through CPU 0's page tables and
 * the memory API as a gdbstub write does (TBs over the bytes invalidated,
 * pages marked dirty, nv2a's access callbacks told), but only where every
 * byte is writable guest RAM: otherwise nothing is written and it returns
 * false. Page permissions don't apply. xemu_hook_writable() only checks.
 */
bool xemu_hook_write(XemuHookCtx *ctx, vaddr va, const void *buf,
                     unsigned len);
bool xemu_hook_writable(XemuHookCtx *ctx, vaddr va, unsigned len);

/*
 * Address expressions: a register or a constant, then '+'/'-' constant
 * offsets, with [ ] reading the 32-bit pointer at what is inside, at most
 * XEMU_HOOK_EXPR_DEREFS deep. Examples: "esp+0x14", "0x2f1c40",
 * "[[ecx+0x20]]+0x15c". Registers: eax ecx edx ebx esp ebp esi edi eip.
 * Outside a hook, exec/xemu-hook-expr.h reads one through CPU 0.
 */
#define XEMU_HOOK_EXPR_DEREFS 8
#define XEMU_HOOK_EXPR_OPS 24

struct XemuHookExpr {
    int8_t base_reg;            /* -1: base is a constant */
    uint8_t nops;
    uint32_t base;
    struct {
        bool deref;             /* read the u32 at the value */
        uint32_t add;           /* else add this (mod 2^32) */
    } op[XEMU_HOOK_EXPR_OPS];
};

bool xemu_hook_expr_parse(const char *s, XemuHookExpr *e, Error **errp);
/* False when a dereference reads unmapped or non-RAM memory. */
bool xemu_hook_expr_eval(XemuHookCtx *ctx, const XemuHookExpr *e,
                         uint32_t *out);

/*
 * Conditions (QAPI XemuHookCond, xemu-hook-pred.c): a register, or 1, 2,
 * 4 or 8 bytes at an address expression, compared as unsigned, signed or
 * float with eq/ne/lt/gt/le/ge. Unreadable memory makes one false.
 */
#define XEMU_HOOK_MAX_CONDS 16

typedef struct XemuHookPred {
    int reg;                    /* -1: the @size bytes at @addr */
    XemuHookExpr addr;
    int size;
    int type;                   /* XemuHookType */
    int op;                     /* XemuHookCmp */
    uint64_t u;
    int64_t s;
    uint64_t f;                 /* float64 bits */
} XemuHookPred;

struct XemuHookCond;
bool xemu_hook_pred_compile(struct XemuHookCond *in, XemuHookPred *p,
                            Error **errp);
bool xemu_hook_pred_test(XemuHookCtx *ctx, const XemuHookPred *p);
/* The test on an operand already read: @size little-endian bytes. */
uint64_t xemu_hook_pred_raw(const uint8_t *b, int size);
bool xemu_hook_pred_compare(const XemuHookPred *p, uint64_t raw);

/*
 * Translator side (vCPU thread, under RCU): whether the insn at @pc, RAM
 * address @phys (-1: none), starts a site's TB. A TB at another mapping
 * of a site's code is cut there too: under CF_PCREL it may run at the
 * site's PC.
 */
extern unsigned xemu_hooks_nsites;
bool xemu_hooks_site_at_slow(vaddr pc, uint64_t phys);

static inline bool xemu_hooks_site_at(vaddr pc, uint64_t phys)
{
    return unlikely(qatomic_read(&xemu_hooks_nsites) != 0) &&
           xemu_hooks_site_at_slow(pc, phys);
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
/* ui/xemu.c: vblanks delivered so far (see query-xemu-frame). */
int64_t xemu_frame_count(void);
/* ui/xemu.c: xemu-run's value stops, asked at each vblank of an advance. */
void xemu_frame_advance_set_vblank_stop(bool (*fn)(void));
/* ui/xemu-input.c: XEMU_INPUT_PIPE lines handed over by QMP (BQL held). */
bool xemu_input_check_line(const char *line, Error **errp);
void xemu_input_queue_line(const char *line);

#endif
