/*
 * Translation Block Maintenance
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/cacheflush.h"
#include "qemu/interval-tree.h"
#include "qemu/qtree.h"
#include "exec/cputlb.h"
#include "exec/log.h"
#include "exec/page-protection.h"
#include "exec/mmap-lock.h"
#include "exec/tb-flush.h"
#include "exec/target_page.h"
#include "accel/tcg/cpu-ops.h"
#include "tb-internal.h"
#include "system/tcg.h"
#include "tcg/tcg.h"
#include "tb-hash.h"
#include "tb-context.h"
#include "tb-internal.h"
#include "internal-common.h"
#ifdef CONFIG_USER_ONLY
#include "user/page-protection.h"
#define runstate_is_running()  true
#else
#include "system/runstate.h"
#endif
#if defined(XBOX) && !defined(CONFIG_USER_ONLY)
#include "qemu/fast-hash.h"
#include "qemu/rcu.h"
#include "system/memory.h"
#include "system/physmem.h"
#endif
#include "trace.h"
#include "xemu-inv-prof.h"
#include "xemu-xpage.h"
#include "exec/xemu-subpage-fast.h"

#if defined(XBOX)
/*
 * XEMU_INV_PROF=1: SMC / TB-invalidation-churn counters. See
 * xemu-inv-prof.h for the model (plain uint64, single vCPU writer, exit
 * dump). Definitions live here because tb-maint.c owns the invalidation
 * paths; the recycle (c) and census (d) sites reference these externs.
 */
int xemu_inv_cur_src;

uint64_t xemu_inv_total;
uint64_t xemu_inv_by_src[XEMU_INV_SRC_MAX];

uint64_t xemu_inv_range_evals;
uint64_t xemu_inv_false_share;

uint64_t xemu_inv_traps;      /* notdirty writes that reached a code page */
uint64_t xemu_inv_unprotect;  /* pages emptied of TBs -> tlb_unprotect_code */

uint64_t xemu_nd_calls;
uint64_t xemu_nd_calls_inval;
uint64_t xemu_nd_ticks_total;
uint64_t xemu_nd_ticks_inval;

/* Sub-page dirty tracking (arm (a) + refuter); defined here so the dump sees
 * them. Latches/helpers live after xemu_inv_timing_on(). */
uint64_t xemu_subpage_skips;
uint64_t xemu_subpage_refute_total;
uint64_t xemu_subpage_refute_hits;
uint64_t xemu_subpage_refute_violations;

uint64_t xemu_inv_recycle_attempts;
uint64_t xemu_inv_recycle_hits;
uint64_t xemu_inv_recycle_true_smc;
uint64_t xemu_inv_recycle_cold;

uint64_t xemu_inv_span_nochain;

/*
 * (f) MMIO recompile census, counted in cputlb.c's io_prepare. The
 * recompiles are exactly what the can_do_io elision (translator.c)
 * removes, so normal-mode runs read ~0; icount and replay keep the stores
 * and populate it.
 */
uint64_t xemu_io_prepare_calls;
uint64_t xemu_io_recompiles;

/* Wall clock at the moment the dump was armed, for per-second rates. */
static int64_t xemu_inv_prof_t0_us;

uint64_t xemu_inv_gototb_emitted;
uint64_t xemu_inv_jcprobe_emitted;
uint64_t xemu_inv_retmemo_emitted;

static void xemu_inv_prof_dump(void)
{
    uint64_t t = xemu_inv_total;
    uint64_t re = xemu_inv_range_evals;
    uint64_t rec_seen = xemu_inv_recycle_hits + xemu_inv_recycle_true_smc;
    uint64_t exits = xemu_inv_gototb_emitted + xemu_inv_jcprobe_emitted +
                     xemu_inv_retmemo_emitted;

    fprintf(stderr, "xemu: INV_PROF summary (tb_flush=%u)\n",
            qatomic_read(&tb_ctx.tb_flush_count));

    fprintf(stderr,
            "xemu:  (a) invalidations total=%llu  notdirty=%llu (%.1f%%)  "
            "explicit=%llu (%.1f%%)  single=%llu (%.1f%%)  other=%llu (%.1f%%)\n",
            (unsigned long long)t,
            (unsigned long long)xemu_inv_by_src[XEMU_INV_SRC_NOTDIRTY],
            t ? 100.0 * xemu_inv_by_src[XEMU_INV_SRC_NOTDIRTY] / t : 0.0,
            (unsigned long long)xemu_inv_by_src[XEMU_INV_SRC_EXPLICIT],
            t ? 100.0 * xemu_inv_by_src[XEMU_INV_SRC_EXPLICIT] / t : 0.0,
            (unsigned long long)xemu_inv_by_src[XEMU_INV_SRC_SINGLE],
            t ? 100.0 * xemu_inv_by_src[XEMU_INV_SRC_SINGLE] / t : 0.0,
            (unsigned long long)xemu_inv_by_src[XEMU_INV_SRC_OTHER],
            t ? 100.0 * xemu_inv_by_src[XEMU_INV_SRC_OTHER] / t : 0.0);

    fprintf(stderr,
            "xemu:  (b) whole-page range_evals=%llu  false_share=%llu "
            "(%.1f%% would be skipped by byte-range check)\n",
            (unsigned long long)re,
            (unsigned long long)xemu_inv_false_share,
            re ? 100.0 * xemu_inv_false_share / re : 0.0);

    fprintf(stderr,
            "xemu:  (b') notdirty traps(code page)=%llu  unprotect(page emptied)"
            "=%llu  invals/trap=%.2f\n",
            (unsigned long long)xemu_inv_traps,
            (unsigned long long)xemu_inv_unprotect,
            xemu_inv_traps ? (double)xemu_inv_total / xemu_inv_traps : 0.0);

    if (xemu_inv_timing_on() || xemu_nd_calls) {
        double ns = xemu_inv_tick_ns();
        uint64_t rest = xemu_nd_ticks_total - xemu_nd_ticks_inval;
        double tot_ns = xemu_nd_ticks_total * ns;
        double inv_ns = xemu_nd_ticks_inval * ns;
        double rest_ns = (double)rest * ns;
        /* Calibrate the bracketing read cost: two back-to-back reads, floored
         * over many iters. The body is bracketed by 2 reads (total) with 2
         * more inside (inval) => ~2 read-pairs of overhead per call sits inside
         * the measured segments; report it so the split can be de-biased. */
        uint64_t cal = ~0ULL;
        for (int i = 0; i < 4096; i++) {
            uint64_t a = xemu_inv_ticks();
            uint64_t b = xemu_inv_ticks();
            if (b - a < cal) {
                cal = b - a;
            }
        }
        fprintf(stderr,
                "xemu:  (b'') notdirty_write body timing: calls=%llu "
                "(inval=%llu, no-inval=%llu)  tick=%.2fns read-pair-floor=%llut\n"
                "xemu:       total=%.3fms (%.1fns/call)  inval=%.3fms "
                "(%.1fns/call-inval, %.1f%% of body)  rest=%.3fms (%.1fns/call)\n",
                (unsigned long long)xemu_nd_calls,
                (unsigned long long)xemu_nd_calls_inval,
                (unsigned long long)(xemu_nd_calls - xemu_nd_calls_inval),
                ns, (unsigned long long)cal,
                tot_ns / 1.0e6,
                xemu_nd_calls ? tot_ns / xemu_nd_calls : 0.0,
                inv_ns / 1.0e6,
                xemu_nd_calls_inval ? inv_ns / xemu_nd_calls_inval : 0.0,
                xemu_nd_ticks_total ?
                    100.0 * xemu_nd_ticks_inval / xemu_nd_ticks_total : 0.0,
                rest_ns / 1.0e6,
                xemu_nd_calls ? rest_ns / xemu_nd_calls : 0.0);
    }

    fprintf(stderr,
            "xemu:  (c) recycle attempts=%llu  hits=%llu  true_smc=%llu  "
            "cold=%llu  |  hit-rate-on-invalidated-set=%.1f%%\n",
            (unsigned long long)xemu_inv_recycle_attempts,
            (unsigned long long)xemu_inv_recycle_hits,
            (unsigned long long)xemu_inv_recycle_true_smc,
            (unsigned long long)xemu_inv_recycle_cold,
            rec_seen ? 100.0 * xemu_inv_recycle_hits / rec_seen : 0.0);

    fprintf(stderr,
            "xemu:  (d) exits: goto_tb=%llu (%.1f%%) jc_probe=%llu (%.1f%%) "
            "ret_memo=%llu (%.1f%%)\n",
            (unsigned long long)xemu_inv_gototb_emitted,
            exits ? 100.0 * xemu_inv_gototb_emitted / exits : 0.0,
            (unsigned long long)xemu_inv_jcprobe_emitted,
            exits ? 100.0 * xemu_inv_jcprobe_emitted / exits : 0.0,
            (unsigned long long)xemu_inv_retmemo_emitted,
            exits ? 100.0 * xemu_inv_retmemo_emitted / exits : 0.0);

    /* Runtime count of chain refusals at the spanning-dest guard (each a
     * full dispatch-loop exit); near zero since gen_jmp_rel's page-end rule. */
    fprintf(stderr,
            "xemu:  (d') spanning-dest chain declines=%llu\n",
            (unsigned long long)xemu_inv_span_nochain);

    {
        double secs = xemu_inv_prof_t0_us ?
            (g_get_monotonic_time() - xemu_inv_prof_t0_us) / 1.0e6 : 0.0;
        extern uint64_t xemu_io_ra_misses;  /* translate-all.c */
        unsigned io_pc_bits, io_pcs = xemu_io_pc_count(&io_pc_bits);
        fprintf(stderr,
                "xemu:  (f) io_prepare=%llu  cpu_io_recompile=%llu (%.1f%% of "
                "io_prepare)  window=%.1fs => %.1fk recompiles/s  "
                "io-ra cache misses=%llu  io pcs=%u (filter bits %u)\n",
                (unsigned long long)xemu_io_prepare_calls,
                (unsigned long long)xemu_io_recompiles,
                xemu_io_prepare_calls ?
                    100.0 * xemu_io_recompiles / xemu_io_prepare_calls : 0.0,
                secs, secs > 0.0 ? xemu_io_recompiles / secs / 1000.0 : 0.0,
                (unsigned long long)xemu_io_ra_misses, io_pcs, io_pc_bits);
    }

    fprintf(stderr,
            "xemu:  (e) SUBPAGE refute=%d  fast-skips=%llu"
            "  |  refute: examined=%llu would-skip=%llu VIOLATIONS=%llu\n",
            xemu_subpage_refute_on() ? 1 : 0,
            (unsigned long long)xemu_subpage_skips,
            (unsigned long long)xemu_subpage_refute_total,
            (unsigned long long)xemu_subpage_refute_hits,
            (unsigned long long)xemu_subpage_refute_violations);
}

static bool xemu_inv_dump_armed;

bool xemu_inv_prof_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_INV_PROF");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on && !xemu_inv_dump_armed) {
            xemu_inv_dump_armed = true;
            xemu_inv_prof_t0_us = g_get_monotonic_time();
            atexit(xemu_inv_prof_dump);
        }
    }
    return on;
}

bool xemu_inv_timing_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_INV_TIMING");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on && !xemu_inv_dump_armed) {
            xemu_inv_dump_armed = true;
            xemu_inv_prof_t0_us = g_get_monotonic_time();
            atexit(xemu_inv_prof_dump);
        }
    }
    return on;
}

