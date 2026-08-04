/*
 * xemu: x87/SSE translator census, dead-store elision and exception-pointer
 * deferral (fork optimization audit 2026-08-04, section 1.4).
 *
 * Header-only and private to target/i386/tcg/translate.c: that file is the
 * only translation unit that instantiates ops_fpu.h (once per precision) and
 * includes emit.c.inc, so file-static counters and latches reach every census
 * site without a new build-system entry.
 *
 * Counter discipline follows accel/tcg/xemu-inv-prof.h: plain unsynchronized
 * uint64 written only by the single xbox vCPU thread, dumped from atexit()
 * after that thread has stopped.
 *
 * Three mechanisms live here, each with its own latch:
 *
 *   (1) XEMU_X87_CENSUS=1   Gate-0 census. Splits every register-cache
 *       write-back into clean-ST(i) / dead-FT0 / must-keep, counts the
 *       per-instruction FIP/FDP store pairs against the deferred updates
 *       that replace them, measures mean x87 run length, and counts SSE
 *       scalar compares by operand width.
 *   (2) XEMU_X87_ELIDE_FT0  (default on, =0 legacy) drops the write-back of
 *       the FT0 scratch when no reader of that value is still ahead.
 *   (3) XEMU_X87_DEFER_FIP  (default on, =0 legacy) defers FIP/FCS/FDP/FDS
 *       to one update per contiguous x87 run.
 *   (4) XEMU_X87_ELIDE_CLEAN (default OFF) drops the write-back of an ST(i)
 *       slot that was only read. Dark: it is a semantics change, not a pure
 *       elision -- see the comment on xemu_x87_clean_mode().
 *   (5) XEMU_X87_REFUTE=1 puts (2)(3)(4) in shadow mode: the legacy stores
 *       all still happen, the elision decision is computed anyway, and every
 *       point where the elided state could be observed is checked at runtime.
 *       Promotion gate is zero violations over the campaign soak.
 */
#ifndef TARGET_I386_XEMU_X87_H
#define TARGET_I386_XEMU_X87_H

#if defined(XBOX)

/*
 * Defined further down translate.c; declared here because ops_fpu.h is
 * instantiated well above it and the census sites live inside that template.
 */
static void xemu_gen_counter_inc(uint64_t *counter);

/* (1) census -------------------------------------------------------------- */
static uint64_t xemu_x87_wb_st_clean;    /* ST(i) write-back, slot only read */
static uint64_t xemu_x87_wb_st_dirty;    /* ST(i) write-back, slot written */
static uint64_t xemu_x87_wb_ft0_dead;    /* FT0 write-back, no reader ahead */
static uint64_t xemu_x87_wb_ft0_keep;    /* FT0 write-back, reader still ahead */
static uint64_t xemu_x87_fip_legacy;     /* per-insn FIP/FCS store pairs */
static uint64_t xemu_x87_fdp_legacy;     /* per-insn FDP/FDS store pairs */
static uint64_t xemu_x87_ptr_flushes;    /* deferred exc-pointer updates */
static uint64_t xemu_x87_runs;           /* x87 runs closed by a flush */
static uint64_t xemu_x87_run_insns;      /* x87 insns inside those runs */
static uint64_t xemu_sse_comi_ss;        /* (U)COMISS -- 32-bit scalar form */
static uint64_t xemu_sse_comi_sd;        /* (U)COMISD -- 64-bit scalar form */

/* (5) refuter ------------------------------------------------------------- */
static uint64_t xemu_x87_ft0_stale;      /* 1 while an elided FT0 store stands */
static uint64_t xemu_x87_ft0_viol;       /* stale env->ft0 fed to a reader */
static uint64_t xemu_x87_ptr_checks;     /* deferred-vs-legacy comparisons */
static uint64_t xemu_x87_ptr_viol;       /* deferred value != legacy value */
static uint64_t xemu_x87_clean_checks;   /* clean write-backs re-read */
static uint64_t xemu_x87_clean_viol;     /* clean slot's memory moved under us */

