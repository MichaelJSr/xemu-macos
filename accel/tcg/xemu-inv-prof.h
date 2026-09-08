/*
 * xemu: SMC / TB-invalidation-churn profiling (XEMU_INV_PROF=1).
 *
 * Default off, zero cost when unset: every counter site is gated by
 * xemu_inv_prof_on(), which latches a single getenv (XEMU_TB_PROF /
 * XEMU_MMIO_PROF model). Counters are plain unsynchronized uint64: the
 * xbox machine runs exactly one vCPU thread, which is the only writer of
 * every path counted here, and the exit dump runs from atexit() after the
 * thread has stopped. Declarations are unconditional (harmless on any
 * target); the definitions and all counting sites are #if defined(XBOX).
 *
 * Counter groups (see docs / roadmap item "SMC / TB-invalidation churn"):
 *   (a) invalidations: total + per-source attribution.
 *   (b) would-the-range-check-have-saved-it: inside the XBOX whole-page
 *       invalidation, also evaluate the upstream byte-range overlap test in
 *       counting mode (no behavior change). A TB invalidated whose range
 *       does not overlap the written range is a false (over-)invalidation.
 *   (c) recycling: inv_htable recycle attempts / hits / true-SMC / cold.
 *   (d) census (cheap translate-time emission counters): FPU flcr check
 *       emitted-vs-compile-skipped; goto_tb direct chain vs inline
 *       jump-cache probe vs ret-memo exits.
 */
#ifndef XEMU_INV_PROF_H
#define XEMU_INV_PROF_H

/* Invalidation source buckets (index into xemu_inv_by_src). */
enum {
    XEMU_INV_SRC_OTHER = 0,   /* unattributed / setup */
    XEMU_INV_SRC_NOTDIRTY,    /* guest data/SMC write to a code page (notdirty) */
    XEMU_INV_SRC_EXPLICIT,    /* tb_invalidate_phys_range: fault retval / DMA / ROM */
    XEMU_INV_SRC_SINGLE,      /* public tb_phys_invalidate: single TB (fault) */
    XEMU_INV_SRC_MAX
};

/* Latched enable check; also arms the atexit dump on first true. */
bool xemu_inv_prof_on(void);

/*
 * XEMU_INV_TIMING=1 (default off): cycle-level timing of notdirty_write, split
 * into {body-total, invalidation+scan+recycle}. Gated separately from
 * XEMU_INV_PROF so a pure-count run is never perturbed by the timer reads.
 * Sizes the sub-page dirty-tracking arms (docs/subpage-gate0-prediction.md): the ratio of
 * the invalidation segment to the rest of the trap body decides whether a
 * scheme that multiplies the trap count (helper-top consult) can ever net
 * positive against the whole-page-invalidation epoch. Counts are load-immune;
 * these timings are order-of-magnitude (other agents compile concurrently).
 */
bool xemu_inv_timing_on(void);

/*
 * Monotonic tick source for the timing split. On Apple Silicon the virtual
 * counter (cntvct_el0, 24 MHz => ~41.6 ns/tick) is a couple ns to read; the
 * isb keeps the read from being reordered around the measured work. Per-call
 * quantization averages out over the ~10^5..10^6 traps per window. Apple
 * non-aarch64 uses the RAW uptime clock (ns units); every other x86 host
 * (MSYS2/MinGW Windows, x86_64 Linux) uses the TSC, which plays the cntvct
 * role. Any other host has no source and reports zeros. Convert with
 * xemu_inv_tick_ns().
 */
static inline uint64_t xemu_inv_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#elif defined(__x86_64__) || defined(__i386__)
    /*
     * rdtsc is a gcc/clang builtin on every x86 target (no <x86intrin.h>
     * needed, so this header stays include-free). Invariant TSC on every
     * host this fork targets; the lfence keeps the read from being hoisted
     * across the measured work, mirroring the aarch64 isb above.
     */
    __asm__ volatile("lfence" ::: "memory");
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

