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
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/coroutine.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "cpu.h"
#include "exec/icount.h"
#include "system/runstate.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "tcg/xemu-hooks.h"

#define UNTIL_MAX_ENTRIES 64

typedef struct UntilEntry {
    vaddr pc;
    int index;
    int ncond;
    int hook_id;
    XemuHookPred cond[XEMU_HOOK_MAX_CONDS];
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
        if (!xemu_hook_pred_test(ctx, &e->cond[i])) {
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
            if (e->ncond == XEMU_HOOK_MAX_CONDS) {
                error_setg(errp, "at most %d conditions per entry",
                           XEMU_HOOK_MAX_CONDS);
                goto fail;
            }
            if (!xemu_hook_pred_compile(c->value, &e->cond[e->ncond++],
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
    /* Hook records, and a hook mismatch or empty queue that stopped. */
    r->hooks = info->hooks;
    info->hooks = NULL;
    bool mismatch = !until.stopped && r->hooks && r->hooks->stop;
    if (until.stopped || mismatch) {
        const XemuHookStop *st = xemu_hooks_last_stop();

        r->has_pc = true;
        r->pc = st->pc;
        if (mismatch) {
            /* Or a set hook's empty queue (stop-empty). */
            r->reason = r->hooks->stop->has_mismatch
                        ? XEMU_RUN_UNTIL_REASON_MISMATCH
                        : XEMU_RUN_UNTIL_REASON_EMPTY;
        } else {
            r->reason = XEMU_RUN_UNTIL_REASON_HIT;
            r->has_index = true;
            r->index = until.stop_index;
        }
        r->regs = xemu_hook_regs_qapi(st->regs);
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
