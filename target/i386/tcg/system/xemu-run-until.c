/*
 * xemu QMP xemu-run-until: run until a guest PC is hit with conditions
 * true N times, or a frame budget runs out (PROMPT-7 B1); and xemu-run,
 * the same run after a checkpoint restore and queued input, also ended
 * by a value in guest memory (H(c)).
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
 *
 * xemu-run's value stops are read at each vblank of its advance, from
 * ui/xemu.c's vblank (xemu_frame_advance_set_vblank_stop), where the
 * trace samples are (in normal mode on the vCPU thread right after it,
 * since an address may use registers): RAM only, with no hook and no
 * retranslation. One that fires ends the advance on that vblank as its
 * last frame would.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/coroutine.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "cpu.h"
#include "system/runstate.h"
#include "block/aio.h"
#include "tcg/xemu-hooks.h"

#define UNTIL_MAX_ENTRIES 64
#define RUN_MAX_STOPS 64
#define RUN_MAX_INPUT 65536

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

/* One @until entry (or xemu-run @pc stop), its reply index @index. */
static bool until_entry_init(UntilEntry *e, int index, uint64_t pc,
                             XemuHookCondList *cond, Error **errp)
{
    e->pc = pc;
    e->index = index;
    if (pc > UINT32_MAX) {
        error_setg(errp, "pc 0x%" PRIx64 " is not a 32-bit address", pc);
        return false;
    }
    for (XemuHookCondList *c = cond; c; c = c->next) {
        if (e->ncond == XEMU_HOOK_MAX_CONDS) {
            error_setg(errp, "at most %d conditions per entry",
                       XEMU_HOOK_MAX_CONDS);
            return false;
        }
        if (!xemu_hook_pred_compile(c->value, &e->cond[e->ncond++], errp)) {
            return false;
        }
    }
    return true;
}

/*
 * Arm @set's hooks (it may be empty), run the frame advance, disarm them
 * and say what ended it. Takes @set. The VM is stopped.
 */
static XemuRunUntilInfo *coroutine_fn until_run(UntilSet *set, int64_t hits,
                                                int64_t frames,
                                                bool has_trace,
                                                XemuTraceRegionList *trace,
                                                bool has_trace_every,
                                                int64_t trace_every,
                                                Error **errp)
{
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
    if (set->n) {
        xemu_hooks_commit();
    }
    qatomic_set(&until.armed, true);

    XemuFrameInfo *info = qmp_xemu_frame_advance(true, frames, has_trace,
                                                 trace, has_trace_every,
                                                 trace_every, errp);
    qatomic_set(&until.armed, false);
    for (int i = 0; i < set->n; i++) {
        xemu_hooks_remove(set->e[i].hook_id);
    }
    if (set->n) {
        xemu_hooks_commit();
    }
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
        r->has_pc = true;
        r->pc = xemu_hooks_last_stop()->pc;
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
        r->regs = xemu_hook_regs_qapi(xemu_hooks_last_stop()->regs);
    } else if (info->has_stopped) {
        r->reason = info->stopped == RUN_STATE_DEBUG
                    ? XEMU_RUN_UNTIL_REASON_DEBUG
                    : XEMU_RUN_UNTIL_REASON_STOPPED;
        r->has_stopped = true;
        r->stopped = info->stopped;
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
}

/*
 * What both commands refuse before they change anything: a running VM
 * (the hooks go in, and the code they change out, while it is stopped),
 * and whatever the advance they end in would refuse.
 */
static bool until_can_run(int64_t frames, XemuTraceRegionList *trace,
                          bool has_trace_every, int64_t trace_every,
                          Error **errp)
{
    if (runstate_is_running()) {
        error_setg(errp, "pause the VM first (xemu-frame-advance frames=0)");
        return false;
    }
    return xemu_frame_advance_check(frames, trace, has_trace_every,
                                    trace_every, errp);
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
    if (!until_can_run(frames, trace, has_trace_every, trace_every, errp)) {
        return NULL;
    }

    UntilSet *set = g_malloc0(sizeof(*set) + n * sizeof(UntilEntry));
    set->n = n;
    n = 0;
    for (XemuUntilList *l = list; l; l = l->next, n++) {
        if (!until_entry_init(&set->e[n], n, l->value->pc, l->value->cond,
                              errp)) {
            g_free(set);
            return NULL;
        }
    }
    return until_run(set, hits, frames, has_trace, trace, has_trace_every,
                     trace_every, errp);
}

/*
 * xemu-run (PROMPT-7 H(c)): restore, queue input, run as run-until does,
 * also ended by a value stop. Value stops are main-thread state, read at
 * vblanks with the BQL held while no guest instruction runs (under
 * icount; otherwise the vCPU may be mid-update, as for trace samples).
 */
