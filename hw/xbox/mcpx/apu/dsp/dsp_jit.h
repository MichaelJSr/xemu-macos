/*
 * MCPX APU DSP (DSP56300 / M56001) JIT
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#ifndef HW_XBOX_MCPX_APU_DSP_JIT_H
#define HW_XBOX_MCPX_APU_DSP_JIT_H

#include "dsp_cpu.h"

#if defined(__APPLE__) && defined(__aarch64__)
#define DSP_JIT_SUPPORTED 1
#else
#define DSP_JIT_SUPPORTED 0
#endif

#if DSP_JIT_SUPPORTED

/*
 * Initialize / finalize per-core JIT state (code buffer, block
 * cache, translation context). Called from dsp_init / dsp_destroy.
 */
void dsp_jit_init(dsp_core_t *dsp);
void dsp_jit_finalize(dsp_core_t *dsp);

/*
 * Execute translated code starting at dsp->pc. Returns the number
 * of cycles consumed across the block (sum of dsp->instr_cycle per
 * op). On any fallback-required condition (unsupported op on first
 * translation, interrupt pending on entry, etc.) the function may
 * return 0 without advancing, and the caller should run one
 * interpreter step.
 */
unsigned int dsp_jit_execute_block(dsp_core_t *dsp);

/*
 * Invalidate any translated block whose PC range includes addr.
 * Called from dsp56k_write_memory when writing to P-space.
 */
void dsp_jit_invalidate(dsp_core_t *dsp, uint32_t addr);

/*
 * Invalidate all translated blocks (e.g., on DSP reset, bootstrap
 * reload of pram, or code-cache full).
 */
void dsp_jit_invalidate_all(dsp_core_t *dsp);

/*
 * Runtime flags. Parsed from XEMU_DSP_JIT / XEMU_DSP_JIT_DIFF on
 * first call. Cached thereafter.
 */
bool dsp_jit_enabled(void);
bool dsp_jit_diff_enabled(void);

/*
 * Sentinel (debug / bisect) harness for the deferred round-4
 * cur_inst skip optimization. XEMU_DSP_JIT_SENTINEL=1 re-applies
 * the skip but substitutes DSP_JIT_SENTINEL_POISON (0x00adbeef)
 * for the correct inst. Any inlined-op handler path that reads
 * dsp->cur_inst at runtime will observe the poison and typically
 * surface as a visible failure (lookup_opcode_slow assert with
 * "op = 00adbeef", or a DIFF mismatch on a sub-field decode).
 * The stack trace / DIFF diff pinpoints the reader.
 *
 * The companion PC-skip sentinel (old bit 1) was removed after
 * the round-4 EPI_NO_PC optimization was dropped permanently —
 * see the EPI_NO_PC drop commit for the analysis (mode-6 parmove
 * lengthening + REP/DO loop rewinds make it unsafe).
 */
#define DSP_JIT_SENTINEL_CURINST  (1u << 0)
#define DSP_JIT_SENTINEL_POISON   0x00adbeefu
bool dsp_jit_sentinel_enabled(uint32_t bit);

/*
 * Force-apply the deferred round-4 cur_inst skip literally
 * (no sentinel poison). Use XEMU_DSP_JIT_FORCE=1 to reproduce
 * the underlying regression on demand while bisecting. If a
 * future fix makes FORCE=1 run clean, the cur_inst skip can be
 * re-landed permanently.
 */
#define DSP_JIT_FORCE_CURINST_SKIP  (1u << 0)
bool dsp_jit_force_enabled(uint32_t bit);

/*
 * Called from APU init (gp_ep.c) with g_config.audio.dsp_jit.enabled.
 * Keeps the JIT source decoupled from ui/xemu-settings.h so the
 * standalone DSP test binary (which links libdsp.a without the
 * xemu settings library) continues to link.
 */
void dsp_jit_set_enabled_from_config(bool enabled);

/*
 * Internal shims implemented in dsp_cpu.c and called from translated
 * code in dsp_jit.c. Declared here only so both TUs agree on types.
 * Not part of the public API — do not call from outside the JIT.
 */
void dsp_jit_helper_postexecute_update_pc(dsp_core_t *dsp);
void dsp_jit_helper_postexecute_interrupts(dsp_core_t *dsp);