/*
 * Sub-page dirty tracking (docs/Finished/subpage-gate0-prediction.md
 * arm (a): the helper-top O(1)
 * consult). A per-PageDesc bitmap records which sub-blocks of the page a live
 * TB covers; a guest data store that lands entirely outside every code
 * sub-block skips the whole-page invalidation (and its page-collection lock and
 * TB scan) — it never empties the page, so the page stays write-protected and
 * the store re-traps next time, but each such trap is ~O(1) instead of the
 * ~4.5 us whole-page invalidation. INV_PROF measured 100% false-share / 0 true
 * SMC, so almost every trap is skippable.
 *
 * The bitmap is a NEW per-PageDesc field — it never aliases or touches the
 * ram_list.dirty_memory[DIRTY_MEMORY_*] bitmaps the PFIFO-thread vertex/texture
 * consumers read (design race clause 2). 64 sub-blocks/page (64 B on the 4 KiB
 * Xbox page), one uint64_t.
 *
 * Safety asymmetry: an OVER-set bit (stale code bit after a TB left) only costs
 * a needless trap (benign); an UNDER-set bit (a live TB with no code bit) would
 * let a code store skip invalidation -> wrong-code hang. So bits are SET
 * completely on every tb_page_add (before tb_link_page publishes the TB, clause
 * 1) and cleared only when the page empties of TBs (clause 3), never lazily on
 * single-TB removal. XEMU_SUBPAGE_REFUTE=1 runs the skip decision against a
 * ground-truth TB-overlap scan and counts any under-set (violation) loudly.
 */
#define XEMU_SUBPAGE_BLOCK_BITS  (TARGET_PAGE_BITS - 6)   /* 64 blocks/page */

/* Counters defined in the top counter block: xemu_subpage_skips,
 * xemu_subpage_refute_{total,hits,violations}. */

bool xemu_subpage_refute_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_SUBPAGE_REFUTE");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on && !xemu_inv_dump_armed) {
            xemu_inv_dump_armed = true;
            atexit(xemu_inv_prof_dump);
        }
#if !defined(__aarch64__)
        /*
         * Arm (b) -- "subpage-fast", the inline NOTDIRTY store pre-filter --
         * is emitted only by the aarch64 TCG backend, whose emitter is the only
         * consumer of xemu_subpage_fast_mode(), so on any other host
         * XEMU_SUBPAGE_FAST=1 was silently inert. Say so once from
         * this latch, which every host reaches on its first code-page store.
         * Apple Silicon is aarch64 and never compiles this branch.
         */
        {
            const char *f = getenv("XEMU_SUBPAGE_FAST");
            const char *r = getenv("XEMU_SUBPAGE_FAST_REFUTE");
            if ((f && f[0] == '1') || (r && r[0] == '1')) {
                fprintf(stderr,
                        "xemu: XEMU_SUBPAGE_FAST: no inline store-prefilter "
                        "emitter for this TCG host backend (aarch64 only); "
                        "ignored\n");
            }
        }
#endif
    }
    return on;
}

/* Block mask for the in-page byte range [start, last] (same page). */
static inline uint64_t xemu_subpage_range_mask(tb_page_addr_t start,
                                               tb_page_addr_t last)
{
    unsigned lo = (start & ~TARGET_PAGE_MASK) >> XEMU_SUBPAGE_BLOCK_BITS;
    unsigned hi = (last  & ~TARGET_PAGE_MASK) >> XEMU_SUBPAGE_BLOCK_BITS;
    uint64_t lo_mask = ~0ULL << lo;
    uint64_t hi_mask = (hi >= 63) ? ~0ULL : ((1ULL << (hi + 1)) - 1);
    return lo_mask & hi_mask;
}
#endif /* XBOX */

/* List iterators for lists of tagged pointers in TranslationBlock. */
#define TB_FOR_EACH_TAGGED(head, tb, n, field)                          \
    for (n = (head) & 1, tb = (TranslationBlock *)((head) & ~1);        \
         tb; tb = (TranslationBlock *)tb->field[n], n = (uintptr_t)tb & 1, \
             tb = (TranslationBlock *)((uintptr_t)tb & ~1))

#define TB_FOR_EACH_JMP(head_tb, tb, n)                                 \
    TB_FOR_EACH_TAGGED((head_tb)->jmp_list_head, tb, n, jmp_list_next)

static bool tb_cmp(const void *ap, const void *bp)
{
    const TranslationBlock *a = ap;
    const TranslationBlock *b = bp;

    return ((tb_cflags(a) & CF_PCREL || a->pc == b->pc) &&
            a->cs_base == b->cs_base &&
            a->flags == b->flags &&
            (tb_cflags(a) & ~CF_INVALID) == (tb_cflags(b) & ~CF_INVALID) &&
            tb_page_addr0(a) == tb_page_addr0(b) &&
            tb_page_addr1(a) == tb_page_addr1(b));
}

static bool inv_tb_cmp(const void *ap, const void *bp)
{
    const TranslationBlock *a = ap, *b = bp;
    return tb_cmp(ap, bp) && a->ihash == b->ihash;
}

void tb_htable_init(void)
{
    unsigned int mode = QHT_MODE_AUTO_RESIZE;

    qht_init(&tb_ctx.htable, tb_cmp, CODE_GEN_HTABLE_SIZE, mode);
    qht_init(&tb_ctx.inv_htable, inv_tb_cmp, CODE_GEN_HTABLE_SIZE, mode);
}

typedef struct PageDesc PageDesc;

#ifdef CONFIG_USER_ONLY

/*
 * In user-mode page locks aren't used; mmap_lock is enough.
 */
#define assert_page_locked(pd) tcg_debug_assert(have_mmap_lock())

static inline void tb_lock_pages(const TranslationBlock *tb) { }

/*
 * For user-only, since we are protecting all of memory with a single lock,
 * and because the two pages of a TranslationBlock are always contiguous,
 * use a single data structure to record all TranslationBlocks.
 */
static IntervalTreeRoot tb_root;

static void tb_remove_all(void)
{
    /*
     * Only called from tb_flush__exclusive_or_serial, where we have already
     * asserted that we're in an exclusive state.
     */
    memset(&tb_root, 0, sizeof(tb_root));
}

/* Call with mmap_lock held. */
static void tb_record(TranslationBlock *tb)
{
    vaddr addr;
    int flags;

    assert_memory_lock();
    tb->itree.last = tb->itree.start + tb->size - 1;

    /* translator_loop() must have made all TB pages non-writable */
    addr = tb_page_addr0(tb);
    flags = page_get_flags(addr);
    assert(!(flags & PAGE_WRITE));

    addr = tb_page_addr1(tb);
    if (addr != -1) {
        flags = page_get_flags(addr);
        assert(!(flags & PAGE_WRITE));
    }

    interval_tree_insert(&tb->itree, &tb_root);
}

/* Call with mmap_lock held. */
static void tb_remove(TranslationBlock *tb)
{
    assert_memory_lock();
    interval_tree_remove(&tb->itree, &tb_root);
}

/* TODO: For now, still shared with translate-all.c for system mode. */
#define PAGE_FOR_EACH_TB(start, last, pagedesc, T, N)   \
    for (T = foreach_tb_first(start, last),             \
         N = foreach_tb_next(T, start, last);           \
         T != NULL;                                     \
         T = N, N = foreach_tb_next(N, start, last))

typedef TranslationBlock *PageForEachNext;

static PageForEachNext foreach_tb_first(tb_page_addr_t start,
                                        tb_page_addr_t last)
{
    IntervalTreeNode *n = interval_tree_iter_first(&tb_root, start, last);
    return n ? container_of(n, TranslationBlock, itree) : NULL;
}

static PageForEachNext foreach_tb_next(PageForEachNext tb,
                                       tb_page_addr_t start,
                                       tb_page_addr_t last)
{
    IntervalTreeNode *n;

    if (tb) {
        n = interval_tree_iter_next(&tb->itree, start, last);
        if (n) {
            return container_of(n, TranslationBlock, itree);
        }
    }
    return NULL;
}

#else
/*
 * In system mode we want L1_MAP to be based on ram offsets.
 */
#define L1_MAP_ADDR_SPACE_BITS  HOST_LONG_BITS

/* Size of the L2 (and L3, etc) page tables.  */
#define V_L2_BITS 10
#define V_L2_SIZE (1 << V_L2_BITS)

/*
 * L1 Mapping properties
 */
static int v_l1_size;
static int v_l1_shift;
static int v_l2_levels;

/*
 * The bottom level has pointers to PageDesc, and is indexed by
 * anything from 4 to (V_L2_BITS + 3) bits, depending on target page size.
 */
#define V_L1_MIN_BITS 4
#define V_L1_MAX_BITS (V_L2_BITS + 3)
#define V_L1_MAX_SIZE (1 << V_L1_MAX_BITS)

static void *l1_map[V_L1_MAX_SIZE];

struct PageDesc {
    QemuSpin lock;
    /* list of TBs intersecting this ram page */
    uintptr_t first_tb;
#if defined(XBOX)
    /*
     * Sub-page dirty tracking: bit b set => some live TB on this page covers
     * sub-block b (64 B). Separate from ram_list.dirty_memory[*] (never
     * aliases the DIRTY_MEMORY_* bitmaps the PFIFO consumers read). See the
     * block comment near xemu_subpage_refute_on().
     */
    uint64_t code_blocks;
#endif
};

void page_table_config_init(void)
{
    uint32_t v_l1_bits;

    assert(TARGET_PAGE_BITS);
    /* The bits remaining after N lower levels of page tables.  */
    v_l1_bits = (L1_MAP_ADDR_SPACE_BITS - TARGET_PAGE_BITS) % V_L2_BITS;
    if (v_l1_bits < V_L1_MIN_BITS) {
        v_l1_bits += V_L2_BITS;
    }

    v_l1_size = 1 << v_l1_bits;
    v_l1_shift = L1_MAP_ADDR_SPACE_BITS - TARGET_PAGE_BITS - v_l1_bits;
    v_l2_levels = v_l1_shift / V_L2_BITS - 1;

    assert(v_l1_bits <= V_L1_MAX_BITS);
    assert(v_l1_shift % V_L2_BITS == 0);
    assert(v_l2_levels >= 0);
}

static PageDesc *page_find_alloc(tb_page_addr_t index, bool alloc)
{
    PageDesc *pd;
    void **lp;

    /* Level 1.  Always allocated.  */
    lp = l1_map + ((index >> v_l1_shift) & (v_l1_size - 1));

    /* Level 2..N-1.  */
    for (int i = v_l2_levels; i > 0; i--) {
        void **p = qatomic_rcu_read(lp);

        if (p == NULL) {
            void *existing;

            if (!alloc) {
                return NULL;
            }
            p = g_new0(void *, V_L2_SIZE);
            existing = qatomic_cmpxchg(lp, NULL, p);
            if (unlikely(existing)) {
                g_free(p);
                p = existing;
            }
        }

        lp = p + ((index >> (i * V_L2_BITS)) & (V_L2_SIZE - 1));
    }

    pd = qatomic_rcu_read(lp);
    if (pd == NULL) {
        void *existing;

        if (!alloc) {
            return NULL;
        }

        pd = g_new0(PageDesc, V_L2_SIZE);
        for (int i = 0; i < V_L2_SIZE; i++) {
            qemu_spin_init(&pd[i].lock);
        }

        existing = qatomic_cmpxchg(lp, NULL, pd);
        if (unlikely(existing)) {
            for (int i = 0; i < V_L2_SIZE; i++) {
                qemu_spin_destroy(&pd[i].lock);
            }
            g_free(pd);
            pd = existing;
        }
    }

    return pd + (index & (V_L2_SIZE - 1));
}

static inline PageDesc *page_find(tb_page_addr_t index)
{
    return page_find_alloc(index, false);
}

/**
 * struct page_entry - page descriptor entry
 * @pd:     pointer to the &struct PageDesc of the page this entry represents
 * @index:  page index of the page
 * @locked: whether the page is locked
 *
 * This struct helps us keep track of the locked state of a page, without
 * bloating &struct PageDesc.
 *
 * A page lock protects accesses to all fields of &struct PageDesc.
 *
 * See also: &struct page_collection.
 */
struct page_entry {
    PageDesc *pd;
    tb_page_addr_t index;
    bool locked;
};