typedef struct RunWatch {
    int index;                  /* in stop-when */
    XemuHookExpr addr;
    int size;
    bool change;
    bool has_lo, has_hi;
    XemuHookPred lo, hi;        /* ge lo, le hi */
    bool base_ok, ok;           /* readable at the start, at the last check */
    uint8_t base[8], val[8];
} RunWatch;

static struct {
    int n;
    RunWatch *w;
    int fired;                  /* index into w, or -1 */
    bool cur_ok;                /* the fired check's bytes */
    uint8_t cur[8];
} run_watch;

static bool watch_read(RunWatch *w, uint8_t *buf)
{
    memset(buf, 0, 8);
    return xemu_hook_expr_read_cpu0(&w->addr, buf, w->size, false) ==
           XEMU_HOOK_EXPR_READ_OK;
}

static bool run_vblank_stop(void)
{
    for (int i = 0; i < run_watch.n; i++) {
        RunWatch *w = &run_watch.w[i];
        uint8_t cur[8];
        bool ok = watch_read(w, cur);
        uint64_t raw = xemu_hook_pred_raw(cur, w->size);
        bool fired =
            (w->change && (ok != w->base_ok ||
                           (ok && memcmp(cur, w->base, w->size)))) ||
            (w->has_lo && (!ok || !xemu_hook_pred_compare(&w->lo, raw))) ||
            (w->has_hi && (!ok || !xemu_hook_pred_compare(&w->hi, raw)));

        if (fired) {
            run_watch.fired = i;
            run_watch.cur_ok = ok;
            memcpy(run_watch.cur, cur, sizeof(cur));
            return true;
        }
        w->ok = ok;
        memcpy(w->val, cur, sizeof(cur));
    }
    return false;
}

/* An @addr stop's bound: a condition on the same operand, compiled. */
static bool watch_bound(XemuRunStop *in, QObject *value, XemuHookCmp op,
                        XemuHookPred *p, Error **errp)
{
    XemuHookCond c = {
        .addr = in->addr, .has_size = in->has_size, .size = in->size,
        .has_type = in->has_type, .type = in->type, .op = op,
        .value = value,
    };
    return xemu_hook_pred_compile(&c, p, errp);
}

static bool watch_init(RunWatch *w, int index, XemuRunStop *in, Error **errp)
{
    w->index = index;
    w->size = in->has_size ? in->size : 4;
    w->change = in->has_change && in->change;
    w->has_lo = !!in->lo;
    w->has_hi = !!in->hi;
    if (in->cond) {
        error_setg(errp, "stop-when %d: cond goes with pc", index);
        return false;
    }
    if (!w->change && !w->has_lo && !w->has_hi) {
        error_setg(errp, "stop-when %d: give lo, hi or change", index);
        return false;
    }
    /* The bounds check size, type and expression; change needs them too. */
    if (w->has_lo && !watch_bound(in, in->lo, XEMU_HOOK_CMP_GE, &w->lo,
                                  errp)) {
        return false;
    }
    if (w->has_hi && !watch_bound(in, in->hi, XEMU_HOOK_CMP_LE, &w->hi,
                                  errp)) {
        return false;
    }
    if (w->size != 1 && w->size != 2 && w->size != 4 && w->size != 8) {
        error_setg(errp, "stop-when %d: size must be 1, 2, 4 or 8", index);
        return false;
    }
    if (in->has_type && in->type == XEMU_HOOK_TYPE_F && w->size != 4 &&
        w->size != 8) {
        error_setg(errp, "stop-when %d: a float has size 4 or 8", index);
        return false;
    }
    return xemu_hook_expr_parse(in->addr, &w->addr, errp);
}

typedef struct RunRestore {
    Coroutine *co;
    const char *name;
    bool incremental;
    Error *err;
} RunRestore;

/*
 * Out of coroutine context, as QMP runs xemu-checkpoint-restore. By
 * default incremental: it ends on the same machine as a full restore,
 * comparing only the pages the dirty log or the two images name.
 */
static void run_restore_bh(void *opaque)
{
    RunRestore *rr = opaque;

    qapi_free_XemuCheckpointInfo(qmp_xemu_checkpoint_restore(rr->name,
                                                             true,
                                                             rr->incremental,
                                                             false, false,
                                                             &rr->err));
    aio_co_wake(rr->co);
}

static char *watch_hex(bool ok, const uint8_t *b, int size)
{
    return ok ? xemu_hook_hex(b, size) : g_strdup("");
}

