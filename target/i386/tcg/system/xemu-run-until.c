/*
 * xemu QMP xemu-run-until: run until a guest PC is hit with conditions
 * true N times, or a frame budget runs out (PROMPT-7 B1).
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Each @until entry is a hook (tcg/xemu-hooks.h) at the until-checks
 * priority, so it runs before any other hook at its PC. A hit that
 * reaches the count ends the frame advance with the VM stopped before
 * the instruction (xemu_hooks_stop); other arrivals return at once. The
 * hooks live for the call only; their sites stay, so walking from hit to
 * hit retranslates nothing after the first call.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/cutils.h"
#include "qemu/coroutine.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "fpu/softfloat.h"
#include "cpu.h"
#include "exec/icount.h"
#include "system/runstate.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "tcg/xemu-hooks.h"

#define UNTIL_MAX_ENTRIES 64
#define UNTIL_MAX_CONDS 16

QEMU_BUILD_BUG_ON(XEMU_HOOK_REG_EAX != R_EAX || XEMU_HOOK_REG_EDI != R_EDI ||
                  XEMU_HOOK_REG_EIP != XEMU_HREG_EIP ||
                  XEMU_HOOK_REG_EFLAGS != XEMU_HREG_EFLAGS);

typedef struct UntilCond {
    int reg;                    /* -1: the @size bytes at @addr */
    XemuHookExpr addr;
    int size;
    XemuHookType type;
    XemuHookCmp op;
    uint64_t u;
    int64_t s;
    float64 f;
} UntilCond;

typedef struct UntilEntry {
    vaddr pc;
    int index;
    int ncond;
    int hook_id;
    UntilCond cond[UNTIL_MAX_CONDS];
} UntilEntry;

typedef struct UntilSet {
    struct rcu_head rcu;        /* first, for g_free_rcu */
    int n;
    UntilEntry e[];
} UntilSet;

/* Written by the vCPU's hooks, read on the main thread once stopped. */
static struct {
    bool armed;
    bool stopped;
    bool skip;                  /* the start PC's first arrival is free */
    vaddr start_pc;
    uint64_t start_call;
    uint64_t last_call;         /* one count per arrival */
    int64_t target;
    int64_t hits;
    int stop_index;
} until;

static bool until_cond(XemuHookCtx *ctx, const UntilCond *c)
{
    uint64_t raw;
    int size = c->size;
    int rel;

    if (c->reg >= 0) {
        raw = xemu_hook_reg(ctx, c->reg);
    } else {
        uint32_t a;
        uint8_t b[8];

        if (!xemu_hook_expr_eval(ctx, &c->addr, &a) ||
            !xemu_hook_read(ctx->env, a, b, size)) {
            return false;
        }
        raw = size == 1 ? b[0] : size == 2 ? lduw_le_p(b)
            : size == 4 ? (uint32_t)ldl_le_p(b) : ldq_le_p(b);
    }

    switch (c->type) {
    case XEMU_HOOK_TYPE_S: {
        int64_t v = size == 8 ? (int64_t)raw : sextract64(raw, 0, size * 8);
        rel = v < c->s ? -1 : v > c->s;
        break;
    }
    case XEMU_HOOK_TYPE_F: {
        /* Softfloat on a local status: the host FPCR holds the guest's. */
        float_status st = { 0 };
        float64 v;

        if (size == 4) {
            if (float32_is_any_nan(make_float32(raw))) {
                return c->op == XEMU_HOOK_CMP_NE;
            }
            v = float32_to_float64(make_float32(raw), &st);
        } else {
            v = make_float64(raw);
            if (float64_is_any_nan(v)) {
                return c->op == XEMU_HOOK_CMP_NE;
            }
        }
        rel = float64_compare_quiet(v, c->f, &st);
        break;
    }
    default:
        rel = raw < c->u ? -1 : raw > c->u;
        break;
    }

    switch (c->op) {
    case XEMU_HOOK_CMP_EQ:
        return rel == 0;
    case XEMU_HOOK_CMP_NE:
        return rel != 0;
    case XEMU_HOOK_CMP_LT:
        return rel < 0;
    case XEMU_HOOK_CMP_GT:
        return rel > 0;
    case XEMU_HOOK_CMP_LE:
        return rel <= 0;
    default:
        return rel >= 0;
    }
}

