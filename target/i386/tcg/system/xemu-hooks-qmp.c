/*
 * xemu QMP xemu-hooks-set / xemu-hooks-clear / query-xemu-hooks: guest PC
 * hooks that stay installed across advances and, at each hit, copy guest
 * memory into a record without stopping the VM (PROMPT-7 B2).
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A hook (PHook) is one engine hook (tcg/xemu-hooks.h) per PC, at the
 * action priority, sharing one state: the hit count, the last key and the
 * expect queue. Only the vCPU touches that state while the VM runs. Hits
 * append to one record buffer in execution order, under rec_lock (the
 * vCPU runs hooks without the BQL), as raw bytes; xemu-frame-advance and
 * xemu-run-until take the buffer into their reply
 * (xemu_hooks_take_trace), where it becomes hex.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/clone-visitor.h"
#include "qapi/qapi-commands-xemu.h"
#include "qapi/qapi-visit-xemu.h"
#include "cpu.h"
#include "exec/icount.h"
#include "system/runstate.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "tcg/xemu-hooks.h"

#define HOOKS_MAX 64
#define HOOK_MAX_PCS 16
#define HOOK_MAX_REGIONS 64
#define HOOK_MAX_BYTES (64 * KiB)
#define HOOK_KEY_MAX 64
#define REC_MAX (256 * 1024)
#define REC_DATA_MAX (16 * MiB)
#define REC_READABLE 0x80000000u    /* region word: bytes follow */

typedef struct Region {
    XemuHookExpr addr;
    uint32_t size;
} Region;

typedef struct PHook {
    struct rcu_head rcu;
    int id;
    int npcs;
    vaddr pcs[HOOK_MAX_PCS];
    int engine[HOOK_MAX_PCS];
    int npred;
    XemuHookPred pred[XEMU_HOOK_MAX_CONDS];
    bool has_key;
    XemuHookExpr key;
    uint32_t key_size;
    int nregions;
    Region *regions;
    uint32_t total;             /* bytes collected per hit */
    bool regs;
    bool stop;
    bool reads_memory;
    uint32_t nexpect;
    uint32_t *exp_off;          /* nexpect + 1 offsets into exp_val/mask */
    uint8_t *exp_val, *exp_mask;

    /* vCPU state (read on the main thread while stopped) */
    int64_t hits;
    bool have_key;
    uint8_t last_key[HOOK_KEY_MAX];
    uint8_t *buf;               /* @total bytes */
    bool *readable;             /* per region */
} PHook;

/* Main thread (BQL). */
static GPtrArray *phooks;       /* PHook, ascending id */
static int phook_next_id = 1;
static bool eager_held;

/* A hit, as the vCPU stores it. */
typedef struct RecHdr {
    int64_t frame;
    int64_t hit;
    uint64_t off;               /* into rec_data: per region a word, bytes */
    uint32_t pc;
    int32_t hook;
    uint16_t nregions;
    bool mismatch;
    bool has_regs;
    uint32_t regs[XEMU_HREG_COUNT];
} RecHdr;

static QemuMutex rec_lock;
static GArray *rec_hdrs;        /* RecHdr */
static GByteArray *rec_data;
static int64_t rec_dropped;
static int64_t rec_stop = -1;   /* index of the record whose stop held */

static void phook_free(PHook *h)
{
    g_free(h->regions);
    g_free(h->exp_off);
    g_free(h->exp_val);
    g_free(h->exp_mask);
    g_free(h->buf);
    g_free(h->readable);
    g_free(h);
}

static void phook_free_rcu(struct rcu_head *head)
{
    phook_free(container_of(head, PHook, rcu));
}

static void phooks_init(void)
{
    if (phooks) {
        return;
    }
    phooks = g_ptr_array_new();
    qemu_mutex_init(&rec_lock);
    rec_hdrs = g_array_new(false, false, sizeof(RecHdr));
    rec_data = g_byte_array_new();
}

/* vCPU side. */