/**
 * struct page_collection - tracks a set of pages (i.e. &struct page_entry's)
 * @tree:   Binary search tree (BST) of the pages, with key == page index
 * @max:    Pointer to the page in @tree with the highest page index
 *
 * To avoid deadlock we lock pages in ascending order of page index.
 * When operating on a set of pages, we need to keep track of them so that
 * we can lock them in order and also unlock them later. For this we collect
 * pages (i.e. &struct page_entry's) in a binary search @tree. Given that the
 * @tree implementation we use does not provide an O(1) operation to obtain the
 * highest-ranked element, we use @max to keep track of the inserted page
 * with the highest index. This is valuable because if a page is not in
 * the tree and its index is higher than @max's, then we can lock it
 * without breaking the locking order rule.
 *
 * Note on naming: 'struct page_set' would be shorter, but we already have a few
 * page_set_*() helpers, so page_collection is used instead to avoid confusion.
 *
 * See also: page_collection_lock().
 */
struct page_collection {
    QTree *tree;
    struct page_entry *max;
};

typedef int PageForEachNext;
#define PAGE_FOR_EACH_TB(start, last, pagedesc, tb, n) \
    TB_FOR_EACH_TAGGED((pagedesc)->first_tb, tb, n, page_next)

#ifdef CONFIG_DEBUG_TCG

static __thread GHashTable *ht_pages_locked_debug;

static void ht_pages_locked_debug_init(void)
{
    if (ht_pages_locked_debug) {
        return;
    }
    ht_pages_locked_debug = g_hash_table_new(NULL, NULL);
}

static bool page_is_locked(const PageDesc *pd)
{
    PageDesc *found;

    ht_pages_locked_debug_init();
    found = g_hash_table_lookup(ht_pages_locked_debug, pd);
    return !!found;
}

static void page_lock__debug(PageDesc *pd)
{
    ht_pages_locked_debug_init();
    g_assert(!page_is_locked(pd));
    g_hash_table_insert(ht_pages_locked_debug, pd, pd);
}

static void page_unlock__debug(const PageDesc *pd)
{
    bool removed;

    ht_pages_locked_debug_init();
    g_assert(page_is_locked(pd));
    removed = g_hash_table_remove(ht_pages_locked_debug, pd);
    g_assert(removed);
}

static void do_assert_page_locked(const PageDesc *pd,
                                  const char *file, int line)
{
    if (unlikely(!page_is_locked(pd))) {
        error_report("assert_page_lock: PageDesc %p not locked @ %s:%d",
                     pd, file, line);
        abort();
    }
}
#define assert_page_locked(pd) do_assert_page_locked(pd, __FILE__, __LINE__)

void assert_no_pages_locked(void)
{
    ht_pages_locked_debug_init();
    g_assert(g_hash_table_size(ht_pages_locked_debug) == 0);
}

#else /* !CONFIG_DEBUG_TCG */

static inline void page_lock__debug(const PageDesc *pd) { }
static inline void page_unlock__debug(const PageDesc *pd) { }
static inline void assert_page_locked(const PageDesc *pd) { }

#endif /* CONFIG_DEBUG_TCG */

static void page_lock(PageDesc *pd)
{
    page_lock__debug(pd);
    qemu_spin_lock(&pd->lock);
}

/* Like qemu_spin_trylock, returns false on success */
static bool page_trylock(PageDesc *pd)
{
    bool busy = qemu_spin_trylock(&pd->lock);
    if (!busy) {
        page_lock__debug(pd);
    }
    return busy;
}

static void page_unlock(PageDesc *pd)
{
    qemu_spin_unlock(&pd->lock);
    page_unlock__debug(pd);
}

void tb_lock_page0(tb_page_addr_t paddr)
{
    page_lock(page_find_alloc(paddr >> TARGET_PAGE_BITS, true));
}

void tb_lock_page1(tb_page_addr_t paddr0, tb_page_addr_t paddr1)
{
    tb_page_addr_t pindex0 = paddr0 >> TARGET_PAGE_BITS;
    tb_page_addr_t pindex1 = paddr1 >> TARGET_PAGE_BITS;
    PageDesc *pd0, *pd1;

    if (pindex0 == pindex1) {
        /* Identical pages, and the first page is already locked. */
        return;
    }

    pd1 = page_find_alloc(pindex1, true);
    if (pindex0 < pindex1) {
        /* Correct locking order, we may block. */
        page_lock(pd1);
        return;
    }

    /* Incorrect locking order, we cannot block lest we deadlock. */
    if (!page_trylock(pd1)) {
        return;
    }

    /*
     * Drop the lock on page0 and get both page locks in the right order.
     * Restart translation via longjmp.
     */
    pd0 = page_find_alloc(pindex0, false);
    page_unlock(pd0);
    page_lock(pd1);
    page_lock(pd0);
    siglongjmp(tcg_ctx->jmp_trans, -3);
}

void tb_unlock_page1(tb_page_addr_t paddr0, tb_page_addr_t paddr1)
{
    tb_page_addr_t pindex0 = paddr0 >> TARGET_PAGE_BITS;
    tb_page_addr_t pindex1 = paddr1 >> TARGET_PAGE_BITS;

    if (pindex0 != pindex1) {
        page_unlock(page_find_alloc(pindex1, false));
    }
}

static void tb_lock_pages(TranslationBlock *tb)
{
    tb_page_addr_t paddr0 = tb_page_addr0(tb);
    tb_page_addr_t paddr1 = tb_page_addr1(tb);
    tb_page_addr_t pindex0 = paddr0 >> TARGET_PAGE_BITS;
    tb_page_addr_t pindex1 = paddr1 >> TARGET_PAGE_BITS;

    if (unlikely(paddr0 == -1)) {
        return;
    }
    if (unlikely(paddr1 != -1) && pindex0 != pindex1) {
        if (pindex0 < pindex1) {
            page_lock(page_find_alloc(pindex0, true));
            page_lock(page_find_alloc(pindex1, true));
            return;
        }
        page_lock(page_find_alloc(pindex1, true));
    }
    page_lock(page_find_alloc(pindex0, true));
}

void tb_unlock_pages(TranslationBlock *tb)
{
    tb_page_addr_t paddr0 = tb_page_addr0(tb);
    tb_page_addr_t paddr1 = tb_page_addr1(tb);
    tb_page_addr_t pindex0 = paddr0 >> TARGET_PAGE_BITS;
    tb_page_addr_t pindex1 = paddr1 >> TARGET_PAGE_BITS;

    if (unlikely(paddr0 == -1)) {
        return;
    }
    if (unlikely(paddr1 != -1) && pindex0 != pindex1) {
        page_unlock(page_find_alloc(pindex1, false));
    }
    page_unlock(page_find_alloc(pindex0, false));
}

static inline struct page_entry *
page_entry_new(PageDesc *pd, tb_page_addr_t index)
{
    struct page_entry *pe = g_malloc(sizeof(*pe));

    pe->index = index;
    pe->pd = pd;
    pe->locked = false;
    return pe;
}

static void page_entry_destroy(gpointer p)
{
    struct page_entry *pe = p;

    g_assert(pe->locked);
    page_unlock(pe->pd);
    g_free(pe);
}

/* returns false on success */
static bool page_entry_trylock(struct page_entry *pe)
{
    bool busy = page_trylock(pe->pd);
    if (!busy) {
        g_assert(!pe->locked);
        pe->locked = true;
    }
    return busy;
}

static void do_page_entry_lock(struct page_entry *pe)
{
    page_lock(pe->pd);
    g_assert(!pe->locked);
    pe->locked = true;
}

static gboolean page_entry_lock(gpointer key, gpointer value, gpointer data)
{
    struct page_entry *pe = value;

    do_page_entry_lock(pe);
    return FALSE;
}

static gboolean page_entry_unlock(gpointer key, gpointer value, gpointer data)
{
    struct page_entry *pe = value;

    if (pe->locked) {
        pe->locked = false;
        page_unlock(pe->pd);
    }
    return FALSE;
}

/*
 * Trylock a page, and if successful, add the page to a collection.
 * Returns true ("busy") if the page could not be locked; false otherwise.
 */
static bool page_trylock_add(struct page_collection *set, tb_page_addr_t addr)
{
    tb_page_addr_t index = addr >> TARGET_PAGE_BITS;
    struct page_entry *pe;
    PageDesc *pd;

    pe = q_tree_lookup(set->tree, &index);
    if (pe) {
        return false;
    }

    pd = page_find(index);
    if (pd == NULL) {
        return false;
    }

    pe = page_entry_new(pd, index);
    q_tree_insert(set->tree, &pe->index, pe);

    /*
     * If this is either (1) the first insertion or (2) a page whose index
     * is higher than any other so far, just lock the page and move on.
     */
    if (set->max == NULL || pe->index > set->max->index) {
        set->max = pe;
        do_page_entry_lock(pe);
        return false;
    }
    /*
     * Try to acquire out-of-order lock; if busy, return busy so that we acquire
     * locks in order.
     */
    return page_entry_trylock(pe);
}

static gint tb_page_addr_cmp(gconstpointer ap, gconstpointer bp, gpointer udata)
{
    tb_page_addr_t a = *(const tb_page_addr_t *)ap;
    tb_page_addr_t b = *(const tb_page_addr_t *)bp;

    if (a == b) {
        return 0;
    } else if (a < b) {
        return -1;
    }
    return 1;
}

/*
 * Lock a range of pages ([@start,@last]) as well as the pages of all
 * intersecting TBs.
 * Locking order: acquire locks in ascending order of page index.
 */
static struct page_collection *page_collection_lock(tb_page_addr_t start,
                                                    tb_page_addr_t last)
{
    struct page_collection *set = g_malloc(sizeof(*set));
    tb_page_addr_t index;
    PageDesc *pd;

    start >>= TARGET_PAGE_BITS;
    last >>= TARGET_PAGE_BITS;
    g_assert(start <= last);

    set->tree = q_tree_new_full(tb_page_addr_cmp, NULL, NULL,
                                page_entry_destroy);
    set->max = NULL;
    assert_no_pages_locked();

 retry:
    q_tree_foreach(set->tree, page_entry_lock, NULL);

    for (index = start; index <= last; index++) {
        TranslationBlock *tb;
        PageForEachNext n;

        pd = page_find(index);
        if (pd == NULL) {
            continue;
        }
        if (page_trylock_add(set, index << TARGET_PAGE_BITS)) {
            q_tree_foreach(set->tree, page_entry_unlock, NULL);
            goto retry;
        }
        assert_page_locked(pd);
        PAGE_FOR_EACH_TB(unused, unused, pd, tb, n) {
            if (page_trylock_add(set, tb_page_addr0(tb)) ||
                (tb_page_addr1(tb) != -1 &&
                 page_trylock_add(set, tb_page_addr1(tb)))) {
                /* drop all locks, and reacquire in order */
                q_tree_foreach(set->tree, page_entry_unlock, NULL);
                goto retry;
            }
        }
    }
    return set;
}

static void page_collection_unlock(struct page_collection *set)
{
    /* entries are unlocked and freed via page_entry_destroy */
    q_tree_destroy(set->tree);
    g_free(set);
}

/* Set to NULL all the 'first_tb' fields in all PageDescs. */
static void tb_remove_all_1(int level, void **lp)
{
    int i;

    if (*lp == NULL) {
        return;
    }
    if (level == 0) {
        PageDesc *pd = *lp;

        for (i = 0; i < V_L2_SIZE; ++i) {
            page_lock(&pd[i]);
            pd[i].first_tb = (uintptr_t)NULL;
#if defined(XBOX)
            pd[i].code_blocks = 0;
#endif
            page_unlock(&pd[i]);
        }
    } else {
        void **pp = *lp;

        for (i = 0; i < V_L2_SIZE; ++i) {
            tb_remove_all_1(level - 1, pp + i);
        }
    }
}

static void tb_remove_all(void)
{
    int i, l1_sz = v_l1_size;

    for (i = 0; i < l1_sz; i++) {
        tb_remove_all_1(v_l2_levels, l1_map + i);
    }
#if defined(XBOX)
    xemu_sf_clear_all();
#endif
}

