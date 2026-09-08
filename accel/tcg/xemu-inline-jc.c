/*
 * xemu: inline jump-cache probe emission
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Lives in accel/tcg because it is the only place that may know the
 * CPUJumpCache layout. The target's translator calls this at
 * indirect/cross-page exits with its translate-time context
 * constants; on a validated hit the emitted code jumps straight to
 * the cached TB, otherwise it falls through to the caller's helper
 * fallback. Validation mirrors a helper jump-cache hit: entry pc
 * compare, then tb->cs_base/flags/cflags against the exit site's
 * constants (exact because runtime context at a TB exit equals that
 * TB's own translation context; non-base-cflags exit TBs simply
 * never match, which is a safe fallback). The live tb->cflags load
 * catches CF_INVALID from invalidation; breakpoint insertion
 * invalidates the affected TBs, so hits cannot skip a new
 * breakpoint. Interrupts are seen at the target TB's entry check,
 * as with any goto_tb/goto_ptr chain.
 */
#include "qemu/osdep.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "tcg/tcg-op-common.h"
#include "tcg/tcg-temp-internal.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"

#if defined(XBOX)
void tcg_gen_xemu_jc_probe_and_goto_ptr(TCGv_i32 pc32,
                                        intptr_t jc_env_offset,
                                        uint64_t cs_base, uint32_t flags,
                                        uint32_t cflags)
{
    /*
     * This file is the sole owner of the CPUJumpCache layout, and the entry
     * address below is formed with a hardcoded shift rather than an
     * offsetof()-derived one. Pin both layout facts at compile time so a
     * future field addition or a narrower vaddr cannot silently make the
     * probe read a neighbouring entry (wrong pc/flags -> dispatch to the
     * wrong TB): the entry stride must stay {TranslationBlock *, vaddr} = 16
     * bytes for the `shli 4`, and the pc field must stay 64-bit for the
     * ld_i64/brcond_i64 compare against the zero-extended guest pc.
     */
    QEMU_BUILD_BUG_ON(sizeof(((CPUJumpCache *)0)->array[0]) != 16);
    QEMU_BUILD_BUG_ON(sizeof(((CPUJumpCache *)0)->array[0].pc) != 8);

    TCGLabel *slow = gen_new_label();
    TCGv_i32 tmp = tcg_temp_new_i32();
    TCGv_i32 h = tcg_temp_new_i32();
    TCGv_i32 t32 = tcg_temp_new_i32();
    TCGv_i64 pc64 = tcg_temp_new_i64();
    TCGv_i64 t64 = tcg_temp_new_i64();
    TCGv_ptr jc = tcg_temp_new_ptr();
    TCGv_ptr tbp = tcg_temp_new_ptr();
    TCGv_ptr tcp = tcg_temp_new_ptr();

    tcg_gen_extu_i32_i64(pc64, pc32);

    /* tb_jmp_cache_hash_func for a 32-bit guest pc. */
    tcg_gen_shri_i32(tmp, pc32, TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
    tcg_gen_xor_i32(tmp, tmp, pc32);
    tcg_gen_shri_i32(h, tmp, TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
    tcg_gen_andi_i32(h, h, TB_JMP_PAGE_MASK);
    tcg_gen_andi_i32(tmp, tmp, TB_JMP_ADDR_MASK);
    tcg_gen_or_i32(h, h, tmp);

    tcg_gen_ld_ptr(jc, tcg_env, jc_env_offset);
    tcg_gen_shli_i32(h, h, 4); /* entry stride: {tb, pc} = 16 bytes */
    tcg_gen_ext_i32_ptr(tbp, h);
    tcg_gen_add_ptr(jc, jc, tbp);

    tcg_gen_ld_ptr(tbp, jc, offsetof(CPUJumpCache, array) +
                            offsetof(typeof(((CPUJumpCache *)0)->array[0]),
                                     tb));
    tcg_gen_brcondi_ptr(TCG_COND_EQ, tbp, 0, slow);
    tcg_gen_ld_i64(t64, jc, offsetof(CPUJumpCache, array) +
                            offsetof(typeof(((CPUJumpCache *)0)->array[0]),
                                     pc));
    tcg_gen_brcond_i64(TCG_COND_NE, t64, pc64, slow);

    tcg_gen_ld_i32(t32, tbp, offsetof(TranslationBlock, flags));
    tcg_gen_brcondi_i32(TCG_COND_NE, t32, (int32_t)flags, slow);
    tcg_gen_ld_i64(t64, tbp, offsetof(TranslationBlock, cs_base));
    tcg_gen_brcondi_i64(TCG_COND_NE, t64, (int64_t)cs_base, slow);
    tcg_gen_ld_i32(t32, tbp, offsetof(TranslationBlock, cflags));
    tcg_gen_brcondi_i32(TCG_COND_NE, t32, (int32_t)cflags, slow);

    tcg_gen_ld_ptr(tcp, tbp, offsetof(TranslationBlock, tc) +
                             offsetof(struct tb_tc, ptr));
    tcg_gen_xemu_goto_ptr(tcp);

    gen_set_label(slow);
}
#endif
