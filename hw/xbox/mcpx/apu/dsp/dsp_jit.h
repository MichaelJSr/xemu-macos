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
 * Phase 2 shim — exposes the static emu_ccr_update_e_u_n_z helper
 * from dsp_emu.c.inc so inline ALU kernels in the JIT can BLR it
 * for the post-op E/U/N/Z flag update. See dsp_cpu.c for the
 * trivial wrapper.
 */
void dsp_jit_helper_ccr_e_u_n_z(dsp_core_t *dsp, uint32_t reg0,
                                uint32_t reg1, uint32_t reg2);

/*
 * Phase 2 shim — dsp_rnd56 wrapper for the JIT's MPYR / MACR
 * round path. Unpacks / repacks the 64-bit accumulator convention.
 */
uint64_t dsp_jit_helper_rnd56(dsp_core_t *dsp, uint64_t packed);

/*
 * Phase 5 shims — expose the static emu_calc_cc + dsp_stack_push /
 * dsp_stack_pop helpers from dsp_cpu.c so the JIT's inline control-
 * flow emitters can BLR them for the conditional / subroutine ops.
 *
 * dsp_jit_helper_stack_push wraps dsp_stack_push(dsp, pc, sr, 0) —
 * the "sshOnly = 0" form that every control-flow handler uses.
 */
int  dsp_jit_helper_calc_cc(dsp_core_t *dsp, uint32_t cc_code);
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
    DSP_JIT_CF_REP_IMM,
    DSP_JIT_CF_DO_IMM,
    DSP_JIT_CF_DOR_IMM,
    DSP_JIT_CF_ENDDO,
};
int dsp_jit_helper_classify_cf(void *fn);

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
