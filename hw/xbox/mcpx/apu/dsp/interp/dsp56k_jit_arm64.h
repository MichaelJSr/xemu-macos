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
#ifndef HW_XBOX_MCPX_APU_DSP56K_JIT_ARM64_H
#define HW_XBOX_MCPX_APU_DSP56K_JIT_ARM64_H

#include "dsp_cpu.h"

#if defined(__APPLE__) && defined(__aarch64__)
#define DSP56K_JIT_SUPPORTED 1
#else
#define DSP56K_JIT_SUPPORTED 0
#endif

#if DSP56K_JIT_SUPPORTED

/*
 * Initialize / finalize per-core JIT state (code buffer, block
 * cache, translation context). Called from dsp_init / dsp_destroy.
 */
void dsp56k_jit_init(dsp_core_t *dsp);
void dsp56k_jit_finalize(dsp_core_t *dsp);

/*
 * Execute translated code starting at dsp->pc. Returns the number
 * of cycles consumed across the block (sum of dsp->instr_cycle per
 * op). On any fallback-required condition (unsupported op on first
 * translation, interrupt pending on entry, etc.) the function may
 * return 0 without advancing, and the caller should run one
 * interpreter step.
 */
unsigned int dsp56k_jit_execute_block(dsp_core_t *dsp);

/*
 * Invalidate any translated block whose PC range includes addr.
 * Called from dsp56k_write_memory when writing to P-space.
 */
void dsp56k_jit_invalidate(dsp_core_t *dsp, uint32_t addr);

/*
 * Invalidate all translated blocks (e.g., on DSP reset, bootstrap
 * reload of pram, or code-cache full).
 */
void dsp56k_jit_invalidate_all(dsp_core_t *dsp);

/*
 * Runtime flags. Parsed from XEMU_DSP_JIT / XEMU_DSP_JIT_DIFF on
 * first call. Cached thereafter.
 */
bool dsp56k_jit_enabled(void);
bool dsp56k_jit_diff_enabled(void);

/*
 * Sentinel (debug / bisect) harness for the deferred round-4
 * cur_inst skip optimization. XEMU_DSP_JIT_SENTINEL=1 re-applies
 * the skip but substitutes DSP56K_JIT_SENTINEL_POISON (0x00adbeef)
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
#define DSP56K_JIT_SENTINEL_CURINST  (1u << 0)
#define DSP56K_JIT_SENTINEL_POISON   0x00adbeefu
bool dsp56k_jit_sentinel_enabled(uint32_t bit);

/*
 * Force-apply the deferred round-4 cur_inst skip literally
 * (no sentinel poison). Use XEMU_DSP_JIT_FORCE=1 to reproduce
 * the underlying regression on demand while bisecting. If a
 * future fix makes FORCE=1 run clean, the cur_inst skip can be
 * re-landed permanently.
 */
#define DSP56K_JIT_FORCE_CURINST_SKIP  (1u << 0)
bool dsp56k_jit_force_enabled(uint32_t bit);

/*
 * Called from APU init (gp_ep.c) with g_config.audio.dsp_jit.enabled.
 * Keeps the JIT source decoupled from ui/xemu-settings.h so the
 * standalone DSP test binary (which links libdsp.a without the
 * xemu settings library) continues to link.
 */
void dsp56k_jit_set_enabled_from_config(bool enabled);

/*
 * Internal shims implemented in dsp_cpu.c and called from translated
 * code in dsp56k_jit_arm64.c. Declared here only so both TUs agree on types.
 * Not part of the public API — do not call from outside the JIT.
 */
void dsp56k_jit_helper_postexecute_update_pc(dsp_core_t *dsp);
void dsp56k_jit_helper_postexecute_interrupts(dsp_core_t *dsp);

/* Pin-audit diagnostic (XEMU_DSP_JIT_PIN_AUDIT=1). Called by the
 * JIT's per-op check when x26 (A) or x27 (B) pin diverges from
 * registers[]. See dsp_cpu.c for arg semantics. */
void dsp56k_jit_helper_pin_audit_fail(dsp_core_t *dsp, uint32_t which,
                                   uint32_t pin_lo, uint32_t pin_hi,
                                   uint32_t pc, uint32_t inst);
typedef void (*dsp_emu_func_t)(dsp_core_t *dsp);
dsp_emu_func_t dsp56k_jit_helper_lookup_emu(uint32_t inst);
uint32_t dsp56k_jit_helper_inst_length(uint32_t inst);
bool dsp56k_jit_helper_is_terminator(void *fn);