static bool expect_differs(PHook *h, int64_t hit)
{
    uint32_t lo = h->exp_off[hit], n = h->exp_off[hit + 1] - lo;
    const uint8_t *val, *mask;
    uint32_t off = 0;

    if (!n) {
        return false;
    }
    val = h->exp_val + lo;
    mask = h->exp_mask + lo;
    for (int i = 0; i < h->nregions && off < n; i++) {
        uint32_t len = MIN(h->regions[i].size, n - off);

        for (uint32_t j = 0; j < len; j++) {
            uint8_t got = h->readable[i] ? h->buf[off + j] : ~val[off + j];
            if ((got ^ val[off + j]) & mask[off + j]) {
                return true;
            }
        }
        off += h->regions[i].size;
    }
    return false;
}

static void rec_append(XemuHookCtx *ctx, PHook *h, int64_t hit,
                       bool mismatch, bool stop)
{
    RecHdr r = {
        .frame = xemu_frame_count(),
        .hit = hit,
        .pc = ctx->pc,
        .hook = h->id,
        .nregions = h->nregions,
        .mismatch = mismatch,
        .has_regs = h->regs,
    };
    uint64_t need = 4 * h->nregions;

    if (h->regs) {
        for (int i = 0; i < XEMU_HREG_COUNT; i++) {
            r.regs[i] = xemu_hook_reg(ctx, i);
        }
    }
    for (int i = 0; i < h->nregions; i++) {
        need += h->readable[i] ? h->regions[i].size : 0;
    }

    qemu_mutex_lock(&rec_lock);
    if (!stop && (rec_hdrs->len >= REC_MAX ||
                  rec_data->len + need > REC_DATA_MAX)) {
        rec_dropped++;
    } else {
        uint32_t off = 0;

        r.off = rec_data->len;
        for (int i = 0; i < h->nregions; i++) {
            uint32_t size = h->regions[i].size;
            uint32_t w = size | (h->readable[i] ? REC_READABLE : 0);
            uint8_t word[4];

            stl_le_p(word, w);
            g_byte_array_append(rec_data, word, 4);
            if (h->readable[i]) {
                g_byte_array_append(rec_data, h->buf + off, size);
            }
            off += size;
        }
        if (stop) {
            rec_stop = rec_hdrs->len;
        }
        g_array_append_val(rec_hdrs, r);
    }
    qemu_mutex_unlock(&rec_lock);
}

static void phook_fn(XemuHookCtx *ctx, void *opaque)
{
    PHook *h = opaque;
    uint32_t a, off = 0;
    int64_t hit;
    bool mismatch, stop;

    for (int i = 0; i < h->npred; i++) {
        if (!xemu_hook_pred_test(ctx, &h->pred[i])) {
            return;
        }
    }
    if (h->has_key) {
        uint8_t key[HOOK_KEY_MAX];

        if (!xemu_hook_expr_eval(ctx, &h->key, &a) ||
            !xemu_hook_read(ctx->env, a, key, h->key_size)) {
            return;
        }
        if (h->have_key && !memcmp(key, h->last_key, h->key_size)) {
            return;
        }
        memcpy(h->last_key, key, h->key_size);
        h->have_key = true;
    }
    hit = h->hits++;
    for (int i = 0; i < h->nregions; i++) {
        Region *r = &h->regions[i];

        h->readable[i] = xemu_hook_expr_eval(ctx, &r->addr, &a) &&
                         xemu_hook_read(ctx->env, a, h->buf + off, r->size);
        off += r->size;
    }
    mismatch = hit < h->nexpect && expect_differs(h, hit);
    stop = mismatch && h->stop;
    rec_append(ctx, h, hit, mismatch, stop);
    /* Takes the BQL: never with rec_lock held. Doesn't return if held. */
    if (stop && !xemu_hooks_stop(ctx)) {
        qemu_mutex_lock(&rec_lock);
        rec_stop = -1;
        qemu_mutex_unlock(&rec_lock);
    }
}

/* Records into QAPI (main thread). */

XemuRegs *xemu_hook_regs_qapi(const uint32_t *regs)
{
    XemuRegs *g = g_new(XemuRegs, 1);

    *g = (XemuRegs) {
        .eax = regs[R_EAX], .ecx = regs[R_ECX], .edx = regs[R_EDX],
        .ebx = regs[R_EBX], .esp = regs[R_ESP], .ebp = regs[R_EBP],
        .esi = regs[R_ESI], .edi = regs[R_EDI],
        .eip = regs[XEMU_HREG_EIP], .eflags = regs[XEMU_HREG_EFLAGS],
    };
    return g;
}