static void until_hook(XemuHookCtx *ctx, void *opaque)
{
    UntilEntry *e = opaque;

    if (!qatomic_read(&until.armed) || ctx->call == until.last_call) {
        return;
    }
    /* The run steps off the instruction it starts from. */
    if (until.skip && ctx->call == until.start_call + 1 &&
        ctx->pc == until.start_pc) {
        return;
    }
    for (int i = 0; i < e->ncond; i++) {
        if (!until_cond(ctx, &e->cond[i])) {
            return;
        }
    }
    until.last_call = ctx->call;
    if (++until.hits < until.target) {
        return;
    }
    until.stopped = true;
    until.stop_index = e->index;
    if (!xemu_hooks_stop(ctx)) {
        /* The advance ended first (its budget): this hit came after. */
        until.stopped = false;
        until.hits--;
        until.armed = false;
    }
}

static bool until_value(QObject *o, UntilCond *c, Error **errp)
{
    QNum *qn = qobject_to(QNum, o);
    QString *qs = qobject_to(QString, o);
    int bits = c->size * 8;
    enum { V_INT, V_UINT, V_DBL } kind;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;

    if (qn) {
        if (qnum_get_try_int(qn, &i)) {
            kind = V_INT;
        } else if (qnum_get_try_uint(qn, &u)) {
            kind = V_UINT;
        } else {
            d = qnum_get_double(qn);
            kind = V_DBL;
        }
    } else if (qs) {
        const char *str = qstring_get_str(qs);

        if (!qemu_strtoi64(str, NULL, 0, &i)) {
            kind = V_INT;
        } else if (!qemu_strtou64(str, NULL, 0, &u)) {
            kind = V_UINT;
        } else if (!qemu_strtod_finite(str, NULL, &d)) {
            kind = V_DBL;
        } else {
            error_setg(errp, "value '%s' is not a number", str);
            return false;
        }
    } else {
        error_setg(errp, "a condition's value must be a number");
        return false;
    }

    if (c->type == XEMU_HOOK_TYPE_F) {
        union { double d; uint64_t u; } x;

        x.d = kind == V_INT ? (double)i : kind == V_UINT ? (double)u : d;
        c->f = make_float64(x.u);
        return true;
    }
    if (kind == V_DBL) {
        if (d != floor(d) || d < -0x1p63 || d >= 0x1p64) {
            error_setg(errp, "value %g is not an integer", d);
            return false;
        }
        if (d < 0) {
            i = d;
            kind = V_INT;
        } else {
            u = d;
            kind = V_UINT;
        }
    }
    if (c->type == XEMU_HOOK_TYPE_U) {
        if (kind == V_INT) {
            if (i < 0) {
                error_setg(errp, "negative value for an unsigned compare");
                return false;
            }
            u = i;
        }
        if (bits < 64 && u >> bits) {
            error_setg(errp, "value 0x%" PRIx64 " doesn't fit in %d bytes",
                       u, c->size);
            return false;
        }
        c->u = u;
        return true;
    }
    if (kind == V_UINT) {
        if (u > INT64_MAX) {
            error_setg(errp, "value %" PRIu64 " out of signed range", u);
            return false;
        }
        i = u;
    }
    if (bits < 64 && (i < -(INT64_C(1) << (bits - 1)) ||
                      i >= (INT64_C(1) << (bits - 1)))) {
        error_setg(errp, "value %" PRId64 " doesn't fit in %d signed bytes",
                   i, c->size);
        return false;
    }
    c->s = i;
    return true;
}

static bool until_compile_cond(XemuHookCond *in, UntilCond *c,
                               bool *reads_memory, Error **errp)
{
    if (in->has_reg == !!in->addr) {
        error_setg(errp, "a condition names exactly one of 'reg' and 'addr'");
        return false;
    }
    c->type = in->has_type ? in->type : XEMU_HOOK_TYPE_U;
    c->op = in->op;
    if (in->has_reg) {
        if (in->has_size && in->size != 4) {
            error_setg(errp, "a register condition has size 4");
            return false;
        }
        c->reg = in->reg;
        c->size = 4;
    } else {
        c->reg = -1;
        c->size = in->has_size ? in->size : 4;
        if (c->size != 1 && c->size != 2 && c->size != 4 && c->size != 8) {
            error_setg(errp, "size must be 1, 2, 4 or 8");
            return false;
        }
        if (!xemu_hook_expr_parse(in->addr, &c->addr, errp)) {
            return false;
        }
        *reads_memory = true;
    }
    if (c->type == XEMU_HOOK_TYPE_F && c->size != 4 && c->size != 8) {
        error_setg(errp, "a float condition has size 4 or 8");
        return false;
    }
    return until_value(in->value, c, errp);
}

