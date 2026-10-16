/*
 * xemu: x87/SSE translator census and exception-pointer deferral (fork
 * optimization audit 2026-08-04, section 1.4).
 *
 * Header-only and private to target/i386/tcg/translate.c: that file includes
 * emit.c.inc, so file-static counters and latches reach every census site
 * without a new build-system entry.
 *
 * Counter discipline follows accel/tcg/xemu-inv-prof.h: plain unsynchronized
 * uint64 written only by the single xbox vCPU thread, dumped from atexit()
 * after that thread has stopped.
 *
 * Three mechanisms live here:
 *
 *   (1) XEMU_X87_CENSUS=1   Gate-0 census. Counts the per-instruction
 *       FIP/FDP store pairs against the deferred updates that replace them,
 *       measures mean x87 run length, and counts SSE scalar compares by
 *       operand width.
 *   (2) Exception-pointer deferral: FIP/FCS/FDP/FDS get one update per
 *       contiguous x87 run. A mid-block exit lands a pending FIP/FCS from
 *       insn_start word 2 (xemu_x87_restore_fip). FDP is a runtime value,
 *       so under icount, where a mid-block MMIO access is routine, a
 *       deferred FDP is stored by the slow path of every guest access
 *       while it is pending (write-through blocks, xemu_x87_defer_fdp;
 *       per-insn elsewhere), and FDS is stored only when its segment
 *       changes.
 *   (3) XEMU_X87_REFUTE=1 puts (2) in shadow mode: the legacy stores all
 *       still happen, the deferral is computed anyway, and every point where
 *       the deferred state could be observed is checked at runtime (every
 *       insn boundary and every mid-block exit).
 *       Promotion gate is zero violations over the campaign soak.
 *       It also checks the write-through path's deferred stores
 *       (translate.c x87_wt_pend), which it runs for real: each deferred
 *       slot's env image is overwritten with a poison value no conversion
 *       produces, so a raw read of a pending slot, a flush that misses one
 *       or a restore that lands the wrong set leaves it where the checks
 *       (insn boundaries, flushes, restores) and the RAM hash see it; and
 *       every guest access with stores to land copies their doubles in
 *       line, for the restore to check the shadows its slow path stored.
 *       And it checks TOP-keyed translation (translate.c x87_top): the
 *       static TOP against env at every insn boundary (top-key-*).
 *
 * insn_start word 2 carries both deferrals: bits 0-11 the FIP distance,
 * bits 12-19 the pending x87 stores by shadow slot, bits 20-22 how far
 * shadow slot 0 sits from fpstt, and the deferred stack bookkeeping
 * (translate.c x87_wt_land): bits 23-25 how far fpstt moves, bits 26-33
 * which tag bytes land (by slot, from the same base as the shadows) and
 * bits 34-41 their values. Bit 42, XEMU_X87_REFUTE only: the insn started
 * with a deferred FDP, which the restore checks.
 */
#ifndef TARGET_I386_XEMU_X87_H
#define TARGET_I386_XEMU_X87_H

#if defined(XBOX)

#define XEMU_X87_W2_FIP_MASK    0xfffu
#define XEMU_X87_W2_PEND_SHIFT  12
#define XEMU_X87_W2_TOP_SHIFT   20
#define XEMU_X87_W2_MOVE_SHIFT  23
#define XEMU_X87_W2_TAGM_SHIFT  26
#define XEMU_X87_W2_TAGV_SHIFT  34
#define XEMU_X87_W2_FDP         (1ull << 42)
/* The stack fields, cleared when a flush in the insn lands them early. */
#define XEMU_X87_W2_STACK_MASK  ((7ull << XEMU_X87_W2_MOVE_SHIFT) | \
                                 (0xffull << XEMU_X87_W2_TAGM_SHIFT) | \
                                 (0xffull << XEMU_X87_W2_TAGV_SHIFT))