char *xemu_hook_hex(const uint8_t *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    char *s = g_malloc(2 * n + 1);

    for (size_t i = 0; i < n; i++) {
        s[2 * i] = digits[p[i] >> 4];
        s[2 * i + 1] = digits[p[i] & 15];
    }
    s[2 * n] = 0;
    return s;
}

static XemuHookRecord *rec_build(const RecHdr *r, const uint8_t *data)
{
    XemuHookRecord *rec = g_new0(XemuHookRecord, 1);
    strList **tail = &rec->data;
    const uint8_t *p = data + r->off;

    rec->hook = r->hook;
    rec->pc = r->pc;
    rec->frame = r->frame;
    rec->hit = r->hit;
    for (int i = 0; i < r->nregions; i++) {
        uint32_t w = ldl_le_p(p);

        p += 4;
        if (w & REC_READABLE) {
            uint32_t size = w & ~REC_READABLE;
            QAPI_LIST_APPEND(tail, xemu_hook_hex(p, size));
            p += size;
        } else {
            QAPI_LIST_APPEND(tail, g_strdup(""));
        }
    }
    if (r->has_regs) {
        rec->regs = xemu_hook_regs_qapi(r->regs);
    }
    if (r->mismatch) {
        rec->has_mismatch = true;
        rec->mismatch = true;
    }
    return rec;
}

/*
 * ui/xemu.c, at the end of an xemu-frame-advance: the records since the
 * last call, or NULL while no hook is installed and none are waiting.
 */
XemuHookTrace *xemu_hooks_take_trace(void);
XemuHookTrace *xemu_hooks_take_trace(void)
{
    GArray *hdrs;
    GByteArray *data;
    int64_t dropped, stop;

    if (!phooks) {
        return NULL;
    }
    qemu_mutex_lock(&rec_lock);
    if (!phooks->len && !rec_hdrs->len && !rec_dropped) {
        qemu_mutex_unlock(&rec_lock);
        return NULL;
    }
    hdrs = rec_hdrs;
    data = rec_data;
    dropped = rec_dropped;
    stop = rec_stop;
    rec_hdrs = g_array_new(false, false, sizeof(RecHdr));
    rec_data = g_byte_array_new();
    rec_dropped = 0;
    rec_stop = -1;
    qemu_mutex_unlock(&rec_lock);

    XemuHookTrace *t = g_new0(XemuHookTrace, 1);
    XemuHookRecordList **tail = &t->records;

    t->dropped = dropped;
    for (guint i = 0; i < hdrs->len; i++) {
        XemuHookRecord *rec = rec_build(&g_array_index(hdrs, RecHdr, i),
                                        data->data);
        if ((int64_t)i == stop) {
            t->stop = QAPI_CLONE(XemuHookRecord, rec);
        }
        QAPI_LIST_APPEND(tail, rec);
    }
    g_array_free(hdrs, true);
    g_byte_array_free(data, true);
    return t;
}

/* Hook reads copy RAM directly, past the lazy GPU report traps. */
static void eager_update(void)
{
    bool want = false;

    for (guint i = 0; icount_enabled() && i < phooks->len; i++) {
        want |= ((PHook *)g_ptr_array_index(phooks, i))->reads_memory;
    }
    if (want != eager_held) {
        nv2a_set_reports_eager(want);
        eager_held = want;
    }
}

/* QMP. */

static bool region_compile(XemuHookRegion *in, Region *r, uint32_t max,
                           Error **errp)
{
    if (in->size < 1 || in->size > max) {
        error_setg(errp, "a region's size must be 1 to %u", max);
        return false;
    }
    r->size = in->size;
    return xemu_hook_expr_parse(in->addr, &r->addr, errp);
}

static int hex_nibble(char c)
{
    return g_ascii_xdigit_value(c);
}

