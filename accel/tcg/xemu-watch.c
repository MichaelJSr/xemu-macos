/*
 * xemu: guest memory access log (QMP xemu-watch-start/-drain/-stop)
 *
 * A watched page gets TLB_WATCHPOINT in its TLB entries at refill
 * (xemu_watch_tlb_flags), as a gdb watchpoint's page does, so its loads and
 * stores take the softmmu slow path, where cputlb.c calls xemu_watch_check.
 * A hit is only recorded: no state restore, no cpu_loop_exit, no
 * invalidation, so the guest runs as it would without the log (under
 * icount, the same RAM). The pc comes from the TB's unwind data, the way a
 * fault would restore it, without writing the CPU state.
 *
 * A store's value is not known before it happens, so a write hit keeps its
 * host address and reads the bytes back later: at the next hit (unless that
 * is the same call site on other bytes, such as the second page of a store
 * that crosses one, which runs before either store), from a vCPU work
 * item, and at drain or stop. The first pending write queues the work
 * item, and queueing kicks the vCPU (cpu_exit), so it leaves its chain of
 * translated blocks at the next block boundary to run it: one exit per
 * write hit that finds no read-back queued. In between only guest stores
 * through other mappings (virtual ranges) or device DMA could change those
 * bytes.
 *
 * Beside gdb watchpoints: an access that hits one leaves before it
 * happens, and its insn reruns alone; the log skips what the rerun repeats
 * (xemu_watch_check).
 *
 * Everything but the QMP argument checks runs on the vCPU thread: the hook
 * and TLB refills inside translated code, the rest as run_on_cpu work, so
 * the log needs no lock. The Xbox has one vCPU; others are ignored.
 *
 * Copyright (c) 2026 xemu-macos contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#if defined(XBOX)

#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "accel/tcg/cpu-ops.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "exec/tlb-common.h"
#include "exec/translation-block.h"
#include "exec/xemu-watch.h"
#include "hw/core/cpu.h"
#include "system/tcg.h"
#include "tcg/insn-start-words.h"
#include "tcg/tcg.h"
#include "tb-internal.h"

#define WATCH_MAX_RANGES 64
#define WATCH_MAX_SIZE (16 * MiB)
#define WATCH_MAX_HITS 1000000
#define WATCH_DEFAULT_HITS 100000
#define WATCH_VALUE_MAX 64
#define WATCH_PEND_MAX 16
/* Virtual pages flushed one by one at start and stop; more: a full flush. */
#define WATCH_FLUSH_PAGES 1024

int64_t xemu_frame_count(void);

typedef struct WatchRange {
    uint64_t lo, hi;            /* [lo, hi), virtual, or physical I/O */
    uintptr_t host_lo, host_hi; /* a physical range in RAM: its host bytes */
    bool virt;
    int access;                 /* BP_MEM_READ | BP_MEM_WRITE */
} WatchRange;

typedef struct WatchHit {
    int64_t frame;
    uint64_t pc, addr, paddr;
    uint32_t size;
    uint32_t voff;              /* value bytes in watch_values */
    uint8_t vlen;               /* 0: no value (I/O) */
    uint8_t access;             /* BP_MEM_READ | BP_MEM_WRITE */
    bool pending;               /* a write not read back yet */
    const uint8_t *host;
    uintptr_t ra;
} WatchHit;

typedef struct WatchLog {
    GArray *hits;               /* WatchHit */
    GByteArray *values;
    int64_t dropped;
} WatchLog;

bool xemu_watch_active;

/* vCPU thread only. */
static CPUState *watch_cpu;
static WatchRange *watch_ranges;
static int watch_nranges;
static int64_t watch_max;
static WatchLog watch_log;
static uint32_t watch_pend[WATCH_PEND_MAX];
static int watch_npend;
static bool watch_fill_queued;
/* A gdb watchpoint hit's one-insn rerun (see xemu_watch_check). */
static enum {
    WATCH_RERUN_NONE,
    WATCH_RERUN_SKIP,           /* expected: skip up to the access that hit */
    WATCH_RERUN_LOG,            /* in it, at or past that access */
} watch_rerun;

/* Main thread, BQL: whether a log runs. */
static bool watch_started;

static bool overlaps(uint64_t lo, uint64_t len, uint64_t rlo, uint64_t rhi)
{
    return lo < rhi && lo + len > rlo;
}

static void watch_read_back(WatchHit *h)
{
    memcpy(watch_log.values->data + h->voff, h->host, h->vlen);
    h->pending = false;
}

/*
 * Read back the pending writes, except, with @ra, those from call site @ra
 * that don't overlap @host: the access now starting may be the rest of the
 * same store (its other page), which runs after both checks.
 */
