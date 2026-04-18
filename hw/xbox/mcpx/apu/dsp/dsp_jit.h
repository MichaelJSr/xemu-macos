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

#endif  /* DSP_JIT_SUPPORTED */

#endif  /* HW_XBOX_MCPX_APU_DSP_JIT_H */