/* Phase 4 shims (parmove inlining). See dsp_cpu.c for definitions. */
int  dsp56k_jit_helper_pm_read_accu24(dsp_core_t *dsp, int numreg, uint32_t *dest);
int  dsp56k_jit_helper_calc_ea(dsp_core_t *dsp, uint32_t ea_mode, uint32_t *dst_addr);
void dsp56k_jit_helper_update_rn(dsp_core_t *dsp, uint32_t numreg, int16_t modifier);
dsp_emu_func_t dsp56k_jit_helper_lookup_alu(uint32_t inst);
bool dsp56k_jit_helper_alu_is_move(dsp_emu_func_t fn);
void dsp56k_jit_helper_pm_4x(dsp_core_t *dsp);

/*
 * Phase 2 shim — dsp_rnd56 wrapper for the JIT's MPYR / MACR
 * round path. Unpacks / repacks the 64-bit accumulator convention.
 */
uint64_t dsp56k_jit_helper_rnd56(dsp_core_t *dsp, uint64_t packed);

/*
 * Phase 5 shims — expose the static dsp_stack_push / dsp_stack_pop
 * helpers from dsp_cpu.c so the JIT's inline control-flow emitters
 * can BLR them for the subroutine-call / return ops.
 *
 * dsp56k_jit_helper_stack_push wraps dsp_stack_push(dsp, pc, sr, 0) —
 * the "sshOnly = 0" form that every control-flow handler uses.
 *
 * Round 2 removed the emu_calc_cc shim: the 16-case cc switch is
 * now materialised inline at translate time, since cc_code is a
 * known bitfield of the instruction word. See emit_cf_calc_cc in
 * dsp56k_jit_arm64.c.
 */
void dsp56k_jit_helper_stack_push(dsp_core_t *dsp, uint32_t newpc,
                               uint32_t newsr);
void dsp56k_jit_helper_stack_pop(dsp_core_t *dsp, uint32_t *newpc,
                              uint32_t *newsr);

/*
 * Phase 5 shim — classifies an emu_func_t control-flow handler so
 * the JIT can pick an inline emitter without direct access to the
 * file-static emu_* symbols. Returns one of the DSP56K_JIT_CF_* tags
 * below, or DSP56K_JIT_CF_NONE for handlers not covered by the inline
 * control-flow path (fallback to BLR).
 */