/* ns per tick of xemu_inv_ticks() (1.0 when the source already counts ns). */
static inline double xemu_inv_tick_ns(void)
{
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? 1.0e9 / (double)f : 0.0;
#elif !defined(__APPLE__) && (defined(__x86_64__) || defined(__i386__))
    /*
     * The TSC has no architectural frequency register, so calibrate ns/tick
     * once against the monotonic clock (QueryPerformanceCounter-backed on
     * Windows) with a ~10 ms spin, then cache it. Only ever reached from the
     * XEMU_INV_TIMING exit dump, so the spin is paid once, off any hot path.
     */
    static double ns_per_tick;
    if (ns_per_tick == 0.0) {
        int64_t t0_us = g_get_monotonic_time();
        uint64_t c0 = xemu_inv_ticks();
        int64_t t1_us = t0_us;
        uint64_t c1;
        while (t1_us - t0_us < 10000) {
            t1_us = g_get_monotonic_time();
        }
        c1 = xemu_inv_ticks();
        ns_per_tick = (c1 > c0) ?
            ((double)(t1_us - t0_us) * 1000.0) / (double)(c1 - c0) : 0.0;
    }
    return ns_per_tick;
#else
    return 1.0;
#endif
}

/*
 * XEMU_TB_RANGE_INV=1 (default off): re-apply the exact upstream byte-range
 * overlap filter inside the XBOX whole-page invalidation, so only TBs whose
 * bytes actually overlap the written range are invalidated. Correctness-safe
 * (a TB whose bytes were not written cannot have been modified); runtime-gated
 * so it can be A/B'd against the default whole-page behavior on one binary.
 */
bool xemu_tb_range_inv_on(void);

/*
 * Sub-page dirty tracking (docs/subpage-gate0-prediction.md arm (a)). XEMU_SUBPAGE_DIRTY=1
 * fast-skips whole-page invalidation for a data store that misses every code
 * sub-block; XEMU_SUBPAGE_REFUTE=1 runs that skip decision against a
 * ground-truth TB-overlap scan and counts violations (design falsified if > 0).
 * Both default off, zero cost when unset. Implemented entirely inside
 * tb-maint.c's XBOX paths; declared here so the shared INV_PROF dump can print
 * their counters.
 */
bool xemu_subpage_dirty_on(void);
bool xemu_subpage_refute_on(void);

/* Current invalidation source, set by the entry points, read in
 * do_tb_phys_invalidate. Single vCPU thread, so a plain global is safe. */
extern int xemu_inv_cur_src;

/* (a) invalidations */
extern uint64_t xemu_inv_total;
extern uint64_t xemu_inv_by_src[XEMU_INV_SRC_MAX];

/* (b) range-check-would-have-saved-it */
extern uint64_t xemu_inv_range_evals;   /* overlap tests evaluated (cost proxy) */
extern uint64_t xemu_inv_false_share;    /* invalidations that did NOT overlap */

/* (b') trap-frequency (the tlb_unprotect_code tradeoff of the range filter) */
extern uint64_t xemu_inv_traps;          /* notdirty writes reaching a code page */
extern uint64_t xemu_inv_unprotect;      /* pages emptied of TBs -> unprotected */

/*
 * (b'') notdirty_write body timing (XEMU_INV_TIMING). Raw ticks accumulated
 * over every trap; convert with xemu_inv_tick_ns(). nd_calls is every
 * notdirty_write; nd_calls_inval is the subset that ran the invalidation
 * (page's DIRTY_MEMORY_CODE was clear). ticks_total brackets the whole body;
 * ticks_inval brackets tb_invalidate_phys_range_fast; (total - inval) is the
 * preamble+tail the helper-top consult would still pay per trap.
 */
extern uint64_t xemu_nd_calls;
extern uint64_t xemu_nd_calls_inval;
extern uint64_t xemu_nd_ticks_total;
extern uint64_t xemu_nd_ticks_inval;