/* (1) census -------------------------------------------------------------- */
static uint64_t xemu_x87_fip_legacy;     /* per-insn FIP/FCS store pairs */
static uint64_t xemu_x87_fdp_legacy;     /* per-insn FDP/FDS store pairs */
static uint64_t xemu_x87_ptr_flushes;    /* deferred exc-pointer updates */
static uint64_t xemu_x87_runs;           /* x87 runs closed by a flush */
static uint64_t xemu_x87_run_insns;      /* x87 insns inside those runs */
static uint64_t xemu_sse_comi_ss;        /* (U)COMISS -- 32-bit scalar form */
static uint64_t xemu_sse_comi_sd;        /* (U)COMISD -- 64-bit scalar form */
static uint64_t xemu_x87_wt_writes;      /* write-through results (set_st) */
static uint64_t xemu_x87_wt_st80f;       /* ... st80f actually executed */
static uint64_t xemu_x87_wt_landed;      /* deferred stores landed by restores */
static uint64_t xemu_x87_wt_stack_landed; /* ... deferred fpstt/tags */
static uint64_t xemu_x87_wt_stack_early; /* fpstt/tags landed early in an
                                          * insn (x87_wt_land_early) */
static uint64_t xemu_x87_wt_cmp_inline;  /* fucom*, fcomi* on the doubles */
static uint64_t xemu_x87_wt_cmp_helper;  /* ... left to the helper */
static uint64_t xemu_x87_wt_fcmov_inline; /* fcmov as a select */
static uint64_t xemu_x87_wt_fcmov_branch; /* ... as a branch */

/* (3) refuter ------------------------------------------------------------- */
static uint64_t xemu_x87_ptr_checks;     /* deferred-vs-legacy comparisons */
static uint64_t xemu_x87_ptr_viol;       /* deferred value != legacy value */
static uint64_t xemu_x87_fip_rt_pend;    /* 1 while a deferred FIP is unlanded */
static uint64_t xemu_x87_rst_checks;     /* mid-block exits checked */
static uint64_t xemu_x87_rst_pend;       /* ... of which with word 2 non-zero */
static uint64_t xemu_x87_bnd_checks;     /* insn boundaries checked */
static uint64_t xemu_x87_bnd_pend;       /* ... with a deferred FIP pending */
static uint64_t xemu_x87_rst_viol;       /* restorable FIP/FCS != legacy */
static uint64_t xemu_x87_fdp_rst_checks; /* restores with an FDP deferred */
static uint64_t xemu_x87_fdp_rst_viol;   /* ... where env's FDP != per-insn */
static uint64_t xemu_x87_wt_bnd_checks;  /* insn boundaries checked (stores) */
static uint64_t xemu_x87_wt_bnd_viol;    /* poison where none is pending,
                                          * or none where one is */
static uint64_t xemu_x87_wt_flush_viol;  /* poison left after a flush */
static uint64_t xemu_x87_wt_exit_checks; /* guest accesses checked (stores) */
static uint64_t xemu_x87_wt_exit_viol;   /* a landing slot without poison,
                                          * or fpstt moved within the insn */
static uint64_t xemu_x87_wt_rst_checks;  /* mid-block exits checked (stores) */
static uint64_t xemu_x87_wt_rst_pend;    /* ... with stores to land */
static uint64_t xemu_x87_wt_rst_viol;    /* poison left after the restore,
                                          * a shadow the slow path did not
                                          * store, or a landing that changes
                                          * a register already stored */
static uint64_t xemu_x87_wt_top_checks;  /* deferred fpstt/tags checked */
static uint64_t xemu_x87_wt_top_viol;    /* ... != the eager copy */
static uint64_t xemu_x87_key_checks;     /* static TOP (x87_top) checked */
static uint64_t xemu_x87_key_viol;       /* ... != env's fpstt */