static bool expect_compile(PHook *h, strList *list, Error **errp)
{
    GByteArray *val = g_byte_array_new(), *mask = g_byte_array_new();
    GArray *off = g_array_new(false, false, sizeof(uint32_t));
    uint32_t zero = 0;

    g_array_append_val(off, zero);
    for (strList *l = list; l; l = l->next) {
        const char *s = l->value;
        size_t n = strlen(s);

        if (n % 2 || n / 2 > h->total) {
            error_setg(errp, "expect entry %u: %s", off->len - 1,
                       n % 2 ? "odd number of hex digits"
                             : "longer than the collected bytes");
            goto out;
        }
        for (size_t i = 0; i < n; i += 2) {
            uint8_t v = 0, m = 0xff;

            if (s[i] == '?' && s[i + 1] == '?') {
                m = 0;
            } else if (hex_nibble(s[i]) >= 0 && hex_nibble(s[i + 1]) >= 0) {
                v = hex_nibble(s[i]) << 4 | hex_nibble(s[i + 1]);
            } else {
                error_setg(errp, "expect entry %u: '%.2s' is neither hex "
                           "nor two question marks", off->len - 1, s + i);
                goto out;
            }
            g_byte_array_append(val, &v, 1);
            g_byte_array_append(mask, &m, 1);
        }
        uint32_t end = val->len;
        g_array_append_val(off, end);
    }
    h->nexpect = off->len - 1;
    h->exp_off = (uint32_t *)g_array_free(off, false);
    h->exp_val = g_byte_array_free(val, false);
    h->exp_mask = g_byte_array_free(mask, false);
    return true;

out:
    g_array_free(off, true);
    g_byte_array_free(val, true);
    g_byte_array_free(mask, true);
    return false;
}

static PHook *phook_compile(XemuHookSpec *in, Error **errp)
{
    PHook *h = g_new0(PHook, 1);
    int n = 0;

    for (uint64List *l = in->pcs; l; l = l->next) {
        bool dup = false;

        if (l->value > UINT32_MAX) {
            error_setg(errp, "pc 0x%" PRIx64 " is not a 32-bit address",
                       l->value);
            goto fail;
        }
        for (int i = 0; i < h->npcs; i++) {
            dup |= h->pcs[i] == l->value;
        }
        if (dup) {
            continue;
        }
        if (h->npcs == HOOK_MAX_PCS) {
            error_setg(errp, "at most %d pcs per hook", HOOK_MAX_PCS);
            goto fail;
        }
        h->pcs[h->npcs++] = l->value;
    }
    if (!h->npcs) {
        error_setg(errp, "a hook needs at least one pc");
        goto fail;
    }
    for (XemuHookCondList *c = in->cond; c; c = c->next) {
        if (h->npred == XEMU_HOOK_MAX_CONDS) {
            error_setg(errp, "at most %d conditions per hook",
                       XEMU_HOOK_MAX_CONDS);
            goto fail;
        }
        if (!xemu_hook_pred_compile(c->value, &h->pred[h->npred++],
                                    &h->reads_memory, errp)) {
            goto fail;
        }
    }
    if (in->key) {
        Region k;

        if (!region_compile(in->key, &k, HOOK_KEY_MAX, errp)) {
            goto fail;
        }
        h->has_key = true;
        h->key = k.addr;
        h->key_size = k.size;
        h->reads_memory = true;
    }
    for (XemuHookRegionList *l = in->collect; l; l = l->next) {
        n++;
    }
    if (n > HOOK_MAX_REGIONS) {
        error_setg(errp, "at most %d collect regions per hook",
                   HOOK_MAX_REGIONS);
        goto fail;
    }
    h->regions = g_new0(Region, MAX(n, 1));
    for (XemuHookRegionList *l = in->collect; l; l = l->next) {
        Region *r = &h->regions[h->nregions];

        if (!region_compile(l->value, r, 4096, errp)) {
            goto fail;
        }
        h->nregions++;
        h->total += r->size;
        if (h->total > HOOK_MAX_BYTES) {
            error_setg(errp, "a hook collects at most %d bytes a hit",
                       (int)HOOK_MAX_BYTES);
            goto fail;
        }
        h->reads_memory = true;
    }
    if (!expect_compile(h, in->expect, errp)) {
        goto fail;
    }
    h->regs = in->has_regs && in->regs;
    h->stop = !in->has_stop || in->stop;
    h->buf = g_malloc0(MAX(h->total, 1));
    h->readable = g_new0(bool, MAX(h->nregions, 1));
    return h;

fail:
    phook_free(h);
    return NULL;
}