#if defined(XBOX)
/* In-page byte range [*s, *l] that TB @tb occupies on its page index @n. */
static inline void xemu_subpage_tb_range(const TranslationBlock *tb,
                                         unsigned int n,
                                         tb_page_addr_t *s, tb_page_addr_t *l)
{
    tb_page_addr_t start = tb_page_addr0(tb);
    tb_page_addr_t last = start + tb->size - 1;
    if (n == 0) {
        last = MIN(last, start | ~TARGET_PAGE_MASK);
    } else {
        start = tb_page_addr1(tb);
        last = start + (last & ~TARGET_PAGE_MASK);
    }
    *s = start;
    *l = last;
}

/*
 * SET the code sub-block bits @tb covers on page @n. Called from tb_page_add
 * with the page lock held, before tb_link_page publishes the TB (design clause
 * 1: the bit is visible before the TB is executable). qatomic_or gives the
 * release the consult's qatomic_read pairs with.
 */
static void xemu_subpage_set_code(PageDesc *p, const TranslationBlock *tb,
                                  unsigned int n)
{
    tb_page_addr_t s, l;
    xemu_subpage_tb_range(tb, n, &s, &l);
    qatomic_or(&p->code_blocks, xemu_subpage_range_mask(s, l));
    /* Mirror for the subpage-fast generated-code probe (same thread;
     * set-before-executable ordering is program order here). */
    xemu_sf_sync_page(tb->page_addr[n] & TARGET_PAGE_MASK,
                      qatomic_read(&p->code_blocks));
}

/* True if [start, start+len) overlaps any code sub-block on @p. */
static inline bool xemu_subpage_code_overlaps(PageDesc *p, ram_addr_t start,
                                              unsigned len)
{
    uint64_t bits = qatomic_read(&p->code_blocks);
    if (!bits) {
        return false;
    }
    return (bits & xemu_subpage_range_mask(start, start + len - 1)) != 0;
}

/*
 * REFUTE: the store [start, last] is one the sub-block filter would fast-path
 * (@filter_noncode). Prove it truly overlaps no live TB's bytes on this page
 * (nor a spanning TB's second page). A single overlap = the bitmap under-set a
 * code block = the design's wrong-code-hang risk. Called with the page locked.
 */
static void xemu_subpage_refute_check(PageDesc *p, ram_addr_t start,
                                      ram_addr_t last, bool filter_noncode)
{
    TranslationBlock *tb;
    PageForEachNext n;

    xemu_subpage_refute_total++;
    if (!filter_noncode) {
        return;   /* filter keeps the invalidation -> no skip -> no risk */
    }
    xemu_subpage_refute_hits++;

    PAGE_FOR_EACH_TB(start, last, p, tb, n) {
        tb_page_addr_t ts, tl;
        xemu_subpage_tb_range(tb, n, &ts, &tl);
        if (!(tl < start || ts > last)) {
            xemu_subpage_refute_violations++;
            fprintf(stderr,
                    "xemu: *** SUBPAGE REFUTE VIOLATION *** store "
                    "[0x%" PRIxPTR "..0x%" PRIxPTR "] the filter would "
                    "fast-path OVERLAPS live TB bytes [0x%" PRIxPTR
                    "..0x%" PRIxPTR "] (pc=0x%" PRIxPTR ", n=%d) — sub-page "
                    "filter is UNSAFE (wrong-code-hang), design FALSIFIED\n",
                    (uintptr_t)start, (uintptr_t)last,
                    (uintptr_t)ts, (uintptr_t)tl, (uintptr_t)tb->pc, n);
            return;
        }
    }
}

/*
 * Subpage-fast refuter ground truth: does [paddr, paddr+len) overlap
 * translated code, per BOTH the bitmap and a live TB byte scan? Runs on
 * the vCPU thread from the refute store helper (page lock taken here).
 * Stores are <= 8 bytes and the stub's discriminator rejects
 * page-crossing accesses, so the range never spans pages.
 */
bool xemu_sf_ground_truth(hwaddr paddr, unsigned len,
                          bool *bitmap_says, bool *live_says)
{
    PageDesc *p = page_find(paddr >> TARGET_PAGE_BITS);
    tb_page_addr_t start = paddr & ~TARGET_PAGE_MASK;
    tb_page_addr_t last = start + len - 1;
    TranslationBlock *tb;
    PageForEachNext n;
    bool overlap = false;

    if (!p) {
        *bitmap_says = *live_says = true;
        return true;
    }
    *bitmap_says = !xemu_subpage_code_overlaps(p, start, last - start + 1);

    page_lock(p);
    PAGE_FOR_EACH_TB(start, last, p, tb, n) {
        tb_page_addr_t ts, tl;
        xemu_subpage_tb_range(tb, n, &ts, &tl);
        if (!(tl < start || ts > last)) {
            overlap = true;
            break;
        }
    }
    page_unlock(p);
    *live_says = !overlap;
    return *bitmap_says && *live_says;
}
#endif /* XBOX */

/*
 * Add the tb in the target page and protect it if necessary.
 * Called with @p->lock held.
 */
static void tb_page_add(PageDesc *p, TranslationBlock *tb, unsigned int n)
{
    bool page_already_protected;

    assert_page_locked(p);

    tb->page_next[n] = p->first_tb;
    page_already_protected = p->first_tb != 0;
    p->first_tb = (uintptr_t)tb | n;

#if defined(XBOX)
    xemu_subpage_set_code(p, tb, n);
#endif

    /*
     * If some code is already present, then the pages are already
     * protected. So we handle the case where only the first TB is
     * allocated in a physical page.
     */
    if (!page_already_protected) {
        tlb_protect_code(tb->page_addr[n] & TARGET_PAGE_MASK);
    }
}

static void tb_record(TranslationBlock *tb)
{
    tb_page_addr_t paddr0 = tb_page_addr0(tb);
    tb_page_addr_t paddr1 = tb_page_addr1(tb);
    tb_page_addr_t pindex0 = paddr0 >> TARGET_PAGE_BITS;
    tb_page_addr_t pindex1 = paddr1 >> TARGET_PAGE_BITS;

    assert(paddr0 != -1);
    if (unlikely(paddr1 != -1) && pindex0 != pindex1) {
        tb_page_add(page_find_alloc(pindex1, false), tb, 1);
    }
    tb_page_add(page_find_alloc(pindex0, false), tb, 0);
}

static void tb_page_remove(PageDesc *pd, TranslationBlock *tb)
{
    TranslationBlock *tb1;
    uintptr_t *pprev;
    PageForEachNext n1;

    assert_page_locked(pd);
    pprev = &pd->first_tb;
    PAGE_FOR_EACH_TB(unused, unused, pd, tb1, n1) {
        if (tb1 == tb) {
            *pprev = tb1->page_next[n1];
            return;
        }
        pprev = &tb1->page_next[n1];
    }
    g_assert_not_reached();
}

static void tb_remove(TranslationBlock *tb)
{
    tb_page_addr_t paddr0 = tb_page_addr0(tb);
    tb_page_addr_t paddr1 = tb_page_addr1(tb);
    tb_page_addr_t pindex0 = paddr0 >> TARGET_PAGE_BITS;
    tb_page_addr_t pindex1 = paddr1 >> TARGET_PAGE_BITS;

    assert(paddr0 != -1);
    if (unlikely(paddr1 != -1) && pindex0 != pindex1) {
        tb_page_remove(page_find_alloc(pindex1, false), tb);
    }
    tb_page_remove(page_find_alloc(pindex0, false), tb);
}
#endif /* CONFIG_USER_ONLY */

/*
 * Flush all the translation blocks.
 * Must be called from a context in which no cpus are running,
 * e.g. start_exclusive() or vm_stop().
 */
#if defined(XBOX)
/*
 * Every live XEMU_TB_PEEKS TB, which every load drops (what it read past
 * its end isn't in its ihash), so that the scoped check after an
 * incremental restore (xemu_tb_revalidate_pages) finds them off its pages.
 * Entries of TBs invalidated since go stale (CF_INVALID; such TBs are
 * never recycled); a load or a flush empties the list.
 */
static QemuSpin xemu_peeks_lock;
static GPtrArray *xemu_peeks;

static void xemu_peeks_note(TranslationBlock *tb)
{
    qemu_spin_lock(&xemu_peeks_lock);
    if (!xemu_peeks) {
        xemu_peeks = g_ptr_array_new();
    }
    if (xemu_peeks->len >= 1024) {
        for (guint i = 0; i < xemu_peeks->len;) {
            TranslationBlock *old = g_ptr_array_index(xemu_peeks, i);
            if (tb_cflags(old) & CF_INVALID) {
                g_ptr_array_remove_index_fast(xemu_peeks, i);
            } else {
                i++;
            }
        }
    }
    g_ptr_array_add(xemu_peeks, tb);
    qemu_spin_unlock(&xemu_peeks_lock);
}

/* Move the live ones to @stale (with @stale NULL, forget them). */
static void xemu_peeks_take(GPtrArray *stale)
{
    qemu_spin_lock(&xemu_peeks_lock);
    for (guint i = 0; stale && xemu_peeks && i < xemu_peeks->len; i++) {
        TranslationBlock *tb = g_ptr_array_index(xemu_peeks, i);
        if (!(tb_cflags(tb) & CF_INVALID)) {
            g_ptr_array_add(stale, tb);
        }
    }
    if (xemu_peeks) {
        g_ptr_array_set_size(xemu_peeks, 0);
    }
    qemu_spin_unlock(&xemu_peeks_lock);
}
#endif

void tb_flush__exclusive_or_serial(void)
{
    CPUState *cpu;

    trace_tb_flush();
    assert(tcg_enabled());
    /* Note that cpu_in_serial_context checks cpu_in_exclusive_context. */
    assert(!runstate_is_running() ||
           (current_cpu && cpu_in_serial_context(current_cpu)));

    CPU_FOREACH(cpu) {
        tcg_flush_jmp_cache(cpu);
    }

    qht_reset_size(&tb_ctx.htable, CODE_GEN_HTABLE_SIZE);
    qht_reset_size(&tb_ctx.inv_htable, CODE_GEN_HTABLE_SIZE);
    tb_remove_all();
#if defined(XBOX)
    xemu_peeks_take(NULL);
#endif

    /* The buffer is reused: run this thread's queued link flushes first. */
    flush_idcache_link_drain();
    tcg_region_reset_all();
    /* XXX: flush processor icache at this point if cache flush is expensive */
    qatomic_inc(&tb_ctx.tb_flush_count);
    qemu_plugin_flush_cb();
}

static void do_tb_flush(CPUState *cpu, run_on_cpu_data tb_flush_count)
{
    /* If it is already been done on request of another CPU, just retry. */
    if (tb_ctx.tb_flush_count == tb_flush_count.host_int) {
        tb_flush__exclusive_or_serial();
    }
}

void queue_tb_flush(CPUState *cs)
{
    if (tcg_enabled()) {
        unsigned tb_flush_count = qatomic_read(&tb_ctx.tb_flush_count);
        async_safe_run_on_cpu(cs, do_tb_flush,
                              RUN_ON_CPU_HOST_INT(tb_flush_count));
    }
}

/* remove @orig from its @n_orig-th jump list */
static inline void tb_remove_from_jmp_list(TranslationBlock *orig, int n_orig)
{
    uintptr_t ptr, ptr_locked;
    TranslationBlock *dest;
    TranslationBlock *tb;
    uintptr_t *pprev;
    int n;

    /* mark the LSB of jmp_dest[] so that no further jumps can be inserted */
    ptr = qatomic_or_fetch(&orig->jmp_dest[n_orig], 1);
    dest = (TranslationBlock *)(ptr & ~1);
    if (dest == NULL) {
        return;
    }

    qemu_spin_lock(&dest->jmp_lock);
    /*
     * While acquiring the lock, the jump might have been removed if the
     * destination TB was invalidated; check again.
     */
    ptr_locked = qatomic_read(&orig->jmp_dest[n_orig]);
    if (ptr_locked != ptr) {
        qemu_spin_unlock(&dest->jmp_lock);
        /*
         * The only possibility is that the jump was unlinked via
         * tb_jump_unlink(dest). Seeing here another destination would be a bug,
         * because we set the LSB above.
         */
        g_assert(ptr_locked == 1 && dest->cflags & CF_INVALID);
        return;
    }
    /*
     * We first acquired the lock, and since the destination pointer matches,
     * we know for sure that @orig is in the jmp list.
     */
    if (dest == orig) {
        /*
         * In the case of a TB that links to itself, removing the entry
         * from the list means that it won't be present later during
         * tb_jmp_unlink -- unlink now.
         */
        tb_reset_jump(orig, n_orig);
    }
    pprev = &dest->jmp_list_head;
    TB_FOR_EACH_JMP(dest, tb, n) {
        if (tb == orig && n == n_orig) {
            *pprev = tb->jmp_list_next[n];
            /* no need to set orig->jmp_dest[n]; setting the LSB was enough */
            qemu_spin_unlock(&dest->jmp_lock);
            return;
        }
        pprev = &tb->jmp_list_next[n];
    }
    g_assert_not_reached();
}

