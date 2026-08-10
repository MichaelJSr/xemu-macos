/*
 *  emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
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
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "hw/core/cpu.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/helper-retaddr.h"
#include "trace.h"
#include "disas/disas.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/page-protection.h"
#include "exec/mmap-lock.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "qemu/atomic.h"
#include "qemu/rcu.h"
#include "exec/log.h"
#include "qemu/main-loop.h"
#include "exec/icount.h"
#include "exec/replay-core.h"
#include "system/tcg.h"
#include "exec/helper-proto-common.h"
#include "tcg-accel-ops.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"
#include "tb-code-hash.h"
#include "tb-context.h"
#include "tb-internal.h"
#include "internal-common.h"
#if defined(XBOX)
#include "xemu-inv-prof.h"
#include "xemu-xpage.h"
#endif

#if defined(XBOX) && !defined(CONFIG_USER_ONLY)
/*
 * Defined in target/i386/tcg/tcg-cpu.c (function-local extern idiom: this
 * file is built target-agnostic, so target/i386/cpu.h is out of reach). The
 * fork builds exactly one system target, so the hot TB-lookup sites below
 * call the state extractor directly instead of through TCGCPUOps; the
 * identity of the vtable slot is asserted in tcg_exec_realizefn().
 */
TCGTBCPUState x86_get_tb_cpu_state(CPUState *cs);
#define xemu_get_tb_cpu_state(cpu) x86_get_tb_cpu_state(cpu)
#else
#define xemu_get_tb_cpu_state(cpu) ((cpu)->cc->tcg_ops->get_tb_cpu_state(cpu))
#endif

/* -icount align implementation. */

typedef struct SyncClocks {
    int64_t diff_clk;
    int64_t last_cpu_icount;
    int64_t realtime_clock;
} SyncClocks;

#if !defined(CONFIG_USER_ONLY)
/* Allow the guest to have a max 3ms advance.
 * The difference between the 2 clocks could therefore
 * oscillate around 0.
 */
#define VM_CLOCK_ADVANCE 3000000
#define THRESHOLD_REDUCE 1.5
#define MAX_DELAY_PRINT_RATE 2000000000LL
#define MAX_NB_PRINTS 100

int64_t max_delay;
int64_t max_advance;

static void align_clocks(SyncClocks *sc, CPUState *cpu)
{
    int64_t cpu_icount;

    if (!icount_align_option) {
        return;
    }

    cpu_icount = cpu->icount_extra + cpu->neg.icount_decr.u16.low;
    sc->diff_clk += icount_to_ns(sc->last_cpu_icount - cpu_icount);
    sc->last_cpu_icount = cpu_icount;

    if (sc->diff_clk > VM_CLOCK_ADVANCE) {
#ifndef _WIN32
        struct timespec sleep_delay, rem_delay;
        sleep_delay.tv_sec = sc->diff_clk / 1000000000LL;
        sleep_delay.tv_nsec = sc->diff_clk % 1000000000LL;
        if (nanosleep(&sleep_delay, &rem_delay) < 0) {
            sc->diff_clk = rem_delay.tv_sec * 1000000000LL + rem_delay.tv_nsec;
        } else {
            sc->diff_clk = 0;
        }
#else
        Sleep(sc->diff_clk / SCALE_MS);
        sc->diff_clk = 0;
#endif
    }
}

static void print_delay(const SyncClocks *sc)
{
    static float threshold_delay;
    static int64_t last_realtime_clock;
    static int nb_prints;

    if (icount_align_option &&
        sc->realtime_clock - last_realtime_clock >= MAX_DELAY_PRINT_RATE &&
        nb_prints < MAX_NB_PRINTS) {
        if ((-sc->diff_clk / (float)1000000000LL > threshold_delay) ||
            (-sc->diff_clk / (float)1000000000LL <
             (threshold_delay - THRESHOLD_REDUCE))) {
            threshold_delay = (-sc->diff_clk / 1000000000LL) + 1;
            qemu_printf("Warning: The guest is now late by %.1f to %.1f seconds\n",
                        threshold_delay - 1,
                        threshold_delay);
            nb_prints++;
            last_realtime_clock = sc->realtime_clock;
        }
    }
}

static void init_delay_params(SyncClocks *sc, CPUState *cpu)
{
    if (!icount_align_option) {
        return;
    }
    sc->realtime_clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    sc->diff_clk = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - sc->realtime_clock;
    sc->last_cpu_icount
        = cpu->icount_extra + cpu->neg.icount_decr.u16.low;
    if (sc->diff_clk < max_delay) {
        max_delay = sc->diff_clk;
    }
    if (sc->diff_clk > max_advance) {
        max_advance = sc->diff_clk;
    }

    /* Print every 2s max if the guest is late. We limit the number
       of printed messages to NB_PRINT_MAX(currently 100) */
    print_delay(sc);
}
#else
static void align_clocks(SyncClocks *sc, const CPUState *cpu)
{
}

static void init_delay_params(SyncClocks *sc, const CPUState *cpu)
{
}
#endif /* CONFIG USER ONLY */

struct tb_desc {
    TCGTBCPUState s;
    CPUArchState *env;
    tb_page_addr_t page_addr0;
};

static bool tb_lookup_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const struct tb_desc *desc = d;

    if ((tb_cflags(tb) & CF_PCREL || tb->pc == desc->s.pc) &&
        tb_page_addr0(tb) == desc->page_addr0 &&
        tb->cs_base == desc->s.cs_base &&
        tb->flags == desc->s.flags &&
        (tb_cflags(tb) & ~CF_INVALID) == desc->s.cflags) {
        /* check next page if needed */
        tb_page_addr_t tb_phys_page1 = tb_page_addr1(tb);
        if (tb_phys_page1 == -1) {
            return true;
        } else {
            tb_page_addr_t phys_page1;
            vaddr virt_page1;

            /*
             * We know that the first page matched, and an otherwise valid TB
             * encountered an incomplete instruction at the end of that page,
             * therefore we know that generating a new TB from the current PC
             * must also require reading from the next page -- even if the
             * second pages do not match, and therefore the resulting insn
             * is different for the new TB.  Therefore any exception raised
             * here by the faulting lookup is not premature.
             */
            virt_page1 = TARGET_PAGE_ALIGN(desc->s.pc);
            phys_page1 = get_page_addr_code(desc->env, virt_page1);
            if (tb_phys_page1 == phys_page1) {
                return true;
            }
        }
    }
    return false;
}

static TranslationBlock *
tb_htable_lookup_common(CPUState *cpu, TCGTBCPUState s, const struct qht *ht,
                        qht_lookup_func_t func)
{
    tb_page_addr_t phys_pc;
    struct tb_desc desc;
    uint32_t h;

    desc.s = s;
    desc.env = cpu_env(cpu);
    phys_pc = get_page_addr_code(desc.env, s.pc);
    if (phys_pc == -1) {
        return NULL;
    }
    desc.page_addr0 = phys_pc;
    h = tb_hash_func(phys_pc, (s.cflags & CF_PCREL ? 0 : s.pc),
                     s.flags, s.cs_base, s.cflags);
    return qht_lookup_custom(ht, &desc, h, func);
}