static void xemu_x87_dump(void)
{
    uint64_t st = xemu_x87_wb_st_clean + xemu_x87_wb_st_dirty;
    uint64_t ft = xemu_x87_wb_ft0_dead + xemu_x87_wb_ft0_keep;

    fprintf(stderr, "xemu: x87 write-backs: ST(i) clean=%llu dirty=%llu "
            "(%.1f%% elidable) FT0 dead=%llu keep=%llu (%.1f%% elidable)\n",
            (unsigned long long)xemu_x87_wb_st_clean,
            (unsigned long long)xemu_x87_wb_st_dirty,
            st ? 100.0 * xemu_x87_wb_st_clean / st : 0.0,
            (unsigned long long)xemu_x87_wb_ft0_dead,
            (unsigned long long)xemu_x87_wb_ft0_keep,
            ft ? 100.0 * xemu_x87_wb_ft0_dead / ft : 0.0);
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
    fprintf(stderr, "xemu: x87 refuter: ft0 violations=%llu; exc-ptr "
            "checks=%llu violations=%llu; clean-wb checks=%llu violations=%llu\n",
            (unsigned long long)xemu_x87_ft0_viol,
            (unsigned long long)xemu_x87_ptr_checks,
            (unsigned long long)xemu_x87_ptr_viol,
            (unsigned long long)xemu_x87_clean_checks,
            (unsigned long long)xemu_x87_clean_viol);
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

/*
 * Mode latches. 0 = legacy, 1 = mechanism live, 2 = refuter (legacy behavior
 * plus the shadow check). Latched on first use, which is inside the first
 * translated TB, so the value cannot change between TBs.
 */
static int xemu_x87_ft0_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("XEMU_X87_ELIDE_FT0");
        int m = (e && e[0] == '0') ? 0 : 1;
        if (m && xemu_x87_refute_on()) {
            m = 2;
        }
        mode = m;
    }
    return mode;
}

static int xemu_x87_ptr_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("XEMU_X87_DEFER_FIP");
        int m = (e && e[0] == '0') ? 0 : 1;
        if (m && xemu_x87_refute_on()) {
            m = 2;
        }
        mode = m;
    }
    return mode;
}

/*
 * Default OFF, unlike the other two. Dropping the write-back of a slot that
 * was only read is not a pure elision: the cache holds the double-precision
 * projection of an 80-bit env->fpregs[] entry, so today's unconditional
 * write-back also truncates that entry to what a double can hold. Eliding it
 * leaves the original floatx80 in place, which the inline path cannot observe
 * (it re-projects to the same double) but FSTPT / FSAVE / FXSAVE and the
 * softfloat transcendental helpers can. That is a behavior change in the
 * more-accurate direction, not a no-op, so it stays dark until the campaign
 * decides. XEMU_X87_ELIDE_CLEAN=1 enables it.
 */
static int xemu_x87_clean_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("XEMU_X87_ELIDE_CLEAN");
        int m = (e && e[0] == '1') ? 1 : 0;
        if (m && xemu_x87_refute_on()) {
            m = 2;
        }
        mode = m;
    }
    return mode;
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

/* Emit "*slot = v". */
static void xemu_x87_gen_store_u64(uint64_t *slot, uint64_t v)
{
    tcg_gen_st_i64(tcg_constant_i64(v), tcg_constant_ptr(slot), 0);
}

/*
 * Emit "*counter += xemu_x87_ft0_stale; xemu_x87_ft0_stale = 0" -- a reader
 * has just reloaded env->ft0, so if an elided write-back was outstanding the
 * elision would have handed it a stale operand.
 */