/*
 * Reset the jump entry 'n' of a TB so that it is not chained to another TB.
 */
void tb_reset_jump(TranslationBlock *tb, int n)
{
    uintptr_t addr = (uintptr_t)(tb->tc.ptr + tb->jmp_reset_offset[n]);
    tb_set_jmp_target(tb, n, addr);
}

/* remove any jumps to the TB */
static inline void tb_jmp_unlink(TranslationBlock *dest)
{
    TranslationBlock *tb;
    int n;

    qemu_spin_lock(&dest->jmp_lock);

    TB_FOR_EACH_JMP(dest, tb, n) {
        tb_reset_jump(tb, n);
        qatomic_and(&tb->jmp_dest[n], (uintptr_t)NULL | 1);
        /* No need to clear the list entry; setting the dest ptr is enough */
    }
    dest->jmp_list_head = (uintptr_t)NULL;

    qemu_spin_unlock(&dest->jmp_lock);
}

#if defined(XBOX)
/*
 * Cross-page direct-chain link registry (docs/Finished/xpage-design.md §3).
 *
 * Every cross-page direct jump created by tb_add_jump registers its
 * DESTINATION TB here (deduped by tb->xemu_xpage_reg). When a full-TLB-flush
 * class fires (generic tlb_flush_by_mmuidx, CR3 reload), the backstop severs
 * exactly these incoming chains via tb_jmp_unlink() instead of dropping the
 * whole translation cache: a queued tb_flush costs a retranslation storm on
 * every guest mapping burst — measured 139 tb_flushes in 20 s of first-visit
 * area streaming (2026-07-12, the "new-area lag" report) — while severing
 * keeps every translation and chains relink on the next execution.
 *
 * Same-page links into a registered dest are severed too (they relink
 * lazily; harmless). TB pointers stay valid between tb_flushes (region
 * memory is only recycled by a flush), and the generation check drops the
 * registry wholesale once a real flush retires that generation's TBs. On
 * overflow the backstop falls back to the old queued full flush —
 * correctness never depends on registry capacity. Lock order: xpage_reg_lock
 * -> dest->jmp_lock; tb_add_jump registers only after releasing jmp_lock.
 */
#define XEMU_XPAGE_REG_CAP  (128 * 1024)
static TranslationBlock *xpage_reg[XEMU_XPAGE_REG_CAP];
static unsigned xpage_reg_count;
static unsigned xpage_reg_gen;
static bool xpage_reg_overflowed;
static QemuSpin xpage_reg_lock = { .value = 0 };
/* The unlink walk's queued jump-patch icache flushes; under xpage_reg_lock. */
static FlushIdcacheBatch xpage_unlink_icache;

uint64_t xemu_xpage_unlink_events;
uint64_t xemu_xpage_unlink_dests;
uint64_t xemu_xpage_reg_overflows;
uint64_t xemu_xpage_reg_phys_only;
uint64_t xemu_xpage_unlink_total_us;
uint64_t xemu_xpage_unlink_max_us;
uint64_t xemu_xpage_walk_same;
uint64_t xemu_xpage_walk_cross;
uint64_t xemu_xpage_walk_emitter_same;
uint64_t xemu_xpage_links;
uint64_t xemu_xpage_link_stale;
uint64_t xemu_xpage_link_stale_flushed;
uint64_t xemu_xpage_link_exit_flushed;

static void xpage_reg_reset_locked(unsigned gen)
{
    xpage_reg_gen = gen;
    xpage_reg_count = 0;
    xpage_reg_overflowed = false;
}

void xemu_xpage_link_note_cross(TranslationBlock *dest, bool phys_only)
{
    unsigned gen = qatomic_read(&tb_ctx.tb_flush_count);

    qemu_spin_lock(&xpage_reg_lock);
    if (xpage_reg_gen != gen) {
        xpage_reg_reset_locked(gen);
    }
    if (!(dest->xemu_xpage_reg & XEMU_XPAGE_TB_REGISTERED)) {
        if (xpage_reg_count < XEMU_XPAGE_REG_CAP) {
            dest->xemu_xpage_reg |= XEMU_XPAGE_TB_REGISTERED;
            xpage_reg[xpage_reg_count++] = dest;
            if (phys_only) {
                /* Canary: a non-emitter link whose dest is off the
                 * source's first physical page. Non-emitter slots target
                 * the source's first virtual page, so only a guest remap
                 * between the two lookups lands here; expected ~0. */
                xemu_xpage_reg_phys_only++;
            }
        } else if (!xpage_reg_overflowed) {
            xpage_reg_overflowed = true;
            xemu_xpage_reg_overflows++;
        }
    }
    qemu_spin_unlock(&xpage_reg_lock);
}

uint64_t xemu_xpage_refute_frame(tb_page_addr_t addr)
{
    unsigned gen = qatomic_read(&tb_ctx.tb_flush_count);
    uint64_t n = 0;

    qemu_spin_lock(&xpage_reg_lock);
    for (unsigned i = 0; xpage_reg_gen == gen && i < xpage_reg_count; i++) {
        TranslationBlock *dest = xpage_reg[i];

        if (!(tb_cflags(dest) & CF_INVALID) &&
            !((tb_page_addr0(dest) ^ addr) & TARGET_PAGE_MASK) &&
            qatomic_read(&dest->jmp_list_head)) {
            fprintf(stderr, "xemu: tlb-reval refute: a cross-page chain "
                    "still enters TB %p on ram page 0x%" PRIx64 ", whose "
                    "mapping changed\n", dest, (uint64_t)addr);
            abort();
        }
        n++;
    }
    qemu_spin_unlock(&xpage_reg_lock);
    return n;
}

bool xemu_xpage_unlink_registered(CPUState *cs)
{
    unsigned gen = qatomic_read(&tb_ctx.tb_flush_count);
    unsigned n = 0;
    bool complete;

    qemu_spin_lock(&xpage_reg_lock);
    if (xpage_reg_gen != gen) {
        xpage_reg_reset_locked(gen);
    }
    complete = !xpage_reg_overflowed;
    /*
     * tb_jmp_unlink patches host code (tb_reset_jump); on Apple Silicon
     * hardened runtime this thread is in execute mode when the backstop
     * fires from a TLB-flush helper, so bracket with the same manual
     * write/execute pair tb_phys_invalidate uses (see the
     * pthread_jit_write_with_callback_np war story at that site). The
     * missing bracket wedged the vCPU thread in a fault loop at the
     * first boot backstop when this landed (no crash report — the
     * monitor answers once, then nothing).
     *
     * A stream-e2 walk severs ~300-400 dests and resets ~650-850 jumps
     * on average, up to ~3k. Each reset's 4-byte icache flush cost
     * ~350 ns as its own DSB/IC/DSB/ISB, ~80% of the walk, so they are
     * queued and run as one batch before this returns. The walk runs on
     * the vCPU's own thread (a CR3 write, or tlb_flush_by_mmuidx, which
     * asserts it) or with the VM stopped, so no TB executes before the
     * batch is done.
     */
    int64_t t0 = g_get_monotonic_time();
    bool prof = xemu_xpage_prof_on();
    qemu_thread_jit_write();
    flush_idcache_defer_begin(&xpage_unlink_icache);
    while (xpage_reg_count) {
        TranslationBlock *dest = xpage_reg[--xpage_reg_count];
        /* Only the REGISTERED bit — CROSS_EMITTER is translate-time state
         * that must survive for the TB's remaining lifetime. */
        dest->xemu_xpage_reg &= ~XEMU_XPAGE_TB_REGISTERED;
        if (unlikely(prof)) {
            /*
             * Census of the jumps this walk resets, by tb_add_jump's
             * registration predicate: same-page ones need no sever. An
             * emitter's jump onto its own page may come from its
             * same-page slot (the flag is per TB, not per slot).
             */
            TranslationBlock *src;
            int k;

            qemu_spin_lock(&dest->jmp_lock);
            TB_FOR_EACH_JMP(dest, src, k) {
                if (!xemu_xpage_jump_cross(src, dest)) {
                    xemu_xpage_walk_same++;
                } else if ((tb_page_addr0(src) ^ tb_page_addr0(dest)) &
                           TARGET_PAGE_MASK) {
                    xemu_xpage_walk_cross++;
                } else {
                    xemu_xpage_walk_emitter_same++;
                }
            }
            qemu_spin_unlock(&dest->jmp_lock);
        }
        tb_jmp_unlink(dest);
        n++;
    }
    flush_idcache_defer_end(&xpage_unlink_icache);
    qemu_thread_jit_execute();
    xpage_reg_overflowed = false;
    qemu_spin_unlock(&xpage_reg_lock);

    if (n) {
        /* Walk-duration stats: a walk stalls the vCPU mid-frame, so its
         * tail cost must stay well under a frame period (jitter suspect
         * instrumentation, 2026-07-12). */
        uint64_t us = (uint64_t)(g_get_monotonic_time() - t0);
        xemu_xpage_unlink_events++;
        xemu_xpage_unlink_dests += n;
        xemu_xpage_unlink_total_us += us;
        if (us > xemu_xpage_unlink_max_us) {
            xemu_xpage_unlink_max_us = us;
        }
    }
    return complete;
}
#endif /* XBOX */

#if defined(XBOX)
bool xemu_tb_page_has_tbs(tb_page_addr_t addr)
{
    PageDesc *pd = page_find(addr >> TARGET_PAGE_BITS);
    bool has = false;

    /*
     * Under the page lock, so that a TB another thread is invalidating
     * counts until its incoming jumps are unlinked.
     */
    if (pd) {
        page_lock(pd);
        has = pd->first_tb != 0;
        page_unlock(pd);
    }
    return has;
}

/*
 * Set while xemu_tb_revalidate_after_load invalidates with the vCPUs
 * stopped. It clears every jump cache once afterwards, instead of once per
 * CF_PCREL TB (most of the cost when a load drops tens of thousands).
 */
static bool tb_jmp_cache_inval_deferred;
#endif

static void tb_jmp_cache_inval_tb(TranslationBlock *tb)
{
    CPUState *cpu;

#if defined(XBOX)
    if (qatomic_read(&tb_jmp_cache_inval_deferred)) {
        return;
    }
#endif
    if (tb_cflags(tb) & CF_PCREL) {
        /* A TB may be at any virtual address */
        CPU_FOREACH(cpu) {
#if defined(XBOX)
            xemu_flush_jmp_cache(cpu, false);
#else
            tcg_flush_jmp_cache(cpu);
#endif
        }
    } else {
#if defined(XBOX)
        uint32_t h = tb_jmp_cache_way0(tb->pc);
#else
        uint32_t h = tb_jmp_cache_hash_func(tb->pc);
#endif

        CPU_FOREACH(cpu) {
            CPUJumpCache *jc = cpu->tb_jmp_cache;

            if (qatomic_read(&jc->array[h].tb) == tb) {
                qatomic_set(&jc->array[h].tb, NULL);
            }
#if defined(XBOX)
            if (qatomic_read(&jc->array[h | 1].tb) == tb) {
                qatomic_set(&jc->array[h | 1].tb, NULL);
            }
#endif
        }
    }
}

/*
 * In user-mode, call with mmap_lock held.
 * In !user-mode, if @rm_from_page_list is set, call with the TB's pages'
 * locks held.
 */