static TranslationBlock *tb_htable_lookup(CPUState *cpu, TCGTBCPUState s)
{
    return tb_htable_lookup_common(cpu, s, &tb_ctx.htable, tb_lookup_cmp);
}

static bool inv_tb_lookup_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const struct tb_desc *desc = d;

    return tb_lookup_cmp(p, d) &&
           tb->ihash == tb_code_hash_func(desc->env, desc->s.pc, tb->size);
}

TranslationBlock *inv_tb_htable_lookup(CPUState *cpu, TCGTBCPUState s)
{
    return tb_htable_lookup_common(cpu, s, &tb_ctx.inv_htable, inv_tb_lookup_cmp);
}

#if defined(XBOX)
/*
 * XEMU_INV_PROF (c): classify a recycle miss. inv_tb_htable_lookup failed;
 * this repeats the lookup WITHOUT the ihash byte comparator. A hit here means
 * an invalidated TB with identical pc/cs_base/flags/cflags exists but its code
 * bytes changed -> genuine SMC. A miss means no invalidated peer -> cold/fresh
 * code. Counting-only; used off the fresh-translation (slow) path.
 */
TranslationBlock *xemu_inv_htable_lookup_ignore_bytes(CPUState *cpu,
                                                      TCGTBCPUState s)
{
    return tb_htable_lookup_common(cpu, s, &tb_ctx.inv_htable, tb_lookup_cmp);
}
#endif

/**
 * tb_lookup:
 * @cpu: CPU that will execute the returned translation block
 * @pc: guest PC
 * @cs_base: arch-specific value associated with translation block
 * @flags: arch-specific translation block flags
 * @cflags: CF_* flags
 *
 * Look up a translation block inside the QHT using @pc, @cs_base, @flags and
 * @cflags. Uses @cpu's tb_jmp_cache. Might cause an exception, so have a
 * longjmp destination ready.
 *
 * Returns: an existing translation block or NULL.
 */
#if defined(XBOX)
/*
 * XEMU_GUEST_PROF=1: one-run guest profiler.
 *  - SIGPROF sampler: raw host PCs into a ring (async-signal-safe
 *    store only); resolved at exit via the tc.ptr side table (guest
 *    TBs) or dladdr (helpers/host).
 *  - Exit-kind classification: the i386 emitter tags ret / indirect
 *    jmp / indirect call; tb_gen_code() records {guest pc, kind} per
 *    tc.ptr (CF_PCREL leaves tb->pc unusable, so tc.ptr is the key);
 *    helper_lookup_tb_ptr buckets its calls by source-TB kind.
 * Measurement-run tool: the classification adds a cached source-TB
 * resolve per lookup and is not benchmark-neutral.
 */
#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#define XEMU_GUEST_PROF_SAMPLER 1
#endif

enum {
    XEMU_GP_KIND_OTHER = 0,
    XEMU_GP_KIND_RET = 1,
    XEMU_GP_KIND_IJMP = 2,
    XEMU_GP_KIND_ICALL = 3,
};
#define XEMU_GP_NKINDS 4

/* Set by the i386 emitter for the TB being translated. */
int xemu_guestprof_pending_kind;

bool xemu_guestprof_on(void);
void xemu_guestprof_note_tb(const void *tc_ptr, uint64_t guest_pc, int kind);

bool xemu_guestprof_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_GUEST_PROF");
        on = (e && e[0] == '1') ? 1 : 0;
    }
    return on;
}

/* tc.ptr -> {guest pc, exit kind}; open-addressed, translation-locked. */
#define XEMU_GP_TAB_BITS 20
#define XEMU_GP_TAB_SIZE (1u << XEMU_GP_TAB_BITS)
typedef struct {
    const void *tc_ptr;
    uint64_t guest_pc;
    uint8_t kind;
} XemuGpEnt;
static XemuGpEnt *xemu_gp_tab;

static inline uint32_t xemu_gp_hash(const void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 29;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 32;
    return (uint32_t)v & (XEMU_GP_TAB_SIZE - 1);
}

void xemu_guestprof_note_tb(const void *tc_ptr, uint64_t guest_pc, int kind)
{
    if (!xemu_gp_tab) {
        xemu_gp_tab = g_malloc0(sizeof(XemuGpEnt) * XEMU_GP_TAB_SIZE);
    }
    uint32_t h = xemu_gp_hash(tc_ptr);
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & (XEMU_GP_TAB_SIZE - 1)) {
        if (!xemu_gp_tab[h].tc_ptr || xemu_gp_tab[h].tc_ptr == tc_ptr) {
            xemu_gp_tab[h].tc_ptr = tc_ptr;
            xemu_gp_tab[h].guest_pc = guest_pc;
            xemu_gp_tab[h].kind = (uint8_t)kind;
            return;
        }
    }
}

static XemuGpEnt *xemu_gp_find(const void *tc_ptr)
{
    if (!xemu_gp_tab) {
        return NULL;
    }
    uint32_t h = xemu_gp_hash(tc_ptr);
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & (XEMU_GP_TAB_SIZE - 1)) {
        if (xemu_gp_tab[h].tc_ptr == tc_ptr) {
            return &xemu_gp_tab[h];
        }
        if (!xemu_gp_tab[h].tc_ptr) {
            return NULL;
        }
    }
    return NULL;
}

/* Per-exit-kind lookup counters (vCPU thread only). */
static uint64_t xemu_gp_kind_lookups[XEMU_GP_NKINDS];
static uint64_t xemu_gp_kind_misses[XEMU_GP_NKINDS];
static int xemu_gp_cur_kind;

/*
 * Source-TB exit kind for a helper return address; direct-mapped
 * cache in front of the qht walk (hot source TBs are few).
 */
static uintptr_t xemu_gp_ra_key[256];
static uint8_t xemu_gp_ra_kind[256];

static int xemu_gp_kind_for_ra(uintptr_t ra)
{
    uint32_t h = (ra >> 4) & 255;
    if (xemu_gp_ra_key[h] == ra) {
        return xemu_gp_ra_kind[h];
    }
    TranslationBlock *stb = tcg_tb_lookup(ra);
    int k = XEMU_GP_KIND_OTHER;
    if (stb) {
        XemuGpEnt *e = xemu_gp_find(stb->tc.ptr);
        if (e) {
            k = e->kind;
        }
    }
    xemu_gp_ra_key[h] = ra;
    xemu_gp_ra_kind[h] = (uint8_t)k;
    return k;
}

#if defined(XEMU_GUEST_PROF_SAMPLER)
/*
 * macOS delivers process-wide SIGPROF to an arbitrary thread (not the
 * one consuming CPU), so sample the vCPU thread from a dedicated
 * thread instead: suspend, read ARM_THREAD_STATE64.pc, resume — the
 * same approach sample(1) uses. The vCPU registers its mach port on
 * first cpu_exec entry.
 */