static void xemu_x87_dump(void)
{
    fprintf(stderr, "xemu: x87 exc-ptrs: fip=%llu fdp=%llu per-insn, "
            "deferred updates=%llu\n",
            (unsigned long long)xemu_x87_fip_legacy,
            (unsigned long long)xemu_x87_fdp_legacy,
            (unsigned long long)xemu_x87_ptr_flushes);
    fprintf(stderr, "xemu: x87 runs=%llu insns=%llu mean=%.2f insn/run\n",
            (unsigned long long)xemu_x87_runs,
            (unsigned long long)xemu_x87_run_insns,
            xemu_x87_runs ? (double)xemu_x87_run_insns / xemu_x87_runs : 0.0);
    fprintf(stderr, "xemu: sse scalar compares: ss=%llu sd=%llu\n",
            (unsigned long long)xemu_sse_comi_ss,
            (unsigned long long)xemu_sse_comi_sd);
    fprintf(stderr, "xemu: x87 wt stores: writes=%llu st80f=%llu "
            "landed-by-restore=%llu stack-landed-by-restore=%llu "
            "stack-early=%llu\n",
            (unsigned long long)xemu_x87_wt_writes,
            (unsigned long long)xemu_x87_wt_st80f,
            (unsigned long long)xemu_x87_wt_landed,
            (unsigned long long)xemu_x87_wt_stack_landed,
            (unsigned long long)xemu_x87_wt_stack_early);
    fprintf(stderr, "xemu: x87 wt compares: inline=%llu helper=%llu "
            "fcmov inline=%llu branch=%llu\n",
            (unsigned long long)xemu_x87_wt_cmp_inline,
            (unsigned long long)xemu_x87_wt_cmp_helper,
            (unsigned long long)xemu_x87_wt_fcmov_inline,
            (unsigned long long)xemu_x87_wt_fcmov_branch);
    fprintf(stderr, "xemu: x87 refuter: "
            "exc-ptr-checks=%llu exc-ptr-violations=%llu restores=%llu "
            "restore-pending=%llu boundaries=%llu boundary-pending=%llu "
            "boundary-violations=%llu fdp-restores=%llu "
            "fdp-restore-violations=%llu\n",
            (unsigned long long)xemu_x87_ptr_checks,
            (unsigned long long)xemu_x87_ptr_viol,
            (unsigned long long)xemu_x87_rst_checks,
            (unsigned long long)xemu_x87_rst_pend,
            (unsigned long long)xemu_x87_bnd_checks,
            (unsigned long long)xemu_x87_bnd_pend,
            (unsigned long long)xemu_x87_rst_viol,
            (unsigned long long)xemu_x87_fdp_rst_checks,
            (unsigned long long)xemu_x87_fdp_rst_viol);
    fprintf(stderr, "xemu: x87 refuter: wt-boundaries=%llu "
            "wt-boundary-violations=%llu wt-flush-violations=%llu "
            "wt-exits=%llu wt-exit-violations=%llu "
            "wt-restores=%llu wt-restore-pending=%llu "
            "wt-restore-violations=%llu wt-stack-checks=%llu "
            "wt-stack-violations=%llu top-key-checks=%llu "
            "top-key-violations=%llu\n",
            (unsigned long long)xemu_x87_wt_bnd_checks,
            (unsigned long long)xemu_x87_wt_bnd_viol,
            (unsigned long long)xemu_x87_wt_flush_viol,
            (unsigned long long)xemu_x87_wt_exit_checks,
            (unsigned long long)xemu_x87_wt_exit_viol,
            (unsigned long long)xemu_x87_wt_rst_checks,
            (unsigned long long)xemu_x87_wt_rst_pend,
            (unsigned long long)xemu_x87_wt_rst_viol,
            (unsigned long long)xemu_x87_wt_top_checks,
            (unsigned long long)xemu_x87_wt_top_viol,
            (unsigned long long)xemu_x87_key_checks,
            (unsigned long long)xemu_x87_key_viol);
}