static void do_tb_phys_invalidate(TranslationBlock *tb, bool rm_from_page_list)
{
    uint32_t h;
    tb_page_addr_t phys_pc;
    uint32_t orig_cflags = tb_cflags(tb);
    void *existing = NULL;

    assert_memory_lock();

    /* make sure no further incoming jumps will be chained to this TB */
    qemu_spin_lock(&tb->jmp_lock);
    qatomic_set(&tb->cflags, tb->cflags | CF_INVALID);
    qemu_spin_unlock(&tb->jmp_lock);

    /* remove the TB from the hash list */
    phys_pc = tb_page_addr0(tb);
    h = tb_hash_func(phys_pc, (orig_cflags & CF_PCREL ? 0 : tb->pc),
                     tb->flags, tb->cs_base, orig_cflags);
    if (!qht_remove(&tb_ctx.htable, tb, h)) {
        return;
    }

#if defined(XBOX)
    /* The recycler's byte hash doesn't cover what it read past its end. */
    if (!(tb->xemu_xpage_reg & XEMU_TB_PEEKS))
#endif
    {
        qht_insert(&tb_ctx.inv_htable, tb, h, &existing);
        g_assert(existing == NULL);
    }

    /* remove the TB from the page list */
    if (rm_from_page_list) {
        tb_remove(tb);
    }

    /* remove the TB from the hash list */
    tb_jmp_cache_inval_tb(tb);

    /* suppress this TB from the two jump lists */
    tb_remove_from_jmp_list(tb, 0);
    tb_remove_from_jmp_list(tb, 1);

    /* suppress any remaining jumps to this TB */
    tb_jmp_unlink(tb);

    qatomic_set(&tb_ctx.tb_phys_invalidate_count,
                tb_ctx.tb_phys_invalidate_count + 1);

#if defined(XBOX)
    if (unlikely(xemu_inv_prof_on())) {
        xemu_inv_total++;
        xemu_inv_by_src[xemu_inv_cur_src]++;
    }
#endif
}

static void tb_phys_invalidate__locked(TranslationBlock *tb)
{
    /*
     * Manual write-then-execute pair. An earlier optimization used
     * pthread_jit_write_with_callback_np (macOS 14.4+) to amortize the
     * permission flip; it was reverted because it was observed to
     * correlate with rare freezes during heavy TB invalidation (level
     * transitions, death-reload). The new API's scoping semantics
     * differ subtly from the manual pair (it forcibly drops write
     * permission on callback return, regardless of whether a nested
     * JIT-writer on the same thread still needs it) and nested-use
     * paths are hard to rule out across QEMU's TB invalidation flow.
     * The manual pair is exactly the pre-xemu-macos upstream behavior
     * and has been battle-tested.
     */
    qemu_thread_jit_write();
    do_tb_phys_invalidate(tb, true);
    qemu_thread_jit_execute();
}

#if defined(XBOX)
/*
 * For cpu_io_recompile (icount): drop a TB whose MMIO access is not its
 * last instruction, so translator_loop retranslates it to end at the access
 * (xemu_io_pc_is_set). Keep it out of the recycler, which would bring the
 * old shape back. Called from a helper, with the thread in execute mode;
 * the TB lives in the code buffer, so bracket the writes.
 */
void tb_phys_invalidate_io(TranslationBlock *tb)
{
    uint32_t cflags = tb_cflags(tb);
    tb_page_addr_t phys_pc = tb_page_addr0(tb);
    uint32_t h = tb_hash_func(phys_pc, (cflags & CF_PCREL ? 0 : tb->pc),
                              tb->flags, tb->cs_base, cflags);

    qemu_thread_jit_write();
    if (phys_pc != -1) {
        tb_lock_pages(tb);
        do_tb_phys_invalidate(tb, true);
        tb_unlock_pages(tb);
    } else {
        do_tb_phys_invalidate(tb, false);
    }
    qemu_thread_jit_execute();
    qht_remove(&tb_ctx.inv_htable, tb, h);
}
#endif

/*
 * Invalidate one TB.
 * Called with mmap_lock held in user-mode.
 */
void tb_phys_invalidate(TranslationBlock *tb, tb_page_addr_t page_addr)
{
#if defined(XBOX)
    if (unlikely(xemu_inv_prof_on())) {
        xemu_inv_cur_src = XEMU_INV_SRC_SINGLE;
    }
#endif
    /*
     * The only caller is tb_check_watchpoint, from a load/store helper:
     * the thread is in execute mode and the TB lives in the code buffer.
     * Without the bracket the first write (tb->jmp_lock) faulted forever,
     * which was the Z2/Z3/Z4 "livelock" on Apple Silicon.
     */
    qemu_thread_jit_write();
    if (page_addr == -1 && tb_page_addr0(tb) != -1) {
        tb_lock_pages(tb);
        do_tb_phys_invalidate(tb, true);
        tb_unlock_pages(tb);
    } else {
        do_tb_phys_invalidate(tb, false);
    }
    qemu_thread_jit_execute();
}

/*
 * Add a new TB and link it to the physical page tables.
 * Called with mmap_lock held for user-mode emulation.
 *
 * Returns a pointer @tb, or a pointer to an existing TB that matches @tb.
 * Note that in !user-mode, another thread might have already added a TB
 * for the same block of guest code that @tb corresponds to. In that case,
 * the caller should discard the original @tb, and use instead the returned TB.
 */
TranslationBlock *tb_link_page(TranslationBlock *tb)
{
    void *existing_tb = NULL;
    uint32_t h;

    assert_memory_lock();
    tcg_debug_assert(!(tb->cflags & CF_INVALID));

    tb_record(tb);

    /* add in the hash table */
    h = tb_hash_func(tb_page_addr0(tb), (tb->cflags & CF_PCREL ? 0 : tb->pc),
                     tb->flags, tb->cs_base, tb->cflags);
    qht_insert(&tb_ctx.htable, tb, h, &existing_tb);

    /* remove TB from the page(s) if we couldn't insert it */
    if (unlikely(existing_tb)) {
        tb_remove(tb);
        tb_unlock_pages(tb);
        return existing_tb;
    }
#if defined(XBOX)
    if (unlikely(tb->xemu_xpage_reg & XEMU_TB_PEEKS)) {
        xemu_peeks_note(tb);
    }
#endif

    tb_unlock_pages(tb);
    return tb;
}

#ifdef CONFIG_USER_ONLY
/*
 * Invalidate all TBs which intersect with the target address range.
 * Called with mmap_lock held for user-mode emulation.
 * NOTE: this function must not be called while a TB is running.
 */
void tb_invalidate_phys_range(CPUState *cpu, tb_page_addr_t start,
                              tb_page_addr_t last)
{
    TranslationBlock *tb;
    PageForEachNext n;

    assert_memory_lock();

    PAGE_FOR_EACH_TB(start, last, unused, tb, n) {
        tb_phys_invalidate__locked(tb);
    }
}

/*
 * Invalidate all TBs which intersect with the target address page @addr.
 * Called with mmap_lock held for user-mode emulation
 * NOTE: this function must not be called while a TB is running.
 */
static void tb_invalidate_phys_page(tb_page_addr_t addr)
{
    tb_page_addr_t start, last;

    start = addr & TARGET_PAGE_MASK;
    last = addr | ~TARGET_PAGE_MASK;
    tb_invalidate_phys_range(NULL, start, last);
}

/*
 * Called with mmap_lock held. If pc is not 0 then it indicates the
 * host PC of the faulting store instruction that caused this invalidate.
 * Returns true if the caller needs to abort execution of the current TB.
 */
bool tb_invalidate_phys_page_unwind(CPUState *cpu, tb_page_addr_t addr,
                                    uintptr_t pc)
{
    TranslationBlock *current_tb;
    bool current_tb_modified;
    TranslationBlock *tb;
    PageForEachNext n;
    tb_page_addr_t last;

    /*
     * Without precise smc semantics, or when outside of a TB,
     * we can skip to invalidate.
     */
    if (!pc || !cpu || !cpu->cc->tcg_ops->precise_smc) {
        tb_invalidate_phys_page(addr);
        return false;
    }

    assert_memory_lock();
    current_tb = tcg_tb_lookup(pc);

    last = addr | ~TARGET_PAGE_MASK;
    addr &= TARGET_PAGE_MASK;
    current_tb_modified = false;

    PAGE_FOR_EACH_TB(addr, last, unused, tb, n) {
        if (current_tb == tb &&
            (tb_cflags(current_tb) & CF_COUNT_MASK) != 1) {
            /*
             * If we are modifying the current TB, we must stop its
             * execution. We could be more precise by checking that
             * the modification is after the current PC, but it would
             * require a specialized function to partially restore
             * the CPU state.
             */
            current_tb_modified = true;
            cpu_restore_state_from_tb(cpu, current_tb, pc);
        }
        tb_phys_invalidate__locked(tb);
    }

    if (current_tb_modified) {
        /* Force execution of one insn next time.  */
        cpu->cflags_next_tb = 1 | CF_NOIRQ | curr_cflags(cpu);
        return true;
    }
    return false;
}
#else
/*
 * @p must be non-NULL.
 * Call with all @pages locked.
 * (@cpu, @retaddr) may be (NULL, 0) outside of a cpu context,
 * in which case precise_smc need not be detected.
 */
#if defined(XBOX)
/*
 * Returns whether the page still holds code afterwards: false means it was
 * emptied and tlb_unprotect_code() ran, i.e. DIRTY_MEMORY_CODE is now set.
 * notdirty_write's tail consumes that instead of re-reading the bitmap.
 */