#define XEMU_GP_RING_BITS 21
#define XEMU_GP_RING_SIZE (1u << XEMU_GP_RING_BITS)
static uint64_t *xemu_gp_ring;
static uint32_t xemu_gp_ring_idx;
static mach_port_t xemu_gp_vcpu_port;
static bool xemu_gp_sampler_stop;

void xemu_guestprof_register_vcpu(void);
void xemu_guestprof_register_vcpu(void)
{
    if (xemu_guestprof_on() && !xemu_gp_vcpu_port) {
        xemu_gp_vcpu_port = pthread_mach_thread_np(pthread_self());
    }
}

static void *xemu_gp_sampler(void *arg)
{
    while (!qatomic_read(&xemu_gp_sampler_stop)) {
        mach_port_t port = xemu_gp_vcpu_port;
        if (port) {
            arm_thread_state64_t st;
            mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            if (thread_suspend(port) == KERN_SUCCESS) {
                if (thread_get_state(port, ARM_THREAD_STATE64,
                                     (thread_state_t)&st,
                                     &cnt) == KERN_SUCCESS) {
                    uint32_t i = xemu_gp_ring_idx++;
                    xemu_gp_ring[i & (XEMU_GP_RING_SIZE - 1)] =
                        arm_thread_state64_get_pc(st);
                }
                thread_resume(port);
            }
        }
        g_usleep(1000);
    }
    return NULL;
}

/*
 * Shared top-N report block: iterate a count table into a fixed array,
 * selection-sort by count, print ranked rows.  key_is_str selects the %s vs
 * 0x%08llx row argument; header_fmt may consume the distinct count as %d.
 */
static void xemu_gp_report_top(GHashTable *table, uint32_t filled, int cap,
                               int max_rows, bool key_is_str,
                               const char *header_fmt, const char *row_fmt)
{
    GHashTableIter it;
    gpointer kk, vv;
    struct { gpointer k; uint64_t c; } top[4096];
    int cnt = 0;

    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &kk, &vv) && cnt < cap) {
        top[cnt].k = kk;
        top[cnt].c = (uintptr_t)vv;
        cnt++;
    }
    for (int a = 0; a < cnt; a++) {
        for (int b = a + 1; b < cnt; b++) {
            if (top[b].c > top[a].c) {
                typeof(top[0]) t = top[a];
                top[a] = top[b];
                top[b] = t;
            }
        }
    }
    fprintf(stderr, header_fmt, cnt);
    for (int rank = 0; rank < MIN(cnt, max_rows); rank++) {
        if (key_is_str) {
            fprintf(stderr, row_fmt, 100.0 * top[rank].c / filled,
                    (const char *)top[rank].k);
        } else {
            fprintf(stderr, row_fmt, 100.0 * top[rank].c / filled,
                    (unsigned long long)(uintptr_t)top[rank].k);
        }
    }
}
#endif

static void xemu_guestprof_dump(void)
{
#if defined(XEMU_GUEST_PROF_SAMPLER)
    qatomic_set(&xemu_gp_sampler_stop, true);
    g_usleep(5000);

    uint32_t n = qatomic_read(&xemu_gp_ring_idx);
    uint32_t filled = MIN(n, XEMU_GP_RING_SIZE);
    if (filled == 0) {
        return;
    }

    /* Aggregate: guest TBs by tc-side-table hit, else host symbol. */
    GHashTable *guest = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *pages = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *host = g_hash_table_new(g_str_hash, g_str_equal);
    uint64_t guest_total = 0, host_total = 0, unknown = 0;

    for (uint32_t i = 0; i < filled; i++) {
        uint64_t pc = xemu_gp_ring[i];
        TranslationBlock *tb = tcg_tb_lookup((uintptr_t)pc);
        if (tb) {
            XemuGpEnt *e = xemu_gp_find(tb->tc.ptr);
            uint64_t gpc = e ? e->guest_pc : 0;
            guest_total++;
            g_hash_table_insert(
                guest, (gpointer)(uintptr_t)gpc,
                (gpointer)((uintptr_t)g_hash_table_lookup(
                               guest, (gpointer)(uintptr_t)gpc) + 1));
            g_hash_table_insert(
                pages, (gpointer)(uintptr_t)(gpc & ~0xfffull),
                (gpointer)((uintptr_t)g_hash_table_lookup(
                               pages,
                               (gpointer)(uintptr_t)(gpc & ~0xfffull)) + 1));
        } else {
            Dl_info di;
            const char *name = "?";
            if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_sname) {
                name = di.dli_sname;
            } else {
                unknown++;
            }
            host_total++;
            char *key = g_strdup(name);
            gpointer old = g_hash_table_lookup(host, key);
            if (old) {
                g_free(key);
                key = NULL;
            }
            g_hash_table_insert(host, key ? key : g_strdup(name),
                                (gpointer)((uintptr_t)old + 1));
        }
    }

    fprintf(stderr,
            "xemu: guestprof %u samples: guest-TCG %llu (%.1f%%), "
            "host %llu (%.1f%%, %llu unsymbolized)\n",
            filled, (unsigned long long)guest_total,
            100.0 * guest_total / filled, (unsigned long long)host_total,
            100.0 * host_total / filled, (unsigned long long)unknown);

    xemu_gp_report_top(host, filled, 512, 25, true,
                       "xemu: guestprof top host symbols:\n",
                       "xemu:   %6.2f%%  %s\n");
    xemu_gp_report_top(guest, filled, 4096, 30, false,
                       "xemu: guestprof top guest TBs (%d distinct):\n",
                       "xemu:   %6.2f%%  tb_pc=0x%08llx\n");
    xemu_gp_report_top(pages, filled, 4096, 15, false,
                       "xemu: guestprof top guest 4K pages (%d distinct):\n",
                       "xemu:   %6.2f%%  page=0x%08llx\n");

    g_hash_table_destroy(guest);
    g_hash_table_destroy(pages);
    g_hash_table_destroy(host);
#endif

    {
        static const char *const kn[XEMU_GP_NKINDS] = {
            "other", "ret", "ijmp", "icall"
        };
        uint64_t tot = 0;
        for (int i = 0; i < XEMU_GP_NKINDS; i++) {
            tot += xemu_gp_kind_lookups[i];
        }
        if (tot) {
            fprintf(stderr, "xemu: guestprof lookup exit kinds:\n");
            for (int i = 0; i < XEMU_GP_NKINDS; i++) {
                uint64_t l = xemu_gp_kind_lookups[i];
                uint64_t m = xemu_gp_kind_misses[i];
                fprintf(stderr,
                        "xemu:   %-5s lookups=%llu (%.1f%%) "
                        "jc_miss=%llu (%.2f%%)\n",
                        kn[i], (unsigned long long)l, 100.0 * l / tot,
                        (unsigned long long)m, l ? 100.0 * m / l : 0.0);
            }
        }
    }
}

