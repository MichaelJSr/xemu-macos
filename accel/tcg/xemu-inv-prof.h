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
 * XEMU_TB_RANGE_INV=1 (default off): re-apply the exact upstream byte-range
 * overlap filter inside the XBOX whole-page invalidation, so only TBs whose
 * bytes actually overlap the written range are invalidated. Correctness-safe
 * (a TB whose bytes were not written cannot have been modified); runtime-gated
 * so it can be A/B'd against the default whole-page behavior on one binary.
 */
bool xemu_tb_range_inv_on(void);

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

#endif /* XEMU_INV_PROF_H */