XemuRunUntilInfo *coroutine_fn qmp_xemu_run_until(XemuUntilList *list,
                                                  bool has_hits,
                                                  int64_t hits,
                                                  int64_t frames,
                                                  bool has_trace,
                                                  XemuTraceRegionList *trace,
                                                  bool has_trace_every,
                                                  int64_t trace_every,
                                                  Error **errp)
{
    bool reads_memory = false;
    int n = 0;

    if (!has_hits) {
        hits = 1;
    }
    if (hits < 1) {
        error_setg(errp, "hits must be at least 1");
        return NULL;
    }
    for (XemuUntilList *l = list; l; l = l->next) {
        n++;
    }
    if (n < 1 || n > UNTIL_MAX_ENTRIES) {
        error_setg(errp, "until takes 1 to %d entries", UNTIL_MAX_ENTRIES);
        return NULL;
    }
    /* The hooks go in, and the code they change out, while it is stopped. */
    if (runstate_is_running()) {
        error_setg(errp, "pause the VM first (xemu-frame-advance frames=0)");
        return NULL;
    }
    /* What the advance would refuse, before any hook goes in. */
    if (!xemu_frame_advance_check(frames, trace, has_trace_every, trace_every,
                                  errp)) {
        return NULL;
    }

    UntilSet *set = g_malloc0(sizeof(*set) + n * sizeof(UntilEntry));
    set->n = n;
    n = 0;
    for (XemuUntilList *l = list; l; l = l->next, n++) {
        UntilEntry *e = &set->e[n];

        e->pc = l->value->pc;
        e->index = n;
        if (e->pc > UINT32_MAX) {
            error_setg(errp, "pc 0x%" PRIx64 " is not a 32-bit address",
                       l->value->pc);
            goto fail;
        }
        for (XemuHookCondList *c = l->value->cond; c; c = c->next) {
            if (e->ncond == UNTIL_MAX_CONDS) {
                error_setg(errp, "at most %d conditions per entry",
                           UNTIL_MAX_CONDS);
                goto fail;
            }
            if (!until_compile_cond(c->value, &e->cond[e->ncond++],
                                    &reads_memory, errp)) {
                goto fail;
            }
        }
    }

    memset(&until, 0, sizeof(until));
    until.target = hits;
    if (first_cpu) {
        CPUX86State *env = cpu_env(first_cpu);

        until.skip = true;
        until.start_pc = (uint32_t)(env->segs[R_CS].base + env->eip);
        until.start_call = xemu_hooks_calls();
    }
    for (int i = 0; i < set->n; i++) {
        set->e[i].hook_id = xemu_hooks_add(set->e[i].pc, XEMU_HOOK_PRIO_UNTIL,
                                           until_hook, &set->e[i]);
    }
    xemu_hooks_commit();
    qatomic_set(&until.armed, true);

    /* Hook reads copy RAM directly, past the lazy report traps. */
    bool eager = reads_memory && icount_enabled();
    if (eager) {
        nv2a_set_reports_eager(true);
    }
    XemuFrameInfo *info = qmp_xemu_frame_advance(true, frames, has_trace,
                                                 trace, has_trace_every,
                                                 trace_every, errp);
    if (eager) {
        nv2a_set_reports_eager(false);
    }
    qatomic_set(&until.armed, false);
    for (int i = 0; i < set->n; i++) {
        xemu_hooks_remove(set->e[i].hook_id);
    }
    xemu_hooks_commit();
    /* Stopped, unless an error left it so; then a hook may still run. */
    g_free_rcu(set, rcu);
    if (!info) {
        return NULL;
    }

    XemuRunUntilInfo *r = g_new0(XemuRunUntilInfo, 1);
    r->frame = info->frame;
    r->running = info->running;
    r->hits = until.hits;
    if (until.stopped) {
        const XemuHookStop *st = xemu_hooks_last_stop();
        XemuRegs *g = g_new0(XemuRegs, 1);

        r->reason = XEMU_RUN_UNTIL_REASON_HIT;
        r->has_pc = true;
        r->pc = st->pc;
        r->has_index = true;
        r->index = until.stop_index;
        g->eax = st->regs[R_EAX];
        g->ecx = st->regs[R_ECX];
        g->edx = st->regs[R_EDX];
        g->ebx = st->regs[R_EBX];
        g->esp = st->regs[R_ESP];
        g->ebp = st->regs[R_EBP];
        g->esi = st->regs[R_ESI];
        g->edi = st->regs[R_EDI];
        g->eip = st->eip;
        g->eflags = st->eflags;
        r->regs = g;
    } else if (info->has_debug_stop && info->debug_stop) {
        r->reason = XEMU_RUN_UNTIL_REASON_DEBUG;
    } else {
        r->reason = XEMU_RUN_UNTIL_REASON_BUDGET;
    }
    if (info->has_trace) {
        r->has_trace = true;
        r->trace = info->trace;
        info->trace = NULL;
        info->has_trace = false;
    }
    qapi_free_XemuFrameInfo(info);
    return r;

fail:
    g_free(set);
    return NULL;
}
