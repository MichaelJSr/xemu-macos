/*
 * xemu QMP xemu-hooks-set / xemu-hooks-clear / query-xemu-hooks /
 * xemu-hooks-push: guest PC hooks that stay installed across advances and,
 * at each hit, copy guest memory into a record (PROMPT-7 B2) and write the
 * next entry of a queue (B3), without stopping the VM.
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A hook (PHook) is one engine hook (tcg/xemu-hooks.h) per PC, at the
 * action priority, sharing one state: the hit count, the last key and the
 * expect queue, the write queue. Only the vCPU touches that state while
 * the VM runs; the main thread changes it (xemu-hooks-push) only while
 * the VM is stopped. Hits append to one record buffer in execution order,
 * under rec_lock (the vCPU runs hooks without the BQL), as raw bytes;
 * xemu-frame-advance and xemu-run-until take the buffer into their reply
 * (xemu_hooks_take_trace), where it becomes hex, and xemu-hooks-drain
 * takes it outside them, also while the VM runs.
 *
 * At a hit a hook reads its collect regions, checks its expect entry,
 * takes its next write-queue entry and writes it (xemu_hook_write: RAM
 * only, as the gdbstub writes), then records the hit. A stop (a mismatch,
 * or an empty queue with stop-empty) comes before the write: the engine
 * calls the hook again when the instruction runs on resume
 * (xemu_hooks_stop_rerun), and the write lands then, at the same
 * instruction, so a run cut by stops writes what an uncut run does.
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
#include "system/runstate.h"
#include "tcg/xemu-hooks.h"

#define HOOKS_MAX 64
#define HOOK_MAX_PCS 16
#define HOOK_MAX_REGIONS 64
#define HOOK_MAX_BYTES (64 * KiB)
#define HOOK_KEY_MAX 64
#define REC_MAX (256 * 1024)
#define REC_DATA_MAX (16 * MiB)
#define REC_READABLE 0x80000000u    /* region word: bytes follow */
#define SET_MAX_WAITING (1024 * 1024)   /* queued entries per hook */
#define SET_MAX_BYTES (64 * MiB)        /* queued bytes per hook */
#define SET_COMPACT 1024                /* taken entries kept before a cut */

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
    uint32_t nexpect;
    uint32_t *exp_off;          /* nexpect + 1 offsets into exp_val/mask */
    uint8_t *exp_val, *exp_mask;
    int nwrites;                /* set hooks: the write regions */
    Region *writes;
    uint32_t wtotal;
    bool stop_empty;

    /* vCPU state (read on the main thread while stopped) */
    int64_t hits;
    bool have_key;
    uint8_t last_key[HOOK_KEY_MAX];
    uint8_t *buf;               /* @total bytes */
    bool *readable;             /* per region */
    /*
     * The write queue: entry q_base + i spans [q_off[i], q_off[i + 1]) of
     * q_val (bytes) and q_mask (0xff: write the byte, 0: keep it). The
     * main thread appends while the VM is stopped.
     */
    GArray *q_off;              /* uint32_t */
    GByteArray *q_val, *q_mask;
    int64_t q_base;             /* the entry q_off[0] starts */
    int64_t q_head;             /* the next entry a hit takes */
    int64_t pend;               /* taken by a stop, written on resume */
    bool pend_mismatch;
} PHook;