static void xemu_x87_arm_dump(void)
{
    static bool armed;
    if (!armed) {
        atexit(xemu_x87_dump);
        armed = true;
    }
}

static bool xemu_x87_census_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_X87_CENSUS");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on) {
            xemu_x87_arm_dump();
        }
    }
    return on;
}

/*
 * Latched on first use, which is inside the first translated TB, so the
 * value cannot change between TBs.
 */
static bool xemu_x87_refute_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_X87_REFUTE");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on) {
            xemu_x87_arm_dump();
        }
    }
    return on;
}

/* Under icount: FDS on segment change, FDP exact at every exit (see (2)). */
static bool xemu_x87_fdp_eager(void)
{
    return icount_enabled();
}

/* Emit "*counter += n". */
static void xemu_x87_gen_counter_add(uint64_t *counter, uint64_t n)
{
    TCGv_ptr p = tcg_constant_ptr(counter);
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, p, 0);
    tcg_gen_addi_i64(t, t, n);
    tcg_gen_st_i64(t, p, 0);
}

/* Emit "*counter += 1": the translate-time census bump the x87 sites here
 * and in ops_fpu.h, and the SSE COMI sites in emit.c.inc, share. */
static void xemu_gen_counter_inc(uint64_t *counter)
{
    xemu_x87_gen_counter_add(counter, 1);
}

/* Emit "*slot = v". */
static void xemu_x87_gen_store_u64(uint64_t *slot, uint64_t v)
{
    tcg_gen_st_i64(tcg_constant_i64(v), tcg_constant_ptr(slot), 0);
}

/* Emit "*counter += delta". */
static void xemu_x87_gen_acc_i64(uint64_t *counter, TCGv_i64 delta)
{
    TCGv_ptr p = tcg_constant_ptr(counter);
    TCGv_i64 acc = tcg_temp_new_i64();

    tcg_gen_ld_i64(acc, p, 0);
    tcg_gen_add_i64(acc, acc, delta);
    tcg_gen_st_i64(acc, p, 0);
}

/* Branchless "*counter += (a != b)" in the two widths the checks need. */
static void xemu_x87_gen_ne_count_i32(uint64_t *counter, TCGv_i32 a,
                                      TCGv_i32 b)
{
    TCGv_i32 d = tcg_temp_new_i32();
    TCGv_i64 d64 = tcg_temp_new_i64();

    tcg_gen_setcond_i32(TCG_COND_NE, d, a, b);
    tcg_gen_extu_i32_i64(d64, d);
    xemu_x87_gen_acc_i64(counter, d64);
}

static void xemu_x87_gen_ne_count_tl(uint64_t *counter, TCGv a, TCGv b)
{
    TCGv d = tcg_temp_new();
    TCGv_i64 d64 = tcg_temp_new_i64();

    tcg_gen_setcond_tl(TCG_COND_NE, d, a, b);
    tcg_gen_extu_tl_i64(d64, d);
    xemu_x87_gen_acc_i64(counter, d64);
}

/*
 * Refuter poison for a deferred x87 store's env image: an unnormal (integer
 * bit clear) that no conversion or helper produces.
 */
#define XEMU_X87_POISON_LO  0x0badcafedeadbeefull
#define XEMU_X87_POISON_HI  0x5a5a

static void xemu_x87_gen_poison(X87Ptr p)
{
    tcg_gen_st_i64(tcg_constant_i64(XEMU_X87_POISON_LO), p.base, p.ofs);
    tcg_gen_st16_i32(tcg_constant_i32(XEMU_X87_POISON_HI), p.base, p.ofs + 8);
}

/* Emit "*counter += (image at p is poison) != want". */
static void xemu_x87_gen_poison_check(uint64_t *counter, X87Ptr p, bool want)
{
    TCGv_i64 lo = tcg_temp_new_i64();
    TCGv_i64 d = tcg_temp_new_i64();

    tcg_gen_ld_i64(lo, p.base, p.ofs);
    tcg_gen_setcondi_i64(want ? TCG_COND_NE : TCG_COND_EQ, d, lo,
                         XEMU_X87_POISON_LO);
    xemu_x87_gen_acc_i64(counter, d);
}