static void xemu_guestprof_start(void)
{
#if defined(XEMU_GUEST_PROF_SAMPLER)
    xemu_gp_ring = g_malloc0(sizeof(uint64_t) * XEMU_GP_RING_SIZE);
    pthread_t th;
    pthread_create(&th, NULL, xemu_gp_sampler, NULL);
#endif
    atexit(xemu_guestprof_dump);
}

/*
 * XEMU_TB_PROF=1: jump-cache effectiveness counters for the single Xbox
 * vCPU (plain increments; the vCPU thread is the only writer).
 */
static uint64_t xemu_tbprof_lookups;
static uint64_t xemu_tbprof_jc_hits;
static uint64_t xemu_tbprof_ht_found;

static bool xemu_tbprof_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_TB_PROF");
        on = (e && e[0] == '1') ? 1 : 0;
    }
    return on;
}

static void xemu_tbprof_dump(void)
{
    uint64_t l = xemu_tbprof_lookups;
    uint64_t h = xemu_tbprof_jc_hits;
    uint64_t f = xemu_tbprof_ht_found;
    if (l == 0) {
        return;
    }
    fprintf(stderr,
            "xemu: tbprof lookups=%llu jc_hit=%llu (%.2f%%) ht_found=%llu "
            "(%.2f%%) translate=%llu tb_flush=%u\n",
            (unsigned long long)l, (unsigned long long)h, 100.0 * h / l,
            (unsigned long long)f, 100.0 * f / l,
            (unsigned long long)(l - h - f),
            qatomic_read(&tb_ctx.tb_flush_count));
}
#endif

static inline TranslationBlock *tb_lookup(CPUState *cpu, TCGTBCPUState s)
{
    TranslationBlock *tb;
    CPUJumpCache *jc;
    uint32_t hash;

    /* we should never be trying to look up an INVALID tb */
    tcg_debug_assert(!(s.cflags & CF_INVALID));

    hash = tb_jmp_cache_hash_func(s.pc);
    jc = cpu->tb_jmp_cache;

#if defined(XBOX)
    if (unlikely(xemu_tbprof_on())) {
        xemu_tbprof_lookups++;
    }
#endif

    tb = qatomic_read(&jc->array[hash].tb);
    if (likely(tb &&
               jc->array[hash].pc == s.pc &&
               tb->cs_base == s.cs_base &&
               tb->flags == s.flags &&
               tb_cflags(tb) == s.cflags)) {
#if defined(XBOX)
        if (unlikely(xemu_tbprof_on())) {
            xemu_tbprof_jc_hits++;
        }
#endif
        goto hit;
    }

#if defined(XBOX)
    if (unlikely(xemu_guestprof_on())) {
        xemu_gp_kind_misses[xemu_gp_cur_kind]++;
    }
#endif
    tb = tb_htable_lookup(cpu, s);
    if (tb == NULL) {
        return NULL;
    }
#if defined(XBOX)
    if (unlikely(xemu_tbprof_on())) {
        xemu_tbprof_ht_found++;
    }
#endif

    jc->array[hash].pc = s.pc;
    qatomic_set(&jc->array[hash].tb, tb);

hit:
    /*
     * As long as tb is not NULL, the contents are consistent.  Therefore,
     * the virtual PC has to match for non-CF_PCREL translations.
     */
    assert((tb_cflags(tb) & CF_PCREL) || tb->pc == s.pc);
    return tb;
}

static void log_cpu_exec(vaddr pc, CPUState *cpu,
                         const TranslationBlock *tb)
{
    if (qemu_log_in_addr_range(pc)) {
        qemu_log_mask(CPU_LOG_EXEC,
                      "Trace %d: %p [%08" PRIx64
                      "/%016" VADDR_PRIx "/%08x/%08x] %s\n",
                      cpu->cpu_index, tb->tc.ptr, tb->cs_base, pc,
                      tb->flags, tb->cflags, lookup_symbol(pc));

        if (qemu_loglevel_mask(CPU_LOG_TB_CPU)) {
            FILE *logfile = qemu_log_trylock();
            if (logfile) {
                int flags = CPU_DUMP_CCOP;

                if (qemu_loglevel_mask(CPU_LOG_TB_FPU)) {
                    flags |= CPU_DUMP_FPU;
                }
                if (qemu_loglevel_mask(CPU_LOG_TB_VPU)) {
                    flags |= CPU_DUMP_VPU;
                }
                cpu_dump_state(cpu, logfile, flags);
                qemu_log_unlock(logfile);
            }
        }
    }
}

static bool check_for_breakpoints_slow(CPUState *cpu, vaddr pc,
                                       uint32_t *cflags)
{
    CPUBreakpoint *bp;
    bool match_page = false;

    /*
     * Singlestep overrides breakpoints.
     * This requirement is visible in the record-replay tests, where
     * we would fail to make forward progress in reverse-continue.
     *
     * TODO: gdb singlestep should only override gdb breakpoints,
     * so that one could (gdb) singlestep into the guest kernel's
     * architectural breakpoint handler.
     */
    if (cpu->singlestep_enabled) {
        return false;
    }

    QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
        /*
         * If we have an exact pc match, trigger the breakpoint.
         * Otherwise, note matches within the page.
         */
        if (pc == bp->pc) {
            bool match_bp = false;

            if (bp->flags & BP_GDB) {
                match_bp = true;
            } else if (bp->flags & BP_CPU) {
#ifdef CONFIG_USER_ONLY
                g_assert_not_reached();
#else
                const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
                assert(tcg_ops->debug_check_breakpoint);
                match_bp = tcg_ops->debug_check_breakpoint(cpu);
#endif
            }

            if (match_bp) {
                cpu->exception_index = EXCP_DEBUG;
                return true;
            }
        } else if (((pc ^ bp->pc) & TARGET_PAGE_MASK) == 0) {
            match_page = true;
        }
    }

    /*
     * Within the same page as a breakpoint, single-step,
     * returning to helper_lookup_tb_ptr after each insn looking
     * for the actual breakpoint.
     *
     * TODO: Perhaps better to record all of the TBs associated
     * with a given virtual page that contains a breakpoint, and
     * then invalidate them when a new overlapping breakpoint is
     * set on the page.  Non-overlapping TBs would not be
     * invalidated, nor would any TB need to be invalidated as
     * breakpoints are removed.
     */
    if (match_page) {
        *cflags = (*cflags & ~CF_COUNT_MASK) | CF_NO_GOTO_TB | CF_BP_PAGE | 1;
    }
    return false;
}

static inline bool check_for_breakpoints(CPUState *cpu, vaddr pc,
                                         uint32_t *cflags)
{
    return unlikely(!QTAILQ_EMPTY(&cpu->breakpoints)) &&
        check_for_breakpoints_slow(cpu, pc, cflags);
}

/**
 * helper_lookup_tb_ptr: quick check for next tb
 * @env: current cpu state
 *
 * Look for an existing TB matching the current cpu state.
 * If found, return the code pointer.  If not found, return
 * the tcg epilogue so that we return into cpu_tb_exec.
 */