/* Main thread (BQL). */
static GPtrArray *phooks;       /* PHook, ascending id */
static int phook_next_id = 1;

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
    bool set_hook;              /* the hook has write regions */
    bool empty;
    bool unwritten;
    int64_t set;                /* the entry taken, or -1 */
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
    g_free(h->writes);
    if (h->q_off) {
        g_array_free(h->q_off, true);
        g_byte_array_free(h->q_val, true);
        g_byte_array_free(h->q_mask, true);
    }
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
                       bool mismatch, bool stop, int64_t set, bool empty,
                       bool unwritten)
{
    RecHdr r = {
        .frame = xemu_frame_count(),
        .hit = hit,
        .pc = ctx->pc,
        .hook = h->id,
        .nregions = h->nregions,
        .mismatch = mismatch,
        .has_regs = h->regs,
        .set_hook = h->nwrites > 0,
        .empty = empty,
        .unwritten = unwritten,
        .set = set,
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

static int64_t q_end(PHook *h)
{
    return h->q_base + h->q_off->len - 1;
}

/*
 * Write queue entry @idx over the write regions (with !write, only check
 * that it could be): each region's address is evaluated just before its
 * bytes are written, and only the runs of bytes its mask selects are.
 * False when part of the entry could not be written.
 */
static bool set_apply(XemuHookCtx *ctx, PHook *h, int64_t idx, bool write)
{
    int64_t i = idx - h->q_base;
    uint32_t lo, n, pos = 0;
    const uint8_t *val, *mask;
    bool ok = true;

    assert(i >= 0 && i + 1 < h->q_off->len);
    lo = g_array_index(h->q_off, uint32_t, i);
    n = g_array_index(h->q_off, uint32_t, i + 1) - lo;
    val = h->q_val->data + lo;
    mask = h->q_mask->data + lo;
    for (int r = 0; r < h->nwrites && pos < n; r++) {
        uint32_t len = MIN(h->writes[r].size, n - pos), a = 0, j = 0;
        bool have_addr = false;

        while (j < len) {
            uint32_t k = j;

            if (!mask[pos + j]) {
                j++;
                continue;
            }
            while (k < len && mask[pos + k]) {
                k++;
            }
            if (!have_addr) {
                if (!xemu_hook_expr_eval(ctx, &h->writes[r].addr, &a)) {
                    ok = false;
                    break;
                }
                have_addr = true;
            }
            if (write) {
                ok &= xemu_hook_write(ctx, (uint32_t)(a + j), val + pos + j,
                                      k - j);
            } else {
                ok &= xemu_hook_writable(ctx, (uint32_t)(a + j), k - j);
            }
            j = k;
        }
        pos += h->writes[r].size;
    }
    return ok;
}

static void collect(XemuHookCtx *ctx, PHook *h)
{
    uint32_t a, off = 0;

    for (int i = 0; i < h->nregions; i++) {
        Region *r = &h->regions[i];

        h->readable[i] = xemu_hook_expr_eval(ctx, &r->addr, &a) &&
                         xemu_hook_read(ctx, a, h->buf + off, r->size);
        off += r->size;
    }
}

/*
 * The engine's re-run, when the instruction a stop held back runs again:
 * write the entry the stop took or, after a stop on an empty queue, take
 * the first entry pushed since (recording the hit again with it).
 */
static void phook_resumed(XemuHookCtx *ctx, PHook *h)
{
    int64_t take = h->pend;
    bool unwritten;

    h->pend = -1;
    if (take >= 0) {
        set_apply(ctx, h, take, true);
        return;
    }
    if (h->q_head >= q_end(h)) {
        return;
    }
    take = h->q_head++;
    collect(ctx, h);
    unwritten = !set_apply(ctx, h, take, true);
    rec_append(ctx, h, h->hits - 1, h->pend_mismatch, false, take, false,
               unwritten);
}

static void phook_fn(XemuHookCtx *ctx, void *opaque)
{
    PHook *h = opaque;
    uint32_t a;
    int64_t hit, take = -1;
    bool mismatch, stop, empty = false;

    if (unlikely(ctx->resumed)) {
        phook_resumed(ctx, h);
        return;
    }
    for (int i = 0; i < h->npred; i++) {
        if (!xemu_hook_pred_test(ctx, &h->pred[i])) {
            return;
        }
    }
    if (h->has_key) {
        uint8_t key[HOOK_KEY_MAX];

        if (!xemu_hook_expr_eval(ctx, &h->key, &a) ||
            !xemu_hook_read(ctx, a, key, h->key_size)) {
            return;
        }
        if (h->have_key && !memcmp(key, h->last_key, h->key_size)) {
            return;
        }
        memcpy(h->last_key, key, h->key_size);
        h->have_key = true;
    }
    hit = h->hits++;
    collect(ctx, h);
    mismatch = hit < h->nexpect && expect_differs(h, hit);
    stop = mismatch && h->stop;
    if (h->nwrites) {
        if (h->q_head < q_end(h)) {
            take = h->q_head++;
        } else {
            empty = true;
            stop |= h->stop_empty;
        }
    }
    if (!stop) {
        bool unwritten = take >= 0 && !set_apply(ctx, h, take, true);

        rec_append(ctx, h, hit, mismatch, false, take, empty, unwritten);
        return;
    }
    rec_append(ctx, h, hit, mismatch, true, take, empty,
               take >= 0 && !set_apply(ctx, h, take, false));
    h->pend = take;
    h->pend_mismatch = mismatch;
    /*
     * Takes the BQL: never with rec_lock held. Doesn't return if the stop
     * held; a set hook writes on resume (phook_resumed).
     */
    if (!(h->nwrites ? xemu_hooks_stop_rerun(ctx) : xemu_hooks_stop(ctx))) {
        /* No advance to end: the VM runs on, so write now. */
        qemu_mutex_lock(&rec_lock);
        rec_stop = -1;
        qemu_mutex_unlock(&rec_lock);
        h->pend = -1;
        if (take >= 0) {
            set_apply(ctx, h, take, true);
        }
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
    if (r->set_hook && r->set >= 0) {
        rec->has_set = true;
        rec->set = r->set;
    }
    if (r->empty) {
        rec->has_empty = true;
        rec->empty = true;
    }
    if (r->unwritten) {
        rec->has_unwritten = true;
        rec->unwritten = true;
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

/*
 * Append @list's hex strings ("??" a masked byte) as entries of at most
 * @max bytes: bytes to @val, masks (0xff, or 0 for "??") to @mask, each
 * entry's end offset to @off. On error nothing is appended.
 */
static bool hex_entries_append(GByteArray *val, GByteArray *mask,
                               GArray *off, strList *list, uint32_t max,
                               const char *what, const char *of,
                               Error **errp)
{
    guint val0 = val->len, off0 = off->len;
    unsigned idx = 0;

    for (strList *l = list; l; l = l->next, idx++) {
        const char *s = l->value;
        size_t n = strlen(s);

        if (n % 2 || n / 2 > max) {
            error_setg(errp, "%s entry %u: %s", what, idx,
                       n % 2 ? "odd number of hex digits" : of);
            goto undo;
        }
        for (size_t i = 0; i < n; i += 2) {
            uint8_t v = 0, m = 0xff;

            if (s[i] == '?' && s[i + 1] == '?') {
                m = 0;
            } else if (hex_nibble(s[i]) >= 0 && hex_nibble(s[i + 1]) >= 0) {
                v = hex_nibble(s[i]) << 4 | hex_nibble(s[i + 1]);
            } else {
                error_setg(errp, "%s entry %u: '%.2s' is neither hex "
                           "nor two question marks", what, idx, s + i);
                goto undo;
            }
            g_byte_array_append(val, &v, 1);
            g_byte_array_append(mask, &m, 1);
        }
        uint32_t end = val->len;
        g_array_append_val(off, end);
    }
    return true;

undo:
    g_byte_array_set_size(val, val0);
    g_byte_array_set_size(mask, val0);
    g_array_set_size(off, off0);
    return false;
}

static bool expect_compile(PHook *h, strList *list, Error **errp)
{
    GByteArray *val = g_byte_array_new(), *mask = g_byte_array_new();
    GArray *off = g_array_new(false, false, sizeof(uint32_t));
    uint32_t zero = 0;

    g_array_append_val(off, zero);
    if (!hex_entries_append(val, mask, off, list, h->total, "expect",
                            "longer than the collected bytes", errp)) {
        g_array_free(off, true);
        g_byte_array_free(val, true);
        g_byte_array_free(mask, true);
        return false;
    }
    h->nexpect = off->len - 1;
    h->exp_off = (uint32_t *)g_array_free(off, false);
    h->exp_val = g_byte_array_free(val, false);
    h->exp_mask = g_byte_array_free(mask, false);
    return true;
}

/*
 * Append write-queue entries (the VM stopped), within the caps on what may
 * wait; first drop entries long taken (keeping one a stop holds back).
 */
static bool set_push(PHook *h, strList *list, Error **errp)
{
    int64_t keep = h->pend >= 0 ? MIN(h->pend, h->q_head) : h->q_head;
    guint drop = keep - h->q_base;

    if (drop >= SET_COMPACT) {
        uint32_t cut = g_array_index(h->q_off, uint32_t, drop);

        g_byte_array_remove_range(h->q_val, 0, cut);
        g_byte_array_remove_range(h->q_mask, 0, cut);
        g_array_remove_range(h->q_off, 0, drop);
        for (guint i = 0; i < h->q_off->len; i++) {
            g_array_index(h->q_off, uint32_t, i) -= cut;
        }
        h->q_base = keep;
    }

    guint off0 = h->q_off->len, val0 = h->q_val->len;
    if (!hex_entries_append(h->q_val, h->q_mask, h->q_off, list, h->wtotal,
                            "set", "longer than the write regions", errp)) {
        return false;
    }
    int64_t waiting = q_end(h) - h->q_head;
    uint32_t from = g_array_index(h->q_off, uint32_t, h->q_head - h->q_base);
    if (waiting > SET_MAX_WAITING || h->q_val->len - from > SET_MAX_BYTES) {
        error_setg(errp, "at most %d entries or %d MiB may wait in a hook's "
                   "queue", SET_MAX_WAITING, (int)(SET_MAX_BYTES / MiB));
        g_byte_array_set_size(h->q_val, val0);
        g_byte_array_set_size(h->q_mask, val0);
        g_array_set_size(h->q_off, off0);
        return false;
    }
    return true;
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
        if (!xemu_hook_pred_compile(c->value, &h->pred[h->npred++], errp)) {
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
    }
    if (!expect_compile(h, in->expect, errp)) {
        goto fail;
    }
    n = 0;
    for (XemuHookRegionList *l = in->write; l; l = l->next) {
        n++;
    }
    if (n > HOOK_MAX_REGIONS) {
        error_setg(errp, "at most %d write regions per hook",
                   HOOK_MAX_REGIONS);
        goto fail;
    }
    h->writes = g_new0(Region, MAX(n, 1));
    for (XemuHookRegionList *l = in->write; l; l = l->next) {
        Region *r = &h->writes[h->nwrites];

        if (!region_compile(l->value, r, 4096, errp)) {
            goto fail;
        }
        h->nwrites++;
        h->wtotal += r->size;
        if (h->wtotal > HOOK_MAX_BYTES) {
            error_setg(errp, "a hook writes at most %d bytes a hit",
                       (int)HOOK_MAX_BYTES);
            goto fail;
        }
    }
    h->stop_empty = in->has_stop_empty && in->stop_empty;
    if (!h->nwrites && (in->set || h->stop_empty)) {
        error_setg(errp, "set and stop-empty need write regions");
        goto fail;
    }
    h->pend = -1;
    if (h->nwrites) {
        uint32_t zero = 0;

        h->q_off = g_array_new(false, false, sizeof(uint32_t));
        h->q_val = g_byte_array_new();
        h->q_mask = g_byte_array_new();
        g_array_append_val(h->q_off, zero);
        if (!set_push(h, in->set, errp)) {
            goto fail;
        }
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
    if (h->nwrites) {
        info->has_queued = true;
        info->queued = q_end(h) - h->q_head;
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
    }
    /* A running vCPU may still be in a hook until a grace period ends. */
    for (guint i = 0; i < drop->len; i++) {
        PHook *h = g_ptr_array_index(drop, i);
        call_rcu1(&h->rcu, phook_free_rcu);
    }
    g_ptr_array_free(drop, true);
}

XemuHookInfo *qmp_xemu_hooks_push(int64_t id, strList *set, Error **errp)
{
    PHook *h = NULL;

    phooks_init();
    /* The queue is the vCPU's while the VM runs. */
    if (runstate_is_running()) {
        error_setg(errp, "pause the VM first (xemu-frame-advance frames=0)");
        return NULL;
    }
    for (guint i = 0; i < phooks->len && !h; i++) {
        PHook *p = g_ptr_array_index(phooks, i);
        h = p->id == id ? p : NULL;
    }
    if (!h) {
        error_setg(errp, "no hook %" PRId64, id);
        return NULL;
    }
    if (!h->nwrites) {
        error_setg(errp, "hook %" PRId64 " has no write regions", id);
        return NULL;
    }
    if (!set_push(h, set, errp)) {
        return NULL;
    }
    return phook_info(h);
}

XemuHookTrace *qmp_xemu_hooks_drain(Error **errp)
{
    /*
     * QMP runs one command at a time, so no advance (which takes the
     * records itself, its stop included) is under way here. The vCPU
     * appends under rec_lock: the VM may run. A stop marked here is one
     * the hook is still about to find no advance for (it waits for the
     * BQL we hold, then clears it and runs on), so it is no stop.
     */
    XemuHookTrace *t = xemu_hooks_take_trace();

    if (!t) {
        return g_new0(XemuHookTrace, 1);
    }
    qapi_free_XemuHookRecord(t->stop);
    t->stop = NULL;
    return t;
}

XemuHookInfoList *qmp_query_xemu_hooks(Error **errp)
{
    XemuHookInfoList *ret = NULL, **tail = &ret;

    for (guint i = 0; phooks && i < phooks->len; i++) {
        QAPI_LIST_APPEND(tail, phook_info(g_ptr_array_index(phooks, i)));
    }
    return ret;
}
