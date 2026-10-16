/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#define TB_JMP_CACHE_BITS 12
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

#if defined(XBOX)
/*
 * Deterministic-mode step TB cache (cpu-exec.c, xemu_step_lookup):
 * one-insn TBs by virtual pc, direct-mapped on the low pc bits, since a
 * cut block steps through consecutive pcs. It drops pages exactly when
 * the jump cache does (tb_jmp_cache_clear_page, tcg_flush_jmp_cache),
 * without walking it: each clear stamps its page's bucket with a new
 * step_clock value, and an entry counts only if it was filled at or
 * after its page bucket's stamp. Only the vCPU thread, or code running
 * with it stopped, touches these fields; TB invalidation, which may run
 * on other threads, leaves them alone (CF_INVALID makes the TB miss).
 */
#define XEMU_STEPC_BITS 13
#define XEMU_STEPC_SIZE (1 << XEMU_STEPC_BITS)
#define XEMU_STEPC_PAGE_BITS 8
#define XEMU_STEPC_PAGES (1 << XEMU_STEPC_PAGE_BITS)
#endif

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
#if defined(XBOX)
    uint64_t step_clock;    /* counts clears; starts at 1 */
    uint64_t step_page_at[XEMU_STEPC_PAGES];    /* clock at last clear */
    struct {
        vaddr pc;
        TranslationBlock *tb;
        uint64_t at;        /* clock at fill; 0 (never valid) when zeroed */
    } step[XEMU_STEPC_SIZE];
#endif
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