const void *HELPER(lookup_tb_ptr)(CPUArchState *env)
{
    CPUState *cpu = env_cpu(env);
    TranslationBlock *tb;

    /*
     * By definition we've just finished a TB, so I/O is OK.
     * Avoid the possibility of calling cpu_io_recompile() if
     * a page table walk triggered by tb_lookup() calling
     * probe_access_internal() happens to touch an MMIO device.
     * The next TB, if we chain to it, will clear the flag again.
     */
    cpu->neg.can_do_io = true;

    TCGTBCPUState s = xemu_get_tb_cpu_state(cpu);
    s.cflags = curr_cflags(cpu);

    if (check_for_breakpoints(cpu, s.pc, &s.cflags)) {
        cpu_loop_exit(cpu);
    }

#if defined(XBOX)
    if (unlikely(xemu_guestprof_on())) {
        int k = xemu_gp_kind_for_ra(
            (uintptr_t)__builtin_return_address(0));
        xemu_gp_kind_lookups[k]++;
        xemu_gp_cur_kind = k;
    }
#endif
    tb = tb_lookup(cpu, s);
#if defined(XBOX)
    xemu_gp_cur_kind = XEMU_GP_KIND_OTHER;
#endif
    if (tb == NULL) {
        return tcg_code_gen_epilogue;
    }

    if (qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC)) {
        log_cpu_exec(s.pc, cpu, tb);
    }

    return tb->tc.ptr;
}

#if defined(XBOX)
/*
 * The ret-memo fill path needs the TranslationBlock itself, not just
 * tc.ptr: exporting the lookup lets it memoize directly instead of
 * paying the tcg_tb_lookup() tc.ptr->tb g_tree reversal per fill.
 * Logging is the caller's concern (the ret helper falls back to
 * helper_lookup_tb_ptr whenever exec logging is on).
 */
TranslationBlock *xemu_lookup_tb(CPUState *cpu)
{
    cpu->neg.can_do_io = true;

    TCGTBCPUState s = xemu_get_tb_cpu_state(cpu);
    s.cflags = curr_cflags(cpu);

    if (check_for_breakpoints(cpu, s.pc, &s.cflags)) {
        cpu_loop_exit(cpu);
    }

    if (unlikely(xemu_guestprof_on())) {
        int k = xemu_gp_kind_for_ra(
            (uintptr_t)__builtin_return_address(0));
        xemu_gp_kind_lookups[k]++;
        xemu_gp_cur_kind = k;
    }
    TranslationBlock *tb = tb_lookup(cpu, s);
    xemu_gp_cur_kind = XEMU_GP_KIND_OTHER;
    return tb;
}
#endif


/* Return the current PC from CPU, which may be cached in TB. */
static vaddr log_pc(CPUState *cpu, const TranslationBlock *tb)
{
    if (tb_cflags(tb) & CF_PCREL) {
        return cpu->cc->get_pc(cpu);
    } else {
        return tb->pc;
    }
}

/* Execute a TB, and fix up the CPU state afterwards if necessary */
/*
 * Disable CFI checks.
 * TCG creates binary blobs at runtime, with the transformed code.
 * A TB is a blob of binary code, created at runtime and called with an
 * indirect function call. Since such function did not exist at compile time,
 * the CFI runtime has no way to verify its signature and would fail.
 * TCG is not considered a security-sensitive part of QEMU so this does not
 * affect the impact of CFI in environment with high security requirements
 */
static inline TranslationBlock * QEMU_DISABLE_CFI
cpu_tb_exec(CPUState *cpu, TranslationBlock *itb, int *tb_exit)
{
    uintptr_t ret;
    TranslationBlock *last_tb;
    const void *tb_ptr = itb->tc.ptr;

    if (qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC)) {
        log_cpu_exec(log_pc(cpu, itb), cpu, itb);
    }

    qemu_thread_jit_execute();
    ret = tcg_qemu_tb_exec(cpu_env(cpu), tb_ptr);
    cpu->neg.can_do_io = true;
    qemu_plugin_disable_mem_helpers(cpu);
    /*
     * TODO: Delay swapping back to the read-write region of the TB
     * until we actually need to modify the TB.  The read-only copy,
     * coming from the rx region, shares the same host TLB entry as
     * the code that executed the exit_tb opcode that arrived here.
     * If we insist on touching both the RX and the RW pages, we
     * double the host TLB pressure.
     */
    last_tb = tcg_splitwx_to_rw((void *)(ret & ~TB_EXIT_MASK));
    *tb_exit = ret & TB_EXIT_MASK;

    trace_exec_tb_exit(last_tb, *tb_exit);

    if (*tb_exit > TB_EXIT_IDX1) {
        /* We didn't start executing this TB (eg because the instruction
         * counter hit zero); we must restore the guest PC to the address
         * of the start of the TB.
         */
        CPUClass *cc = cpu->cc;
        const TCGCPUOps *tcg_ops = cc->tcg_ops;

        if (tcg_ops->synchronize_from_tb) {
            tcg_ops->synchronize_from_tb(cpu, last_tb);
        } else {
            tcg_debug_assert(!(tb_cflags(last_tb) & CF_PCREL));
            assert(cc->set_pc);
            cc->set_pc(cpu, last_tb->pc);
        }
        if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
            vaddr pc = log_pc(cpu, last_tb);
            if (qemu_log_in_addr_range(pc)) {
                qemu_log("Stopped execution of TB chain before %p [%016"
                         VADDR_PRIx "] %s\n",
                         last_tb->tc.ptr, pc, lookup_symbol(pc));
            }
        }
    }

    /*
     * If gdb single-step, and we haven't raised another exception,
     * raise a debug exception.  Single-step with another exception
     * is handled in cpu_handle_exception.
     */
    if (unlikely(cpu->singlestep_enabled) && cpu->exception_index == -1) {
        cpu->exception_index = EXCP_DEBUG;
        cpu_loop_exit(cpu);
    }

    return last_tb;
}


static void cpu_exec_enter(CPUState *cpu)
{
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;

    if (tcg_ops->cpu_exec_enter) {
        tcg_ops->cpu_exec_enter(cpu);
    }
}

static void cpu_exec_exit(CPUState *cpu)
{
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;

    if (tcg_ops->cpu_exec_exit) {
        tcg_ops->cpu_exec_exit(cpu);
    }
}