static void watch_fill(const uint8_t *host, vaddr len, uintptr_t ra)
{
    int keep = 0;

    for (int i = 0; i < watch_npend; i++) {
        WatchHit *h = &g_array_index(watch_log.hits, WatchHit, watch_pend[i]);

        if (ra && h->ra == ra &&
            !(host && overlaps((uintptr_t)host, len, (uintptr_t)h->host,
                               (uintptr_t)h->host + h->size))) {
            watch_pend[keep++] = watch_pend[i];
        } else {
            watch_read_back(h);
        }
    }
    watch_npend = keep;
}

static void watch_fill_work(CPUState *cpu, run_on_cpu_data data)
{
    watch_fill_queued = false;
    if (watch_log.hits) {
        watch_fill(NULL, 0, 0);
    }
}

/*
 * x86: insn_start word 0 is the linear pc, or under CF_PCREL its offset in
 * the page of the TB's first insn, which every insn of a TB starts on and
 * env->eip stays on until the TB exits (x86_restore_state_to_opc).
 */
static uint64_t watch_pc(CPUState *cpu, uintptr_t ra)
{
    uint64_t data[INSN_START_WORDS];
    TranslationBlock *tb;

    if (ra && cpu_unwind_state_data(cpu, ra, data) &&
        (tb = tcg_tb_lookup(ra))) {
        if (tb_cflags(tb) & CF_PCREL) {
            return (cpu->cc->get_pc(cpu) & TARGET_PAGE_MASK) | data[0];
        }
        return data[0];
    }
    /* From a helper that saved the state first, as for a fault. */
    return cpu->cc->get_pc(cpu);
}

int xemu_watch_page_flags(CPUState *cpu, vaddr page, hwaddr ppage,
                          uintptr_t host_page)
{
    int flags = 0;

    if (cpu != watch_cpu) {
        return 0;
    }
    for (int i = 0; i < watch_nranges; i++) {
        WatchRange *r = &watch_ranges[i];
        bool hit;

        if (r->virt) {
            hit = overlaps(page, TARGET_PAGE_SIZE, r->lo, r->hi);
        } else if (r->host_lo) {
            hit = host_page &&
                  overlaps(host_page, TARGET_PAGE_SIZE, r->host_lo, r->host_hi);
        } else {
            hit = overlaps(ppage, TARGET_PAGE_SIZE, r->lo, r->hi);
        }
        if (hit) {
            flags |= r->access;
        }
    }
    return flags;
}

void xemu_watch_check(CPUState *cpu, vaddr addr, vaddr len, int flags,
                      CPUTLBEntryFull *full, int tlb_flags, void *host,
                      uintptr_t ra)
{
    hwaddr paddr = full->phys_addr | (addr & ~TARGET_PAGE_MASK);
    const uint8_t *h8 = tlb_flags & TLB_MMIO ? NULL : host;
    bool rerun;
    int match = 0;

    if (likely(!qatomic_read(&xemu_watch_active))) {
        return;
    }
    if (cpu != watch_cpu) {
        return;
    }
    /*
     * An access in translated code that hits a gdb (or debug-register)
     * watchpoint leaves before it happens (cpu_check_watchpoint, right
     * after this), and its insn reruns alone with cpu->watchpoint_hit set.
     * That rerun repeats the accesses before it, which this pass logged,
     * then makes it and the rest: log from it on. Nothing reads stores back
     * in the rerun: the first pass's are pending too, and a page-crossing
     * store writes neither page before both are checked. The insn's end or
     * the work item reads them back. (A hit in an interrupt delivery goes
     * ahead without a rerun.)
     */
    if (!cpu->watchpoint_hit) {
        watch_rerun = WATCH_RERUN_NONE;
        if ((cpu_watchpoint_address_matches(cpu, addr, len) & flags) &&
            cpu_watchpoint_hit_reruns(cpu, ra)) {
            watch_rerun = WATCH_RERUN_SKIP;
            return;
        }
    } else if (watch_rerun == WATCH_RERUN_SKIP) {
        if (!(cpu_watchpoint_address_matches(cpu, addr, len) & flags)) {
            return;
        }
        watch_rerun = WATCH_RERUN_LOG;
    }
    rerun = watch_rerun == WATCH_RERUN_LOG;
    /*
     * I/O that io_prepare sends through cpu_io_recompile, which reruns it
     * (icount; cpu->neg.can_do_io stays true there, xemu_io_ok).
     */
    if (!h8 && !xemu_io_ok(cpu, ra)) {
        return;
    }
    if (watch_npend && !rerun) {
        watch_fill(h8, len, ra);
    }
    for (int i = 0; i < watch_nranges; i++) {
        WatchRange *r = &watch_ranges[i];
        bool hit;

        if (!(r->access & flags)) {
            continue;
        }
        if (r->virt) {
            hit = overlaps(addr, len, r->lo, r->hi);
        } else if (r->host_lo) {
            hit = h8 && overlaps((uintptr_t)h8, len, r->host_lo, r->host_hi);
        } else {
            hit = overlaps(paddr, len, r->lo, r->hi);
        }
        if (hit) {
            match |= r->access & flags;
        }
    }
    if (!match) {
        return;
    }
    if (watch_log.hits->len >= watch_max) {
        watch_log.dropped++;
        return;
    }

    WatchHit hit = {
        .frame = xemu_frame_count(),
        .pc = watch_pc(cpu, ra),
        .addr = addr,
        .paddr = paddr,
        .size = len,
        .access = match,
        .host = h8,
        .ra = ra,
    };
    if (h8) {
        hit.vlen = MIN(len, WATCH_VALUE_MAX);
        hit.voff = watch_log.values->len;
        /* A read's value; a write's is read back once it has happened. */
        g_byte_array_append(watch_log.values, h8, hit.vlen);
        hit.pending = match & BP_MEM_WRITE;
    }
    g_array_append_val(watch_log.hits, hit);
    if (hit.pending) {
        if (watch_npend == WATCH_PEND_MAX) {
            /* Not reached by one insn's stores; keep the newest. */
            watch_read_back(&g_array_index(watch_log.hits, WatchHit,
                                           watch_pend[0]));
            memmove(watch_pend, watch_pend + 1,
                    sizeof(watch_pend[0]) * --watch_npend);
        }
        watch_pend[watch_npend++] = watch_log.hits->len - 1;
        if (!watch_fill_queued) {
            /* Kicks the vCPU: runs at the end of the current block. */
            watch_fill_queued = true;
            async_run_on_cpu(cpu, watch_fill_work, RUN_ON_CPU_NULL);
        }
    }
}