static void xemu_x87_gen_ft0_check(void)
{
    TCGv_ptr sp = tcg_constant_ptr(&xemu_x87_ft0_stale);
    TCGv_i64 stale = tcg_temp_new_i64();
    TCGv_i64 acc = tcg_temp_new_i64();

    tcg_gen_ld_i64(stale, sp, 0);
    tcg_gen_ld_i64(acc, tcg_constant_ptr(&xemu_x87_ft0_viol), 0);
    tcg_gen_add_i64(acc, acc, stale);
    tcg_gen_st_i64(acc, tcg_constant_ptr(&xemu_x87_ft0_viol), 0);
    tcg_gen_st_i64(tcg_constant_i64(0), sp, 0);
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

/* Branchless "*counter += (a != b)" in the three widths the checks need. */
static void xemu_x87_gen_ne_count_i64(uint64_t *counter, TCGv_i64 a,
                                      TCGv_i64 b)
{
    TCGv_i64 d = tcg_temp_new_i64();

    tcg_gen_setcond_i64(TCG_COND_NE, d, a, b);
    xemu_x87_gen_acc_i64(counter, d);
}

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
 * (4) dirty tracking. Indexed by fpregs[] slot, which is the physical x87
 * register file index, so an FPUSH/FPOP rotation carries the bit with the
 * value it describes. Invariant maintained everywhere: a set bit implies a
 * live cache entry.
 */
static void xemu_x87_mark_dirty(DisasContext *s, int opreg)
{
    s->xemu_fp_dirty |= 1u << ((s->fpstt_delta + opreg) & 7);
}

static void xemu_x87_clear_dirty(DisasContext *s, int slot)
{
    s->xemu_fp_dirty &= ~(1u << slot);
}

/*
 * (2) FT0 liveness. Every FT0 reader in hard-fpu-inline mode is preceded by
 * an FT0 writer inside the same guest instruction, so once the reader has
 * consumed the value nothing can observe env->ft0: no helper reachable in
 * that mode reads it (every FT0-reading helper sits in the
 * !g_use_hard_fpu_inline arm of translate.c) and it is not in the vmstate.
 * The one window where the write-back is live is a helper call emitted
 * between the writer and the reader -- gen_compute_eflags() ahead of the
 * inline FCOMI/FUCOMI -- which the pending flag keeps honest.
 *
 * Only the store is worth eliding. The paired dead ld80f a writer's get_ft0()
 * emits already vanishes in liveness_pass_1 (one output, no side effects);
 * st80f declares no outputs, which tcg.c:4436 reads as "assume side effects",
 * so it survives to the host as real stores.
 */
static void xemu_x87_ft0_produce(DisasContext *s)
{
    s->xemu_ft0_pending = true;
    s->xemu_ft0_reloaded = false;
}

static void xemu_x87_ft0_consume(DisasContext *s)
{
    if (unlikely(s->xemu_ft0_reloaded) && xemu_x87_ft0_mode() == 2) {
        xemu_x87_gen_ft0_check();
    }
    s->xemu_ft0_reloaded = false;
    s->xemu_ft0_pending = false;
}

/* (3) deferred FIP/FCS/FDP/FDS ------------------------------------------- */

static void xemu_x87_record_fip(DisasContext *s, TCGv eip)
{
    s->xemu_fip_val = eip;
    s->xemu_fip_pend = true;
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
 * Emit the deferred exception-pointer update. Called from gen_flush_fp(), so
 * it runs at every BB_END op and at the top of every tcg_gen_callN() -- which
 * covers the only TCG-visible reader (do_fstenv, reached through the FNSTENV
 * and FNSAVE helpers), every helper that overwrites the fields (FLDENV,
 * FRSTOR, FNINIT, FXRSTOR), and the TB terminal in i386_tr_tb_stop().
 */
static void xemu_x87_flush_ptrs(DisasContext *s)
{
    int mode;

    if (!s->xemu_fip_pend && !s->xemu_fdp_pend) {
        return;
    }
    mode = xemu_x87_ptr_mode();

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
        if (mode == 2) {
            TCGv_i32 was = tcg_temp_new_i32();
            TCGv was_ip = tcg_temp_new();
            tcg_gen_ld16u_i32(was, tcg_env, offsetof(CPUX86State, fpcs));
            tcg_gen_ext16u_i32(sel, sel);
            xemu_x87_gen_ne_count_i32(&xemu_x87_ptr_viol, sel, was);
            tcg_gen_ld_tl(was_ip, tcg_env, offsetof(CPUX86State, fpip));
            xemu_x87_gen_ne_count_tl(&xemu_x87_ptr_viol, s->xemu_fip_val,
                                     was_ip);
            xemu_gen_counter_inc(&xemu_x87_ptr_checks);
        } else {
            tcg_gen_st16_i32(sel, tcg_env, offsetof(CPUX86State, fpcs));
            tcg_gen_st_tl(s->xemu_fip_val, tcg_env,
                          offsetof(CPUX86State, fpip));
        }
        s->xemu_fip_pend = false;
    }

    if (s->xemu_fdp_pend) {
        if (mode == 2) {
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

/* ops_fpu.h is instantiated per precision; these keep its writer/reader
 * annotations to one line each and compile away on a non-xbox target. */
#define XEMU_FT0_PRODUCE(s)  xemu_x87_ft0_produce(s)
#define XEMU_FT0_CONSUME(s)  xemu_x87_ft0_consume(s)
#define XEMU_ST_DIRTY(s, n)  xemu_x87_mark_dirty((s), (n))

#else /* !XBOX */

#define XEMU_FT0_PRODUCE(s)  ((void)0)
#define XEMU_FT0_CONSUME(s)  ((void)0)
#define XEMU_ST_DIRTY(s, n)  ((void)0)

#endif /* XBOX */

#endif /* TARGET_I386_XEMU_X87_H */