/* Emit a count of poisoned images among all eight registers and FT0. */
static void xemu_x87_gen_poison_scan(uint64_t *counter)
{
    for (int i = 0; i < 8; i++) {
        xemu_x87_gen_poison_check(counter, gen_stn_ptr(i), false);
    }
    xemu_x87_gen_poison_check(counter, gen_ft0_ptr(), false);
}

/* (2) deferred FIP/FCS/FDP/FDS ------------------------------------------- */

static void xemu_x87_record_fip(DisasContext *s, TCGv eip)
{
    s->xemu_fip_pc = s->base.pc_next;
    s->xemu_fip_val = eip;
    s->xemu_fip_pend = true;
    if (xemu_x87_refute_on()) {
        xemu_x87_gen_store_u64(&xemu_x87_fip_rt_pend, 1);
    }
}

static void xemu_x87_record_fdp(DisasContext *s, int seg, TCGv addr)
{
    /*
     * The selector is captured now rather than at the flush: the data segment
     * is guest-writable mid-TB, and only the protected-mode path funnels
     * through a helper (whose call site flushes first). CS is not captured --
     * see xemu_x87_flush_ptrs().
     */
    s->xemu_fdp_sel = tcg_temp_new_i32();
    tcg_gen_ld_i32(s->xemu_fdp_sel, tcg_env,
                   offsetof(CPUX86State, segs[seg].selector));
    s->xemu_fdp_val = addr;
    s->xemu_fdp_pend = true;
}

/*
 * Under icount, on the write-through path: defer an insn's FDP like FIP (FDS
 * stays eager, on segment change, so a pending FDP and env's FDS always
 * come from the same insn). Its value is a runtime one, so every guest
 * access while it is pending stores it on its slow path
 * (gen_ldst_prologue), the one way that access leaves the TB, and the
 * flush stores it in line. XEMU_X87_REFUTE also stores the per-insn value
 * to xemu_x87_fdp_chk, which restores at an insn that started with an
 * FDP pending compare (XEMU_X87_W2_FDP). False: store it now.
 */
static bool xemu_x87_defer_fdp(DisasContext *s, TCGv addr)
{
    if (!x87_wt(s)) {
        return false;
    }
    s->xemu_fdp_sel = NULL;
    s->xemu_fdp_val = addr;
    s->xemu_fdp_pend = true;
    if (xemu_x87_refute_on()) {
        tcg_gen_st_tl(addr, tcg_env, offsetof(CPUX86State, xemu_x87_fdp_chk));
    }
    return true;
}

/*
 * Emit the deferred exception-pointer update. Called from gen_flush_fp(), so
 * it runs at every BB_END op and at the top of every tcg_gen_callN() -- which
 * covers the only TCG-visible reader (do_fstenv, reached through the FNSTENV
 * and FNSAVE helpers), every helper that overwrites the fields (FLDENV,
 * FRSTOR, FNINIT, FXRSTOR), and the TB terminal in i386_tr_tb_stop().
 */