/* Drop the TLB entries of the watched pages; refills set or clear traps. */
static void watch_flush_tlb(CPUState *cpu)
{
    int pages = 0;
    bool full = false;

    for (int i = 0; i < watch_nranges; i++) {
        WatchRange *r = &watch_ranges[i];

        if (r->virt) {
            pages += ((r->hi - 1) >> TARGET_PAGE_BITS) -
                     (r->lo >> TARGET_PAGE_BITS) + 1;
        } else if (!r->host_lo) {
            full = true;
        }
    }
    if (full || pages > WATCH_FLUSH_PAGES) {
        tlb_flush(cpu);
        return;
    }
    for (int i = 0; i < watch_nranges; i++) {
        WatchRange *r = &watch_ranges[i];

        if (r->virt) {
            for (uint64_t p = r->lo & TARGET_PAGE_MASK; p < r->hi;
                 p += TARGET_PAGE_SIZE) {
                tlb_flush_page(cpu, p);
            }
        } else {
            tlb_flush_host_range(cpu, r->host_lo, r->host_hi - r->host_lo);
        }
    }
}

static void watch_log_new(void)
{
    watch_log.hits = g_array_new(false, false, sizeof(WatchHit));
    watch_log.values = g_byte_array_new();
    watch_log.dropped = 0;
    watch_npend = 0;
}

typedef struct WatchStart {
    WatchRange *ranges;
    int n;
    int64_t max;
} WatchStart;

static void watch_start_work(CPUState *cpu, run_on_cpu_data data)
{
    WatchStart *s = data.host_ptr;

    watch_cpu = cpu;
    watch_ranges = s->ranges;
    watch_nranges = s->n;
    watch_max = s->max;
    watch_rerun = WATCH_RERUN_NONE;
    watch_log_new();
    qatomic_set(&xemu_watch_active, true);
    watch_flush_tlb(cpu);
}

typedef struct WatchTake {
    WatchLog log;
    bool stop;
} WatchTake;

/* Hand the log to the main thread; start a new one unless stopping. */
static void watch_take_work(CPUState *cpu, run_on_cpu_data data)
{
    WatchTake *t = data.host_ptr;

    watch_fill(NULL, 0, 0);
    t->log = watch_log;
    if (t->stop) {
        qatomic_set(&xemu_watch_active, false);
        watch_flush_tlb(cpu);
        g_free(watch_ranges);
        watch_ranges = NULL;
        watch_nranges = 0;
        watch_cpu = NULL;
        watch_log = (WatchLog){ 0 };
        watch_npend = 0;
    } else {
        watch_log_new();
    }
}