/* Pin-audit diagnostic (XEMU_DSP_JIT_PIN_AUDIT=1). Called by the
 * JIT's per-op check when x26 (A) or x27 (B) pin diverges from
 * registers[]. See dsp_cpu.c for arg semantics. */
void dsp_jit_helper_pin_audit_fail(dsp_core_t *dsp, uint32_t which,
                                   uint32_t pin_lo, uint32_t pin_hi,
                                   uint32_t pc, uint32_t inst);
typedef void (*dsp_emu_func_t)(dsp_core_t *dsp);
dsp_emu_func_t dsp_jit_helper_lookup_emu(uint32_t inst);
uint32_t dsp_jit_helper_inst_length(uint32_t inst);
bool dsp_jit_helper_is_terminator(void *fn);

/* Phase 4 shims (parmove inlining). See dsp_cpu.c for definitions. */
int  dsp_jit_helper_pm_read_accu24(dsp_core_t *dsp, int numreg, uint32_t *dest);
int  dsp_jit_helper_calc_ea(dsp_core_t *dsp, uint32_t ea_mode, uint32_t *dst_addr);
void dsp_jit_helper_update_rn(dsp_core_t *dsp, uint32_t numreg, int16_t modifier);
dsp_emu_func_t dsp_jit_helper_lookup_alu(uint32_t inst);
bool dsp_jit_helper_alu_is_move(dsp_emu_func_t fn);
void dsp_jit_helper_pm_4x(dsp_core_t *dsp);

/*
 * Phase 2 shim — dsp_rnd56 wrapper for the JIT's MPYR / MACR
 * round path. Unpacks / repacks the 64-bit accumulator convention.
 */
uint64_t dsp_jit_helper_rnd56(dsp_core_t *dsp, uint64_t packed);

/*
 * Phase 5 shims — expose the static dsp_stack_push / dsp_stack_pop
 * helpers from dsp_cpu.c so the JIT's inline control-flow emitters
 * can BLR them for the subroutine-call / return ops.
 *
 * dsp_jit_helper_stack_push wraps dsp_stack_push(dsp, pc, sr, 0) —
 * the "sshOnly = 0" form that every control-flow handler uses.
 *
 * Round 2 removed the emu_calc_cc shim: the 16-case cc switch is
 * now materialised inline at translate time, since cc_code is a
 * known bitfield of the instruction word. See emit_cf_calc_cc in
 * dsp_jit.c.
 */
void dsp_jit_helper_stack_push(dsp_core_t *dsp, uint32_t newpc,
                               uint32_t newsr);
void dsp_jit_helper_stack_pop(dsp_core_t *dsp, uint32_t *newpc,
                              uint32_t *newsr);

/*
 * Phase 5 shim — classifies an emu_func_t control-flow handler so
 * the JIT can pick an inline emitter without direct access to the
 * file-static emu_* symbols. Returns one of the DSP_JIT_CF_* tags
 * below, or DSP_JIT_CF_NONE for handlers not covered by the inline
 * control-flow path (fallback to BLR).
 */