static void cpu_exec_longjmp_cleanup(CPUState *cpu)
{
    /* Non-buggy compilers preserve this; assert the correct value. */
    g_assert(cpu == current_cpu);

#ifdef CONFIG_USER_ONLY
    clear_helper_retaddr();
    if (have_mmap_lock()) {
        mmap_unlock();
    }
#else
    /*
     * For softmmu, a tlb_fill fault during translation will land here,
     * and we need to release any page locks held.  In system mode we
     * have one tcg_ctx per thread, so we know it was this cpu doing
     * the translation.
     *
     * Alternative 1: Install a cleanup to be called via an exception
     * handling safe longjmp.  It seems plausible that all our hosts
     * support such a thing.  We'd have to properly register unwind info
     * for the JIT for EH, rather that just for GDB.
     *
     * Alternative 2: Set and restore cpu->jmp_env in tb_gen_code to
     * capture the cpu_loop_exit longjmp, perform the cleanup, and
     * jump again to arrive here.
     */
    if (tcg_ctx->gen_tb) {
        tb_unlock_pages(tcg_ctx->gen_tb);
        tcg_ctx->gen_tb = NULL;
    }
#endif
    if (bql_locked()) {
        bql_unlock();
    }
    assert_no_pages_locked();
}

void cpu_exec_step_atomic(CPUState *cpu)
{
    TranslationBlock *tb;
    int tb_exit;

    if (sigsetjmp(cpu->jmp_env, 0) == 0) {
        start_exclusive();
        g_assert(cpu == current_cpu);
        g_assert(!cpu->running);
        cpu->running = true;

        TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
        s.cflags = curr_cflags(cpu);

        /* Execute in a serial context. */
        s.cflags &= ~CF_PARALLEL;
        /* After 1 insn, return and release the exclusive lock. */
        s.cflags |= CF_NO_GOTO_TB | CF_NO_GOTO_PTR | 1;
        /*
         * No need to check_for_breakpoints here.
         * We only arrive in cpu_exec_step_atomic after beginning execution
         * of an insn that includes an atomic operation we can't handle.
         * Any breakpoint for this insn will have been recognized earlier.
         */

        tb = tb_lookup(cpu, s);
        if (tb == NULL) {
            mmap_lock();
            tb = tb_gen_code(cpu, s);
            mmap_unlock();
        }

        cpu_exec_enter(cpu);
        /* execute the generated code */
        trace_exec_tb(tb, s.pc);
        cpu_tb_exec(cpu, tb, &tb_exit);
        cpu_exec_exit(cpu);
    } else {
        cpu_exec_longjmp_cleanup(cpu);
    }

    /*
     * As we start the exclusive region before codegen we must still
     * be in the region if we longjump out of either the codegen or
     * the execution.
     */
    g_assert(cpu_in_exclusive_context(cpu));
    cpu->running = false;
    end_exclusive();
}

void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{
    /*
     * Get the rx view of the structure, from which we find the
     * executable code address, and tb_target_set_jmp_target can
     * produce a pc-relative displacement to jmp_target_addr[n].
     */
    const TranslationBlock *c_tb = tcg_splitwx_to_rx(tb);
    uintptr_t offset = tb->jmp_insn_offset[n];
    uintptr_t jmp_rx = (uintptr_t)tb->tc.ptr + offset;
    uintptr_t jmp_rw = jmp_rx - tcg_splitwx_diff;

    tb->jmp_target_addr[n] = addr;
    tb_target_set_jmp_target(c_tb, n, jmp_rx, jmp_rw);
}

static inline void tb_add_jump(TranslationBlock *tb, int n,
                               TranslationBlock *tb_next)
{
    uintptr_t old;

    qemu_thread_jit_write();
    assert(n < ARRAY_SIZE(tb->jmp_list_next));
    qemu_spin_lock(&tb_next->jmp_lock);

    /* make sure the destination TB is valid */
    if (tb_next->cflags & CF_INVALID) {
        goto out_unlock_next;
    }
    /* Atomically claim the jump destination slot only if it was NULL */
    old = qatomic_cmpxchg(&tb->jmp_dest[n], (uintptr_t)NULL,
                          (uintptr_t)tb_next);
    if (old) {
        goto out_unlock_next;
    }

    /* patch the native jump address */
    tb_set_jmp_target(tb, n, (uintptr_t)tb_next->tc.ptr);

    /* add in TB jmp list */
    tb->jmp_list_next[n] = tb_next->jmp_list_head;
    tb_next->jmp_list_head = (uintptr_t)tb | n;

    qemu_spin_unlock(&tb_next->jmp_lock);

#if defined(XBOX)
    /*
     * Cross-page direct link (permitted at translate time by
     * xemu_xpage_allow): register the destination so the full-TLB-flush
     * backstop can sever exactly these chains instead of flushing the
     * whole translation cache (tb-maint.c xpage link registry). Under
     * upstream same-page-only chaining the condition is never true.
     * Registered strictly after dropping jmp_lock (registry lock order).
     */
    {
        tb_page_addr_t d0 = tb_page_addr0(tb_next);
        bool phys_cross = d0 != tb_page_addr0(tb) &&
            (tb_page_addr1(tb) == (tb_page_addr_t)-1 ||
             d0 != tb_page_addr1(tb));
        bool emitter = tb->xemu_xpage_reg & XEMU_XPAGE_TB_CROSS_EMITTER;
        if (emitter || phys_cross) {
            xemu_xpage_link_note_cross(tb_next, phys_cross && !emitter);
        }
    }
#endif

    qemu_log_mask(CPU_LOG_EXEC, "Linking TBs %p index %d -> %p\n",
                  tb->tc.ptr, n, tb_next->tc.ptr);
    return;

 out_unlock_next:
    qemu_spin_unlock(&tb_next->jmp_lock);
}

static inline bool cpu_handle_halt(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    if (cpu->halted) {
        const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
        bool leave_halt = tcg_ops->cpu_exec_halt(cpu);

        if (!leave_halt) {
            return true;
        }

        cpu->halted = 0;
    }
#endif /* !CONFIG_USER_ONLY */

    return false;
}

static inline void cpu_handle_debug_exception(CPUState *cpu)
{
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
    CPUWatchpoint *wp;

    if (!cpu->watchpoint_hit) {
        QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }

    if (tcg_ops->debug_excp_handler) {
        tcg_ops->debug_excp_handler(cpu);
    }
}

static inline bool cpu_handle_exception(CPUState *cpu, int *ret)
{
    if (cpu->exception_index < 0) {
#ifndef CONFIG_USER_ONLY
        if (replay_has_exception()
            && cpu->neg.icount_decr.u16.low + cpu->icount_extra == 0) {
            /* Execute just one insn to trigger exception pending in the log */
            cpu->cflags_next_tb = (curr_cflags(cpu) & ~CF_USE_ICOUNT)
                | CF_NOIRQ | 1;
        }
#endif
        return false;
    }

    if (cpu->exception_index >= EXCP_INTERRUPT) {
        /* exit request from the cpu execution loop */
        *ret = cpu->exception_index;
        if (*ret == EXCP_DEBUG) {
            cpu_handle_debug_exception(cpu);
        }
        cpu->exception_index = -1;
        return true;
    }

#if defined(CONFIG_USER_ONLY)
    /*
     * If user mode only, we simulate a fake exception which will be
     * handled outside the cpu execution loop.
     */
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
    if (tcg_ops->fake_user_interrupt) {
        tcg_ops->fake_user_interrupt(cpu);
    }
    *ret = cpu->exception_index;
    cpu->exception_index = -1;
    return true;
#else
    if (replay_exception()) {
        const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;

        bql_lock();
        tcg_ops->do_interrupt(cpu);
        bql_unlock();
        cpu->exception_index = -1;

        if (unlikely(cpu->singlestep_enabled)) {
            /*
             * After processing the exception, ensure an EXCP_DEBUG is
             * raised when single-stepping so that GDB doesn't miss the
             * next instruction.
             */
            *ret = EXCP_DEBUG;
            cpu_handle_debug_exception(cpu);
            return true;
        }
    } else if (!replay_has_interrupt()) {
        /* give a chance to iothread in replay mode */
        *ret = EXCP_INTERRUPT;
        return true;
    }
#endif

    return false;
}