static XemuHookInfo *phook_info(PHook *h)
{
    XemuHookInfo *info = g_new0(XemuHookInfo, 1);
    uint64List **tail = &info->pcs;

    info->id = h->id;
    info->hits = h->hits;
    for (int i = 0; i < h->npcs; i++) {
        QAPI_LIST_APPEND(tail, h->pcs[i]);
    }
    return info;
}

XemuHookInfoList *qmp_xemu_hooks_set(XemuHookSpecList *hooks, Error **errp)
{
    g_autoptr(GPtrArray) add = g_ptr_array_new();
    XemuHookInfoList *ret = NULL, **tail = &ret;
    int n = 0;

    phooks_init();
    /* The hooks go in, and the code they change out, while it is stopped. */
    if (runstate_is_running()) {
        error_setg(errp, "pause the VM first (xemu-frame-advance frames=0)");
        return NULL;
    }
    for (XemuHookSpecList *l = hooks; l; l = l->next) {
        n++;
    }
    if (!n || phooks->len + n > HOOKS_MAX) {
        error_setg(errp, "1 to %d hooks may be installed (%u are)",
                   HOOKS_MAX, phooks->len);
        return NULL;
    }
    for (XemuHookSpecList *l = hooks; l; l = l->next) {
        PHook *h = phook_compile(l->value, errp);

        if (!h) {
            for (guint i = 0; i < add->len; i++) {
                phook_free(g_ptr_array_index(add, i));
            }
            return NULL;
        }
        g_ptr_array_add(add, h);
    }
    for (guint i = 0; i < add->len; i++) {
        PHook *h = g_ptr_array_index(add, i);

        h->id = phook_next_id++;
        for (int j = 0; j < h->npcs; j++) {
            h->engine[j] = xemu_hooks_add(h->pcs[j], XEMU_HOOK_PRIO_ACTION,
                                          phook_fn, h);
        }
        g_ptr_array_add(phooks, h);
        QAPI_LIST_APPEND(tail, phook_info(h));
    }
    xemu_hooks_commit();
    eager_update();
    return ret;
}

void qmp_xemu_hooks_clear(bool has_ids, intList *ids, Error **errp)
{
    GPtrArray *drop = g_ptr_array_new();

    phooks_init();
    for (guint i = 0; i < phooks->len; i++) {
        PHook *h = g_ptr_array_index(phooks, i);
        bool hit = !has_ids;

        for (intList *l = ids; l && !hit; l = l->next) {
            hit = l->value == h->id;
        }
        if (hit) {
            g_ptr_array_add(drop, h);
        }
    }
    for (intList *l = has_ids ? ids : NULL; l; l = l->next) {
        bool found = false;

        for (guint i = 0; i < drop->len; i++) {
            found |= ((PHook *)g_ptr_array_index(drop, i))->id == l->value;
        }
        if (!found) {
            error_setg(errp, "no hook %" PRId64, l->value);
            g_ptr_array_free(drop, true);
            return;
        }
    }
    for (guint i = 0; i < drop->len; i++) {
        PHook *h = g_ptr_array_index(drop, i);

        for (int j = 0; j < h->npcs; j++) {
            xemu_hooks_remove(h->engine[j]);
        }
        g_ptr_array_remove(phooks, h);
    }
    if (drop->len) {
        xemu_hooks_commit();
        eager_update();
    }
    /* A running vCPU may still be in a hook until a grace period ends. */
    for (guint i = 0; i < drop->len; i++) {
        PHook *h = g_ptr_array_index(drop, i);
        call_rcu1(&h->rcu, phook_free_rcu);
    }
    g_ptr_array_free(drop, true);
}

XemuHookInfoList *qmp_query_xemu_hooks(Error **errp)
{
    XemuHookInfoList *ret = NULL, **tail = &ret;

    for (guint i = 0; phooks && i < phooks->len; i++) {
        QAPI_LIST_APPEND(tail, phook_info(g_ptr_array_index(phooks, i)));
    }
    return ret;
}