static void xemu_x87_flush_ptrs(DisasContext *s)
{
    bool refute;

    if (!s->xemu_fip_pend && !s->xemu_fdp_pend) {
        return;
    }
    refute = xemu_x87_refute_on();

    if (s->xemu_fip_pend) {
        /*
         * CS is loaded here rather than captured per instruction: every
         * instruction that can change it (far JMP/CALL/RET, IRET, INT, task
         * switch) ends the TB, and the helper that performs the change is
         * itself a call site that flushes ahead of the change.
         */
        TCGv_i32 sel = tcg_temp_new_i32();
        tcg_gen_ld_i32(sel, tcg_env,
                       offsetof(CPUX86State, segs[R_CS].selector));
        if (refute) {
            TCGv_i32 was = tcg_temp_new_i32();
            TCGv was_ip = tcg_temp_new();
            tcg_gen_ld16u_i32(was, tcg_env, offsetof(CPUX86State, fpcs));
            tcg_gen_ext16u_i32(sel, sel);
            xemu_x87_gen_ne_count_i32(&xemu_x87_ptr_viol, sel, was);
            tcg_gen_ld_tl(was_ip, tcg_env, offsetof(CPUX86State, fpip));
            xemu_x87_gen_ne_count_tl(&xemu_x87_ptr_viol, s->xemu_fip_val,
                                     was_ip);
            xemu_gen_counter_inc(&xemu_x87_ptr_checks);
            xemu_x87_gen_store_u64(&xemu_x87_fip_rt_pend, 0);
        } else {
            tcg_gen_st16_i32(sel, tcg_env, offsetof(CPUX86State, fpcs));
            tcg_gen_st_tl(s->xemu_fip_val, tcg_env,
                          offsetof(CPUX86State, fpip));
        }
        s->xemu_fip_pend = false;
    }

    if (s->xemu_fdp_pend) {
        if (!s->xemu_fdp_sel) {
            /* xemu_x87_defer_fdp: FDS is in env already. */
            tcg_gen_st_tl(s->xemu_fdp_val, tcg_env,
                          offsetof(CPUX86State, fpdp));
        } else if (refute) {
            TCGv_i32 was = tcg_temp_new_i32();
            TCGv was_dp = tcg_temp_new();
            tcg_gen_ld16u_i32(was, tcg_env, offsetof(CPUX86State, fpds));
            tcg_gen_ext16u_i32(s->xemu_fdp_sel, s->xemu_fdp_sel);
            xemu_x87_gen_ne_count_i32(&xemu_x87_ptr_viol, s->xemu_fdp_sel,
                                      was);
            tcg_gen_ld_tl(was_dp, tcg_env, offsetof(CPUX86State, fpdp));
            xemu_x87_gen_ne_count_tl(&xemu_x87_ptr_viol, s->xemu_fdp_val,
                                     was_dp);
            xemu_gen_counter_inc(&xemu_x87_ptr_checks);
        } else {
            tcg_gen_st16_i32(s->xemu_fdp_sel, tcg_env,
                             offsetof(CPUX86State, fpds));
            tcg_gen_st_tl(s->xemu_fdp_val, tcg_env,
                          offsetof(CPUX86State, fpdp));
        }
        s->xemu_fdp_pend = false;
    }

    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_ptr_flushes);
    }
}

/*
 * Under icount: is the FDS store needed? Not when this basic block already
 * stored it from the same segment, whose selector changes only in a helper
 * (every call ends the block's tracking via gen_flush_fp) or in
 * gen_op_movl_seg_real. The refuter stores anyway and checks the skip.
 */
static bool xemu_x87_fds_needed(DisasContext *s, int seg)
{
    if (s->xemu_fds_seg != seg) {
        s->xemu_fds_seg = seg;
        return true;
    }
    if (xemu_x87_refute_on()) {
        TCGv_i32 sel = tcg_temp_new_i32();
        TCGv_i32 was = tcg_temp_new_i32();
        tcg_gen_ld16u_i32(sel, tcg_env,
                          offsetof(CPUX86State, segs[seg].selector));
        tcg_gen_ld16u_i32(was, tcg_env, offsetof(CPUX86State, fpds));
        xemu_x87_gen_ne_count_i32(&xemu_x87_ptr_viol, sel, was);
        xemu_gen_counter_inc(&xemu_x87_ptr_checks);
        return true;
    }
    return false;
}

/*
 * Refuter, at the top of every insn: each boundary is a possible mid-block
 * exit. With an FIP pending, what xemu_x87_restore_fip would land must be
 * the legacy FIP/FCS; with none, nothing may be left unlanded (the FNSTENV
 * and FNSAVE readers sit behind a call-site flush inside their insn).
 */