void tcg_kick_vcpu_thread(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    /*
     * Ensure cpu_exec will see the reason why the exit request was set.
     * FIXME: this is not always needed.  Other accelerators instead
     * read interrupt_request and set exit_request on demand from the
     * CPU thread; see kvm_arch_pre_run() for example.
     */
    qatomic_store_release(&cpu->exit_request, true);
#endif

    /* Ensure cpu_exec will see the exit request after TCG has exited.  */
    qatomic_store_release(&cpu->neg.icount_decr.u16.high, -1);
}

static inline bool icount_exit_request(CPUState *cpu)
{
    if (!icount_enabled()) {
        return false;
    }
    if (cpu->cflags_next_tb != -1 && !(cpu->cflags_next_tb & CF_USE_ICOUNT)) {
        return false;
    }
    return cpu->neg.icount_decr.u16.low + cpu->icount_extra == 0;
}

static inline bool cpu_handle_interrupt(CPUState *cpu,
                                        TranslationBlock **last_tb)
{
    /*
     * If we have requested custom cflags with CF_NOIRQ we should
     * skip checking here. Any pending interrupts will get picked up
     * by the next TB we execute under normal cflags.
     */
    if (cpu->cflags_next_tb != -1 && cpu->cflags_next_tb & CF_NOIRQ) {
        return false;
    }

    /* Clear the interrupt flag now since we're processing
     * cpu->interrupt_request and cpu->exit_request.
     * Ensure zeroing happens before reading cpu->exit_request or
     * cpu->interrupt_request (see also store-release in
     * tcg_kick_vcpu_thread())
     */
    qatomic_set_mb(&cpu->neg.icount_decr.u16.high, 0);

#ifdef CONFIG_USER_ONLY
    assert(!cpu_test_interrupt(cpu, ~0));
#else
    if (unlikely(cpu_test_interrupt(cpu, ~0))) {
        bql_lock();
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_DEBUG)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_DEBUG);
            cpu->exception_index = EXCP_DEBUG;
            bql_unlock();
            return true;
        }
        if (replay_mode == REPLAY_MODE_PLAY && !replay_has_interrupt()) {
            /* Do nothing */
        } else if (cpu_test_interrupt(cpu, CPU_INTERRUPT_HALT)) {
            replay_interrupt();
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_HALT);
            cpu->halted = 1;
            cpu->exception_index = EXCP_HLT;
            bql_unlock();
            return true;
        } else {
            const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
            int interrupt_request = cpu->interrupt_request;

            if (cpu_test_interrupt(cpu, CPU_INTERRUPT_RESET)) {
                replay_interrupt();
                tcg_ops->cpu_exec_reset(cpu);
                bql_unlock();
                return true;
            }

            if (unlikely(cpu->singlestep_enabled & SSTEP_NOIRQ)) {
                /* Mask out external interrupts for this step. */
                interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
            }

            /*
             * The target hook has 3 exit conditions:
             * False when the interrupt isn't processed,
             * True when it is, and we should restart on a new TB,
             * and via longjmp via cpu_loop_exit.
             */
            if (tcg_ops->cpu_exec_interrupt(cpu, interrupt_request)) {
                if (!tcg_ops->need_replay_interrupt ||
                    tcg_ops->need_replay_interrupt(interrupt_request)) {
                    replay_interrupt();
                }
                /*
                 * After processing the interrupt, ensure an EXCP_DEBUG is
                 * raised when single-stepping so that GDB doesn't miss the
                 * next instruction.
                 */
                if (unlikely(cpu->singlestep_enabled)) {
                    cpu->exception_index = EXCP_DEBUG;
                    bql_unlock();
                    return true;
                }
                cpu->exception_index = -1;
                *last_tb = NULL;
            }
        }
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_EXITTB)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_EXITTB);
            /* ensure that no TB jump will be modified as
               the program flow was changed */
            *last_tb = NULL;
        }

        /* If we exit via cpu_loop_exit/longjmp it is reset in cpu_exec */
        bql_unlock();
    }
#endif /* !CONFIG_USER_ONLY */

    /*
     * Finally, check if we need to exit to the main loop.
     * The corresponding store-release is in cpu_exit.
     */
    if (unlikely(qatomic_load_acquire(&cpu->exit_request)) || icount_exit_request(cpu)) {
        if (cpu->exception_index == -1) {
            cpu->exception_index = EXCP_INTERRUPT;
        }
        return true;
    }

    return false;
}

static inline void cpu_loop_exec_tb(CPUState *cpu, TranslationBlock *tb,
                                    vaddr pc, TranslationBlock **last_tb,
                                    int *tb_exit)
{
#if defined(XBOX)
    /*
     * XEMU_CCOP_CENSUS: count (predecessor tail, successor head) flag-
     * liveness pairs. The loop's own last_tb cannot serve here: under the
     * census cflags every exit is exit_tb(NULL), so cpu_tb_exec returns
     * NULL and last_tb never survives a transition. Track the previously
     * ENTERED TB instead — equivalent while chaining is off, since each
     * entered TB runs to its own exit. Reset across tb_flush generations
     * (stale pointers); interrupt/exception boundaries smear a sub-percent
     * of pairs and are unfusable by a cross-block optimizer anyway.
     */
    if (unlikely(xemu_ccop_census_on())) {
        static TranslationBlock *census_prev_tb;
        static unsigned census_prev_flush;
        unsigned fc = qatomic_read(&tb_ctx.tb_flush_count);

        if (fc != census_prev_flush) {
            census_prev_flush = fc;
            census_prev_tb = NULL;
        }
        if (census_prev_tb) {
            unsigned head = (tb->xemu_ccop >> XEMU_CCOP_HEAD_SHIFT) & 0x3;
            xemu_ccop_pairs[census_prev_tb->xemu_ccop & XEMU_CCOP_TAIL_MASK]
                           [head]++;
            if (census_prev_tb->xemu_ccop & XEMU_CCOP_REGION_CAND) {
                xemu_region_cand_pairs[head]++;
            }
        } else {
            xemu_ccop_pairs_nolast++;
        }
        census_prev_tb = tb;
    }
#endif
    trace_exec_tb(tb, pc);
    tb = cpu_tb_exec(cpu, tb, tb_exit);
    if (*tb_exit != TB_EXIT_REQUESTED) {
        *last_tb = tb;
        return;
    }

    *last_tb = NULL;
    if (cpu_loop_exit_requested(cpu)) {
        /* Something asked us to stop executing chained TBs; just
         * continue round the main loop. Whatever requested the exit
         * will also have set something else (eg exit_request or
         * interrupt_request) which will be handled by
         * cpu_handle_interrupt.  cpu_handle_interrupt will also
         * clear cpu->icount_decr.u16.high.
         */
        return;
    }

    /* Instruction counter expired.  */
    assert(icount_enabled());
#ifndef CONFIG_USER_ONLY
    /* Ensure global icount has gone forward */
    icount_update(cpu);
    /* Refill decrementer and continue execution.  */
    int32_t insns_left = MIN(0xffff, cpu->icount_budget);
    cpu->neg.icount_decr.u16.low = insns_left;
    cpu->icount_extra = cpu->icount_budget - insns_left;

    /*
     * If the next tb has more instructions than we have left to
     * execute we need to ensure we find/generate a TB with exactly
     * insns_left instructions in it.
     */
    if (insns_left > 0 && insns_left < tb->icount)  {
        assert(insns_left <= CF_COUNT_MASK);
        assert(cpu->icount_extra == 0);
        cpu->cflags_next_tb = (tb->cflags & ~CF_COUNT_MASK) | insns_left;
    }
#endif
}