static bool
#else
static void
#endif
tb_invalidate_phys_page_range__locked(CPUState *cpu,
                                      struct page_collection *pages,
                                      PageDesc *p, tb_page_addr_t start,
                                      tb_page_addr_t last,
                                      uintptr_t retaddr)
{
    TranslationBlock *tb;
    PageForEachNext n;
    bool current_tb_modified = false;
    TranslationBlock *current_tb = NULL;
#if defined(XBOX)
    bool still_code = true;
#endif

    /* Range may not cross a page. */
    tcg_debug_assert(((start ^ last) & TARGET_PAGE_MASK) == 0);

    if (retaddr && cpu && cpu->cc->tcg_ops->precise_smc) {
        current_tb = tcg_tb_lookup(retaddr);
    }

    /*
     * We remove all the TBs in the range [start, last].
     * XXX: see if in some cases it could be faster to invalidate all the code
     */
    PAGE_FOR_EACH_TB(start, last, p, tb, n) {
#ifndef XBOX
        tb_page_addr_t tb_start, tb_last;

        /* NOTE: this is subtle as a TB may span two physical pages */
        tb_start = tb_page_addr0(tb);
        tb_last = tb_start + tb->size - 1;
        if (n == 0) {
            tb_last = MIN(tb_last, tb_start | ~TARGET_PAGE_MASK);
        } else {
            tb_start = tb_page_addr1(tb);
            tb_last = tb_start + (tb_last & ~TARGET_PAGE_MASK);
        }
        if (!(tb_last < start || tb_start > last)) {
#else
        {
            /*
             * Base xemu (703566ce33) invalidates every TB on a written code
             * page, with no byte-range overlap check. (b) counts how many of
             * those TBs the upstream check would have skipped; counting only,
             * the invalidation below stays unconditional. Re-applying the
             * filter was tried and removed (live TBs keep the page trapping
             * every data write); sub-page dirty tracking stops guest stores
             * that miss every code block before they reach this loop.
             */
            if (unlikely(xemu_inv_prof_on())) {
                tb_page_addr_t chk_start, chk_last;
                xemu_subpage_tb_range(tb, n, &chk_start, &chk_last);
                xemu_inv_range_evals++;
                if (chk_last < start || chk_start > last) {
                    xemu_inv_false_share++;
                }
            }
#endif
            if (unlikely(current_tb == tb) &&
                (tb_cflags(current_tb) & CF_COUNT_MASK) != 1) {
                /*
                 * If we are modifying the current TB, we must stop
                 * its execution. We could be more precise by checking
                 * that the modification is after the current PC, but it
                 * would require a specialized function to partially
                 * restore the CPU state.
                 */
                current_tb_modified = true;
                cpu_restore_state_from_tb(cpu, current_tb, retaddr);
            }
            tb_phys_invalidate__locked(tb);
        }
    }

    /* if no code remaining, no need to continue to use slow writes */
    if (!p->first_tb) {
#if defined(XBOX)
        if (unlikely(xemu_inv_prof_on())) {
            xemu_inv_unprotect++;
        }
        /*
         * Page emptied of TBs: clear the sub-block code bitmap in the same
         * breath as tlb_unprotect_code (design clause 3). Clearing only on
         * empty keeps the bitmap a safe over-approximation while any TB
         * remains (a stale-set bit only forfeits a fast-skip; it never
         * under-sets a live block). Re-translation re-sets bits via
         * tb_page_add before the code is executable.
         */
        p->code_blocks = 0;
        xemu_sf_sync_page(start, 0);
        still_code = false;
#endif
        tlb_unprotect_code(start);
    }

    if (unlikely(current_tb_modified)) {
        page_collection_unlock(pages);
        /* Force execution of one insn next time.  */
        cpu->cflags_next_tb = 1 | CF_NOIRQ | curr_cflags(cpu);
        cpu_loop_exit_noexc(cpu);
    }
#if defined(XBOX)
    return still_code;
#endif
}

/*
 * Invalidate all TBs which intersect with the target physical address range
 * [start;last]. NOTE: start and end may refer to *different* physical pages.
 * 'is_cpu_write_access' should be true if called from a real cpu write
 * access: the virtual CPU will exit the current TB if code is modified inside
 * this TB.
 */
void tb_invalidate_phys_range(CPUState *cpu, tb_page_addr_t start,
                              tb_page_addr_t last)
{
    struct page_collection *pages;
    tb_page_addr_t index, index_last;

#if defined(XBOX)
    if (unlikely(xemu_inv_prof_on())) {
        xemu_inv_cur_src = XEMU_INV_SRC_EXPLICIT;
    }
#endif

    pages = page_collection_lock(start, last);

    index_last = last >> TARGET_PAGE_BITS;
    for (index = start >> TARGET_PAGE_BITS; index <= index_last; index++) {
        PageDesc *pd = page_find(index);
        tb_page_addr_t page_start, page_last;

        if (pd == NULL) {
            continue;
        }
        assert_page_locked(pd);
        page_start = index << TARGET_PAGE_BITS;
        page_last = page_start | ~TARGET_PAGE_MASK;
        page_last = MIN(page_last, last);
        tb_invalidate_phys_page_range__locked(cpu, pages, pd,
                                              page_start, page_last, 0);
    }
    page_collection_unlock(pages);
}

/*
 * len must be <= 8 and start must be a multiple of len.
 * Called via softmmu_template.h when code areas are written to with
 * iothread mutex not held.
 */
#if defined(XBOX)
/*
 * XBOX shape: same work, but reports whether the page still holds code
 * (i.e. tlb_unprotect_code() did not run and DIRTY_MEMORY_CODE stayed
 * clear), which is exactly what notdirty_write's tail would otherwise
 * re-read from the dirty bitmap. cputlb.c picks this up with the
 * function-local extern idiom; the upstream void entry point below keeps
 * tb-internal.h's contract.
 */
bool xemu_tb_invalidate_phys_range_fast(CPUState *cpu, ram_addr_t start,
                                        unsigned len, uintptr_t ra);

bool xemu_tb_invalidate_phys_range_fast(CPUState *cpu, ram_addr_t start,
                                        unsigned len, uintptr_t ra)
#else
void tb_invalidate_phys_range_fast(CPUState *cpu, ram_addr_t start,
                                   unsigned len, uintptr_t ra)
#endif
{
    PageDesc *p = page_find(start >> TARGET_PAGE_BITS);

#if defined(XBOX)
    if (unlikely(xemu_inv_prof_on())) {
        xemu_inv_cur_src = XEMU_INV_SRC_NOTDIRTY;
        if (p) {
            /* A notdirty write that reached a code-bearing page = one trap. */
            xemu_inv_traps++;
        }
    }
#endif

    if (p) {
        ram_addr_t last = start + len - 1;
        struct page_collection *pages;

#if defined(XBOX)
        bool filter_noncode = !xemu_subpage_code_overlaps(p, start, len);

        if (unlikely(xemu_subpage_refute_on())) {
            /*
             * Validation mode: take the lock and compare the filter decision
             * to a ground-truth TB-overlap scan (cost irrelevant here), then
             * honor the skip as the default path does, so the refuter
             * validates the live mechanism.
             */
            pages = page_collection_lock(start, last);
            xemu_subpage_refute_check(p, start, last, filter_noncode);
            if (filter_noncode) {
                xemu_subpage_skips++;
                page_collection_unlock(pages);
                return true;
            }
            bool still_code =
                tb_invalidate_phys_page_range__locked(cpu, pages, p,
                                                      start, last, ra);
            page_collection_unlock(pages);
            return still_code;
        }

        if (filter_noncode) {
            /*
             * Arm (a) fast path: the write lands entirely outside every code
             * sub-block, so no TB can have been modified. Skip the
             * page-collection lock, the TB scan, and the invalidation
             * altogether. The page stays write-protected (we did not empty
             * it), so notdirty_write's tail leaves TLB_NOTDIRTY set and the
             * next store re-traps into this O(1) check.
             */
            xemu_subpage_skips++;
            return true;
        }
#endif

        pages = page_collection_lock(start, last);
#if defined(XBOX)
        {
            bool still_code =
                tb_invalidate_phys_page_range__locked(cpu, pages, p,
                                                      start, last, ra);
            page_collection_unlock(pages);
            return still_code;
        }
#else
        tb_invalidate_phys_page_range__locked(cpu, pages, p,
                                              start, last, ra);
        page_collection_unlock(pages);
#endif
    }
#if defined(XBOX)
    /* No PageDesc: nothing to invalidate, so the page's CODE bit is
     * untouched and the caller's tail must not clear TLB_NOTDIRTY. */
    return true;
#endif
}

#if defined(XBOX)
void tb_invalidate_phys_range_fast(CPUState *cpu, ram_addr_t start,
                                   unsigned len, uintptr_t ra)
{
    xemu_tb_invalidate_phys_range_fast(cpu, start, len, ra);
}

/*
 * A load (qemu_loadvm_state) keeps the translation cache. It rewrites RAM
 * through host pointers, around the stores that invalidate translations, so
 * it used to tb_flush, and the frames after every load retranslated every
 * block they ran. Instead each live TB is checked against its own bytes:
 * tb->ihash is fast_hash() of [pc, pc + size) taken at translation, and those
 * bytes sit at ram_addr page_addr[0], then page_addr[1] past the page end. A
 * TB whose bytes changed is invalidated (into the recycler, which revives
 * only on an exact hash match); a kept TB is what translating now would
 * produce. A TB whose translation read past its end (XEMU_TB_PEEKS: an
 * STI checked for the kernel idle loop) is always dropped, and never
 * recycled. This also catches RAM changed by writers that skip
 * invalidation (DSP DMA's memory_region_set_dirty, APU and nv2a
 * host-pointer stores), which the flush used to hide.
 *
 * The rest of what the flush reset:
 * - Write protection. Guest stores invalidate only while a code page's
 *   DIRTY_MEMORY_CODE bit is clear; tb_page_add clears it only for a page's
 *   first TB, and those writers set it with TBs still live. So every page
 *   that keeps code is protected again. A page the check emptied gets the
 *   page-empty tail of tb_invalidate_phys_page_range__locked (sub-page
 *   bits cleared, unprotected); kept pages' code_blocks stay a superset.
 * - Chains. A same-page chain depends only on both TBs' bytes. Cross-page
 *   chains bake in a virtual->phys binding: the tlb_flush below severs them
 *   (xemu_xpage_note_full_flush; the post_load tlb_flush already did) and
 *   clears the jump caches, return-address stack and ret memo.
 * - Host state keyed on tb_flush_count: the icount step
 *   (xemu_istep_disarm) and the XEMU_XPAGE_REFUTE / remap-observer shadows
 *   (xemu_xpage_shadow_reset).
 *
 * Restoring a boot checkpoint over a running game drops 33-62k TBs, so
 * dropping is batched: one jump cache clear rather than one per CF_PCREL
 * TB, and one pass per page list rather than a list search per TB. Such a
 * load costs 9-22 ms in all; one that drops little, 1-6 ms.
 *
 * After an incremental checkpoint restore the check is scoped
 * (xemu_tb_revalidate_pages): a TB's bytes can have changed only on a
 * page something wrote since the last check, and the checkpoint code
 * knows those (xemu_checkpoint_ram_reval_scope: what the restore wrote,
 * and every write the migration dirty log saw since, reset and device
 * load included, as long as the log ran). Only the TBs on those pages are
 * hashed, only those pages are re-protected (the writers that set
 * DIRTY_MEMORY_CODE behind live TBs mark the log too), and the XEMU_TB_PEEKS
 * TBs come from their own list. Restores that copy ~1.7k of 32k pages
 * visit ~2.5k pages instead of every one of ~36k TBs.
 *
 * Call stopped, from the same context as tb_flush__exclusive_or_serial.
 */
typedef struct XemuTbReval {
    GPtrArray *stale;
    uint8_t *buf;
    unsigned kept, pages, reprotected, emptied;
} XemuTbReval;

static void xemu_tb_reval_check(void *p, uint32_t hash, void *userp)
{
    TranslationBlock *tb = p;
    XemuTbReval *r = userp;
    tb_page_addr_t addr0 = tb_page_addr0(tb);
    size_t size = tb->size;
    size_t n0 = MIN(size, TARGET_PAGE_SIZE - (addr0 & ~TARGET_PAGE_MASK));
    const uint8_t *code = qemu_map_ram_ptr(NULL, addr0);

    if (tb->xemu_xpage_reg & XEMU_TB_PEEKS) {
        g_ptr_array_add(r->stale, tb);      /* ihash misses bytes it read */
        return;
    }
    if (n0 < size) {
        memcpy(r->buf, code, n0);
        memcpy(r->buf + n0, qemu_map_ram_ptr(NULL, tb_page_addr1(tb)),
               size - n0);
        code = r->buf;
    }
    if (fast_hash(code, size) == tb->ihash) {
        r->kept++;
    } else {
        g_ptr_array_add(r->stale, tb);
    }
}

/*
 * Take the dropped TBs off their page lists, one pass per page.
 * do_tb_phys_invalidate has marked them CF_INVALID; nothing else on a page
 * list is. Called in JIT write mode: TBs live in the code buffer.
 */
static void xemu_tb_reval_unlink(GPtrArray *stale)
{
    g_autoptr(GArray) idx = g_array_new(false, false, sizeof(tb_page_addr_t));

    for (guint i = 0; i < stale->len; i++) {
        TranslationBlock *tb = g_ptr_array_index(stale, i);
        tb_page_addr_t p0 = tb_page_addr0(tb) >> TARGET_PAGE_BITS;
        tb_page_addr_t p1 = tb_page_addr1(tb);

        g_array_append_val(idx, p0);
        if (p1 != -1 && (p1 >> TARGET_PAGE_BITS) != p0) {
            p1 >>= TARGET_PAGE_BITS;
            g_array_append_val(idx, p1);
        }
    }
    g_array_sort_with_data(idx, tb_page_addr_cmp, NULL);

    for (guint i = 0; i < idx->len; i++) {
        tb_page_addr_t index = g_array_index(idx, tb_page_addr_t, i);
        PageDesc *pd;
        uintptr_t *pprev;
        TranslationBlock *tb;
        PageForEachNext n;

        if (i && index == g_array_index(idx, tb_page_addr_t, i - 1)) {
            continue;
        }
        pd = page_find(index);
        page_lock(pd);
        pprev = &pd->first_tb;
        PAGE_FOR_EACH_TB(unused, unused, pd, tb, n) {
            if (tb_cflags(tb) & CF_INVALID) {
                *pprev = tb->page_next[n];
            } else {
                pprev = &tb->page_next[n];
            }
        }
        page_unlock(pd);
    }
}

/*
 * Re-protect page @pd (at @addr) if it keeps code; give it the page-empty
 * tail if the drop emptied it. Only the stopped vCPU adds TBs, so the
 * unlocked pre-check misses none.
 */
static void xemu_tb_reval_protect_page(PageDesc *pd, ram_addr_t addr,
                                       XemuTbReval *r)
{
    if (!pd->first_tb && !pd->code_blocks) {
        return;
    }
    page_lock(pd);
    if (pd->first_tb) {
        r->pages++;
        r->reprotected += physical_memory_test_and_clear_dirty(
            addr, TARGET_PAGE_SIZE, DIRTY_MEMORY_CODE);
    } else {
        r->emptied++;
        pd->code_blocks = 0;
        xemu_sf_sync_page(addr, 0);
        tlb_unprotect_code(addr);
    }
    page_unlock(pd);
}

/*
 * xemu_tb_reval_protect_page for every page: tb_remove_all_1's walk;
 * @base is the page index.
 */
static void xemu_tb_reval_protect(int level, void **lp, tb_page_addr_t base,
                                  XemuTbReval *r)
{
    if (*lp == NULL) {
        return;
    }
    if (level == 0) {
        PageDesc *pd = *lp;

        for (int i = 0; i < V_L2_SIZE; i++) {
            xemu_tb_reval_protect_page(&pd[i],
                                       (base | i) << TARGET_PAGE_BITS, r);
        }
    } else {
        void **pp = *lp;

        for (int i = 0; i < V_L2_SIZE; i++) {
            xemu_tb_reval_protect(level - 1, pp + i,
                                  base | ((tb_page_addr_t)i <<
                                          (level * V_L2_BITS)), r);
        }
    }
}

/* Drop @stale's TBs: invalidate them, then take them off their pages. */
static void xemu_tb_reval_drop(GPtrArray *stale)
{
    /* TBs live in the code buffer: their fields and jumps are JIT writes. */
    qatomic_set(&tb_jmp_cache_inval_deferred, true);
    qemu_thread_jit_write();
    for (guint i = 0; i < stale->len; i++) {
        do_tb_phys_invalidate(g_ptr_array_index(stale, i), false);
    }
    xemu_tb_reval_unlink(stale);
    qemu_thread_jit_execute();
    qatomic_set(&tb_jmp_cache_inval_deferred, false);
}

static void xemu_tb_reval_protect_all(XemuTbReval *r)
{
    for (int i = 0; i < v_l1_size; i++) {
        xemu_tb_reval_protect(v_l2_levels, l1_map + i,
                              (tb_page_addr_t)i << v_l1_shift, r);
    }
}

/* What every load resets besides the TBs and the protection. */
static void xemu_tb_reval_finish(XemuTbReval *r, int64_t t0,
                                 const char *what)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        tlb_flush(cpu);
    }
    xemu_istep_disarm();
    xemu_xpage_shadow_reset();

    if (unlikely(xemu_inv_prof_on())) {
        fprintf(stderr, "xemu: load%s kept %u TBs, dropped %u; %u code pages "
                "(%u re-protected), %u emptied; %lld us\n",
                what, r->kept, r->stale->len, r->pages, r->reprotected,
                r->emptied, (long long)(g_get_monotonic_time() - t0));
    }
    g_ptr_array_free(r->stale, true);
}