enum {
    DSP56K_JIT_CF_NONE = 0,
    /* Absolute / immediate CF */
    DSP56K_JIT_CF_JMP_IMM,
    DSP56K_JIT_CF_JSR_IMM,
    DSP56K_JIT_CF_RTS,
    DSP56K_JIT_CF_RTI,
    DSP56K_JIT_CF_BRA_IMM,
    DSP56K_JIT_CF_BRA_LONG,
    DSP56K_JIT_CF_BSR_IMM,
    DSP56K_JIT_CF_BSR_LONG,
    DSP56K_JIT_CF_JCC_IMM,
    DSP56K_JIT_CF_JSCC_IMM,
    DSP56K_JIT_CF_BCC_IMM,
    DSP56K_JIT_CF_BCC_LONG,
    /* Loop ops */
    DSP56K_JIT_CF_REP_IMM,
    DSP56K_JIT_CF_REP_AA,
    DSP56K_JIT_CF_REP_EA,
    DSP56K_JIT_CF_REP_REG,
    DSP56K_JIT_CF_DO_IMM,
    DSP56K_JIT_CF_DO_AA,
    DSP56K_JIT_CF_DO_EA,
    DSP56K_JIT_CF_DO_REG,
    DSP56K_JIT_CF_DOR_IMM,
    DSP56K_JIT_CF_DOR_REG,
    DSP56K_JIT_CF_ENDDO,
    /* Misc non-parallel ops. ANDI / ORI manipulate SR / OMR
     * bits in-place; LUA / LUA_REL compute Rn + offset into
     * Rn / Nn. Single-word, no branch — none of them change
     * pc on their own (the common epilogue handles pc += 1). */
    DSP56K_JIT_CF_ANDI,
    DSP56K_JIT_CF_ORI,
    DSP56K_JIT_CF_LUA,
    DSP56K_JIT_CF_LUA_REL,
    DSP56K_JIT_CF_TCC,
    DSP56K_JIT_CF_MOVEC_IMM,
    DSP56K_JIT_CF_MOVEC_REG,
    DSP56K_JIT_CF_MOVEC_AA,
    DSP56K_JIT_CF_MOVEC_EA,
    DSP56K_JIT_CF_MOVEP_0,
    DSP56K_JIT_CF_MOVEP_1,
    DSP56K_JIT_CF_MOVEP_23,
    DSP56K_JIT_CF_MOVEP_X_QQ,
    DSP56K_JIT_CF_MOVEM_AA,
    DSP56K_JIT_CF_MOVEM_EA,
    DSP56K_JIT_CF_NOP,
    /* Phase 9 tail (data-driven, see cf_fallback buckets). */
    DSP56K_JIT_CF_INC,
    DSP56K_JIT_CF_DEC,
    DSP56K_JIT_CF_ADD_IMM,
    DSP56K_JIT_CF_SUB_IMM,
    DSP56K_JIT_CF_CMP_IMM,
    DSP56K_JIT_CF_AND_IMM,
    DSP56K_JIT_CF_MOVE_X_LONG,
    DSP56K_JIT_CF_MOVE_X_IMM,
    DSP56K_JIT_CF_MOVE_Y_IMM,
    /* Shift-immediate family (99.7% of remaining cf_fallback at the
     * time of landing). ASL/ASR operate on the full 56-bit accu and
     * go through the E/U/N/Z ccr shim; LSL is A1-only and only
     * updates C/N/Z/V. All three bake the shift count at translate
     * time from inst bits. */
    DSP56K_JIT_CF_ASL_IMM,
    DSP56K_JIT_CF_ASR_IMM,
    DSP56K_JIT_CF_LSL_IMM,
    /* Unsigned 56-bit compare of the destination accu vs either
     * the other accu (read through pm_read_accu24's saturate/
     * scale pipeline) or X0/X1/Y0/Y1 (direct 24-bit). Updates
     * SR.C (borrow), SR.N, SR.Z; clears SR.V. */
    DSP56K_JIT_CF_CMPU,
    /* Effective-address (Rn-based) CF */
    DSP56K_JIT_CF_JMP_EA,
    DSP56K_JIT_CF_JSR_EA,
    DSP56K_JIT_CF_JCC_EA,
    DSP56K_JIT_CF_JSCC_EA,
    /* Bit-test CF (jclr / jset / jsclr / jsset) — 4 addressing
     * variants each: _aa (direct), _ea (calc_ea), _pp
     * (peripheral), _reg (register). */
    DSP56K_JIT_CF_JCLR_AA,
    DSP56K_JIT_CF_JCLR_EA,
    DSP56K_JIT_CF_JCLR_PP,
    DSP56K_JIT_CF_JCLR_REG,
    DSP56K_JIT_CF_JSET_AA,
    DSP56K_JIT_CF_JSET_EA,
    DSP56K_JIT_CF_JSET_PP,
    DSP56K_JIT_CF_JSET_REG,
    DSP56K_JIT_CF_JSCLR_AA,
    DSP56K_JIT_CF_JSCLR_EA,
    DSP56K_JIT_CF_JSCLR_PP,
    DSP56K_JIT_CF_JSCLR_REG,
    DSP56K_JIT_CF_JSSET_AA,
    DSP56K_JIT_CF_JSSET_EA,
    DSP56K_JIT_CF_JSSET_PP,
    DSP56K_JIT_CF_JSSET_REG,
    /* PC-relative bit-test (brclr / brset) — only _pp and _reg
     * variants have interpreter handlers in the opcode table. */
    DSP56K_JIT_CF_BRCLR_PP,
    DSP56K_JIT_CF_BRCLR_REG,
    DSP56K_JIT_CF_BRSET_PP,
    DSP56K_JIT_CF_BRSET_REG,
};
int dsp56k_jit_helper_classify_cf(void *fn);

/*
 * Bit-manipulation sub-classifier (bset / bclr / bchg / btst across
 * aa / ea / pp / reg addressing). Returns -1 if `fn` is not one of
 * the 16 handlers, else (op_kind << 2) | source_kind with:
 *   op_kind:     0 = bset, 1 = bclr, 2 = bchg, 3 = btst
 *   source_kind: 0 = aa,   1 = ea,   2 = pp,   3 = reg
 * Used by the JIT to emit these inline instead of through the
 * generic BLR fallback (previously the last fallback bucket with
 * meaningful steady-state volume).
 */
int dsp56k_jit_helper_classify_bit_manip(void *fn);

#define DSP56K_JIT_BM_OP(kind)     (((kind) >> 2) & 3)
#define DSP56K_JIT_BM_SOURCE(kind) ((kind) & 3)

/*
 * Fallback-handler classifier. Returns a small integer identifying
 * `fn` if it's a non-inlined non-parallel handler the JIT would
 * BLR-fallback for; 0 (DSP56K_JIT_FB_OTHER) otherwise. Used by the JIT
 * to keep per-handler BLR-fallback stats so we can see what's hot
 * in the cf_fallback bucket and prioritise the next inline work.
 *
 * Kept separate from dsp56k_jit_helper_classify_cf (which classifies
 * handlers the JIT DOES inline) to avoid inflating that enum with
 * handlers we don't emit inline code for.
 */
