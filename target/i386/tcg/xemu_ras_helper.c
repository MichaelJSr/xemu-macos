/*
 * xemu return-address stack: near-return TB prediction
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/helper-proto-common.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "hw/core/cpu.h"
#include "exec/xemu-breakpoints.h"

/*
 * Near-return exit: consult the return-address stack before the
 * generic lookup. A hit passes the same validity checks a jump-cache
 * hit passes — the popped-eip compare plus tb->cs_base substitutes
 * for the pc compare (x86 get_tb_cpu_state pc is cs_base + eip), and
 * the fill-time eip pairing guards coincidental slot reuse.
 * Prediction state only: mispredicts fall back to
 * helper_lookup_tb_ptr, tb pointers are cleared with the jump cache
 * (xemu_i386_ras_flush, and per page xemu_i386_ras_flush_page).
 *
 * Anything that makes curr_cflags() differ from cpu->tcg_cflags —
 * gdb single-step, breakpoints, exec/nochain logging — delegates to
 * the generic helper wholesale, which also keeps its logging and
 * breakpoint semantics exact.
 */

static uint64_t xemu_ras_hits, xemu_ras_fills;

static void xemu_ras_dump(void)
{
    uint64_t tot = xemu_ras_hits + xemu_ras_fills;
    if (tot) {
        fprintf(stderr,
                "xemu: ras hits=%llu (%.1f%%) fills=%llu\n",
                (unsigned long long)xemu_ras_hits,
                100.0 * xemu_ras_hits / tot,
                (unsigned long long)xemu_ras_fills);
    }
}

static void xemu_ras_maybe_register_dump(void)
{
    static bool done;
    if (!done) {
        const char *a = getenv("XEMU_TB_PROF");
        const char *b = getenv("XEMU_GUEST_PROF");
        if ((a && a[0] == '1') || (b && b[0] == '1')) {
            atexit(xemu_ras_dump);
        }
        done = true;
    }
}

const void *HELPER(xemu_lookup_ret)(CPUX86State *env)
{
    CPUState *cpu = env_cpu(env);

    /* A gdb breakpoint's site TB stops by itself (exec/xemu-breakpoints.h). */
    if (likely(!cpu->singlestep_enabled) &&
        likely(!xemu_cpu_breakpoints(cpu)) &&
        likely(!qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC |
                                   CPU_LOG_TB_NOCHAIN))) {
        xemu_ras_maybe_register_dump();

        uint32_t eip = (uint32_t)env->eip;
        uint32_t idx = xemu_retc_index(eip);
        struct XemuRetcEntry *e = &env->xemu_retc[idx];
        if (e->eip == eip) {
            TranslationBlock *tb = e->tb;
            if (tb) {
                TCGTBCPUState s = x86_get_tb_cpu_state(cpu);
                if (e->cs_base == s.cs_base &&
                    e->flags == s.flags &&
                    tb_cflags(tb) == cpu->tcg_cflags &&
                    tb->cs_base == s.cs_base &&
                    tb->flags == s.flags &&
                    (tb_cflags(tb) & CF_PCREL || tb->pc == s.pc)) {
                    xemu_ras_hits++;
                    cpu->neg.can_do_io = true;
                    return tb->tc.ptr;
                }
            }
        }
        /*
         * Miss, stale, or cold: one full lookup via the exported
         * xemu_lookup_tb, which returns the TB itself — the fill no
         * longer pays the tcg_tb_lookup() tc.ptr->tb g_tree reversal.
         * Exec logging is off inside this gate (outer likely() checks),
         * so helper_lookup_tb_ptr's log_cpu_exec is not owed here.
         */
        TranslationBlock *tb = xemu_lookup_tb(cpu);
        if (tb == NULL) {
            return tcg_code_gen_epilogue;
        }
        if (tb_cflags(tb) == cpu->tcg_cflags) {
            e->eip = eip;
            e->flags = tb->flags;
            e->cs_base = tb->cs_base;
            e->tb = tb;
            /*
             * The inline probe jumps to tc_ptr without reading the TB, so
             * an entry must never outlive its TB's invalidation. Every
             * i386 TB is CF_PCREL, and invalidating one clears the whole
             * memo (tb_jmp_cache_inval_tb -> xemu_flush_jmp_cache), after
             * setting CF_INVALID. Another thread may do that between the
             * lookup above and these stores: so store, then re-read the
             * cflags behind a full barrier (it pairs with the lock-ordered
             * CF_INVALID store before the invalidator's memo clear) and
             * take the entry back if the TB died meanwhile.
             */
            qatomic_set(&e->tc_ptr, tb->tc.ptr);
            smp_mb();
            if (unlikely(qatomic_read(&tb->cflags) != cpu->tcg_cflags)) {
                qatomic_set(&e->tc_ptr, NULL);
            }
            /* Same page key as xemu_i386_ras_flush_page(). */
            unsigned pbit = xemu_retc_page_bit((uint32_t)(e->cs_base + eip));
            env->xemu_retc_pages[pbit / 64] |= 1ull << (pbit % 64);
            xemu_ras_fills++;
        }
        return tb->tc.ptr;
    }

    return helper_lookup_tb_ptr(env);
}

#ifdef CONFIG_DEBUG_TCG
/*
 * Behind every inline ret-memo hit (xemu_gen_inline_ret_memo): the entry's
 * tc_ptr must be its TB's code, and the TB must be live with the base
 * cflags, which is what the probe no longer checks. Without icount another
 * thread may be invalidating the TB right now (it sets CF_INVALID, then
 * clears the memo), so give it time to clear the entry before calling it
 * a lost invalidation.
 */
void HELPER(xemu_retc_check)(CPUX86State *env, void *entry, const void *tc)
{
    CPUState *cpu = env_cpu(env);
    struct XemuRetcEntry *e = entry;
    TranslationBlock *tb = qatomic_read(&e->tb);

    if (tb && tb->tc.ptr == tc &&
        qatomic_read(&tb->cflags) == cpu->tcg_cflags) {
        return;
    }
    if (!(cpu->tcg_cflags & CF_USE_ICOUNT)) {
        int64_t end = get_clock_realtime() + NANOSECONDS_PER_SECOND;

        while (qatomic_read(&e->tc_ptr) == tc &&
               get_clock_realtime() < end) {
            g_usleep(100);
        }
        if (qatomic_read(&e->tc_ptr) != tc) {
            return;
        }
    }
    fprintf(stderr, "xemu: ret memo hit eip=%08x tc=%p on tb=%p "
            "(tc %p, cflags %08x, base %08x): a lost invalidation\n",
            e->eip, tc, tb, tb ? tb->tc.ptr : NULL,
            tb ? qatomic_read(&tb->cflags) : 0, cpu->tcg_cflags);
    abort();
}
#endif
