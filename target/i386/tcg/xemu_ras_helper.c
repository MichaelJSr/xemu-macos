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
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/helper-proto-common.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "hw/core/cpu.h"
#include "accel/tcg/cpu-ops.h"

/*
 * Near-return exit: consult the return-address stack before the
 * generic lookup. A hit passes the same validity checks a jump-cache
 * hit passes — the popped-eip compare plus tb->cs_base substitutes
 * for the pc compare (x86 get_tb_cpu_state pc is cs_base + eip), and
 * the fill-time eip pairing guards coincidental slot reuse.
 * Prediction state only: mispredicts fall back to
 * helper_lookup_tb_ptr, tb pointers are cleared with the jump cache
 * on flush (xemu_i386_ras_flush), XEMU_RAS=0 disables both sides.
 *
 * Anything that makes curr_cflags() differ from cpu->tcg_cflags —
 * gdb single-step, breakpoints, exec/nochain logging — delegates to
 * the generic helper wholesale, which also keeps its logging and
 * breakpoint semantics exact.
 */

static bool xemu_ras_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_RAS");
        on = !(e && e[0] == '0');
    }
    return on;
}

static uint64_t xemu_ras_hits, xemu_ras_fills, xemu_ras_mispredicts;

static void xemu_ras_dump(void)
{
    uint64_t tot = xemu_ras_hits + xemu_ras_fills + xemu_ras_mispredicts;
    if (tot) {
        fprintf(stderr,
                "xemu: ras hits=%llu (%.1f%%) fills=%llu mispredicts=%llu\n",
                (unsigned long long)xemu_ras_hits,
                100.0 * xemu_ras_hits / tot,
                (unsigned long long)xemu_ras_fills,
                (unsigned long long)xemu_ras_mispredicts);
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

    if (likely(xemu_ras_on()) &&
        likely(!cpu->singlestep_enabled) &&
        likely(QTAILQ_EMPTY(&cpu->breakpoints)) &&
        likely(!qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC |
                                   CPU_LOG_TB_NOCHAIN))) {
        xemu_ras_maybe_register_dump();

        uint32_t eip = (uint32_t)env->eip;
        uint32_t idx = (eip * 2654435761u) >> (32 - XEMU_RETC_BITS);
        if (env->xemu_retc_eip[idx] == eip) {
            TranslationBlock *tb = env->xemu_retc_tb[idx];
            if (tb) {
                TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
                if (tb->cs_base == s.cs_base &&
                    tb->flags == s.flags &&
                    tb_cflags(tb) == cpu->tcg_cflags &&
                    (tb_cflags(tb) & CF_PCREL || tb->pc == s.pc)) {
                    xemu_ras_hits++;
                    cpu->neg.can_do_io = true;
                    return tb->tc.ptr;
                }
            }
        }
        /* Miss, stale, or cold: full lookup, then memoize. */
        const void *ptr = helper_lookup_tb_ptr(env);
        if (ptr != tcg_code_gen_epilogue) {
            TranslationBlock *tb = tcg_tb_lookup((uintptr_t)ptr);
            if (tb) {
                env->xemu_retc_eip[idx] = eip;
                env->xemu_retc_tb[idx] = tb;
                xemu_ras_fills++;
            }
        }
        return ptr;
    }

    return helper_lookup_tb_ptr(env);
}