enum {
    DSP56K_JIT_FB_OTHER = 0,
    DSP56K_JIT_FB_MOVEP_1,
    DSP56K_JIT_FB_MOVEP_23,
    DSP56K_JIT_FB_MOVEP_X_QQ,
    DSP56K_JIT_FB_DIV,
    DSP56K_JIT_FB_NORM,
    DSP56K_JIT_FB_STOP,
    DSP56K_JIT_FB_WAIT,
    DSP56K_JIT_FB_RESET,
    DSP56K_JIT_FB_NOP,
    DSP56K_JIT_FB_ILLEGAL,
    DSP56K_JIT_FB_UNDEFINED,
    /* Phase 9 tail — grouped buckets to break down what's in the
     * 74% "other" slice so we know what to inline next. Each
     * bucket aggregates a family of related opcodes. */
    DSP56K_JIT_FB_BIT_MANIP,          /* bchg / bclr / bset / btst × aa/ea/pp/reg */
    DSP56K_JIT_FB_SHORT_IMM_ALU,      /* add_imm / sub_imm / cmp_imm / and_imm */
    DSP56K_JIT_FB_SHIFT_IMM,          /* asl_imm / asr_imm / lsl_imm */
    DSP56K_JIT_FB_INC_DEC,            /* inc / dec */
    DSP56K_JIT_FB_CMPU,
    DSP56K_JIT_FB_MPYI,
    DSP56K_JIT_FB_MOVE_EXTENDED,      /* move_x_long / y_long / x_imm / y_imm */
    DSP56K_JIT_FB_MAX,
};
int dsp56k_jit_helper_classify_fallback(void *fn);
const char *dsp56k_jit_helper_fallback_name(int kind);

/* Pack registers_tcc[field][0] / [1] into a single u32:
 *   bits [7:0]  = src reg index
 *   bits [15:8] = dest reg index
 * Called at translate time to decode tcc instructions without
 * exposing the static registers_tcc[] table. */
uint32_t dsp56k_jit_helper_tcc_regs(uint32_t field);

/* Look up registers_mask[numreg] for the JIT's movec_imm / movec
 * emitters. Returns bit width (0-24); 0 for NULL / reserved slots. */
int dsp56k_jit_helper_reg_mask_bits(int numreg);

/*
 * Long-immediate ALU classifier. The "long" variants of the
 * non-parallel ALU ops (add/sub/cmp/and/or) are 2-word
 * instructions where the second word is a 24-bit immediate the
 * interpreter reads via `read_memory_p(pc+1)` at run time.
 * Because pram writes invalidate this block via
 * dsp56k_jit_invalidate, the translator can safely bake the
 * immediate at translate time and skip both the run-time
 * `read_memory_p` and the dispatcher round-trip through the C
 * handler.
 *
 * Returns a DSP56K_JIT_LI_* tag, or DSP56K_JIT_LI_NONE for handlers
 * without a corresponding inline emitter. EOR long is omitted
 * because there is no interpreter handler for it (the opcode
 * table entry is NULL).
 */
enum {
    DSP56K_JIT_LI_NONE = 0,
    DSP56K_JIT_LI_ADD,
    DSP56K_JIT_LI_SUB,
    DSP56K_JIT_LI_CMP,
    DSP56K_JIT_LI_AND,
    DSP56K_JIT_LI_OR,
};
int dsp56k_jit_helper_classify_long_imm(void *fn);

#else  /* !DSP56K_JIT_SUPPORTED */

static inline void dsp56k_jit_init(dsp_core_t *dsp) { (void)dsp; }
static inline void dsp56k_jit_finalize(dsp_core_t *dsp) { (void)dsp; }
static inline unsigned int dsp56k_jit_execute_block(dsp_core_t *dsp) {
    (void)dsp; return 0;
}
static inline void dsp56k_jit_invalidate(dsp_core_t *dsp, uint32_t addr) {
    (void)dsp; (void)addr;
}
static inline void dsp56k_jit_invalidate_all(dsp_core_t *dsp) { (void)dsp; }
static inline bool dsp56k_jit_enabled(void) { return false; }
static inline bool dsp56k_jit_diff_enabled(void) { return false; }
static inline void dsp56k_jit_set_enabled_from_config(bool enabled) { (void)enabled; }

#endif  /* DSP56K_JIT_SUPPORTED */

#endif  /* HW_XBOX_MCPX_APU_DSP56K_JIT_ARM64_H */