enum {
    DSP_JIT_CF_NONE = 0,
    /* Absolute / immediate CF */
    DSP_JIT_CF_JMP_IMM,
    DSP_JIT_CF_JSR_IMM,
    DSP_JIT_CF_RTS,
    DSP_JIT_CF_RTI,
    DSP_JIT_CF_BRA_IMM,
    DSP_JIT_CF_BRA_LONG,
    DSP_JIT_CF_BSR_IMM,
    DSP_JIT_CF_BSR_LONG,
    DSP_JIT_CF_JCC_IMM,
    DSP_JIT_CF_JSCC_IMM,
    DSP_JIT_CF_BCC_IMM,
    DSP_JIT_CF_BCC_LONG,
    /* Loop ops */
    DSP_JIT_CF_REP_IMM,
    DSP_JIT_CF_REP_AA,
    DSP_JIT_CF_REP_EA,
    DSP_JIT_CF_REP_REG,
    DSP_JIT_CF_DO_IMM,
    DSP_JIT_CF_DOR_IMM,
    DSP_JIT_CF_ENDDO,
    /* Misc non-parallel ops. ANDI / ORI manipulate SR / OMR
     * bits in-place; LUA / LUA_REL compute Rn + offset into
     * Rn / Nn. Single-word, no branch — none of them change
     * pc on their own (the common epilogue handles pc += 1). */
    DSP_JIT_CF_ANDI,
    DSP_JIT_CF_ORI,
    DSP_JIT_CF_LUA,
    DSP_JIT_CF_LUA_REL,
    /* Effective-address (Rn-based) CF */
    DSP_JIT_CF_JMP_EA,
    DSP_JIT_CF_JSR_EA,
    DSP_JIT_CF_JCC_EA,
    DSP_JIT_CF_JSCC_EA,
    /* Bit-test CF (jclr / jset / jsclr / jsset) — 4 addressing
     * variants each: _aa (direct), _ea (calc_ea), _pp
     * (peripheral), _reg (register). */
    DSP_JIT_CF_JCLR_AA,
    DSP_JIT_CF_JCLR_EA,
    DSP_JIT_CF_JCLR_PP,
    DSP_JIT_CF_JCLR_REG,
    DSP_JIT_CF_JSET_AA,
    DSP_JIT_CF_JSET_EA,
    DSP_JIT_CF_JSET_PP,
    DSP_JIT_CF_JSET_REG,
    DSP_JIT_CF_JSCLR_AA,
    DSP_JIT_CF_JSCLR_EA,
    DSP_JIT_CF_JSCLR_PP,
    DSP_JIT_CF_JSCLR_REG,
    DSP_JIT_CF_JSSET_AA,
    DSP_JIT_CF_JSSET_EA,
    DSP_JIT_CF_JSSET_PP,
    DSP_JIT_CF_JSSET_REG,
    /* PC-relative bit-test (brclr / brset) — only _pp and _reg
     * variants have interpreter handlers in the opcode table. */
    DSP_JIT_CF_BRCLR_PP,
    DSP_JIT_CF_BRCLR_REG,
    DSP_JIT_CF_BRSET_PP,
    DSP_JIT_CF_BRSET_REG,
};
int dsp_jit_helper_classify_cf(void *fn);

/*
 * Long-immediate ALU classifier. The "long" variants of the
 * non-parallel ALU ops (add/sub/cmp/and/or) are 2-word
 * instructions where the second word is a 24-bit immediate the
 * interpreter reads via `read_memory_p(pc+1)` at run time.
 * Because pram writes invalidate this block via
 * dsp_jit_invalidate, the translator can safely bake the
 * immediate at translate time and skip both the run-time
 * `read_memory_p` and the dispatcher round-trip through the C
 * handler.
 *
 * Returns a DSP_JIT_LI_* tag, or DSP_JIT_LI_NONE for handlers
 * without a corresponding inline emitter. EOR long is omitted
 * because there is no interpreter handler for it (the opcode
 * table entry is NULL).
 */
enum {
    DSP_JIT_LI_NONE = 0,
    DSP_JIT_LI_ADD,
    DSP_JIT_LI_SUB,
    DSP_JIT_LI_CMP,
    DSP_JIT_LI_AND,
    DSP_JIT_LI_OR,
};
int dsp_jit_helper_classify_long_imm(void *fn);

#else  /* !DSP_JIT_SUPPORTED */

static inline void dsp_jit_init(dsp_core_t *dsp) { (void)dsp; }
static inline void dsp_jit_finalize(dsp_core_t *dsp) { (void)dsp; }
static inline unsigned int dsp_jit_execute_block(dsp_core_t *dsp) {
    (void)dsp; return 0;
}
static inline void dsp_jit_invalidate(dsp_core_t *dsp, uint32_t addr) {
    (void)dsp; (void)addr;
}
static inline void dsp_jit_invalidate_all(dsp_core_t *dsp) { (void)dsp; }
static inline bool dsp_jit_enabled(void) { return false; }
static inline bool dsp_jit_diff_enabled(void) { return false; }
static inline void dsp_jit_set_enabled_from_config(bool enabled) { (void)enabled; }

#endif  /* DSP_JIT_SUPPORTED */

#endif  /* HW_XBOX_MCPX_APU_DSP_JIT_H */