static XemuWatchLog *watch_log_to_qapi(WatchLog *l)
{
    XemuWatchLog *ret = g_new0(XemuWatchLog, 1);
    XemuWatchHitList **tail = &ret->hits;

    for (guint i = 0; i < l->hits->len; i++) {
        WatchHit *h = &g_array_index(l->hits, WatchHit, i);
        XemuWatchHit *q = g_new0(XemuWatchHit, 1);

        q->frame = h->frame;
        q->pc = h->pc;
        q->address = h->addr;
        q->physical = h->paddr;
        q->size = h->size;
        q->access = h->access == BP_MEM_READ ? XEMU_WATCH_ACCESS_R :
                    h->access == BP_MEM_WRITE ? XEMU_WATCH_ACCESS_W :
                    XEMU_WATCH_ACCESS_RW;
        if (h->vlen) {
            q->value = g_string_free(
                qemu_hexdump_line(NULL, l->values->data + h->voff, h->vlen,
                                  0, 0), false);
        }
        QAPI_LIST_APPEND(tail, q);
    }
    ret->dropped = l->dropped;
    g_array_free(l->hits, true);
    g_byte_array_free(l->values, true);
    return ret;
}

void qmp_xemu_watch_start(XemuWatchRangeList *ranges, bool has_max,
                          int64_t max, Error **errp)
{
    RAMBlock *rb = qemu_ram_block_by_name("xbox.ram");
    uintptr_t ram = rb ? (uintptr_t)qemu_ram_get_host_addr(rb) : 0;
    uint64_t ram_size = rb ? qemu_ram_get_used_length(rb) : 0;
    WatchStart s = { 0 };

    if (!tcg_enabled() || !first_cpu) {
        error_setg(errp, "the access log needs TCG");
        return;
    }
    if (watch_started) {
        error_setg(errp, "an access log is already running "
                   "(xemu-watch-stop ends it)");
        return;
    }
    if (!has_max) {
        max = WATCH_DEFAULT_HITS;
    }
    if (max < 1 || max > WATCH_MAX_HITS) {
        error_setg(errp, "max must be 1 to %d", WATCH_MAX_HITS);
        return;
    }
    for (XemuWatchRangeList *l = ranges; l; l = l->next) {
        s.n++;
    }
    if (s.n < 1 || s.n > WATCH_MAX_RANGES) {
        error_setg(errp, "give 1 to %d ranges", WATCH_MAX_RANGES);
        return;
    }
    s.ranges = g_new0(WatchRange, s.n);
    s.max = max;
    int i = 0;
    for (XemuWatchRangeList *l = ranges; l; l = l->next, i++) {
        XemuWatchRange *q = l->value;
        WatchRange *r = &s.ranges[i];

        if (q->size < 1 || q->size > WATCH_MAX_SIZE ||
            q->address > 4 * GiB - q->size) {
            error_setg(errp, "range %d: size must be 1 to 16 MiB and the "
                       "range must end at or below 4 GiB", i);
            g_free(s.ranges);
            return;
        }
        r->lo = q->address;
        r->hi = q->address + q->size;
        r->virt = q->has_q_virtual && q->q_virtual;
        r->access = q->access == XEMU_WATCH_ACCESS_R ? BP_MEM_READ :
                    q->access == XEMU_WATCH_ACCESS_W ? BP_MEM_WRITE :
                    BP_MEM_ACCESS;
        if (!r->virt && r->lo < ram_size) {
            if (r->hi > ram_size) {
                error_setg(errp, "range %d: a physical range lies in RAM "
                           "(below 0x%" PRIx64 ") or outside it", i,
                           ram_size);
                g_free(s.ranges);
                return;
            }
            r->host_lo = ram + r->lo;
            r->host_hi = ram + r->hi;
        }
    }
    run_on_cpu(first_cpu, watch_start_work, RUN_ON_CPU_HOST_PTR(&s));
    watch_started = true;
}

static XemuWatchLog *watch_take(bool stop, Error **errp)
{
    WatchTake t = { .stop = stop };

    if (!watch_started) {
        error_setg(errp, "no access log is running (see xemu-watch-start)");
        return NULL;
    }
    run_on_cpu(first_cpu, watch_take_work, RUN_ON_CPU_HOST_PTR(&t));
    if (stop) {
        watch_started = false;
    }
    return watch_log_to_qapi(&t.log);
}

XemuWatchLog *qmp_xemu_watch_drain(Error **errp)
{
    return watch_take(false, errp);
}

XemuWatchLog *qmp_xemu_watch_stop(Error **errp)
{
    return watch_take(true, errp);
}

#endif /* XBOX */