/* main execution loop */

static int __attribute__((noinline))
cpu_exec_loop(CPUState *cpu, SyncClocks *sc)
{
    int ret;

    /* if an exception is pending, we execute it here */
    while (!cpu_handle_exception(cpu, &ret)) {
        TranslationBlock *last_tb = NULL;
        int tb_exit = 0;

        while (!cpu_handle_interrupt(cpu, &last_tb)) {
            TranslationBlock *tb;
            TCGTBCPUState s = xemu_get_tb_cpu_state(cpu);
            s.cflags = cpu->cflags_next_tb;

            /*
             * When requested, use an exact setting for cflags for the next
             * execution.  This is used for icount, precise smc, and stop-
             * after-access watchpoints.  Since this request should never
             * have CF_INVALID set, -1 is a convenient invalid value that
             * does not require tcg headers for cpu_common_reset.
             */
            if (s.cflags == -1) {
                s.cflags = curr_cflags(cpu);
            } else {
                cpu->cflags_next_tb = -1;
            }

            if (check_for_breakpoints(cpu, s.pc, &s.cflags)) {
                break;
            }

            tb = tb_lookup(cpu, s);
            if (tb == NULL) {
                CPUJumpCache *jc;
                uint32_t h;

                mmap_lock();
                tb = tb_gen_code(cpu, s);
                mmap_unlock();

                /*
                 * We add the TB in the virtual pc hash table
                 * for the fast lookup
                 */
                h = tb_jmp_cache_hash_func(s.pc);
                jc = cpu->tb_jmp_cache;
                jc->array[h].pc = s.pc;
                qatomic_set(&jc->array[h].tb, tb);
            }

#ifndef CONFIG_USER_ONLY
            /*
             * We don't take care of direct jumps when address mapping
             * changes in system emulation.  So it's not safe to make a
             * direct jump to a TB spanning two pages because the mapping
             * for the second page can change.
             */
            if (tb_page_addr1(tb) != -1) {
#if defined(XBOX)
                if (unlikely(xemu_inv_prof_on())) {
                    xemu_inv_span_nochain++;
                }
#endif
                last_tb = NULL;
            }
#endif
            /* See if we can patch the calling TB. */
            if (last_tb) {
                tb_add_jump(last_tb, tb_exit, tb);
            }

            cpu_loop_exec_tb(cpu, tb, s.pc, &last_tb, &tb_exit);

            /* Try to align the host and virtual clocks
               if the guest is in advance */
            align_clocks(sc, cpu);
        }
    }
    return ret;
}

static int cpu_exec_setjmp(CPUState *cpu, SyncClocks *sc)
{
    /* Prepare setjmp context for exception handling. */
    if (unlikely(sigsetjmp(cpu->jmp_env, 0) != 0)) {
        cpu_exec_longjmp_cleanup(cpu);
    }

    return cpu_exec_loop(cpu, sc);
}

int cpu_exec(CPUState *cpu)
{
    int ret;
    SyncClocks sc = { 0 };

    /* replay_interrupt may need current_cpu */
    current_cpu = cpu;

#if defined(XBOX) && defined(XEMU_GUEST_PROF_SAMPLER)
    xemu_guestprof_register_vcpu();
#endif

    if (cpu_handle_halt(cpu)) {
        return EXCP_HALTED;
    }

    RCU_READ_LOCK_GUARD();
    cpu_exec_enter(cpu);

    /*
     * Calculate difference between guest clock and host clock.
     * This delay includes the delay of the last cycle, so
     * what we have to do is sleep until it is 0. As for the
     * advance/delay we gain here, we try to fix it next time.
     */
    init_delay_params(&sc, cpu);

    ret = cpu_exec_setjmp(cpu, &sc);

    cpu_exec_exit(cpu);
    return ret;
}

bool tcg_exec_realizefn(CPUState *cpu, Error **errp)
{
    static bool tcg_target_initialized;

    if (!tcg_target_initialized) {
        /* Check mandatory TCGCPUOps handlers */
        const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
#ifndef CONFIG_USER_ONLY
        assert(tcg_ops->cpu_exec_halt);
        assert(tcg_ops->cpu_exec_interrupt);
        assert(tcg_ops->cpu_exec_reset);
        assert(tcg_ops->pointer_wrap);
#endif /* !CONFIG_USER_ONLY */
        assert(tcg_ops->translate_code);
#if defined(XBOX) && !defined(CONFIG_USER_ONLY)
        /* The hot lookup sites bypass the slot; they must agree with it. */
        assert(tcg_ops->get_tb_cpu_state == x86_get_tb_cpu_state);
#else
        assert(tcg_ops->get_tb_cpu_state);
#endif
        assert(tcg_ops->mmu_index);
        tcg_ops->initialize();
        tcg_target_initialized = true;
    }

    cpu->tb_jmp_cache = g_new0(CPUJumpCache, 1);
#if defined(XBOX)
    if (xemu_tbprof_on()) {
        atexit(xemu_tbprof_dump);
    }
    if (xemu_guestprof_on()) {
        xemu_guestprof_start();
    }
#endif
    tlb_init(cpu);
#ifndef CONFIG_USER_ONLY
    tcg_iommu_init_notifier_list(cpu);
#endif /* !CONFIG_USER_ONLY */
    /* qemu_plugin_vcpu_init_hook delayed until cpu_index assigned. */

    return true;
}

/* undo the initializations in reverse order */
void tcg_exec_unrealizefn(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    tcg_iommu_free_notifier_list(cpu);
#endif /* !CONFIG_USER_ONLY */

    tlb_destroy(cpu);
    g_free_rcu(cpu->tb_jmp_cache, rcu);
}