XemuRunInfo *coroutine_fn qmp_xemu_run(const char *checkpoint,
                                       bool has_incremental, bool incremental,
                                       bool has_input, strList *input,
                                       int64_t frames, bool has_trace,
                                       XemuTraceRegionList *trace,
                                       bool has_trace_every,
                                       int64_t trace_every,
                                       bool has_stop_when,
                                       XemuRunStopList *stop_when,
                                       bool has_hits, int64_t hits,
                                       Error **errp)
{
    int npc = 0, nwatch = 0, nstop = 0, ninput = 0;
    UntilSet *set = NULL;
    RunWatch *w = NULL;

    if (!has_hits) {
        hits = 1;
    }
    if (hits < 1) {
        error_setg(errp, "hits must be at least 1");
        return NULL;
    }
    for (strList *l = input; l; l = l->next) {
        if (++ninput > RUN_MAX_INPUT) {
            error_setg(errp, "at most %d input lines", RUN_MAX_INPUT);
            return NULL;
        }
        if (!xemu_input_check_line(l->value, errp)) {
            return NULL;
        }
    }
    for (XemuRunStopList *l = stop_when; l; l = l->next) {
        XemuRunStop *st = l->value;

        if (st->has_pc == !!st->addr) {
            error_setg(errp, "stop-when %d names exactly one of pc and addr",
                       nstop);
            return NULL;
        }
        if (st->has_pc && (st->has_size || st->has_type || st->lo ||
                           st->hi || st->has_change)) {
            error_setg(errp, "stop-when %d: size, type, lo, hi and change "
                       "go with addr", nstop);
            return NULL;
        }
        if (++nstop > RUN_MAX_STOPS) {
            error_setg(errp, "at most %d stop-when entries", RUN_MAX_STOPS);
            return NULL;
        }
        if (st->has_pc) {
            npc++;
        } else {
            nwatch++;
        }
    }
    if (!until_can_run(frames, trace, has_trace_every, trace_every, errp)) {
        return NULL;
    }

    set = g_malloc0(sizeof(*set) + npc * sizeof(UntilEntry));
    w = g_new0(RunWatch, nwatch);
    nstop = 0;
    nwatch = 0;
    for (XemuRunStopList *l = stop_when; l; l = l->next, nstop++) {
        XemuRunStop *st = l->value;
        bool ok = st->has_pc
            ? until_entry_init(&set->e[set->n++], nstop, st->pc, st->cond,
                               errp)
            : watch_init(&w[nwatch++], nstop, st, errp);

        if (!ok) {
            goto fail;
        }
    }

    if (checkpoint) {
        RunRestore rr = { .co = qemu_coroutine_self(), .name = checkpoint,
                          .incremental = !has_incremental || incremental };

        aio_bh_schedule_oneshot(iohandler_get_aio_context(), run_restore_bh,
                                &rr);
        qemu_coroutine_yield();
        if (rr.err) {
            error_propagate(errp, rr.err);
            goto fail;
        }
        /* Something else may have run while the restore waited its turn. */
        if (!until_can_run(frames, trace, has_trace_every, trace_every, errp)) {
            goto fail;
        }
    }
    /* From here to the start nothing runs: the lines count from here. */
    for (strList *l = input; l; l = l->next) {
        xemu_input_queue_line(l->value);
    }

    int64_t frame0 = xemu_frame_count();
    if (nwatch) {
        xemu_hooks_init();
        for (int i = 0; i < nwatch; i++) {
            w[i].base_ok = w[i].ok = watch_read(&w[i], w[i].base);
            memcpy(w[i].val, w[i].base, sizeof(w[i].val));
        }
        run_watch.n = nwatch;
        run_watch.w = w;
        run_watch.fired = -1;
        xemu_frame_advance_set_vblank_stop(run_vblank_stop);
    }
    XemuRunUntilInfo *u = until_run(set, hits, frames, has_trace, trace,
                                    has_trace_every, trace_every, errp);
    set = NULL;
    int fired = -1;
    if (nwatch) {
        xemu_frame_advance_set_vblank_stop(NULL);
        fired = run_watch.fired;
        run_watch.n = 0;
        run_watch.w = NULL;
    }
    if (!u) {
        g_free(w);
        return NULL;
    }

    /* The run-until reply is the base: move its members over. */
    XemuRunInfo *r = g_new0(XemuRunInfo, 1);
    *qapi_XemuRunInfo_base(r) = *u;
    g_free(u);
    r->frames = r->frame - frame0;
    /* A value stop ends the advance as its budget would. */
    if (r->reason == XEMU_RUN_UNTIL_REASON_BUDGET && fired >= 0) {
        RunWatch *f = &w[fired];

        r->reason = XEMU_RUN_UNTIL_REASON_VALUE;
        r->has_index = true;
        r->index = f->index;
        r->data = watch_hex(run_watch.cur_ok, run_watch.cur, f->size);
        r->previous = watch_hex(f->ok, f->val, f->size);
    }
    g_free(w);
    return r;

fail:
    g_free(set);
    g_free(w);
    return NULL;
}