static void xemu_x87_boundary_check(DisasContext *s)
{
    if (!xemu_x87_refute_on()) {
        return;
    }
    if (s->xemu_fip_pend && s->pc_save != -1) {
        TCGv want = tcg_temp_new();
        TCGv was = tcg_temp_new();
        TCGv_i32 cs = tcg_temp_new_i32();
        TCGv_i32 fcs = tcg_temp_new_i32();

        tcg_gen_subi_tl(want, eip_cur_tl(s),
                        s->base.pc_next - s->xemu_fip_pc);
        if (!CODE64(s)) {
            tcg_gen_ext32u_tl(want, want);
        }
        tcg_gen_ld_tl(was, tcg_env, offsetof(CPUX86State, fpip));
        xemu_x87_gen_ne_count_tl(&xemu_x87_rst_viol, want, was);
        tcg_gen_ld16u_i32(cs, tcg_env,
                          offsetof(CPUX86State, segs[R_CS].selector));
        tcg_gen_ld16u_i32(fcs, tcg_env, offsetof(CPUX86State, fpcs));
        xemu_x87_gen_ne_count_i32(&xemu_x87_rst_viol, cs, fcs);
        xemu_gen_counter_inc(&xemu_x87_bnd_pend);
    } else if (!s->xemu_fip_pend) {
        TCGv_i64 pend = tcg_temp_new_i64();

        tcg_gen_ld_i64(pend, tcg_constant_ptr(&xemu_x87_fip_rt_pend), 0);
        xemu_x87_gen_acc_i64(&xemu_x87_rst_viol, pend);
    }
    xemu_gen_counter_inc(&xemu_x87_bnd_checks);
}

/*
 * FIP half of xemu_x87_restore. Non-zero `back` means an FIP update
 * recorded at that distance back has not landed: land it here, as the
 * per-insn stores would have. FCS is the live CS, which changes only
 * inside a helper, after a call-site flush.
 */
static void xemu_x87_restore_fip(CPUX86State *env, const TranslationBlock *tb,
                                 uint64_t back)
{
    bool refute = xemu_x87_refute_on();
    target_ulong fip = env->eip - back;
    uint16_t fcs = env->segs[R_CS].selector;

    if (!back && !refute) {
        return;
    }
    if (!(tb->flags & HF_CS64_MASK)) {
        fip = (uint32_t)fip;
    }
    if (refute) {
        xemu_x87_rst_checks++;
        if (back) {
            xemu_x87_rst_pend++;
            xemu_x87_rst_viol += (target_ulong)env->fpip != fip ||
                                 env->fpcs != fcs;
        } else {
            xemu_x87_rst_viol += xemu_x87_fip_rt_pend;
        }
        xemu_x87_fip_rt_pend = 0;
        return;
    }
    env->fpip = deposit64(env->fpip, 0, TARGET_LONG_BITS, fip);
    env->fpcs = fcs;
}

/*
 * Store half: land the write-through stores still deferred at the start of
 * the insn, as the eager st80f would have left them (translate.c
 * x87_wt_spill): shadow slot b is register (fpstt + top + b) & 7, fpstt
 * having stood still since the insn began. A flush inside the insn may
 * have stored them already, after the access that recorded them; it left
 * their shadows equal to what it stored, so landing them again changes
 * nothing, unless a helper wrote the register since (the refuter counts
 * that).
 */