/*
 * XEMU_TB_REVAL_REFUTE=1: after each scoped check, also run the full one
 * and count what only it does: TBs it drops (their bytes changed off the
 * scope, or an XEMU_TB_PEEKS TB the list missed) and code pages it
 * re-protects. Both must stay 0; the full check's results are kept, so a
 * violation doesn't spread.
 */
static struct {
    int on;
    unsigned loads, extra, unprotected, untailed;
    uint64_t scoped, full;
} xemu_reval_refute = { .on = -1 };

static void xemu_reval_refute_report(void)
{
    fprintf(stderr, "xemu: reval refute: %u scoped loads, %" PRIu64
            " TBs checked by the scope, %" PRIu64 " by the full walk; "
            "extra drops %u, unprotected pages %u, untailed pages %u\n",
            xemu_reval_refute.loads, xemu_reval_refute.scoped,
            xemu_reval_refute.full, xemu_reval_refute.extra,
            xemu_reval_refute.unprotected, xemu_reval_refute.untailed);
}

/* Armed at the first load of any kind, so a run with nothing scoped says so. */
static void xemu_reval_refute_init(void)
{
    if (xemu_reval_refute.on < 0) {
        const char *e = getenv("XEMU_TB_REVAL_REFUTE");

        xemu_reval_refute.on = e && e[0] == '1';
        if (xemu_reval_refute.on) {
            atexit(xemu_reval_refute_report);
        }
    }
}

void xemu_tb_revalidate_after_load(void)
{
    int64_t t0 = unlikely(xemu_inv_prof_on()) ? g_get_monotonic_time() : 0;
    XemuTbReval r = { .stale = g_ptr_array_new() };

    assert(tcg_enabled());
    assert(!runstate_is_running() ||
           (current_cpu && cpu_in_serial_context(current_cpu)));
    xemu_reval_refute_init();

    r.buf = g_malloc(TARGET_PAGE_SIZE);
    WITH_RCU_READ_LOCK_GUARD() {
        qht_iter(&tb_ctx.htable, xemu_tb_reval_check, &r);
    }
    g_free(r.buf);
    /* The walk found every XEMU_TB_PEEKS TB. */
    xemu_peeks_take(NULL);

    xemu_tb_reval_drop(r.stale);
    xemu_tb_reval_protect_all(&r);
    xemu_tb_reval_finish(&r, t0, "");
}

static void xemu_reval_refute_check(XemuTbReval *r, uint8_t *buf)
{
    XemuTbReval f = { .stale = g_ptr_array_new(), .buf = buf };
    XemuTbReval g = { 0 };

    WITH_RCU_READ_LOCK_GUARD() {
        qht_iter(&tb_ctx.htable, xemu_tb_reval_check, &f);
    }
    for (guint i = 0; i < MIN(f.stale->len, 8); i++) {
        TranslationBlock *tb = g_ptr_array_index(f.stale, i);

        fprintf(stderr, "xemu: reval refute: VIOLATION: kept a stale TB at "
                "ram_addr " TB_PAGE_ADDR_FMT " (+" TB_PAGE_ADDR_FMT
                "), %u bytes%s\n", tb_page_addr0(tb), tb_page_addr1(tb),
                tb->size, tb->xemu_xpage_reg & XEMU_TB_PEEKS ? ", peeks" : "");
    }
    xemu_tb_reval_drop(f.stale);
    xemu_tb_reval_protect_all(&g);
    if (f.stale->len || g.reprotected) {
        fprintf(stderr, "xemu: reval refute: VIOLATION: load %u: the full "
                "check drops %u more TBs and re-protects %u pages\n",
                xemu_reval_refute.loads + 1, f.stale->len, g.reprotected);
    }
    xemu_reval_refute.loads++;
    xemu_reval_refute.scoped += r->kept + r->stale->len;
    xemu_reval_refute.full += f.kept + f.stale->len;
    xemu_reval_refute.extra += f.stale->len;
    xemu_reval_refute.unprotected += g.reprotected;
    xemu_reval_refute.untailed += g.emptied;
    if (!(xemu_reval_refute.loads % 256)) {
        xemu_reval_refute_report();
    }
    g_ptr_array_free(f.stale, true);
}

void xemu_tb_revalidate_pages(const unsigned long *scope, uint64_t pages)
{
    int64_t t0 = unlikely(xemu_inv_prof_on()) ? g_get_monotonic_time() : 0;
    XemuTbReval r = { .stale = g_ptr_array_new() };
    g_autoptr(GArray) off = g_array_new(false, false, sizeof(tb_page_addr_t));

    assert(tcg_enabled());
    assert(!runstate_is_running() ||
           (current_cpu && cpu_in_serial_context(current_cpu)));
    xemu_reval_refute_init();

    r.buf = g_malloc(TARGET_PAGE_SIZE);
    /* Every TB that read past its end goes, wherever it is. */
    xemu_peeks_take(r.stale);
    /* xemu_tb_reval_check maps RAM (qemu_map_ram_ptr), as the full walk. */
    WITH_RCU_READ_LOCK_GUARD() {
        for (uint64_t p = find_first_bit(scope, pages); p < pages;
             p = find_next_bit(scope, pages, p + 1)) {
            PageDesc *pd = page_find(p);
            TranslationBlock *tb;
            PageForEachNext n;

            if (!pd || !pd->first_tb) {
                continue;
            }
            page_lock(pd);
            PAGE_FOR_EACH_TB(unused, unused, pd, tb, n) {
                tb_page_addr_t p0 = tb_page_addr0(tb) >> TARGET_PAGE_BITS;

                /* A TB on two pages of the scope is checked from its first. */
                if ((n && p0 < pages && test_bit(p0, scope)) ||
                    (tb->xemu_xpage_reg & XEMU_TB_PEEKS)) {
                    continue;
                }
                xemu_tb_reval_check(tb, 0, &r);
            }
            page_unlock(pd);
        }
    }

    /* Pages a dropped TB also sits on, off the scope: maybe emptied. */
    for (guint i = 0; i < r.stale->len; i++) {
        TranslationBlock *tb = g_ptr_array_index(r.stale, i);
        tb_page_addr_t q[2] = { tb_page_addr0(tb), tb_page_addr1(tb) };

        for (int k = 0; k < 2; k++) {
            tb_page_addr_t idx = q[k] >> TARGET_PAGE_BITS;
            if (q[k] != -1 && (idx >= pages || !test_bit(idx, scope))) {
                g_array_append_val(off, idx);
            }
        }
    }
    xemu_tb_reval_drop(r.stale);
    for (uint64_t p = find_first_bit(scope, pages); p < pages;
         p = find_next_bit(scope, pages, p + 1)) {
        PageDesc *pd = page_find(p);
        if (pd) {
            xemu_tb_reval_protect_page(pd, p << TARGET_PAGE_BITS, &r);
        }
    }
    g_array_sort_with_data(off, tb_page_addr_cmp, NULL);
    for (guint i = 0; i < off->len; i++) {
        tb_page_addr_t idx = g_array_index(off, tb_page_addr_t, i);
        PageDesc *pd;

        if (i && idx == g_array_index(off, tb_page_addr_t, i - 1)) {
            continue;
        }
        pd = page_find(idx);
        if (pd) {
            xemu_tb_reval_protect_page(pd, idx << TARGET_PAGE_BITS, &r);
        }
    }
    if (unlikely(xemu_reval_refute.on)) {
        xemu_reval_refute_check(&r, r.buf);
    }
    g_free(r.buf);
    xemu_tb_reval_finish(&r, t0, " (scoped)");
}

/*
 * Guest PC hooks (target/i386/tcg/xemu-hooks.h): a hook site changes how
 * a page's code translates while its bytes stay the same, so the page's
 * TBs must go and the recycler must not revive them or any older TB of
 * the page. Call with the vCPU stopped (the recycler takes its TBs out of
 * inv_htable without a lock held across the lookup).
 */
static bool xemu_tb_on_page(void *p, uint32_t h, void *userp)
{
    TranslationBlock *tb = p;
    tb_page_addr_t page = *(tb_page_addr_t *)userp;
    tb_page_addr_t p1 = tb_page_addr1(tb);

    return (tb_page_addr0(tb) & TARGET_PAGE_MASK) == page ||
           (p1 != -1 && (p1 & TARGET_PAGE_MASK) == page);
}

void xemu_tb_invalidate_page_norecycle(tb_page_addr_t addr)
{
    tb_page_addr_t page = addr & TARGET_PAGE_MASK;

    tb_invalidate_phys_range(NULL, page, page | ~TARGET_PAGE_MASK);
    qht_iter_remove(&tb_ctx.inv_htable, xemu_tb_on_page, &page);
}
#endif

#endif /* CONFIG_USER_ONLY */