/* (c) recycling */
extern uint64_t xemu_inv_recycle_attempts;  /* inv_tb_htable_lookup calls */
extern uint64_t xemu_inv_recycle_hits;      /* recycled (identical bytes) */
extern uint64_t xemu_inv_recycle_true_smc;  /* same pc/flags in inv_htable, bytes differ */
extern uint64_t xemu_inv_recycle_cold;      /* no inv_htable entry (fresh/cold pc) */

/* (d) census — translate-time emission counts */
extern uint64_t xemu_inv_flcr_emitted;      /* gen_flcr emitted a runtime FPCR check */
extern uint64_t xemu_inv_flcr_skip;         /* gen_flcr compile-skipped (flcr_set true) */
extern uint64_t xemu_inv_gototb_emitted;    /* same-page direct goto_tb chain exits */
extern uint64_t xemu_inv_jcprobe_emitted;   /* inline jump-cache probe (indirect/xpage) */
extern uint64_t xemu_inv_retmemo_emitted;   /* inline ret-memo exits */

/* (d') runtime count of direct-chain refusals because the DEST TB spans
 * two guest pages (cpu-exec.c tb_page_addr1 guard) — sizes the xpage
 * "page-spanning targets decline to chain" coverage gap. */
extern uint64_t xemu_inv_span_nochain;

/*
 * XEMU_CCOP_CENSUS=1 (default off, separate latch): runtime-weighted census
 * of cc-flag liveness across TB boundaries, sizing the cross-block
 * cc_op-elimination half of the superblock roadmap item. When on,
 * curr_cflags() forces CF_NO_GOTO_TB | CF_NO_GOTO_PTR so every TB
 * transition returns to the exec loop (goto_tb chains, the inline jump
 * cache, and the ret-memo all honor those bits), where
 * (predecessor tail class, successor head class) pairs are counted.
 * This distorts speed, never the executed instruction stream — the pair
 * distribution is the one a chained run would produce. Same counter
 * discipline as above: single vCPU writer, atexit dump.
 */
bool xemu_ccop_census_on(void);

/* Classes stored in tb->xemu_ccop at translate time (i386 tb_stop). */
#define XEMU_CCOP_TAIL_MASK   0x3
#define XEMU_CCOP_HEAD_SHIFT  2
#define XEMU_CCOP_TAIL_DYN    0   /* pass-through: TB never produced flags */
#define XEMU_CCOP_TAIL_EFLAGS 1   /* flags concrete in cc_src (no lazy compute) */
#define XEMU_CCOP_TAIL_LAZY   2   /* lazy op pending: cc_op+operands spilled */
#define XEMU_CCOP_HEAD_NONE   0   /* TB never touches flags */
#define XEMU_CCOP_HEAD_KILL   1   /* first flag event overwrites without reading */
#define XEMU_CCOP_HEAD_USE    2   /* first flag event consumes inherited state */

extern uint64_t xemu_ccop_tb_tail[3];   /* translate-time static mix */
extern uint64_t xemu_ccop_tb_head[3];
extern uint64_t xemu_ccop_pairs[3][3];  /* [pred tail][succ head], runtime */
extern uint64_t xemu_ccop_pairs_nolast; /* transition after interrupt/exception */

/*
 * Region-formation candidate census (2026-07-18): bit 4 of tb->xemu_ccop
 * marks a TB whose final exit is a FORWARD conditional with target within
 * XEMU_CCOP_REGION_WINDOW bytes — the diamond-former's candidate shape.
 * Runtime pairs split by the executed successor's head class; the
 * campaign's pre-registered kill gate reads cand[KILL]/total (proxy for
 * "flags dead across the join" — the executed edge's head stands in for
 * join liveness; see docs/roadmap.md item 1).
 */
#define XEMU_CCOP_REGION_CAND  (1 << 4)
#define XEMU_CCOP_REGION_WINDOW 64
extern uint64_t xemu_region_cand_pairs[3];  /* [succ head], runtime */

#endif /* XEMU_INV_PROF_H */