static void xemu_x87_restore_pend(CPUX86State *env, unsigned pend,
                                  unsigned top)
{
    bool refute = xemu_x87_refute_on();

    if (refute) {
        xemu_x87_wt_rst_checks++;
        xemu_x87_wt_rst_pend += pend != 0;
    }
#if defined(__aarch64__)
    for (int b = 0; b < 8; b++) {
        if (pend & (1u << b)) {
            FPReg *f = &env->fpregs[(env->fpstt + top + b) & 7];
            floatx80 v = xemu_x87_double_to_floatx80(env->xemu_x87_shadow[b]);

            if (refute) {
                xemu_x87_wt_rst_viol += memcmp(&env->xemu_x87_shadow[b],
                                               &env->xemu_x87_shadow_chk[b],
                                               sizeof(double)) != 0;
                if (f->d.low != XEMU_X87_POISON_LO) {
                    xemu_x87_wt_rst_viol += f->d.low != v.low ||
                                            f->d.high != v.high;
                }
            }
            f->d = v;
            xemu_x87_wt_landed++;
        }
    }
#else
    /* Only the aarch64 write-through path defers stores. */
    g_assert(!pend);
#endif
    if (refute) {
        for (int i = 0; i < 8; i++) {
            xemu_x87_wt_rst_viol += env->fpregs[i].d.low == XEMU_X87_POISON_LO;
        }
    }
}

/*
 * Stack half: the tag bytes and the fpstt move still deferred at the
 * start of the insn (translate.c x87_wt_land). env->fpstt is still the
 * flush interval's, which nothing in a write-through block writes before
 * the flush: tag slot r is register (fpstt + top + r) & 7, as for the
 * stores, then fpstt moves.
 */
static void xemu_x87_restore_stack(CPUX86State *env, uint64_t word2,
                                   unsigned top)
{
    unsigned move = (word2 >> XEMU_X87_W2_MOVE_SHIFT) & 7;
    unsigned tagm = (word2 >> XEMU_X87_W2_TAGM_SHIFT) & 0xff;
    unsigned tagv = (word2 >> XEMU_X87_W2_TAGV_SHIFT) & 0xff;

    if (!move && !tagm) {
        return;
    }
    for (int r = 0; r < 8; r++) {
        if (tagm & (1u << r)) {
            env->fptags[(env->fpstt + top + r) & 7] = (tagv >> r) & 1;
        }
    }
    env->fpstt = (env->fpstt + move) & 7;
    xemu_x87_wt_stack_landed++;
    if (xemu_x87_refute_on()) {
        xemu_x87_wt_top_checks++;
        xemu_x87_wt_top_viol += env->fpstt != env->xemu_x87_fpstt_chk ||
                                memcmp(env->fptags, env->xemu_x87_fptags_chk,
                                       sizeof(env->fptags)) != 0;
    }
}

/*
 * x86_restore_state_to_opc() calls this on every mid-block exit (a guest
 * fault, cpu_io_recompile) with the insn's restored eip and insn_start word
 * 2 (FIP distance, pending x87 stores and stack, see XEMU_X87_W2_*).
 */
void xemu_x87_restore(CPUX86State *env, const TranslationBlock *tb,
                      uint64_t word2)
{
    unsigned top = (word2 >> XEMU_X87_W2_TOP_SHIFT) & 7;

    if ((word2 & XEMU_X87_W2_FDP) && xemu_x87_refute_on()) {
        xemu_x87_fdp_rst_checks++;
        xemu_x87_fdp_rst_viol += (target_ulong)env->fpdp !=
                                 (target_ulong)env->xemu_x87_fdp_chk;
    }
    xemu_x87_restore_fip(env, tb, word2 & XEMU_X87_W2_FIP_MASK);
    xemu_x87_restore_pend(env, (word2 >> XEMU_X87_W2_PEND_SHIFT) & 0xff,
                          top);
    xemu_x87_restore_stack(env, word2, top);
}

/* Run-length proxy: closed by the same flush that closes the register cache. */
static void xemu_x87_close_run(DisasContext *s)
{
    if (!s->xemu_x87_run) {
        return;
    }
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_runs);
        xemu_x87_gen_counter_add(&xemu_x87_run_insns, s->xemu_x87_run);
    }
    s->xemu_x87_run = 0;
}

#endif /* XBOX */

#endif /* TARGET_I386_XEMU_X87_H */
