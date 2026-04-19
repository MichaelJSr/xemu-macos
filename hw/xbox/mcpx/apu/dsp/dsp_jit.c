/*
 * MCPX APU DSP (DSP56300 / M56001) JIT — Apple Silicon (ARM64)
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Design summary (see docs/dsp-jit-design.md for the full write-up):
 *
 *   The JIT translates DSP basic blocks into ARM64 machine code.
 *   Each translated DSP instruction becomes a short "stub" that:
 *     1. Writes the pre-decoded instruction word into dsp->cur_inst,
 *        resets dsp->cur_inst_len to 1 and dsp->instr_cycle to 2.
 *     2. Either calls the same emu_func_t C handler the interpreter
 *        would, OR (Phase 4) runs inline ARM64 code for parmove +
 *        addressing / memory fast paths and only BLRs to the
 *        opcodes_alu[] ALU kernel.
 *     3. Calls dsp_postexecute_update_pc (PC advance + DO-loop edge).
 *     4. Accumulates dsp->instr_cycle into dsp->num_inst.
 *     5. Runtime-checks for early block exit:
 *          - interrupt_counter > 0       (pending IRQ)
 *          - is_idle                     (stop/wait)
 *          - loop_rep                    (REP entered; block boundary)
 *          - jit_exit_block_request      (self-modifying-code hit)
 *          - pc != expected_next_pc      (branch taken)
 *
 *   Block length is bounded (MAX_OPS_PER_BLOCK). On block exit we
 *   return to the C dispatcher, which re-looks-up a block for the
 *   new PC. Block chaining across known fall-through is a Phase 5
 *   optimisation and is NOT yet done here.
 *
 *   Correctness is equivalent to the interpreter by construction
 *   (same emu_* handlers called; Phase 4 parmove inlines decode the
 *   same bits the interpreter does). Speedup vs the interpreter is
 *   primarily from amortising dispatch overhead, eliminating the
 *   pram_opcache load, and avoiding the per-op tracing / disasm
 *   check. Remaining speedups target the ALU kernel and control
 *   flow in later phases.
 *
 *   A differential-validation harness (XEMU_DSP_JIT_DIFF=1) runs
 *   the interpreter on a private copy of dsp_core_t on a dedicated
 *   worker thread (`mcpx.dsp_diff`) and byte-compares against the
 *   JIT's post-block state, aborting on any divergence. APU-thread
 *   cost is bounded by a per-translation "already validated" flag
 *   so a deterministic block is only validated once — total work
 *   ≈ number of unique block translations rather than blocks
 *   executed per second. See the Differential validator block in
 *   DspJitState / DiffQueue below.
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "dsp_cpu.h"
#include "dsp_jit.h"

#if DSP_JIT_SUPPORTED

#include <sys/mman.h>

/*
 * dsp_postexecute_update_pc and dsp_postexecute_interrupts are
 * file-static in dsp_cpu.c. We call them via small shims exposed
 * from that file (see dsp_jit_helper_* at the bottom of dsp_cpu.c).
 */
void dsp_jit_helper_postexecute_update_pc(dsp_core_t *dsp);
void dsp_jit_helper_postexecute_interrupts(dsp_core_t *dsp);

typedef void (*emu_func_t)(dsp_core_t *dsp);

/* --------------------------------------------------------------- *
 * Runtime flags (parsed once)
 * --------------------------------------------------------------- */

static bool g_jit_parsed;
static bool g_jit_enabled;
static bool g_jit_diff;
static bool g_jit_diff_sync;         /* XEMU_DSP_JIT_DIFF_SYNC=1: on-thread compare */
static uint32_t g_jit_diff_sample;   /* 1 = every block; N>1 = every Nth. */
static uint64_t g_jit_diff_max;      /* 0 = unlimited; else stop after N checks */
static bool g_jit_stats;

/*
 * Set by the main xemu binary (gp_ep.c / apu.c) during APU init
 * based on g_config.audio.dsp_jit.enabled. Avoids a hard link-time
 * dependency on ui/xemu-settings.cc — the tests/xbox/dsp test
 * binary statically links libdsp.a alone and doesn't pull in
 * g_config, so reading config directly from here would fail to
 * link those tests. The XEMU_DSP_JIT env var still overrides if
 * set.
 */
static bool g_jit_config_enabled_override;
static bool g_jit_config_set;

void dsp_jit_set_enabled_from_config(bool enabled)
{
    g_jit_config_enabled_override = enabled;
    g_jit_config_set = true;
}

static void parse_flags_once(void)
{
    if (g_jit_parsed) {
        return;
    }
    g_jit_parsed = true;

    /*
     * Primary source of truth: dsp_jit_set_enabled_from_config()
     * populates this from g_config.audio.dsp_jit.enabled (the
     * menu toggle) during APU init. XEMU_DSP_JIT env var is kept
     * as a developer override so CI / bisect scripts can force
     * JIT on/off without rewriting the TOML.
     */
    g_jit_enabled = g_jit_config_set && g_jit_config_enabled_override;

    const char *e;
    e = getenv("XEMU_DSP_JIT");
    if (e && e[0]) {
        g_jit_enabled = (e[0] == '1');
    }

    /* XEMU_DSP_JIT_DIFF (dev tool): bit-exact validate JIT output
     * against interpreter. No config-spec entry — it's only useful
     * during JIT bring-up / regression tracking.
     *   "0"        — diff mode off (default)
     *   "1"        — diff-check every block (2-10x slowdown)
     *   "N" (N>=2) — diff-check every Nth block (sampling)
     * Capped at 1e6 to keep the modulo cheap. */
    e = getenv("XEMU_DSP_JIT_DIFF");
    if (e && e[0]) {
        long n = strtol(e, NULL, 0);
        if (n >= 1 && n <= 1000000) {
            g_jit_diff = true;
            g_jit_diff_sample = (uint32_t)n;
        }
    }

    /*
     * Optional total-validation cap. The per-translation gate
     * (DspJitBlock.diff_checked) already bounds validator work to
     * ~N_unique_block_translations — typically a few hundred — so
     * most users never need a cap. Unset by default. Set
     * XEMU_DSP_JIT_DIFF_MAX=N to stop validating after N enqueues
     * (useful for CI "validate first N blocks, then run free"
     * gating). The recommended knob for day-to-day runs is
     * XEMU_DSP_JIT_DIFF=N sampling.
     */
    e = getenv("XEMU_DSP_JIT_DIFF_MAX");
    if (e && e[0]) {
        long long n = strtoll(e, NULL, 0);
        if (n > 0) {
            g_jit_diff_max = (uint64_t)n;
        }
    }

    /* Force synchronous (on-thread) validation. Default is async
     * via the validator worker thread. Use SYNC for bring-up
     * debugging where you want the abort to fire immediately on
     * divergence rather than "eventually when the validator gets
     * around to this block". Sync mode re-introduces APU-thread
     * latency and is NOT recommended for extended runs. */
    e = getenv("XEMU_DSP_JIT_DIFF_SYNC");
    g_jit_diff_sync = (e && e[0] == '1');

    e = getenv("XEMU_DSP_JIT_STATS");
    g_jit_stats = (e && e[0] == '1');

    if (g_jit_enabled) {
        const char *mode_desc = "";
        if (g_jit_diff && g_jit_diff_sync && g_jit_diff_sample == 1) {
            mode_desc = " (DIFF=sync, every unique translation)";
        } else if (g_jit_diff && g_jit_diff_sync) {
            mode_desc = " (DIFF=sync, sampled)";
        } else if (g_jit_diff && g_jit_diff_sample == 1) {
            mode_desc = " (DIFF=async, every unique translation)";
        } else if (g_jit_diff) {
            mode_desc = " (DIFF=async, sampled)";
        }
        fprintf(stderr, "xemu: DSP JIT enabled%s%s\n",
                mode_desc, g_jit_stats ? " (stats)" : "");
    }
}

bool dsp_jit_enabled(void)
{
    parse_flags_once();
    return g_jit_enabled;
}

bool dsp_jit_diff_enabled(void)
{
    parse_flags_once();
    return g_jit_diff;
}

/*
 * emu_* symbols are file-static in dsp_emu.c.inc. Classification,
 * opcode lookup, and length detection are provided via shims at the
 * bottom of dsp_cpu.c where those symbols and their dispatch tables
 * are in scope. See declarations below the emitter section.
 */

/* --------------------------------------------------------------- *
 * ARM64 emitter primitives
 *
 * Tiny hand-rolled emitter targeting the subset of ARM64 we need:
 *   MOVZ / MOVK (for 16/32/48/64-bit immediates)
 *   LDR (imm) / LDRH / LDRB / STR (imm) / STRH / STRB
 *   ADD / SUB immediate
 *   CMP immediate / CMP register
 *   CBZ / CBNZ
 *   B.cond (pc-relative, patched after label resolution)
 *   BLR / BR / RET
 *   STP / LDP pre/post-indexed (for FP/LR save)
 *
 * All encodings verified against ARMv8-A reference manual, using
 * little-endian instruction stream. Opcodes are emitted as uint32_t.
 * --------------------------------------------------------------- */

typedef struct {
    uint32_t *buf;       /* write head */
    uint32_t *buf_start;
    uint32_t *buf_end;   /* exclusive */
} ArmEmit;

static inline void emit_u32(ArmEmit *e, uint32_t insn)
{
    /* Caller is responsible for ensuring buf < buf_end before translating */
    *e->buf++ = insn;
}

/* MOVZ Wd, #imm16, LSL #shift    (shift ∈ {0,16})
 * MOVZ Xd, #imm16, LSL #shift    (shift ∈ {0,16,32,48})
 */
static inline void emit_movz_w(ArmEmit *e, int rd, uint16_t imm, int shift)
{
    uint32_t hw = (shift / 16) & 3;
    emit_u32(e, 0x52800000u | (hw << 21) | ((uint32_t)imm << 5) | (rd & 0x1f));
}

static inline void emit_movz_x(ArmEmit *e, int rd, uint16_t imm, int shift)
{
    uint32_t hw = (shift / 16) & 3;
    emit_u32(e, 0xd2800000u | (hw << 21) | ((uint32_t)imm << 5) | (rd & 0x1f));
}

/* MOVK Wd / Xd, #imm16, LSL #shift */
static inline void emit_movk_w(ArmEmit *e, int rd, uint16_t imm, int shift)
{
    uint32_t hw = (shift / 16) & 3;
    emit_u32(e, 0x72800000u | (hw << 21) | ((uint32_t)imm << 5) | (rd & 0x1f));
}

static inline void emit_movk_x(ArmEmit *e, int rd, uint16_t imm, int shift)
{
    uint32_t hw = (shift / 16) & 3;
    emit_u32(e, 0xf2800000u | (hw << 21) | ((uint32_t)imm << 5) | (rd & 0x1f));
}

/* Emit a 32-bit immediate into Wd via MOVZ+MOVK */
static void emit_mov_imm32(ArmEmit *e, int rd, uint32_t imm)
{
    emit_movz_w(e, rd, imm & 0xffff, 0);
    if ((imm >> 16) != 0) {
        emit_movk_w(e, rd, (imm >> 16) & 0xffff, 16);
    }
}

/* Emit a 64-bit immediate into Xd via MOVZ+MOVK chain */
static void emit_mov_imm64(ArmEmit *e, int rd, uint64_t imm)
{
    emit_movz_x(e, rd, imm & 0xffff, 0);
    if (((imm >> 16) & 0xffff) != 0) {
        emit_movk_x(e, rd, (imm >> 16) & 0xffff, 16);
    }
    if (((imm >> 32) & 0xffff) != 0) {
        emit_movk_x(e, rd, (imm >> 32) & 0xffff, 32);
    }
    if (((imm >> 48) & 0xffff) != 0) {
        emit_movk_x(e, rd, (imm >> 48) & 0xffff, 48);
    }
}

/*
 * LDR Wd, [Xn, #imm]   — 32-bit load with unsigned-scaled imm.
 * offset must be a multiple of 4, in range [0, 16380].
 */
static inline void emit_ldr_w_imm(ArmEmit *e, int rd, int rn, unsigned int off)
{
    assert((off & 3) == 0 && off <= 16380);
    uint32_t imm12 = off / 4;
    emit_u32(e, 0xb9400000u | (imm12 << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* LDR Xd, [Xn, #imm]   — 64-bit load, offset multiple of 8. */
G_GNUC_UNUSED static inline void emit_ldr_x_imm(ArmEmit *e, int rd, int rn, unsigned int off)
{
    assert((off & 7) == 0 && off <= 32760);
    uint32_t imm12 = off / 8;
    emit_u32(e, 0xf9400000u | (imm12 << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* LDRH Wd, [Xn, #imm]  — 16-bit zero-extending load, offset mul. of 2. */
static inline void emit_ldrh_imm(ArmEmit *e, int rd, int rn, unsigned int off)
{
    assert((off & 1) == 0 && off <= 8190);
    uint32_t imm12 = off / 2;
    emit_u32(e, 0x79400000u | (imm12 << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* LDRB Wd, [Xn, #imm] */
static inline void emit_ldrb_imm(ArmEmit *e, int rd, int rn, unsigned int off)
{
    assert(off <= 4095);
    uint32_t imm12 = off;
    emit_u32(e, 0x39400000u | (imm12 << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* STR Wd, [Xn, #imm] */
static inline void emit_str_w_imm(ArmEmit *e, int rs, int rn, unsigned int off)
{
    assert((off & 3) == 0 && off <= 16380);
    uint32_t imm12 = off / 4;
    emit_u32(e, 0xb9000000u | (imm12 << 10) | ((rn & 0x1f) << 5) | (rs & 0x1f));
}

/* STRH Wd, [Xn, #imm] */
static inline void emit_strh_imm(ArmEmit *e, int rs, int rn, unsigned int off)
{
    assert((off & 1) == 0 && off <= 8190);
    uint32_t imm12 = off / 2;
    emit_u32(e, 0x79000000u | (imm12 << 10) | ((rn & 0x1f) << 5) | (rs & 0x1f));
}

/* STRB Wd, [Xn, #imm]  — 8-bit store, imm in [0, 4095]. */
static inline void emit_strb_imm(ArmEmit *e, int rs, int rn, unsigned int off)
{
    assert(off <= 4095);
    emit_u32(e, 0x39000000u | (off << 10) | ((rn & 0x1f) << 5) | (rs & 0x1f));
}

/* STRB Wd, [Xn + Xm], used when the offset exceeds 4095. */
static void emit_strb_any(ArmEmit *e, int rs, int rn, int scratch,
                          uint32_t off)
{
    if (off <= 4095) {
        emit_strb_imm(e, rs, rn, off);
        return;
    }
    uint32_t high = off >> 12;
    uint32_t low  = off & 0xfff;
    assert(high <= 0xfff);
    emit_u32(e, 0x91400000u | (high << 10) | ((rn & 0x1f) << 5) | (scratch & 0x1f));
    emit_strb_imm(e, rs, scratch, low);
}

/* STRB WZR, [Xn + off] (any offset). */
static inline void emit_strb_imm_zero(ArmEmit *e, int rn, unsigned int off)
{
    emit_strb_any(e, /*rs=*/31 /* WZR */, rn, /*scratch=*/3 /*SCRATCH*/, off);
}

/* ADD Xd, Xn, #imm (imm must fit in 12 bits, no shift) */
G_GNUC_UNUSED static inline void emit_add_x_imm(ArmEmit *e, int rd, int rn, unsigned int imm)
{
    assert(imm <= 0xfff);
    emit_u32(e, 0x91000000u | (imm << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* ADD Xd, Xn, #imm, LSL #12 */
static inline void emit_add_x_imm12(ArmEmit *e, int rd, int rn, unsigned int imm)
{
    assert(imm <= 0xfff);
    emit_u32(e, 0x91400000u | (imm << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/*
 * Synthesize LDR Wd, [Xn, #off] for arbitrary off in [0, 16 MiB).
 * Uses `scratch` X-register to form the base pointer when off exceeds
 * the direct LDR imm12*4 range.
 */
static void emit_ldr_w_any(ArmEmit *e, int rd, int rn, int scratch,
                           uint32_t off)
{
    assert((off & 3) == 0);
    if (off <= 16380) {
        emit_ldr_w_imm(e, rd, rn, off);
        return;
    }
    uint32_t high = off >> 12;
    uint32_t low  = off & 0xfff;
    assert(high <= 0xfff);
    emit_add_x_imm12(e, scratch, rn, high);
    emit_ldr_w_imm(e, rd, scratch, low);
}

static void emit_str_w_any(ArmEmit *e, int rs, int rn, int scratch,
                           uint32_t off)
{
    assert((off & 3) == 0);
    if (off <= 16380) {
        emit_str_w_imm(e, rs, rn, off);
        return;
    }
    uint32_t high = off >> 12;
    uint32_t low  = off & 0xfff;
    assert(high <= 0xfff);
    emit_add_x_imm12(e, scratch, rn, high);
    emit_str_w_imm(e, rs, scratch, low);
}

static void emit_ldrh_any(ArmEmit *e, int rd, int rn, int scratch,
                          uint32_t off)
{
    assert((off & 1) == 0);
    if (off <= 8190) {
        emit_ldrh_imm(e, rd, rn, off);
        return;
    }
    uint32_t high = off >> 12;
    uint32_t low  = off & 0xfff;
    assert(high <= 0xfff);
    emit_add_x_imm12(e, scratch, rn, high);
    emit_ldrh_imm(e, rd, scratch, low);
}

static void emit_strh_any(ArmEmit *e, int rs, int rn, int scratch,
                          uint32_t off)
{
    assert((off & 1) == 0);
    if (off <= 8190) {
        emit_strh_imm(e, rs, rn, off);
        return;
    }
    uint32_t high = off >> 12;
    uint32_t low  = off & 0xfff;
    assert(high <= 0xfff);
    emit_add_x_imm12(e, scratch, rn, high);
    emit_strh_imm(e, rs, scratch, low);
}

static void emit_ldrb_any(ArmEmit *e, int rd, int rn, int scratch,
                          uint32_t off)
{
    if (off <= 4095) {
        emit_ldrb_imm(e, rd, rn, off);
        return;
    }
    uint32_t high = off >> 12;
    uint32_t low  = off & 0xfff;
    assert(high <= 0xfff);
    emit_add_x_imm12(e, scratch, rn, high);
    emit_ldrb_imm(e, rd, scratch, low);
}

/* ADD Wd, Wn, Wm */
static inline void emit_add_w_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x0b000000u | ((rm & 0x1f) << 16) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* MOV Xd, Xn  (alias of ORR Xd, XZR, Xn) */
static inline void emit_mov_x_reg(ArmEmit *e, int rd, int rn)
{
    emit_u32(e, 0xaa0003e0u | ((rn & 0x1f) << 16) | (rd & 0x1f));
}

/* CMP Wn, Wm  (SUBS WZR, Wn, Wm) */
static inline void emit_cmp_w_reg(ArmEmit *e, int rn, int rm)
{
    emit_u32(e, 0x6b00001fu | ((rm & 0x1f) << 16) | ((rn & 0x1f) << 5));
}

/* CBZ Wn, #imm19     — pc-relative branch if Wn == 0 */
static inline void emit_cbz_w(ArmEmit *e, int rn, int32_t off_bytes)
{
    int32_t imm19 = off_bytes >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    emit_u32(e, 0x34000000u | (((uint32_t)imm19 & 0x7ffff) << 5) | (rn & 0x1f));
}

/* CBNZ Wn, #imm19 */
static inline void emit_cbnz_w(ArmEmit *e, int rn, int32_t off_bytes)
{
    int32_t imm19 = off_bytes >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    emit_u32(e, 0x35000000u | (((uint32_t)imm19 & 0x7ffff) << 5) | (rn & 0x1f));
}

/* B.cond #imm19   — condition encoded in lowest nibble */
#define ARM_COND_EQ 0x0
#define ARM_COND_NE 0x1
static inline void emit_bcond(ArmEmit *e, int cond, int32_t off_bytes)
{
    int32_t imm19 = off_bytes >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    emit_u32(e, 0x54000000u | (((uint32_t)imm19 & 0x7ffff) << 5) | (cond & 0xf));
}

/* B #imm26  — unconditional PC-relative branch, ±128 MiB. */
static inline void emit_b(ArmEmit *e, int32_t off_bytes)
{
    int32_t imm26 = off_bytes >> 2;
    assert(imm26 >= -(1 << 25) && imm26 < (1 << 25));
    emit_u32(e, 0x14000000u | ((uint32_t)imm26 & 0x03ffffff));
}

/* TBNZ Wt, #bit, #imm14     — pc-relative branch if bit set. */
static inline void emit_tbnz_w(ArmEmit *e, int rt, int bit, int32_t off_bytes)
{
    int32_t imm14 = off_bytes >> 2;
    assert(imm14 >= -(1 << 13) && imm14 < (1 << 13));
    assert(bit >= 0 && bit < 32);
    emit_u32(e, 0x37000000u | ((uint32_t)(bit & 0x1f) << 19) |
                (((uint32_t)imm14 & 0x3fff) << 5) | (rt & 0x1f));
}

/* ORR Wd, Wn, Wm  (shifted-register form, shift #0) */
static inline void emit_orr_w_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x2a000000u | ((rm & 0x1f) << 16) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* MOV Wd, Wn (alias of ORR Wd, WZR, Wn) */
static inline void emit_mov_w_reg(ArmEmit *e, int rd, int rn)
{
    emit_u32(e, 0x2a0003e0u | ((rn & 0x1f) << 16) | (rd & 0x1f));
}

/*
 * Shift / bitfield instructions — all lowered via UBFM / SBFM.
 *
 * LSL Wd, Wn, #s   => UBFM Wd, Wn, #((32-s)%32), #(31-s)
 * LSR Wd, Wn, #s   => UBFM Wd, Wn, #s, #31
 * UBFX Wd, Wn, #lsb, #width => UBFM Wd, Wn, #lsb, #(lsb+width-1)
 * ASR Wd, Wn, #s   => SBFM Wd, Wn, #s, #31
 *
 * UBFM 32-bit encoding: 0x53000000 | (immr << 16) | (imms << 10) | (Rn << 5) | Rd
 * SBFM 32-bit encoding: 0x13000000 | (immr << 16) | (imms << 10) | (Rn << 5) | Rd
 */
static inline void emit_lsl_w_imm(ArmEmit *e, int rd, int rn, int shift)
{
    assert(shift >= 0 && shift < 32);
    uint32_t immr = (uint32_t)((32 - shift) & 31);
    uint32_t imms = (uint32_t)(31 - shift);
    emit_u32(e, 0x53000000u | (immr << 16) | (imms << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

G_GNUC_UNUSED static inline void emit_lsr_w_imm(ArmEmit *e, int rd, int rn, int shift)
{
    assert(shift >= 0 && shift < 32);
    emit_u32(e, 0x53000000u | ((uint32_t)shift << 16) | (31u << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

G_GNUC_UNUSED static inline void emit_asr_w_imm(ArmEmit *e, int rd, int rn, int shift)
{
    assert(shift >= 0 && shift < 32);
    emit_u32(e, 0x13000000u | ((uint32_t)shift << 16) | (31u << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

static inline void emit_ubfx_w(ArmEmit *e, int rd, int rn, int lsb, int width)
{
    assert(lsb >= 0 && width > 0 && (lsb + width) <= 32);
    emit_u32(e, 0x53000000u | ((uint32_t)lsb << 16) |
                ((uint32_t)(lsb + width - 1) << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* ADD Wd, Wn, #imm (imm12, no shift) */
static inline void emit_add_w_imm(ArmEmit *e, int rd, int rn, unsigned int imm)
{
    assert(imm <= 0xfff);
    emit_u32(e, 0x11000000u | (imm << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* SUB Wd, Wn, #imm (imm12, no shift) */
static inline void emit_sub_w_imm(ArmEmit *e, int rd, int rn, unsigned int imm)
{
    assert(imm <= 0xfff);
    emit_u32(e, 0x51000000u | (imm << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* CMP Wn, #imm12 (SUBS WZR, Wn, #imm) */
G_GNUC_UNUSED static inline void emit_cmp_w_imm(ArmEmit *e, int rn, unsigned int imm)
{
    assert(imm <= 0xfff);
    emit_u32(e, 0x7100001fu | (imm << 10) | ((rn & 0x1f) << 5));
}

/* SUB Wd, Wn, Wm (shifted-register, shift #0) */
G_GNUC_UNUSED static inline void emit_sub_w_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x4b000000u | ((rm & 0x1f) << 16) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* NEG Wd, Wm (alias of SUB Wd, WZR, Wm) */
G_GNUC_UNUSED static inline void emit_neg_w(ArmEmit *e, int rd, int rm)
{
    emit_u32(e, 0x4b0003e0u | ((rm & 0x1f) << 16) | (rd & 0x1f));
}

/* ADD Xd, Xn, Xm (shifted-register, LSL #0) */
static inline void emit_add_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x8b000000u | ((rm & 0x1f) << 16) | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* CSEL Wd, Wn, Wm, cond — if cond true, Wd = Wn else Wd = Wm. */
G_GNUC_UNUSED static inline void emit_csel_w(ArmEmit *e, int rd, int rn, int rm, int cond)
{
    emit_u32(e, 0x1a800000u | ((rm & 0x1f) << 16) | ((cond & 0xf) << 12) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/*
 * CSET Wd, cond — Wd = (cond) ? 1 : 0. Alias for
 *   CSINC Wd, WZR, WZR, !cond
 * Encoding (CSINC 32-bit): 0x1a800400 | Rm<<16 | cond<<12 | Rn<<5 | Rd
 * With Rn=Rm=WZR=31 and cond inverted:
 *   0x1a800400 | 31<<16 | (cond^1)<<12 | 31<<5 | Rd
 *   = 0x1a9f07e0 | ((cond^1) & 0xf) << 12 | (Rd & 0x1f)
 */
G_GNUC_UNUSED static inline void emit_cset_w(ArmEmit *e, int rd, int cond)
{
    uint32_t inv = (uint32_t)((cond ^ 1) & 0xf);
    emit_u32(e, 0x1a9f07e0u | (inv << 12) | (rd & 0x1f));
}

/* TBZ Wt, #bit, #imm14 */
G_GNUC_UNUSED static inline void emit_tbz_w(ArmEmit *e, int rt, int bit, int32_t off_bytes)
{
    int32_t imm14 = off_bytes >> 2;
    assert(imm14 >= -(1 << 13) && imm14 < (1 << 13));
    assert(bit >= 0 && bit < 32);
    emit_u32(e, 0x36000000u | ((uint32_t)(bit & 0x1f) << 19) |
                (((uint32_t)imm14 & 0x3fff) << 5) | (rt & 0x1f));
}

/* --------------------------------------------------------------- *
 * Phase 2 primitives — added for inline arithmetic / MAC kernels.
 *
 * These are the operations needed to implement 56-bit accumulator
 * arithmetic (ADD/SUB/CMP), 24x24 signed multiply, sign-/zero-
 * extends, and the inline E/U/N/Z flag update. See emit_load_accu56
 * / emit_store_accu56 / emit_alu_* below for how they compose.
 * --------------------------------------------------------------- */

/*
 * EOR (immediate) 32-bit: Wd = Wn ^ bitmask(N, immr, imms).
 *
 * Only the #1 (toggle bit 0) pattern is used today — by the inline
 * calc_cc path when it needs to invert a 0/1 boolean. The bitmask
 * encoding for a single-bit-at-position-0 mask is
 *   N=0, immr=0, imms=0.
 * Generalised emitter because it's trivially extensible: any
 * repeating-bitmask constant ARM64 can represent in one op (1, 3,
 * 7, 0xF, 0xFF, 0xFF00, 0x7FFF, ...) can feed this if we ever need
 * a broader subset.
 */
G_GNUC_UNUSED static inline void emit_eor_w_imm_bitmask(
    ArmEmit *e, int rd, int rn, uint32_t N_immr_imms)
{
    emit_u32(e, 0x52000000u | ((N_immr_imms & 0x1fff) << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* EOR Wd, Wn, #1 — toggles bit 0. Used to flip a 0/1 boolean. */
static inline void emit_eor_w_imm1(ArmEmit *e, int rd, int rn)
{
    /* N=0, immr=0, imms=0 encodes a 1-bit bitmask at bit 0.
     * Packed (N:immr:imms) = 0.000000.000000 → 0. */
    emit_eor_w_imm_bitmask(e, rd, rn, 0);
}

/* AND Wd, Wn, Wm  (shifted-register, shift #0) */
G_GNUC_UNUSED static inline void emit_and_w_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x0a000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* AND Xd, Xn, Xm */
G_GNUC_UNUSED static inline void emit_and_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x8a000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* EOR Wd, Wn, Wm */
G_GNUC_UNUSED static inline void emit_eor_w_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x4a000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* EOR Xd, Xn, Xm */
G_GNUC_UNUSED static inline void emit_eor_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0xca000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* MVN Wd, Wm  (alias of ORN Wd, WZR, Wm) — bitwise NOT */
G_GNUC_UNUSED static inline void emit_mvn_w(ArmEmit *e, int rd, int rm)
{
    emit_u32(e, 0x2a2003e0u | ((rm & 0x1f) << 16) | (rd & 0x1f));
}

/* ORR Xd, Xn, Xm */
G_GNUC_UNUSED static inline void emit_orr_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0xaa000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* ADDS Xd, Xn, Xm  (flag-setting 64-bit add, shifted-register form) */
G_GNUC_UNUSED static inline void emit_adds_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0xab000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* SUB Xd, Xn, Xm  (shifted-register, shift #0) */
G_GNUC_UNUSED static inline void emit_sub_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0xcb000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* SUBS Xd, Xn, Xm (flag-setting) */
G_GNUC_UNUSED static inline void emit_subs_x_reg(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0xeb000000u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* NEG Xd, Xm  (alias of SUB Xd, XZR, Xm) */
G_GNUC_UNUSED static inline void emit_neg_x(ArmEmit *e, int rd, int rm)
{
    emit_u32(e, 0xcb0003e0u | ((rm & 0x1f) << 16) | (rd & 0x1f));
}

/*
 * 64-bit shifts / bitfield extracts. Same UBFM / SBFM encoding
 * scheme as the 32-bit variants already in this file, but with the
 * sf bit (bit 31) set and N bit (bit 22) set for 64-bit bitfield ops.
 *
 *   LSL Xd, Xn, #s   => UBFM Xd, Xn, #((64-s)%64), #(63-s)
 *   LSR Xd, Xn, #s   => UBFM Xd, Xn, #s, #63
 *   ASR Xd, Xn, #s   => SBFM Xd, Xn, #s, #63
 */
G_GNUC_UNUSED static inline void emit_lsl_x_imm(ArmEmit *e, int rd, int rn, int shift)
{
    assert(shift >= 0 && shift < 64);
    uint32_t immr = (uint32_t)((64 - shift) & 63);
    uint32_t imms = (uint32_t)(63 - shift);
    emit_u32(e, 0xd3400000u | (immr << 16) | (imms << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

G_GNUC_UNUSED static inline void emit_lsr_x_imm(ArmEmit *e, int rd, int rn, int shift)
{
    assert(shift >= 0 && shift < 64);
    emit_u32(e, 0xd340fc00u | ((uint32_t)shift << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

G_GNUC_UNUSED static inline void emit_asr_x_imm(ArmEmit *e, int rd, int rn, int shift)
{
    assert(shift >= 0 && shift < 64);
    emit_u32(e, 0x9340fc00u | ((uint32_t)shift << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* UBFX Xd, Xn, #lsb, #width — 64-bit variant of UBFX.
 * Encoding: 0xd3400000 | (N=1 << 22) | (immr=lsb << 16) | (imms=(lsb+width-1) << 10) | Rn5 | Rd. */
G_GNUC_UNUSED static inline void emit_ubfx_x(ArmEmit *e, int rd, int rn, int lsb, int width)
{
    assert(lsb >= 0 && width > 0 && (lsb + width) <= 64);
    emit_u32(e, 0xd3400000u | ((uint32_t)lsb << 16) |
                ((uint32_t)(lsb + width - 1) << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* SBFX Xd, Xn, #lsb, #width — signed (arithmetic) bitfield extract, 64-bit. */
G_GNUC_UNUSED static inline void emit_sbfx_x(ArmEmit *e, int rd, int rn, int lsb, int width)
{
    assert(lsb >= 0 && width > 0 && (lsb + width) <= 64);
    emit_u32(e, 0x93400000u | ((uint32_t)lsb << 16) |
                ((uint32_t)(lsb + width - 1) << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/*
 * BFI Xd, Xn, #lsb, #width — bit field insert (alias of BFM).
 *   BFM Xd, Xn, #((64 - lsb) & 63), #(width - 1)
 */
G_GNUC_UNUSED static inline void emit_bfi_x(ArmEmit *e, int rd, int rn, int lsb, int width)
{
    assert(lsb >= 0 && width > 0 && (lsb + width) <= 64);
    uint32_t immr = (uint32_t)((64 - lsb) & 63);
    uint32_t imms = (uint32_t)(width - 1);
    emit_u32(e, 0xb3400000u | (immr << 16) | (imms << 10) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* SXTB Wd, Wn  — sign-extend byte (alias of SBFM Wd, Wn, #0, #7) */
G_GNUC_UNUSED static inline void emit_sxtb_w(ArmEmit *e, int rd, int rn)
{
    emit_u32(e, 0x13001c00u | ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/* SMULL Xd, Wn, Wm — signed 32x32 -> 64 multiply.
 * Encoding: 0x9b207c00 | (Rm << 16) | (Rn << 5) | Rd. */
G_GNUC_UNUSED static inline void emit_smull(ArmEmit *e, int rd, int rn, int rm)
{
    emit_u32(e, 0x9b207c00u | ((rm & 0x1f) << 16) |
                ((rn & 0x1f) << 5) | (rd & 0x1f));
}

/*
 * Patch an unconditional B to a new byte delta (imm26).
 */
static void patch_b(uint32_t *insn_addr, int32_t new_off_bytes)
{
    int32_t imm26 = new_off_bytes >> 2;
    assert(imm26 >= -(1 << 25) && imm26 < (1 << 25));
    uint32_t insn = *insn_addr & ~0x03ffffffu;
    insn |= (uint32_t)imm26 & 0x03ffffff;
    *insn_addr = insn;
}

/* BLR Xn */
static inline void emit_blr(ArmEmit *e, int rn)
{
    emit_u32(e, 0xd63f0000u | ((rn & 0x1f) << 5));
}

/* BR Xn */
G_GNUC_UNUSED static inline void emit_br(ArmEmit *e, int rn)
{
    emit_u32(e, 0xd61f0000u | ((rn & 0x1f) << 5));
}

/* RET (Xn) — defaults to X30 */
static inline void emit_ret(ArmEmit *e)
{
    emit_u32(e, 0xd65f03c0u);
}

/*
 * STP Xt1, Xt2, [Xn, #imm]!   pre-index
 * offset must be multiple of 8 and in range [-512, 504].
 */
static inline void emit_stp_pre(ArmEmit *e, int rt1, int rt2, int rn, int off)
{
    int imm7 = off / 8;
    assert(imm7 >= -64 && imm7 < 64);
    emit_u32(e, 0xa9800000u | (((uint32_t)imm7 & 0x7f) << 15) |
                ((rt2 & 0x1f) << 10) | ((rn & 0x1f) << 5) | (rt1 & 0x1f));
}

/* LDP Xt1, Xt2, [Xn], #imm    post-index */
static inline void emit_ldp_post(ArmEmit *e, int rt1, int rt2, int rn, int off)
{
    int imm7 = off / 8;
    assert(imm7 >= -64 && imm7 < 64);
    emit_u32(e, 0xa8c00000u | (((uint32_t)imm7 & 0x7f) << 15) |
                ((rt2 & 0x1f) << 10) | ((rn & 0x1f) << 5) | (rt1 & 0x1f));
}

/* Patch a previously-emitted CBNZ/CBZ/B.cond with a new relative
 * offset. Given a pointer to the 32-bit instruction and the desired
 * byte delta, rewrite the imm19 field. */
static void patch_branch(uint32_t *insn_addr, int32_t new_off_bytes)
{
    int32_t imm19 = new_off_bytes >> 2;
    assert(imm19 >= -(1 << 18) && imm19 < (1 << 18));
    uint32_t insn = *insn_addr & ~(0x7ffffu << 5);
    insn |= ((uint32_t)imm19 & 0x7ffff) << 5;
    *insn_addr = insn;
}

/* --------------------------------------------------------------- *
 * JIT state
 * --------------------------------------------------------------- */

#define DSP_JIT_CODE_BYTES (8u * 1024u * 1024u)   /* 8 MiB code buffer per core */
#define DSP_JIT_MAX_OPS_PER_BLOCK 32
#define DSP_JIT_HASH_SIZE 4096  /* power of two, >= DSP_PRAM_SIZE */

typedef void (*dsp_jit_entry_fn)(dsp_core_t *dsp);

/*
 * Per-block "write set" — a conservative over-approximation of
 * which regions of dsp_core_t the block's handlers might write to,
 * computed at translation time. The differential validator uses it
 * to skip byte-comparing regions the block demonstrably didn't
 * touch. Memory arrays account for >75% of dsp_core_t; for the
 * FIR/IIR hot-path blocks (pure register computation, no memory
 * writes) this drops the compare from ~40 KB to ~2 KB per block.
 *
 * The "always-compared" regions (registers, stack's top-of-stack,
 * pc / sr / loop_rep / interrupt_*, num_inst / instr_cycle /
 * cur_inst*) are small and always differ when there's a real
 * translation bug, so they're never skipped regardless of bits.
 */
#define DSP_JIT_WS_XRAM       (1u << 0)
#define DSP_JIT_WS_YRAM       (1u << 1)
#define DSP_JIT_WS_PRAM       (1u << 2)
#define DSP_JIT_WS_MIXBUFFER  (1u << 3)
#define DSP_JIT_WS_PERIPH     (1u << 4)
#define DSP_JIT_WS_ALL        0x1fu

typedef struct DspJitBlock {
    uint32_t pc_start;
    uint32_t pc_end;           /* exclusive — last covered PC + cur_inst_len */
    dsp_jit_entry_fn entry;    /* pointer into code buffer */
    uint32_t num_ops;
    uint32_t write_set;        /* DSP_JIT_WS_* bitmask */
    /*
     * Differential-validator gate. A translated block is fully
     * deterministic given its pre-state (same JIT stubs run, each
     * calling the same emu_func_t handler); a single enqueue /
     * successful validation proves correctness for every future
     * execution of that translation. Once this is set, the diff
     * path skips the block entirely — back to the fast normal
     * path with zero memcpy / validation cost.
     *
     *   0 : not yet enqueued (diff path will enqueue on next hit)
     *   1 : enqueued or validated (diff path skips from now on)
     *
     * Cleared by translate_block (fresh translation) and
     * dsp_jit_invalidate (block evicted). Writes are unsynchronised
     * because a lost update only means at most one extra validation
     * for a block — harmless.
     */
    uint8_t  diff_checked;
    /* Blocks are slotted 1-per-PC into DspJitState.blocks[]. No
     * explicit free list — eviction goes through dsp_jit_invalidate
     * (per-block) or dsp_jit_invalidate_all (cache flush). */
} DspJitBlock;

/* --------------------------------------------------------------- *
 * Differential validator (DIFF mode)
 *
 * There are two modes, selected by XEMU_DSP_JIT_DIFF_SYNC:
 *
 *   async (default): on each block, the APU thread snapshots pre-
 *   and post-state into a pre-allocated SPSC ring slot and publishes.
 *   A dedicated validator thread pops slots, runs the interpreter on
 *   the pre-snapshot to the same num_inst target, and compares
 *   against the post-snapshot. Validation is fully off the APU
 *   thread's critical path — d->lock is released before the
 *   (slow) interpreter replay. On ring full the producer drops
 *   the new entry (validation becomes a sampler) rather than
 *   blocking the APU thread.
 *
 *   sync (XEMU_DSP_JIT_DIFF_SYNC=1): interpreter replay + compare
 *   run inline on the APU thread for each block. Matches the old
 *   behaviour, kept for bring-up debugging where you want the abort
 *   to fire immediately on divergence rather than eventually when
 *   the validator catches up.
 *
 * In either mode the interpreter replays on a *private copy* of
 * dsp_core_t — never on the live dsp. The private copy's
 * read_peripheral / write_peripheral function pointers are replaced
 * with shims that set a flag to skip the compare (because
 * container_of(copy, DSPState, core) would otherwise corrupt the
 * real device state). This, together with core->jit_skip_diff_compare
 * set on the live dsp for DMA triggers, means blocks with
 * externally-visible I/O are validated for state transitions that
 * are *internal* to dsp_core_t, and the visible I/O differences
 * (which interp replay cannot reproduce) are skipped.
 */
#define DSP_JIT_DIFF_SLOTS 16  /* 2 x ~80 KB per slot; ~2.6 MB total */

typedef struct DiffSlot {
    dsp_core_t pre;            /* state before JIT block ran */
    dsp_core_t post;           /* state after JIT block ran */
    uint32_t   pc_start;
    uint32_t   num_inst_before;
    uint32_t   jit_cycles;
    uint32_t   write_set;      /* DSP_JIT_WS_* bitmask */
    uint8_t    skip_compare;   /* JIT hit DMA_CONTROL: skip validation */
    /*
     * Block pointer + its `entry` at enqueue time. The worker marks
     * the block as validated only if block->entry still matches —
     * otherwise the translation has been invalidated and re-emitted
     * (e.g. self-modifying code), and the new code must be
     * re-validated from scratch.
     */
    DspJitBlock *block;
    dsp_jit_entry_fn block_entry;
} DiffSlot;

typedef struct DiffQueue {
    DiffSlot *slots;           /* DSP_JIT_DIFF_SLOTS entries, heap-alloc */
    uint64_t  head;            /* consumer index, monotonic */
    uint64_t  tail;            /* producer index, monotonic */
    uint64_t  dropped;         /* ring-full drops */
    uint64_t  checked;         /* successful validations */
    uint64_t  failures;        /* divergences detected */
    uint64_t  enqueued;        /* total slots accepted into the ring */

    QemuThread worker;
    QemuMutex  mu;             /* guards cond + exiting; not the ring */
    QemuCond   cond;           /* worker wakeup */
    bool       worker_started;
    bool       exiting;

    /* Back-pointer to the owning core so stats logs can name it
     * (gp_ep.c sets dsp->core.is_gp AFTER dsp_jit_init, so we can't
     * cache the bool at queue-creation time — read it fresh). */
    dsp_core_t *owner;
} DiffQueue;

typedef struct DspJitState {
    /* Code cache */
    uint8_t *code_buf;
    size_t   code_cap;
    uint8_t *code_ptr;         /* bump allocator write cursor */

    /* Blocks */
    DspJitBlock blocks[DSP_PRAM_SIZE];  /* one slot per possible PC */
    DspJitBlock *pc_to_block[DSP_PRAM_SIZE];  /* pc -> owner block (or NULL) */

    /* Differential validator (XEMU_DSP_JIT_DIFF=1). NULL if diff off. */
    DiffQueue *diff_q;

    /* Stats */
    uint64_t blocks_translated;
    uint64_t blocks_executed;
    uint64_t cache_flushes;
    uint64_t fallbacks;
    uint64_t diff_ops_checked;

    /* Diff-mode sampling counter. Incremented every block when
     * diff mode is on; only blocks where (counter % sample) == 0
     * actually go through the differential path. */
    uint64_t diff_sample_counter;

    /* Set the first time dsp_jit_print_stats runs for this core,
     * so the atexit handler and dsp_jit_finalize don't both dump
     * the same stats (happens in the test-dsp binary; not the
     * normal xemu path, but cheap to guard). */
    bool stats_printed;
} DspJitState;

/* --------------------------------------------------------------- *
 * Code-buffer allocation (MAP_JIT on macOS via qemu_thread_jit_* pair)
 * --------------------------------------------------------------- */

static uint8_t *jit_code_alloc(size_t bytes)
{
#if defined(__APPLE__)
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    return (uint8_t *)p;
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
#endif
}

static void jit_code_free(uint8_t *p, size_t bytes)
{
    if (p) {
        munmap(p, bytes);
    }
}

static void jit_clear_icache(void *start, void *end)
{
    __builtin___clear_cache((char *)start, (char *)end);
}

/* --------------------------------------------------------------- *
 * Struct offsets the emitted code relies on
 * --------------------------------------------------------------- */

#define OFF_PC                       ((uint32_t)offsetof(dsp_core_t, pc))
#define OFF_CUR_INST                 ((uint32_t)offsetof(dsp_core_t, cur_inst))
#define OFF_CUR_INST_LEN             ((uint32_t)offsetof(dsp_core_t, cur_inst_len))
#define OFF_INSTR_CYCLE              ((uint32_t)offsetof(dsp_core_t, instr_cycle))
#define OFF_NUM_INST                 ((uint32_t)offsetof(dsp_core_t, num_inst))
#define OFF_LOOP_REP                 ((uint32_t)offsetof(dsp_core_t, loop_rep))
#define OFF_PC_ON_REP                ((uint32_t)offsetof(dsp_core_t, pc_on_rep))
#define OFF_IS_IDLE                  ((uint32_t)offsetof(dsp_core_t, is_idle))
#define OFF_INTERRUPT_COUNTER        ((uint32_t)offsetof(dsp_core_t, interrupt_counter))
#define OFF_INTERRUPT_STATE          ((uint32_t)offsetof(dsp_core_t, interrupt_state))
#define OFF_INTERRUPT_PIPELINE_COUNT ((uint32_t)offsetof(dsp_core_t, interrupt_pipeline_count))
#define OFF_SR                       ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_SR))
#define OFF_JIT_EXIT_BLOCK_REQ       ((uint32_t)offsetof(dsp_core_t, jit_exit_block_request))
#define OFF_JIT_SKIP_DIFF            ((uint32_t)offsetof(dsp_core_t, jit_skip_diff_compare))

/* Stack-relative scratch offsets (see emit_prologue for frame layout). */
#define OFF_SP_SCRATCH0  0
#define OFF_SP_SCRATCH1  4

/* Bound check: synthesized-address form (ADD imm12<<12 + LDR imm12*scale)
 * accepts offsets up to 16 MiB. dsp_core_t is ~78 KiB so we're safe. */
QEMU_BUILD_BUG_ON(OFF_PC                > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_CUR_INST          > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_CUR_INST_LEN      > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_INSTR_CYCLE       > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_NUM_INST          > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_LOOP_REP          > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_IS_IDLE           > (16u * 1024u * 1024u));
QEMU_BUILD_BUG_ON(OFF_INTERRUPT_COUNTER > (16u * 1024u * 1024u));

/* --------------------------------------------------------------- *
 * Block translation
 *
 * Translated block layout (per block entry):
 *
 *   prologue:
 *     STP x30, x19, [sp, #-16]!
 *     MOV x19, x0              ; x19 = dsp
 *
 *   for each instruction:
 *     MOV w0, #cur_inst        ; (MOVZ+MOVK if >16 bits)
 *     STR w0, [x19, #OFF_CUR_INST]
 *     MOV w0, #cur_inst_len
 *     STR w0, [x19, #OFF_CUR_INST_LEN]
 *     MOV w0, #2
 *     STRH w0, [x19, #OFF_INSTR_CYCLE]
 *     MOV x0, x19
 *     MOV x1, #emu_func        ; 64-bit immediate chain
 *     BLR x1
 *     MOV x0, x19
 *     MOV x1, #postexecute_update_pc
 *     BLR x1
 *     LDRH w0, [x19, #OFF_INSTR_CYCLE]
 *     LDR  w1, [x19, #OFF_NUM_INST]
 *     ADD  w1, w1, w0
 *     STR  w1, [x19, #OFF_NUM_INST]
 *     ; exit checks:
 *     LDRH w0, [x19, #OFF_INTERRUPT_COUNTER]
 *     CBNZ w0, exit_label
 *     LDR  w0, [x19, #OFF_LOOP_REP]
 *     CBNZ w0, exit_label
 *     LDRB w0, [x19, #OFF_IS_IDLE]
 *     CBNZ w0, exit_label
 *     ; pc mismatch check (branch taken by handler)
 *     LDR  w0, [x19, #OFF_PC]
 *     MOV  w1, #expected_next_pc
 *     CMP  w0, w1
 *     B.NE exit_label
 *
 *   exit_label (shared at block end):
 *     LDP x30, x19, [sp], #16
 *     RET
 *
 * --------------------------------------------------------------- */

/*
 * Pending patch sites for the shared exit_label. We collect them as
 * we emit each op and patch them after the epilogue is placed.
 */
/* Each per-op epilogue records up to 5 exit-check branches
 * (interrupt_counter / loop_rep / is_idle / jit_exit_block_request
 * / pc-mismatch). Size the array accordingly, with a small safety
 * margin. */
typedef struct {
    uint32_t *sites[DSP_JIT_MAX_OPS_PER_BLOCK * 8];
    int count;
} ExitPatchList;

static void record_exit_patch(ExitPatchList *l, uint32_t *site)
{
    assert(l->count < (int)(sizeof(l->sites) / sizeof(l->sites[0])));
    l->sites[l->count++] = site;
}

static void patch_exits(ExitPatchList *l, uint32_t *exit_label)
{
    for (int i = 0; i < l->count; i++) {
        uint32_t *site = l->sites[i];
        int32_t off = (int32_t)((uint8_t *)exit_label - (uint8_t *)site);
        patch_branch(site, off);
    }
}

/*
 * Emit a single instruction stub. Returns false if we should end
 * the block immediately after this op (classified terminator).
 */
#define SCRATCH 3   /* X3 synthesizes the address for far-field access */

/* Compile-time offsets for the DSP register file R/N/M/L banks. */
#define OFF_REGS      ((uint32_t)offsetof(dsp_core_t, registers))
#define OFF_REG(n)    (OFF_REGS + 4u * (uint32_t)(n))
#define OFF_R(n)      OFF_REG(DSP_REG_R0 + (n))
#define OFF_N(n)      OFF_REG(DSP_REG_N0 + (n))
#define OFF_M(n)      OFF_REG(DSP_REG_M0 + (n))
#define OFF_XRAM      ((uint32_t)offsetof(dsp_core_t, xram))
#define OFF_YRAM      ((uint32_t)offsetof(dsp_core_t, yram))

/*
 * Emit the slow-path BLR to dsp_jit_helper_calc_ea(dsp, ea_mode,
 * &SP[OFF_SP_SCRATCH0]). On return, the computed address sits at
 * [SP, #OFF_SP_SCRATCH0] and retour is in w0; we copy them into
 * `out_addr_reg` and (if `want_retour`) [SP, #OFF_SP_SCRATCH1].
 *
 * Clobbers: w0..w3. Preserves: x19-x25.
 */
static void emit_calc_ea_slow_call(ArmEmit *e, uint32_t ea_mode,
                                   int out_addr_reg, bool want_retour)
{
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);               /* x0 = dsp */
    emit_mov_imm32(e, /*rd=*/1, ea_mode);                 /* w1 = ea_mode */
    emit_add_x_imm(e, /*rd=*/2, /*rn=*/31, OFF_SP_SCRATCH0);  /* x2 = &SP[scratch0] */
    emit_mov_imm64(e, /*rd=*/3,
        (uint64_t)(uintptr_t)&dsp_jit_helper_calc_ea);
    emit_blr(e, /*rn=*/3);
    /* w0 now holds the retour flag; scratch0 holds the address. */
    emit_ldr_w_imm(e, out_addr_reg, /*rn=*/31, OFF_SP_SCRATCH0);
    if (want_retour) {
        emit_str_w_imm(e, /*rs=*/0, /*rn=*/31, OFF_SP_SCRATCH1);
    }
}

/*
 * Emit calc_ea inline: fast path for all 8 modes with linear Mn;
 * slow-path BLR for modulo-Mn modes 0-3/5/7. Mode 4 is a pure
 * read (no Mn check). Mode 6 (aa, pram[pc+1]) bakes the immediate
 * at translate time. Leaves the 16-bit address in out_addr_reg
 * (which must be a callee-saved W register x22..x25 if the caller
 * needs it preserved across subsequent BLRs).
 *
 * Semantics summary (matches emu_calc_ea in dsp_emu.c.inc:150):
 *   mode 0  (Rn)-Nn  : addr = Rn; Rn = (Rn - Nn) & 0xFFFF
 *   mode 1  (Rn)+Nn  : addr = Rn; Rn = (Rn + Nn) & 0xFFFF
 *   mode 2  (Rn)-    : addr = Rn; Rn = (Rn - 1) & 0xFFFF
 *   mode 3  (Rn)+    : addr = Rn; Rn = (Rn + 1) & 0xFFFF
 *   mode 4  (Rn)     : addr = Rn; no update
 *   mode 5  (Rn+Nn)  : addr = (Rn+Nn)&0xFFFF; Rn unchanged; cyc+=2
 *   mode 6  aa       : addr = pram[pc+1]; cur_inst_len++; cyc+=2;
 *                      retour = 1 iff numreg != 0 (immediate literal,
 *                      not an address). Immediate baked at translate
 *                      time from `dsp->pram[pc + 1]`.
 *   mode 7  -(Rn)    : addr = (Rn-1)&0xFFFF; Rn updated; cyc+=2
 *
 * When `want_retour` is true, the retour flag (1 == immediate
 * literal returned in out_addr_reg instead of an address) is
 * written to [SP, #OFF_SP_SCRATCH1] so the caller can branch on
 * it.
 *
 * `dsp` and `pc` are only read for the mode-6 immediate bake.
 * Callers that cannot guarantee mode != 6 must pass valid dsp + pc;
 * callers that can (mode baked into ea_mode at translate time and
 * != 6) may pass NULL / 0 respectively.
 *
 * out_addr_reg MUST NOT be 0, 1, 2, or 3 — those are used as
 * temporaries inside this emitter.
 *
 * Clobbers: w0, w1, w2, x3 (SCRATCH). Preserves: x19-x25 (except
 * out_addr_reg is written).
 */
static void emit_calc_ea_inline(ArmEmit *e, uint32_t ea_mode,
                                int out_addr_reg, bool want_retour,
                                dsp_core_t *dsp, uint32_t pc)
{
    assert(out_addr_reg != 0 && out_addr_reg != 1 &&
           out_addr_reg != 2 && out_addr_reg != 3);
    uint32_t mode   = (ea_mode >> 3) & 7;
    uint32_t numreg = ea_mode & 7;

    if (mode == 6) {
        /* Mode 6 (aa / absolute-address) baked inline. The interp
         * does: instr_cycle += 2; *dst = read_memory_p(pc+1);
         *       cur_inst_len++;
         *       return (numreg != 0) ? 1 : 0;
         * We bake read_memory_p(pc+1) at translate time (same as
         * the long-imm ALU bake). If pc+1 is past pram, bake 0 —
         * the interp would assert on that input so the value is
         * "don't care".
         *
         * NOTE: mode-6 EA lengthens the instruction to 2 words.
         * The parmove stub's expected_next_pc is still pc+1, so
         * the post-exec PC-mismatch check will trip and the block
         * will exit — that's the existing behaviour and is
         * preserved here; the win is dropping the BLR to
         * emu_calc_ea for the read. Once the translator is taught
         * to treat mode-6 parmoves as 2-word ops, expected_next_pc
         * will match and the block will stay intact. */
        assert(dsp != NULL);
        uint32_t baked = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;
        baked &= 0xFFFFu;   /* 16-bit address */

        /* instr_cycle += 2 */
        emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
        emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, 2);
        emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);

        /* cur_inst_len++ */
        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
        emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, 1);
        emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

        /* out_addr_reg = baked absolute address. */
        emit_mov_imm32(e, /*rd=*/out_addr_reg, baked);

        if (want_retour) {
            /* retour = (numreg != 0) ? 1 : 0 — known statically. */
            if (numreg != 0) {
                emit_mov_imm32(e, /*rd=*/0, 1);
                emit_str_w_imm(e, /*rs=*/0, /*rn=*/31, OFF_SP_SCRATCH1);
            } else {
                emit_str_w_imm(e, /*rs=*/31 /* WZR */,
                               /*rn=*/31 /* SP */, OFF_SP_SCRATCH1);
            }
        }
        return;
    }

    /* Mode 4 is a pure read (no Rn update) — skip the Mn check.
     * Every other mode needs a linear-Mn guard that falls back to
     * the slow helper on modulo / reverse-carry Mn. */
    uint32_t *to_slow = NULL;
    if (mode != 4) {
        emit_ldr_w_imm(e, /*rd=*/2, /*rn=*/19, OFF_M(numreg));
        emit_mov_imm32(e, /*rd=*/1, 0xFFFFu);
        emit_cmp_w_reg(e, /*rn=*/2, /*rm=*/1);
        to_slow = e->buf;
        emit_bcond(e, ARM_COND_NE, 0);  /* patched below */
    }

    /* Fast path: load Rn into out_addr_reg (modes 0-4, 7) or
     * compute (Rn+Nn) into it (mode 5). */
    if (mode != 5) {
        emit_ldr_w_imm(e, out_addr_reg, /*rn=*/19, OFF_R(numreg));
    }

    switch (mode) {
    case 0: /* (Rn)-Nn */
        emit_ldr_w_imm(e, /*rd=*/1, /*rn=*/19, OFF_N(numreg));
        emit_sub_w_reg(e, /*rd=*/0, /*rn=*/out_addr_reg, /*rm=*/1);
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, 0, 16);
        emit_str_w_imm(e, /*rs=*/0, /*rn=*/19, OFF_R(numreg));
        break;
    case 1: /* (Rn)+Nn */
        emit_ldr_w_imm(e, /*rd=*/1, /*rn=*/19, OFF_N(numreg));
        emit_add_w_reg(e, /*rd=*/0, /*rn=*/out_addr_reg, /*rm=*/1);
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, 0, 16);
        emit_str_w_imm(e, /*rs=*/0, /*rn=*/19, OFF_R(numreg));
        break;
    case 2: /* (Rn)- */
        emit_sub_w_imm(e, /*rd=*/0, /*rn=*/out_addr_reg, 1);
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, 0, 16);
        emit_str_w_imm(e, /*rs=*/0, /*rn=*/19, OFF_R(numreg));
        break;
    case 3: /* (Rn)+ */
        emit_add_w_imm(e, /*rd=*/0, /*rn=*/out_addr_reg, 1);
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, 0, 16);
        emit_str_w_imm(e, /*rs=*/0, /*rn=*/19, OFF_R(numreg));
        break;
    case 4: /* (Rn) — no update */
        break;
    case 5: /* (Rn+Nn) — address = (Rn+Nn)&0xFFFF, Rn unchanged,
             * +2 cycles. Interp's emu_calc_ea does an update_rn +
             * restore dance; in linear mode the net effect on Rn
             * is zero, so we just compute the masked sum directly. */
        emit_ldr_w_imm(e, /*rd=*/0, /*rn=*/19, OFF_R(numreg));
        emit_ldr_w_imm(e, /*rd=*/1, /*rn=*/19, OFF_N(numreg));
        emit_add_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);
        emit_ubfx_w(e, /*rd=*/out_addr_reg, /*rn=*/0, 0, 16);
        /* instr_cycle += 2. */
        emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
        emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, 2);
        emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
        break;
    case 7: /* -(Rn) — Rn = (Rn-1)&0xFFFF, address = new Rn,
             * +2 cycles. */
        emit_sub_w_imm(e, /*rd=*/0, /*rn=*/out_addr_reg, 1);
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, 0, 16);
        emit_str_w_imm(e, /*rs=*/0, /*rn=*/19, OFF_R(numreg));
        /* out_addr_reg = updated Rn. */
        emit_mov_w_reg(e, /*rd=*/out_addr_reg, /*rn=*/0);
        /* instr_cycle += 2. */
        emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
        emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, 2);
        emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
        break;
    default:
        /* Unreachable (mode 6 handled above; others are 0-5, 7). */
        break;
    }

    if (want_retour) {
        /* retour == 0 for all fast-path modes (retour is only set
         * by mode 6 / absolute-immediate in the slow path). */
        emit_str_w_imm(e, /*rs=*/31 /* WZR */, /*rn=*/31 /* SP */,
                       OFF_SP_SCRATCH1);
    }

    if (to_slow) {
        /* Jump past the slow-path block to 'done'. */
        uint32_t *to_done = e->buf;
        emit_b(e, 0);  /* patched below */

        /* Slow-path entry point */
        uint32_t *slow_label = e->buf;
        patch_branch(to_slow, (int32_t)((uint8_t *)slow_label -
                                        (uint8_t *)to_slow));
        emit_calc_ea_slow_call(e, ea_mode, out_addr_reg, want_retour);

        /* Done label */
        uint32_t *done_label = e->buf;
        patch_b(to_done, (int32_t)((uint8_t *)done_label -
                                   (uint8_t *)to_done));
    }
}

/*
 * Inline xram/yram linear memory read (fast path for addr < 0xc00
 * which bypasses the mixbuffer / peripheral / per-space special
 * cases). On slow-path (addr >= 0xc00) BLRs dsp56k_read_memory.
 *
 * memspace: DSP_SPACE_X (=0) or DSP_SPACE_Y (=1). Selected at
 *           translate time; picks xram[] or yram[] base.
 * addr_reg: W-register holding the 16-bit address (unchanged on exit).
 * value_reg: W-register to receive the loaded 24-bit value. Must not
 *           conflict with addr_reg or scratch registers.
 *
 * Clobbers: w0, w1, x2, x3. Preserves: x19-x25 except value_reg.
 */
static void emit_mem_read_xy(ArmEmit *e, int memspace,
                             int addr_reg, int value_reg)
{
    assert(memspace == DSP_SPACE_X || memspace == DSP_SPACE_Y);
    assert(value_reg != addr_reg);
    assert(value_reg != 0 && value_reg != 1 && value_reg != 2 && value_reg != 3);
    assert(addr_reg  != 0 && addr_reg  != 1 && addr_reg  != 2 && addr_reg  != 3);

    /* Fast path: if addr < 0xc00, direct array access.
     *   cmp addr, #0xc00
     *   b.hs slow
     *   (shift, add, ldr at OFF_XRAM/OFF_YRAM) */
    emit_mov_imm32(e, /*rd=*/0, 0xc00u);
    emit_cmp_w_reg(e, /*rn=*/addr_reg, /*rm=*/0);
    uint32_t *to_slow = e->buf;
    emit_bcond(e, 0x2 /* HS = unsigned >= */, 0);   /* patched below */

    /* Fast: w1 = addr << 2 (byte offset); x2 = x19 + x1;
     * value_reg = [x2, #OFF_XRAM_OR_YRAM]. */
    emit_lsl_w_imm(e, /*rd=*/1, /*rn=*/addr_reg, 2);
    emit_add_x_reg(e, /*rd=*/2, /*rn=*/19, /*rm=*/1);
    uint32_t arr_off = (memspace == DSP_SPACE_X) ? OFF_XRAM : OFF_YRAM;
    emit_ldr_w_any(e, /*rd=*/value_reg, /*rn=*/2, SCRATCH, arr_off);

    uint32_t *to_done = e->buf;
    emit_b(e, 0);  /* patched below */

    /* Slow-path: BLR dsp56k_read_memory(dsp, space, addr) */
    uint32_t *slow_label = e->buf;
    patch_branch(to_slow, (int32_t)((uint8_t *)slow_label -
                                    (uint8_t *)to_slow));
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_mov_imm32(e, /*rd=*/1, (uint32_t)memspace);
    emit_mov_w_reg(e, /*rd=*/2, /*rn=*/addr_reg);
    emit_mov_imm64(e, /*rd=*/3,
                   (uint64_t)(uintptr_t)&dsp56k_read_memory);
    emit_blr(e, /*rn=*/3);
    emit_mov_w_reg(e, /*rd=*/value_reg, /*rn=*/0);

    /* Done */
    uint32_t *done_label = e->buf;
    patch_b(to_done, (int32_t)((uint8_t *)done_label -
                               (uint8_t *)to_done));
}

/*
 * Inline xram/yram linear memory write. Symmetric to read:
 * fast-path direct store when addr < 0xc00, else BLR
 * dsp56k_write_memory.
 *
 * addr_reg: W-register with 16-bit address.
 * value_reg: W-register with 24-bit value.
 *
 * Clobbers: w0, w1, x2, x3. Preserves: x19-x25.
 */
static void emit_mem_write_xy(ArmEmit *e, int memspace,
                              int addr_reg, int value_reg)
{
    assert(memspace == DSP_SPACE_X || memspace == DSP_SPACE_Y);
    assert(value_reg != addr_reg);
    assert(value_reg != 0 && value_reg != 1 && value_reg != 2 && value_reg != 3);
    assert(addr_reg  != 0 && addr_reg  != 1 && addr_reg  != 2 && addr_reg  != 3);

    emit_mov_imm32(e, /*rd=*/0, 0xc00u);
    emit_cmp_w_reg(e, /*rn=*/addr_reg, /*rm=*/0);
    uint32_t *to_slow = e->buf;
    emit_bcond(e, 0x2 /* HS */, 0);

    emit_lsl_w_imm(e, /*rd=*/1, /*rn=*/addr_reg, 2);
    emit_add_x_reg(e, /*rd=*/2, /*rn=*/19, /*rm=*/1);
    uint32_t arr_off = (memspace == DSP_SPACE_X) ? OFF_XRAM : OFF_YRAM;
    emit_str_w_any(e, /*rs=*/value_reg, /*rn=*/2, SCRATCH, arr_off);

    uint32_t *to_done = e->buf;
    emit_b(e, 0);

    uint32_t *slow_label = e->buf;
    patch_branch(to_slow, (int32_t)((uint8_t *)slow_label -
                                    (uint8_t *)to_slow));
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_mov_imm32(e, /*rd=*/1, (uint32_t)memspace);
    emit_mov_w_reg(e, /*rd=*/2, /*rn=*/addr_reg);
    emit_mov_w_reg(e, /*rd=*/3, /*rn=*/value_reg);
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp56k_write_memory);
    emit_blr(e, /*rn=*/4);

    uint32_t *done_label = e->buf;
    patch_b(to_done, (int32_t)((uint8_t *)done_label -
                               (uint8_t *)to_done));
}

/* --------------------------------------------------------------- *
 * Parmove support: helpers for emitting DSP-register writes.
 * --------------------------------------------------------------- */

/*
 * Width of each DSP register for the final mask on write.
 * Mirrors `registers_mask[64]` in dsp_cpu.c (kept in-sync by hand
 * — both must stay == the hardware register widths in the DSP56300
 * PRM). 0 entries correspond to unused indices that a valid
 * instruction encoding should never target.
 */
static const uint8_t dsp_jit_reg_bits[64] = {
    [DSP_REG_X0]  = 24, [DSP_REG_X1]  = 24,
    [DSP_REG_Y0]  = 24, [DSP_REG_Y1]  = 24,
    [DSP_REG_A0]  = 24, [DSP_REG_B0]  = 24,
    [DSP_REG_A2]  =  8, [DSP_REG_B2]  =  8,
    [DSP_REG_A1]  = 24, [DSP_REG_B1]  = 24,
    /* A and B are accumulator 'halves' that go through the
     * accu-write path; not directly maskable via UBFX. */
    [DSP_REG_A]   = 24, [DSP_REG_B]   = 24,
    [DSP_REG_R0]  = 16, [DSP_REG_R1]  = 16, [DSP_REG_R2]  = 16, [DSP_REG_R3] = 16,
    [DSP_REG_R4]  = 16, [DSP_REG_R5]  = 16, [DSP_REG_R6]  = 16, [DSP_REG_R7] = 16,
    [DSP_REG_N0]  = 16, [DSP_REG_N1]  = 16, [DSP_REG_N2]  = 16, [DSP_REG_N3] = 16,
    [DSP_REG_N4]  = 16, [DSP_REG_N5]  = 16, [DSP_REG_N6]  = 16, [DSP_REG_N7] = 16,
    [DSP_REG_M0]  = 16, [DSP_REG_M1]  = 16, [DSP_REG_M2]  = 16, [DSP_REG_M3] = 16,
    [DSP_REG_M4]  = 16, [DSP_REG_M5]  = 16, [DSP_REG_M6]  = 16, [DSP_REG_M7] = 16,
    [DSP_REG_OMR] =  8, [DSP_REG_SP]  =  6,
    [DSP_REG_SSH] = 16, [DSP_REG_SSL] = 16,
    [DSP_REG_LA]  = 16, [DSP_REG_LC]  = 16,
    [DSP_REG_SR]  = 16,
};

/*
 * Emit code writing the 24-bit value in `value_reg` (Wd) to the
 * given DSP register.
 *
 *   - DSP_REG_A / DSP_REG_B: do the three-way accu store
 *     (A0=0, A1=value, A2 = sign(bit23))
 *   - everything else: mask to register's native width and
 *     store the single word.
 *
 * If dstreg is a 24-bit "wide" register, the caller must have
 * already performed any "shift by 16" expansion that some parmove
 * variants require. Most callers pass a value that's already in
 * its destination-register layout.
 *
 * Clobbers: w0, w1, x3 (SCRATCH).
 * Preserves value_reg.
 */
/*
 * mask_to_width mirrors the interpreter: some parmove variants
 * apply `value & BITMASK(registers_mask[numreg])` on the final reg
 * store (pm_2_2 line 5398, pm_3 line 5438, pm_5 line 5667) while
 * others don't (pm_0/pm_1/pm_8 store the raw 32-bit `save_reg`
 * they read from memory). When the source value is a 24-bit
 * memory word from xram/yram whose slot has never been written
 * through the normal write path (0xCACACACA init sentinels), this
 * difference is visible — JIT must match interp exactly.
 *
 * For A/B destinations the three-word split (A0/A1/A2) does not
 * mask A1 regardless of mask_to_width, matching every interp path.
 */
static void emit_pm_write_reg(ArmEmit *e, int dstreg, int value_reg,
                              bool mask_to_width)
{
    assert(value_reg != 0 && value_reg != 1 && value_reg != 3);

    if (dstreg == DSP_REG_A || dstreg == DSP_REG_B) {
        int off_x0 = (dstreg == DSP_REG_A) ? OFF_REG(DSP_REG_A0) : OFF_REG(DSP_REG_B0);
        int off_x1 = (dstreg == DSP_REG_A) ? OFF_REG(DSP_REG_A1) : OFF_REG(DSP_REG_B1);
        int off_x2 = (dstreg == DSP_REG_A) ? OFF_REG(DSP_REG_A2) : OFF_REG(DSP_REG_B2);

        /* A0 / B0 = 0 */
        emit_str_w_any(e, /*rs=*/31 /* WZR */, /*rn=*/19, SCRATCH, off_x0);
        /* A1 / B1 = value (unmasked — every interp path writes A1/B1
         * as the raw save word). */
        emit_str_w_any(e, /*rs=*/value_reg,    /*rn=*/19, SCRATCH, off_x1);
        /* A2 / B2 = (value >> 23) ? 0xff : 0
         *   = -(value >> 23) & 0xff   (when bit 23 is 1, -1 = 0xFF...)
         *
         * Compute via: w0 = value >> 23; w0 &= 1; neg w0, w0; mask. */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/value_reg, /*lsb=*/23, /*width=*/1);
        emit_neg_w(e, /*rd=*/0, /*rm=*/0);
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, 0, 8);
        emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, off_x2);
        return;
    }

    int bits = dsp_jit_reg_bits[dstreg & 63];
    assert(bits >= 0 && bits <= 24);

    if (bits == 0) {
        /* Matches interpreter: `save & BITMASK(0) == 0` written to
         * the slot. Happens when a parmove targets a reserved /
         * NULL register index (DSP_REG_NULL 0-3, DSP_REG_LCSAVE 48,
         * and the other unused entries with mask 0 in dsp_cpu.c's
         * registers_mask[]). A zero-store keeps us bit-exact with
         * the interpreter. */
        emit_str_w_any(e, /*rs=*/31 /* WZR */, /*rn=*/19, SCRATCH, OFF_REG(dstreg));
    } else if (bits == 24 && !mask_to_width) {
        /* No masking: store full 32 bits. Matches pm_1 / pm_8 which
         * store `save_reg` unmasked; if the source was an init
         * sentinel like 0xCACACACA the top byte is preserved. */
        emit_str_w_any(e, /*rs=*/value_reg, /*rn=*/19, SCRATCH, OFF_REG(dstreg));
    } else {
        /* bits < 24 always masks (else the top byte would pollute
         * the 16-/8-/6-bit slots). bits == 24 masks only for
         * parmove variants whose interp applies `& BITMASK(24)`. */
        emit_ubfx_w(e, /*rd=*/1, /*rn=*/value_reg, 0, bits);
        emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_REG(dstreg));
    }
}

/*
 * Emit code that reads the current value of DSP register `srcreg`
 * into `value_reg` (W). For accumulators A / B the read goes
 * through dsp_jit_helper_pm_read_accu24, which applies scaling
 * and limiting per SR.S0/S1 and maintains SR.L — this is the
 * slow path but matches the interpreter bit-exactly.
 *
 * Clobbers: w0, w1, x3. Preserves x19-x25 except value_reg.
 * Must NOT be called with value_reg in {0, 1, 2, 3}.
 */
static void emit_pm_read_reg(ArmEmit *e, int srcreg, int value_reg)
{
    assert(value_reg != 0 && value_reg != 1 &&
           value_reg != 2 && value_reg != 3);

    if (srcreg == DSP_REG_A || srcreg == DSP_REG_B) {
        /* BLR pm_read_accu24(dsp, numreg, &SP[scratch0]) */
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm32(e, /*rd=*/1, (uint32_t)srcreg);
        emit_add_x_imm(e, /*rd=*/2, /*rn=*/31, OFF_SP_SCRATCH0);
        emit_mov_imm64(e, /*rd=*/3,
            (uint64_t)(uintptr_t)&dsp_jit_helper_pm_read_accu24);
        emit_blr(e, /*rn=*/3);
        emit_ldr_w_imm(e, /*rd=*/value_reg, /*rn=*/31, OFF_SP_SCRATCH0);
        return;
    }

    /* Simple register load. */
    emit_ldr_w_any(e, /*rd=*/value_reg, /*rn=*/19, SCRATCH, OFF_REG(srcreg));
}

/* --------------------------------------------------------------- *
 * Phase 2 — inline ALU kernels (arithmetic + MAC + logical + shift
 *           + transfer). Replaces the per-parmove `BLR
 *           opcodes_alu[inst & 0xFF]` round-trip with inline ARM64
 *           that mirrors the interpreter's handler bit-exactly.
 *
 * The 56-bit accumulator is held as a sign-extended 64-bit value in
 * an X-register for the duration of a single ALU op: bit 55 is the
 * MSB, bits 63:55 are the sign extension. This collapses the
 * interpreter's 3-word add-with-carry dance into a single 64-bit
 * ADD.
 *
 * Flag computation is also inline — no BLR to
 * emu_ccr_update_e_u_n_z. The S0/S1 scaling bits in SR are read at
 * runtime; the three scaling cases (00, 01, 10) dispatch through a
 * CMP + Bcc sequence (most common case: scaling=00, so the
 * fall-through is fastest).
 *
 * Handlers outside the inline set (emu_rnd_*, emu_rol/ror,
 * emu_adc/sbc, emu_addl/subl/addr/subr, emu_max) still go through a
 * plain BLR to the existing C handler via the ALU_FALLBACK case —
 * see emit_alu_call / alu_classify_opcode at the bottom of this
 * section.
 * --------------------------------------------------------------- */

/* Register offsets used by the accu load / store helpers. */
#define OFF_A0_REG ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_A0))
#define OFF_A1_REG ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_A1))
#define OFF_A2_REG ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_A2))
#define OFF_B0_REG ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_B0))
#define OFF_B1_REG ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_B1))
#define OFF_B2_REG ((uint32_t)(offsetof(dsp_core_t, registers) + 4u * DSP_REG_B2))

/* which_ab is 0 for A, 1 for B. */
static inline uint32_t accu_off(int which_ab, int slot)
{
    if (which_ab == 0) {
        switch (slot) {
        case 0: return OFF_A0_REG;
        case 1: return OFF_A1_REG;
        case 2: return OFF_A2_REG;
        }
    } else {
        switch (slot) {
        case 0: return OFF_B0_REG;
        case 1: return OFF_B1_REG;
        case 2: return OFF_B2_REG;
        }
    }
    assert(!"bad slot");
    return 0;
}

/*
 * Load the 56-bit accumulator A or B into a single 64-bit X-reg,
 * sign-extended from bit 55 so bits 63:56 carry the sign of bit 55.
 * This representation makes 64-bit ADD/SUB produce correct carry /
 * overflow for the 56-bit accumulator (with per-bit-55 flag
 * extraction — see emit_alu_arith).
 *
 * Layout: bits [55:48] = A2 (8-bit), bits [47:24] = A1 (24-bit),
 *         bits [23:0]  = A0 (24-bit), bits [63:56] = sign-ext(A2[7]).
 *
 * Masks A1 and A0 to 24 bits on load so stale upper bits (which the
 * pm_0/pm_1/pm_8 A/B splits leave unmasked per interp parity) don't
 * leak into the arithmetic.
 *
 * Clobbers: w/x tmp regs (caller passes scratch1 / scratch2).
 */
static void emit_load_accu56(ArmEmit *e, int xaccu, int which_ab,
                             int xtmp)
{
    assert(xaccu != xtmp);
    /* Load A2 (byte) and sign-extend directly into xaccu as a
     * 64-bit signed value. After SBFX with lsb=0, width=8, xaccu
     * holds the signed 8-bit interpretation of A2 in bits 63:0
     * (i.e. all 64 bits carry the sign). */
    emit_ldrb_any(e, /*rd=*/xaccu, /*rn=*/19, SCRATCH, accu_off(which_ab, 2));
    emit_sbfx_x(e, /*rd=*/xaccu, /*rn=*/xaccu, 0, 8);
    emit_lsl_x_imm(e, /*rd=*/xaccu, /*rn=*/xaccu, 48);

    /* A1 -> bits [47:24] (bfi handles masking to 24 bits). */
    emit_ldr_w_any(e, /*rd=*/xtmp, /*rn=*/19, SCRATCH, accu_off(which_ab, 1));
    emit_bfi_x(e, /*rd=*/xaccu, /*rn=*/xtmp, 24, 24);

    /* A0 -> bits [23:0]. */
    emit_ldr_w_any(e, /*rd=*/xtmp, /*rn=*/19, SCRATCH, accu_off(which_ab, 0));
    emit_bfi_x(e, /*rd=*/xaccu, /*rn=*/xtmp, 0, 24);
}

/*
 * Unpack a 64-bit X-reg back into A2 / A1 / A0 (or B2 / B1 / B0)
 * with the standard 8 / 24 / 24-bit widths. Uses UBFX so bits
 * outside the slot width are not stored (matching the interp's
 * `& BITMASK(24)` / `& BITMASK(8)` masks).
 */
G_GNUC_UNUSED static void emit_store_accu56(ArmEmit *e, int xaccu,
                                            int which_ab, int xtmp)
{
    assert(xaccu != xtmp);
    /* A0 = accu[23:0] */
    emit_ubfx_x(e, /*rd=*/xtmp, /*rn=*/xaccu, 0, 24);
    emit_str_w_any(e, /*rs=*/xtmp, /*rn=*/19, SCRATCH, accu_off(which_ab, 0));
    /* A1 = accu[47:24] */
    emit_ubfx_x(e, /*rd=*/xtmp, /*rn=*/xaccu, 24, 24);
    emit_str_w_any(e, /*rs=*/xtmp, /*rn=*/19, SCRATCH, accu_off(which_ab, 1));
    /* A2 = accu[55:48] (unsigned 8-bit — matches interp's & BITMASK(8)) */
    emit_ubfx_x(e, /*rd=*/xtmp, /*rn=*/xaccu, 48, 8);
    emit_str_w_any(e, /*rs=*/xtmp, /*rn=*/19, SCRATCH, accu_off(which_ab, 2));
}

/*
 * Source-operand forms for ADD / SUB / CMP / CMPM. Encodes how the
 * interpreter builds its three-word `source[]` array for each
 * handler variant; see dsp_emu.c.inc:516..965 for the reference.
 */
typedef enum {
    ALU_SRC_ACCU_A,    /* source = [A2, A1, A0] — for _A_B variants (source = A) */
    ALU_SRC_ACCU_B,    /* source = [B2, B1, B0] — for _B_A variants (source = B) */
    ALU_SRC_X,         /* source = [sign(X1[23]), X1, X0] — 48-bit "long X" */
    ALU_SRC_Y,         /* source = [sign(Y1[23]), Y1, Y0] — 48-bit "long Y" */
    ALU_SRC_X0_AT_A1,  /* source = [sign(X0[23]), X0, 0] — X0 at bits 47:24 */
    ALU_SRC_Y0_AT_A1,  /* source = [sign(Y0[23]), Y0, 0] */
    ALU_SRC_X1_AT_A1,  /* source = [sign(X1[23]), X1, 0] */
    ALU_SRC_Y1_AT_A1,  /* source = [sign(Y1[23]), Y1, 0] */
} AluSrcForm;

/*
 * Load a 56-bit ALU source operand into a single 64-bit X-reg,
 * sign-extended from bit 55 (same convention as emit_load_accu56).
 *
 * Clobbers xtmp. Produces xsrc.
 */
G_GNUC_UNUSED static void emit_load_alu_src(ArmEmit *e, int xsrc,
                                            AluSrcForm form, int xtmp)
{
    assert(xsrc != xtmp);
    switch (form) {
    case ALU_SRC_ACCU_A:
        emit_load_accu56(e, xsrc, /*which_ab=*/0, xtmp);
        return;
    case ALU_SRC_ACCU_B:
        emit_load_accu56(e, xsrc, /*which_ab=*/1, xtmp);
        return;
    case ALU_SRC_X:
    case ALU_SRC_Y: {
        /*
         * "Long" X or Y: sign-extend the high 24-bit register
         * (X1 or Y1) into bits [63:24] of xsrc, then OR in the
         * low 24 bits from X0 or Y0.
         */
        int off_hi = (form == ALU_SRC_X) ? OFF_REG(DSP_REG_X1)
                                         : OFF_REG(DSP_REG_Y1);
        int off_lo = (form == ALU_SRC_X) ? OFF_REG(DSP_REG_X0)
                                         : OFF_REG(DSP_REG_Y0);
        emit_ldr_w_any(e, /*rd=*/xsrc, /*rn=*/19, SCRATCH, off_hi);
        emit_sbfx_x(e, /*rd=*/xsrc, /*rn=*/xsrc, 0, 24);
        emit_lsl_x_imm(e, /*rd=*/xsrc, /*rn=*/xsrc, 24);
        emit_ldr_w_any(e, /*rd=*/xtmp, /*rn=*/19, SCRATCH, off_lo);
        emit_bfi_x(e, /*rd=*/xsrc, /*rn=*/xtmp, 0, 24);
        return;
    }
    default: {
        /* Single-register source placed at bits [47:24] with
         * bits [23:0] = 0, sign-extended from the 24-bit value's
         * bit 23. */
        int reg;
        switch (form) {
        case ALU_SRC_X0_AT_A1: reg = DSP_REG_X0; break;
        case ALU_SRC_Y0_AT_A1: reg = DSP_REG_Y0; break;
        case ALU_SRC_X1_AT_A1: reg = DSP_REG_X1; break;
        case ALU_SRC_Y1_AT_A1: reg = DSP_REG_Y1; break;
        default: assert(!"bad form"); return;
        }
        emit_ldr_w_any(e, /*rd=*/xsrc, /*rn=*/19, SCRATCH, OFF_REG(reg));
        emit_sbfx_x(e, /*rd=*/xsrc, /*rn=*/xsrc, 0, 24);
        emit_lsl_x_imm(e, /*rd=*/xsrc, /*rn=*/xsrc, 24);
        return;
    }
    }
}

/*
 * Emit a call to the emu_ccr_update_e_u_n_z helper (shimmed from
 * dsp_cpu.c as dsp_jit_helper_ccr_e_u_n_z) to update SR's E/U/N/Z
 * bits based on the new accumulator value.
 *
 * The shim takes (dsp, A2, A1, A0) — we extract the three slots
 * from the 64-bit xaccu via UBFX and pass them in the standard
 * argument registers w1/w2/w3.
 *
 * Inlining this fully would save another ~5-10 cycles per op but
 * requires a 100+ ARM64-instruction emit with runtime branches on
 * SR.S0/S1 — deferred until Phase 8's lazy-flag rework (at which
 * point most blocks never compute these bits at all).
 *
 * Clobbers w0..w4 and x30 (via BLR).
 */
static void emit_ccr_e_u_n_z(ArmEmit *e, int xaccu)
{
    emit_ubfx_x(e, /*rd=*/1, /*rn=*/xaccu, 48, 8);  /* w1 = A2 */
    emit_ubfx_x(e, /*rd=*/2, /*rn=*/xaccu, 24, 24); /* w2 = A1 */
    emit_ubfx_x(e, /*rd=*/3, /*rn=*/xaccu, 0, 24);  /* w3 = A0 */
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);          /* x0 = dsp */
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_ccr_e_u_n_z);
    emit_blr(e, /*rn=*/4);
}

/* --------------------------------------------------------------- *
 * ALU opcode classification
 *
 * Decode the 8-bit ALU opcode byte (inst & 0xff) into an
 * AluVariant that tells the emitter which inline template to use.
 * Anything not covered falls through to ALU_FALLBACK which BLRs
 * the existing opcodes_alu[] handler — zero behavioural change
 * for unsupported opcodes.
 *
 * See dsp_emu.c.inc:5092 for the full opcodes_alu[] table this
 * classifier mirrors.
 * --------------------------------------------------------------- */

typedef enum {
    ALU_KIND_FALLBACK = 0,   /* BLR opcodes_alu[inst&0xff] — unchanged */
    ALU_KIND_MOVE,           /* emu_move: no-op (already skipped) */
    ALU_KIND_ADD,
    ALU_KIND_SUB,
    ALU_KIND_CMP,
    ALU_KIND_CMPM,
    ALU_KIND_TST,
    ALU_KIND_CLR,
    ALU_KIND_NEG,
    ALU_KIND_ABS,
    ALU_KIND_NOT,
    ALU_KIND_AND,
    ALU_KIND_OR,
    ALU_KIND_EOR,
    ALU_KIND_ASL,
    ALU_KIND_ASR,
    ALU_KIND_LSL,
    ALU_KIND_LSR,
    ALU_KIND_TFR,
    ALU_KIND_MPY,
    ALU_KIND_MPYR,
    ALU_KIND_MAC,
    ALU_KIND_MACR,
} AluKind;

typedef struct AluVariant {
    AluKind    kind;
    uint8_t    dst_ab;       /* 0=A, 1=B */
    AluSrcForm src_form;     /* for ALU_KIND_ADD / SUB / CMP / CMPM */

    /* Single-register source (AND/OR/EOR/TFR) — DSP_REG_X0 etc. */
    uint8_t    src_reg;

    /* MAC-family parameters: */
    uint8_t    mac_src1_reg;
    uint8_t    mac_src2_reg;
    uint8_t    mac_sign;      /* 0 = +, 1 = − */
} AluVariant;

/* (src1, src2) pair for the 8 MAC rows at 0x80 + (pair<<4). */
static const uint8_t alu_mac_pair_src1[8] = {
    DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1,
    DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1,
};
static const uint8_t alu_mac_pair_src2[8] = {
    DSP_REG_X0, DSP_REG_Y0, DSP_REG_X0, DSP_REG_Y0,
    DSP_REG_Y1, DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1,
};

/* Source register for the 0x40-0x7F block, indexed by (op >> 4) - 4. */
static const uint8_t alu_47_src_reg[4] = {
    DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1,
};
static const AluSrcForm alu_47_src_form[4] = {
    ALU_SRC_X0_AT_A1, ALU_SRC_Y0_AT_A1,
    ALU_SRC_X1_AT_A1, ALU_SRC_Y1_AT_A1,
};

/* Classify the 8-bit ALU opcode. Returns the AluVariant in *out; the
 * `kind` field is ALU_KIND_FALLBACK when no inline template
 * applies. */
G_GNUC_UNUSED static void alu_classify_opcode(uint8_t alu_op, AluVariant *out)
{
    AluVariant v = { .kind = ALU_KIND_FALLBACK };

    if (alu_op >= 0x80) {
        /* MAC / MPY family (opcodes_alu[0x80..0xff]).
         *   alu_op = 1 _ [pair:3] _ [dst_ab:1] _ [sign:1] _ [round|mac:1] _ [ismac:1]
         * Wait — table row is
         *   mpy_p, mpyr_p, mac_p, macr_p, mpy_m, mpyr_m, mac_m, macr_m
         * so sub-op (low 3 bits) is (is_mac, is_round, sign).
         *   bit 0 : 1 = round variant (mpyr/macr)
         *   bit 1 : 1 = mac variant (mac/macr accumulate)
         *   bit 2 : 1 = sign minus
         * (Verified against the table in dsp_emu.c.inc.)
         */
        uint8_t sub     = alu_op & 0x7;
        uint8_t dst_bit = (alu_op >> 3) & 0x1;
        uint8_t pair    = (alu_op >> 4) & 0x7;
        int is_round = sub & 0x1;
        int is_mac   = (sub & 0x2) != 0;
        int sign_m   = (sub & 0x4) != 0;

        if (is_mac && is_round)       v.kind = ALU_KIND_MACR;
        else if (is_mac)              v.kind = ALU_KIND_MAC;
        else if (is_round)            v.kind = ALU_KIND_MPYR;
        else                          v.kind = ALU_KIND_MPY;

        v.dst_ab       = dst_bit;
        v.mac_src1_reg = alu_mac_pair_src1[pair];
        v.mac_src2_reg = alu_mac_pair_src2[pair];
        v.mac_sign     = (uint8_t)sign_m;
        *out = v;
        return;
    }

    if (alu_op >= 0x40) {
        /* 0x40-0x7f: [add, tfr, or, eor, sub, cmp, and, cmpm] × src × dst.
         *   alu_op = 0100_0000 | (src_idx:2) << 4 | (dst_ab:1) << 3 | sub_op(3)
         * src_idx: 0 = X0, 1 = Y0, 2 = X1, 3 = Y1
         */
        uint8_t sub     = alu_op & 0x7;
        uint8_t dst_bit = (alu_op >> 3) & 0x1;
        uint8_t src_idx = (alu_op >> 4) & 0x3;
        uint8_t src_reg = alu_47_src_reg[src_idx];
        AluSrcForm form = alu_47_src_form[src_idx];
        v.dst_ab   = dst_bit;
        v.src_reg  = src_reg;
        v.src_form = form;
        switch (sub) {
        case 0: v.kind = ALU_KIND_ADD;  break;
        case 1: v.kind = ALU_KIND_TFR;  break;
        case 2: v.kind = ALU_KIND_OR;   break;
        case 3: v.kind = ALU_KIND_EOR;  break;
        case 4: v.kind = ALU_KIND_SUB;  break;
        case 5: v.kind = ALU_KIND_CMP;  break;
        case 6: v.kind = ALU_KIND_AND;  break;
        case 7: v.kind = ALU_KIND_CMPM; break;
        }
        *out = v;
        return;
    }

    /* 0x00-0x3f: irregular block. Decode by explicit opcode. */
    switch (alu_op) {
    case 0x00: v.kind = ALU_KIND_MOVE; break;

    /* TFR B->A / A->B */
    case 0x01: v.kind = ALU_KIND_TFR; v.dst_ab = 0;
               v.src_form = ALU_SRC_ACCU_B; v.src_reg = DSP_REG_B; break;
    case 0x09: v.kind = ALU_KIND_TFR; v.dst_ab = 1;
               v.src_form = ALU_SRC_ACCU_A; v.src_reg = DSP_REG_A; break;

    /* TST */
    case 0x03: v.kind = ALU_KIND_TST; v.dst_ab = 0; break;
    case 0x0b: v.kind = ALU_KIND_TST; v.dst_ab = 1; break;

    /* CMP / CMPM between A and B */
    case 0x05: v.kind = ALU_KIND_CMP;  v.dst_ab = 0;
               v.src_form = ALU_SRC_ACCU_B; break;
    case 0x07: v.kind = ALU_KIND_CMPM; v.dst_ab = 0;
               v.src_form = ALU_SRC_ACCU_B; break;
    case 0x0d: v.kind = ALU_KIND_CMP;  v.dst_ab = 1;
               v.src_form = ALU_SRC_ACCU_A; break;
    case 0x0f: v.kind = ALU_KIND_CMPM; v.dst_ab = 1;
               v.src_form = ALU_SRC_ACCU_A; break;

    /* ADD A<->B, SUB A<->B */
    case 0x10: v.kind = ALU_KIND_ADD; v.dst_ab = 0;
               v.src_form = ALU_SRC_ACCU_B; break;
    case 0x18: v.kind = ALU_KIND_ADD; v.dst_ab = 1;
               v.src_form = ALU_SRC_ACCU_A; break;
    case 0x14: v.kind = ALU_KIND_SUB; v.dst_ab = 0;
               v.src_form = ALU_SRC_ACCU_B; break;
    case 0x1c: v.kind = ALU_KIND_SUB; v.dst_ab = 1;
               v.src_form = ALU_SRC_ACCU_A; break;

    /* CLR / NOT */
    case 0x13: v.kind = ALU_KIND_CLR; v.dst_ab = 0; break;
    case 0x1b: v.kind = ALU_KIND_CLR; v.dst_ab = 1; break;
    case 0x17: v.kind = ALU_KIND_NOT; v.dst_ab = 0; break;
    case 0x1f: v.kind = ALU_KIND_NOT; v.dst_ab = 1; break;

    /* ADD_X / SUB_X (long X) */
    case 0x20: v.kind = ALU_KIND_ADD; v.dst_ab = 0; v.src_form = ALU_SRC_X; break;
    case 0x28: v.kind = ALU_KIND_ADD; v.dst_ab = 1; v.src_form = ALU_SRC_X; break;
    case 0x24: v.kind = ALU_KIND_SUB; v.dst_ab = 0; v.src_form = ALU_SRC_X; break;
    case 0x2c: v.kind = ALU_KIND_SUB; v.dst_ab = 1; v.src_form = ALU_SRC_X; break;

    /* ADD_Y / SUB_Y */
    case 0x30: v.kind = ALU_KIND_ADD; v.dst_ab = 0; v.src_form = ALU_SRC_Y; break;
    case 0x38: v.kind = ALU_KIND_ADD; v.dst_ab = 1; v.src_form = ALU_SRC_Y; break;
    case 0x34: v.kind = ALU_KIND_SUB; v.dst_ab = 0; v.src_form = ALU_SRC_Y; break;
    case 0x3c: v.kind = ALU_KIND_SUB; v.dst_ab = 1; v.src_form = ALU_SRC_Y; break;

    /* Shifts on full 56-bit accu */
    case 0x22: v.kind = ALU_KIND_ASR; v.dst_ab = 0; break;
    case 0x2a: v.kind = ALU_KIND_ASR; v.dst_ab = 1; break;
    case 0x32: v.kind = ALU_KIND_ASL; v.dst_ab = 0; break;
    case 0x3a: v.kind = ALU_KIND_ASL; v.dst_ab = 1; break;

    /* Shifts on A1/B1 alone */
    case 0x23: v.kind = ALU_KIND_LSR; v.dst_ab = 0; break;
    case 0x2b: v.kind = ALU_KIND_LSR; v.dst_ab = 1; break;
    case 0x33: v.kind = ALU_KIND_LSL; v.dst_ab = 0; break;
    case 0x3b: v.kind = ALU_KIND_LSL; v.dst_ab = 1; break;

    /* NEG / ABS */
    case 0x26: v.kind = ALU_KIND_ABS; v.dst_ab = 0; break;
    case 0x2e: v.kind = ALU_KIND_ABS; v.dst_ab = 1; break;
    case 0x36: v.kind = ALU_KIND_NEG; v.dst_ab = 0; break;
    case 0x3e: v.kind = ALU_KIND_NEG; v.dst_ab = 1; break;

    /* Everything else (rnd, addr, subr, addl, subl, max, adc, sbc,
     * ror, rol, undefined) stays at ALU_KIND_FALLBACK. */
    default: break;
    }
    *out = v;
}

/* --------------------------------------------------------------- *
 * Phase 2 inline ALU emitters
 * --------------------------------------------------------------- */

/*
 * Emit: newsr (w5) = overflow*L | overflow*V | carry*C, given the
 * three 56-bit sign-extended X-regs:
 *   xorig = original dest (before op)
 *   xsrc  = source operand
 *   xres  = (dest op src) in 64-bit signed form
 *   is_sub: 0 = add, 1 = sub
 *
 * Carry / borrow is the unsigned carry out of bit 55. For both ADD
 * and SUB, the identity
 *   bit_56_of(orig ^ src ^ result) = carry_out_of_bit_55
 * holds: for ADD it's the classic parity identity; for SUB, after
 * expanding res = a + (~b) + 1, the algebra reduces to the same
 * form (not the negated form). Specifically:
 *   `(a^b^res) bit 56 = 1 ^ carry_add_of(a, ~b, 1)`
 *                     = 1 ^ (NOT borrow_sub(a, b))
 *                     = borrow_sub(a, b)
 * so no inversion is needed. (An earlier version of this code
 * inverted the SUB carry and produced C=1 on `0 - 0` — the 0x45
 * cmp_x0_a bug that surfaced via DIFF=1.)
 *
 * Overflow is the signed-56 overflow bit. From dsp_add56 /
 * dsp_sub56:
 *   ADD: overflow = ((sign_s ^ sign_r) & (sign_d ^ sign_r))
 *   SUB: overflow = ((sign_s ^ sign_d) & (sign_r ^ sign_d))
 * where sign_s=src[55], sign_d=orig[55], sign_r=result[55]. The
 * ADD and SUB forms use different "common" term in the XOR
 * factoring — this emitter handles both.
 *
 * Clobbers w5..w8 (passed back as new_sr in w5).
 */
static void emit_addsub_flags(ArmEmit *e, int xorig, int xsrc, int xres,
                              int is_sub)
{
    /* Carry: same formula for both ADD and SUB. */
    emit_eor_x_reg(e, /*rd=*/6, /*rn=*/xorig, /*rm=*/xsrc);
    emit_eor_x_reg(e, /*rd=*/6, /*rn=*/6,     /*rm=*/xres);
    emit_ubfx_x(e,   /*rd=*/5, /*rn=*/6, 56, 1);          /* w5 = carry */

    /* Overflow. Pick the (A, B, common) triple so the formula is
     * always "(A^common) & (B^common)" bit 55.
     *   ADD: A=orig, B=src,  common=result
     *   SUB: A=src,  B=res,  common=orig
     */
    int xA, xB, xCommon;
    if (is_sub) {
        xA = xsrc;  xB = xres;  xCommon = xorig;
    } else {
        xA = xorig; xB = xsrc;  xCommon = xres;
    }
    emit_eor_x_reg(e, /*rd=*/6, /*rn=*/xA, /*rm=*/xCommon);
    emit_eor_x_reg(e, /*rd=*/7, /*rn=*/xB, /*rm=*/xCommon);
    emit_and_x_reg(e, /*rd=*/6, /*rn=*/6,  /*rm=*/7);
    emit_ubfx_x(e,   /*rd=*/6, /*rn=*/6, 55, 1);          /* w6 = overflow */

    /* Build new_sr = (overflow << L) | (overflow << V) | (carry << C).
     * L=bit 6, V=bit 1, C=bit 0. w5 already has carry at bit 0. */
    emit_bfi_x(e, /*rd=*/5, /*rn=*/6, DSP_SR_V, 1);   /* V */
    emit_bfi_x(e, /*rd=*/5, /*rn=*/6, DSP_SR_L, 1);   /* L */
}

/*
 * Apply a computed new_sr (in w5 — see emit_addsub_flags) to SR,
 * first clearing bits V and C so the OR-in correctly installs the
 * new ones.
 *
 * Clobbers w6.
 */
static void emit_sr_clear_vc_or_newsr(ArmEmit *e, int wnewsr)
{
    emit_ldrh_any(e, /*rd=*/6, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/7, (uint32_t)~((1u << DSP_SR_V) | (1u << DSP_SR_C))
                                & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    emit_orr_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/wnewsr);
    emit_strh_any(e, /*rs=*/6, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * Emit the inline ADD / SUB / CMP / CMPM kernel for a given
 * AluVariant.
 *
 * Register usage:
 *   x10 = dest accu (loaded from A/B)
 *   x11 = source (48/56-bit packed, sign-extended)
 *   x12 = result after ADD/SUB
 *   x13 = saved original dest (for overflow calc)
 *   (internal scratch for emit_addsub_flags: w5, w6, w7, w8)
 *
 * Must NOT clobber the parmove stubs' x22..x25 scratch saves.
 */
static void emit_alu_arith(ArmEmit *e, const AluVariant *v)
{
    int is_sub = (v->kind == ALU_KIND_SUB || v->kind == ALU_KIND_CMP ||
                  v->kind == ALU_KIND_CMPM);
    int no_writeback = (v->kind == ALU_KIND_CMP || v->kind == ALU_KIND_CMPM);
    int is_cmpm = (v->kind == ALU_KIND_CMPM);

    /* Load dest (A or B) into x10. */
    emit_load_accu56(e, /*xaccu=*/10, /*which_ab=*/v->dst_ab, /*xtmp=*/8);

    /* Load source into x11. */
    emit_load_alu_src(e, /*xsrc=*/11, /*form=*/v->src_form, /*xtmp=*/8);

    /* CMPM: abs both operands before comparing.  Inline port of
     * dsp_abs56: if bit 55 set, negate. For our sign-extended
     * 64-bit reps, "abs" is NEG if negative else identity. */
    if (is_cmpm) {
        /* abs(x10) */
        emit_neg_x(e, /*rd=*/12, /*rm=*/10);
        emit_ubfx_x(e, /*rd=*/6, /*rn=*/10, 55, 1);
        emit_cmp_w_imm(e, /*rn=*/6, 0);
        uint32_t *skip_a = e->buf;
        emit_bcond(e, ARM_COND_EQ, 0);
        emit_mov_x_reg(e, /*rd=*/10, /*rn=*/12);
        /* Re-sign-extend: after negating a 56-bit value, the 64-bit
         * result's bits 63:56 may not match bit 55. SBFX fixes it. */
        emit_sbfx_x(e, /*rd=*/10, /*rn=*/10, 0, 56);
        patch_branch(skip_a, (int32_t)((uint8_t *)e->buf - (uint8_t *)skip_a));
        /* abs(x11) */
        emit_neg_x(e, /*rd=*/12, /*rm=*/11);
        emit_ubfx_x(e, /*rd=*/6, /*rn=*/11, 55, 1);
        emit_cmp_w_imm(e, /*rn=*/6, 0);
        uint32_t *skip_b = e->buf;
        emit_bcond(e, ARM_COND_EQ, 0);
        emit_mov_x_reg(e, /*rd=*/11, /*rn=*/12);
        emit_sbfx_x(e, /*rd=*/11, /*rn=*/11, 0, 56);
        patch_branch(skip_b, (int32_t)((uint8_t *)e->buf - (uint8_t *)skip_b));
    }

    /* Save x10 (original dest) into x13 for overflow calc. */
    emit_mov_x_reg(e, /*rd=*/13, /*rn=*/10);

    /* x12 = dest op src (64-bit signed — low 56 bits are the result). */
    if (is_sub) {
        emit_sub_x_reg(e, /*rd=*/12, /*rn=*/10, /*rm=*/11);
    } else {
        emit_add_x_reg(e, /*rd=*/12, /*rn=*/10, /*rm=*/11);
    }

    /* Compute new_sr (carry+V+L bits) from orig/src/res. */
    emit_addsub_flags(e, /*xorig=*/13, /*xsrc=*/11, /*xres=*/12,
                      is_sub);

    /* Writeback unless this is CMP/CMPM. */
    if (!no_writeback) {
        emit_store_accu56(e, /*xaccu=*/12, v->dst_ab, /*xtmp=*/8);
    }

    /* SR update: clear V|C, OR new_sr, then E/U/N/Z via helper. */
    emit_sr_clear_vc_or_newsr(e, /*wnewsr=*/5);
    emit_ccr_e_u_n_z(e, /*xaccu=*/12);
}

/*
 * TST: no arithmetic, just E/U/N/Z update on current accu and
 * clear V. Mirrors emu_tst_a / emu_tst_b.
 */
static void emit_alu_tst(ArmEmit *e, const AluVariant *v)
{
    emit_load_accu56(e, /*xaccu=*/10, v->dst_ab, /*xtmp=*/8);

    /* Clear V only (not C). */
    emit_ldrh_any(e, /*rd=*/6, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/7, (uint32_t)~(1u << DSP_SR_V) & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    emit_strh_any(e, /*rs=*/6, /*rn=*/19, SCRATCH, OFF_SR);

    emit_ccr_e_u_n_z(e, /*xaccu=*/10);
}

/*
 * CLR: zero the accumulator, set SR Z=1 U=1, clear SR E N V.
 * Mirrors emu_clr_a / emu_clr_b.
 */
static void emit_alu_clr(ArmEmit *e, const AluVariant *v)
{
    /* STR 0 to A0, A1, A2 (or B0, B1, B2). */
    emit_str_w_any(e, /*rs=*/31 /*WZR*/, /*rn=*/19, SCRATCH,
                   accu_off(v->dst_ab, 0));
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 1));
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 2));

    /* SR: clear E|N|V, set U|Z. */
    emit_ldrh_any(e, /*rd=*/6, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/7,
                   (uint32_t)~((1u << DSP_SR_E) | (1u << DSP_SR_N) |
                               (1u << DSP_SR_V)) & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    emit_mov_imm32(e, /*rd=*/7, (1u << DSP_SR_U) | (1u << DSP_SR_Z));
    emit_orr_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    emit_strh_any(e, /*rs=*/6, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * Helper for NEG/ABS: compute the "overflowed" bit — 1 iff the
 * 56-bit signed value is 0x80_0000_0000_0000 (minimum negative,
 * can't be negated within 56 bits). Places result in w_ovf.
 *
 * Matches the interp's special-case test:
 *   overflowed = (A0==0 && A1==0 && A2==0x80).
 * Equivalent in 64-bit: (accu == 0xFF80_0000_0000_0000 when
 * sign-extended, == (int64_t)(-(1LL << 55))).
 */
static void emit_overflowed_min_neg(ArmEmit *e, int xaccu, int wovf)
{
    /* Shift out bits 63:56 (sign-ext) by masking to 56 bits unsigned,
     * then compare against (1 << 55). */
    emit_lsl_x_imm(e, /*rd=*/6, /*rn=*/xaccu, 8);
    emit_lsr_x_imm(e, /*rd=*/6, /*rn=*/6, 8);        /* x6 = accu & ((1<<56)-1) */
    /* Compare against (1ULL << 55) in a scratch. */
    emit_mov_imm32(e, /*rd=*/7, 1u);
    emit_lsl_x_imm(e, /*rd=*/7, /*rn=*/7, 55);       /* x7 = 1 << 55 */
    /* CMP x6, x7 -> set Z flag. Use SUBS xzr, x6, x7. */
    emit_u32(e, 0xeb0001ffu | ((7 & 0x1f) << 16) | ((6 & 0x1f) << 5));
    /* CSET wd, eq — w_ovf = 1 if equal, else 0. Encoded as
     * CSINC Wd, WZR, WZR, cond=NE (inverted). */
    emit_u32(e, 0x1a9f17e0u | (uint32_t)(wovf & 0x1f));
}

/*
 * SR update for NEG/ABS: clear V, OR (ovf<<L) | (ovf<<V).
 * C is preserved (unlike ADD/SUB which clear V|C).
 */
static void emit_sr_neg_abs(ArmEmit *e, int wovf)
{
    emit_ldrh_any(e, /*rd=*/6, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/7, (uint32_t)~(1u << DSP_SR_V) & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    /* OR ovf into V (bit 1). */
    emit_bfi_x(e, /*rd=*/6, /*rn=*/wovf, DSP_SR_V, 1);
    /* OR ovf into L (bit 6). */
    emit_bfi_x(e, /*rd=*/6, /*rn=*/wovf, DSP_SR_L, 1);
    emit_strh_any(e, /*rs=*/6, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * NEG: negate the 56-bit accu. Matches emu_neg_a/b (dsp_emu.c.inc
 * :4123). Overflow bit set only when orig == 0x80_0000_0000_0000.
 * C bit is preserved (not touched).
 */
static void emit_alu_neg(ArmEmit *e, const AluVariant *v)
{
    emit_load_accu56(e, /*xaccu=*/13, v->dst_ab, /*xtmp=*/8); /* orig */
    /* overflowed = (orig == 1<<55). Compute before the negate so
     * the original value is still in x13. */
    emit_overflowed_min_neg(e, /*xaccu=*/13, /*wovf=*/5);

    /* Negate. */
    emit_neg_x(e, /*rd=*/12, /*rm=*/13);
    /* Sign-extend to 56-bit (in case negation overflowed bit 55). */
    emit_sbfx_x(e, /*rd=*/12, /*rn=*/12, 0, 56);

    emit_store_accu56(e, /*xaccu=*/12, v->dst_ab, /*xtmp=*/8);
    emit_sr_neg_abs(e, /*wovf=*/5);
    emit_ccr_e_u_n_z(e, /*xaccu=*/12);
}

/*
 * ABS: if accu negative, negate in place. Matches emu_abs_a/b
 * (dsp_emu.c.inc:344). overflowed bit set only when
 * orig == 0x80_0000_0000_0000. C bit preserved.
 */
static void emit_alu_abs(ArmEmit *e, const AluVariant *v)
{
    emit_load_accu56(e, /*xaccu=*/13, v->dst_ab, /*xtmp=*/8);
    /* Compute overflowed bit from the original value. */
    emit_overflowed_min_neg(e, /*xaccu=*/13, /*wovf=*/5);

    /* Default result = orig. */
    emit_mov_x_reg(e, /*rd=*/12, /*rn=*/13);

    /* If bit 55 set (negative), negate. */
    emit_ubfx_x(e, /*rd=*/6, /*rn=*/13, 55, 1);
    emit_cmp_w_imm(e, /*rn=*/6, 0);
    uint32_t *skip_neg = e->buf;
    emit_bcond(e, ARM_COND_EQ, 0);
    emit_neg_x(e, /*rd=*/12, /*rm=*/13);
    emit_sbfx_x(e, /*rd=*/12, /*rn=*/12, 0, 56);
    patch_branch(skip_neg, (int32_t)((uint8_t *)e->buf - (uint8_t *)skip_neg));

    emit_store_accu56(e, /*xaccu=*/12, v->dst_ab, /*xtmp=*/8);
    emit_sr_neg_abs(e, /*wovf=*/5);
    emit_ccr_e_u_n_z(e, /*xaccu=*/12);
}

/* ============================================================== *
 * ALU shifts (round 3) — inline replacements for
 *   emu_asl_a / emu_asl_b  (56-bit shift left by 1)
 *   emu_asr_a / emu_asr_b  (56-bit LOGICAL shift right by 1, despite
 *                           the "ASR" mnemonic — see dsp_asr56 in
 *                           dsp_cpu.c which uses a plain uint64_t
 *                           shift with zero fill, not sign-extend)
 *   emu_lsl_a / emu_lsl_b  (A1-only shift left by 1, leaves A0/A2)
 *   emu_lsr_a / emu_lsr_b  (A1-only shift right by 1)
 *
 * The 56-bit ASL/ASR variants round-trip through emit_load_accu56
 * / emit_store_accu56 and match the interpreter's dsp_asl56 /
 * dsp_asr56 flag behaviour bit-for-bit. The A1-only LSL/LSR ops
 * are much simpler: just read the 24-bit A1/B1 register, shift,
 * and write back; they do NOT touch A0 / A2 and they update a
 * different slice of SR (C, N, Z; clears V; does not invoke the
 * E/U shim).
 * ============================================================== */

/*
 * ASL (shift-left 56-bit by 1). SR updates mirror dsp_asl56(_, 1):
 *   carry_bit = orig[55]
 *   L_bit     = orig[55]        (for shift=1, identical to carry)
 *   V_bit     = orig[55] XOR orig[54]
 * Then clear C|V in SR and OR in the new C|V|L.
 */
static void emit_alu_asl(ArmEmit *e, const AluVariant *v)
{
    /* Load original accu (sign-extended into x13). */
    emit_load_accu56(e, /*xaccu=*/13, v->dst_ab, /*xtmp=*/8);

    /* carry = orig[55]. */
    emit_ubfx_x(e, /*rd=*/5, /*rn=*/13, 55, 1);

    /* V = orig[55] XOR orig[54]. */
    emit_ubfx_x(e, /*rd=*/6, /*rn=*/13, 54, 1);
    emit_eor_w_reg(e, /*rd=*/6, /*rn=*/5, /*rm=*/6);

    /* Shift left by 1; re-sign-extend from bit 55 to maintain the
     * "sign-extended 64-bit" convention of subsequent E/U/N/Z
     * consumers and the emit_store_accu56 unpacker. */
    emit_lsl_x_imm(e, /*rd=*/12, /*rn=*/13, 1);
    emit_sbfx_x(e,   /*rd=*/12, /*rn=*/12, 0, 56);

    emit_store_accu56(e, /*xaccu=*/12, v->dst_ab, /*xtmp=*/8);

    /* Build newsr = (carry << C) | (V << V) | (L << L). For shift=1
     * the L bit is identical to the carry, so we can reuse w5. */
    emit_lsl_w_imm(e, /*rd=*/6, /*rn=*/6, DSP_SR_V);      /* w6 = V << V_bit */
    emit_lsl_w_imm(e, /*rd=*/7, /*rn=*/5, DSP_SR_L);      /* w7 = L << L_bit */
    emit_orr_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);      /* w6 |= w7 */
    emit_orr_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/5);      /* w6 |= C (already at bit 0) */

    emit_sr_clear_vc_or_newsr(e, /*wnewsr=*/6);

    emit_ccr_e_u_n_z(e, /*xaccu=*/12);
}

/*
 * ASR (56-bit LOGICAL shift right by 1 — the "ASR" opcode in the
 * DSP56k ISA as actually implemented by dsp_asr56). SR updates:
 *   carry_bit = orig[0]
 *   Clear V, set C. L is NOT touched by dsp_asr56; interp only
 *   masks C and V before OR'ing newsr, so V stays cleared and L
 *   stays whatever it was.
 */
static void emit_alu_asr(ArmEmit *e, const AluVariant *v)
{
    emit_load_accu56(e, /*xaccu=*/13, v->dst_ab, /*xtmp=*/8);

    /* carry = orig[0]. Extract BEFORE the shift so we don't care
     * whether the upper bits are sign-extended or masked. */
    emit_ubfx_x(e, /*rd=*/5, /*rn=*/13, 0, 1);

    /* Mask to 56 bits BEFORE shifting right, otherwise the sign-
     * extension in bits 63:56 leaks into bit 55 of the result
     * (interp's dsp_asr56 uses a bare uint64_t shift which zero-
     * fills from the top — we must match that logical semantics). */
    emit_ubfx_x(e, /*rd=*/12, /*rn=*/13, 0, 56);
    emit_lsr_x_imm(e, /*rd=*/12, /*rn=*/12, 1);

    emit_store_accu56(e, /*xaccu=*/12, v->dst_ab, /*xtmp=*/8);

    /* newsr = (carry << C). V is cleared; L is untouched by dsp_asr56
     * (V-clear is handled by emit_sr_clear_vc_or_newsr). Since C is
     * bit 0, w5 is already at the right position. */
    emit_sr_clear_vc_or_newsr(e, /*wnewsr=*/5);

    emit_ccr_e_u_n_z(e, /*xaccu=*/12);
}

/*
 * LSL (A1-only shift left by 1). Matches emu_lsl_a/b in
 * dsp_emu.c.inc:1661-1685. Only A1 (or B1) is touched; A0/A2 are
 * untouched; SR: clears C|N|Z|V, then sets C=orig[23], N=new[23],
 * Z=(new==0). No E/U shim invocation.
 */
static void emit_alu_lsl(ArmEmit *e, const AluVariant *v)
{
    unsigned off_a1 = accu_off(v->dst_ab, 1);

    /* w5 = A1 (24 bits; upper 8 bits are don't-care after the
     * mask below). */
    emit_ldr_w_any(e, /*rd=*/5, /*rn=*/19, SCRATCH, off_a1);

    /* carry = orig[23]. */
    emit_ubfx_w(e, /*rd=*/6, /*rn=*/5, 23, 1);

    /* A1 = (A1 << 1) & 0xFFFFFF. */
    emit_lsl_w_imm(e, /*rd=*/5, /*rn=*/5, 1);
    emit_ubfx_w(e, /*rd=*/5, /*rn=*/5, 0, 24);
    emit_str_w_any(e, /*rs=*/5, /*rn=*/19, SCRATCH, off_a1);

    /* N = new[23]. */
    emit_ubfx_w(e, /*rd=*/7, /*rn=*/5, 23, 1);

    /* Z = (new == 0). Use CMP + CSET. */
    emit_cmp_w_imm(e, /*rn=*/5, 0);
    emit_cset_w(e, /*rd=*/8, ARM_COND_EQ);

    /* SR update: clear C|N|Z|V, set C=w6, N=w7, Z=w8. */
    emit_ldrh_any(e, /*rd=*/9, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/10,
                   (uint32_t)~((1u << DSP_SR_C) | (1u << DSP_SR_N) |
                               (1u << DSP_SR_Z) | (1u << DSP_SR_V))
                   & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/9, /*rn=*/9, /*rm=*/10);

    /* Splice C/N/Z bits back in via BFI. (C at bit 0, N at bit 3,
     * Z at bit 2.) */
    emit_bfi_x(e, /*rd=*/9, /*rn=*/6, DSP_SR_C, 1);
    emit_bfi_x(e, /*rd=*/9, /*rn=*/7, DSP_SR_N, 1);
    emit_bfi_x(e, /*rd=*/9, /*rn=*/8, DSP_SR_Z, 1);

    emit_strh_any(e, /*rs=*/9, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * Long-immediate ALU emitters (round 3 / #3).
 *
 * add_long / sub_long / cmp_long: the 2-word ALU forms with a
 *   24-bit immediate that the interpreter reads from pram[pc+1]
 *   at run time. The `xxxx` is baked at translate time here so
 *   the emitted code neither calls `read_memory_p` nor BLRs the
 *   `emu_<op>_long` handler.
 *
 * and_long / or_long: same 2-word form but with A1-only logical
 *   ops. SR updates: clear N|Z|V, set N = new[23], Z = (new==0).
 *
 * All five variants mirror `dsp->cur_inst_len++` (from 1 → 2)
 * via emit_cf_inc_cur_inst_len so the post-exec PC update
 * advances past both words of the instruction. Cycle count is
 * unchanged (the interpreter handlers don't adjust instr_cycle,
 * so the preset 2 persists).
 *
 * dst_ab is decoded from inst[3] (0 = A, 1 = B), same as
 * emu_add_long / emu_sub_long / emu_cmp_long / emu_and_long /
 * emu_or_long in dsp_emu.c.inc.
 */

/* Materialize a sign-extended 56-bit source from a 24-bit immediate
 * into an X-register, matching how emu_add_x / emu_sub_x / emu_cmp_long
 * build `source[]`:
 *   source[0] = (x & (1<<23)) ? 0xff : 0x00   (bits 55:48)
 *   source[1] = x                              (bits 47:24)
 *   source[2] = 0                              (bits 23:0)
 * Equivalent to: ((int64_t)sign_extend(x,24)) << 24. Baked at
 * translate time; the emitter's MOVZ/MOVK chain materialises the
 * final 64-bit value. */
static int64_t li_build_src64(uint32_t imm24)
{
    /* Sign-extend the 24-bit immediate to 32 then to 64, then shift. */
    int32_t signed24 = (int32_t)(imm24 << 8) >> 8;
    return ((int64_t)signed24) << 24;
}

/*
 * ADD / SUB / CMP with a 24-bit baked immediate.
 *   kind_li = DSP_JIT_LI_ADD / SUB / CMP
 *   dst_ab  = destination accumulator (0 = A, 1 = B)
 *   imm24   = baked pram[pc+1] low 24 bits
 *
 * Register usage matches emit_alu_arith: x10 = orig accu,
 * x11 = src, x12 = result, w5..w8 scratch for flags.
 */
static void emit_alu_long_imm_arith(ArmEmit *e, int kind_li,
                                    int dst_ab, uint32_t imm24)
{
    bool is_sub = (kind_li == DSP_JIT_LI_SUB || kind_li == DSP_JIT_LI_CMP);
    bool is_cmp = (kind_li == DSP_JIT_LI_CMP);

    /* Load orig dest accu into x10. */
    emit_load_accu56(e, /*xaccu=*/10, /*which_ab=*/dst_ab, /*xtmp=*/8);

    /* Materialise baked source into x11 (56-bit sign-extended). */
    emit_mov_imm64(e, /*rd=*/11, (uint64_t)li_build_src64(imm24));

    /* x12 = x10 op x11. */
    if (is_sub) {
        emit_sub_x_reg(e, /*rd=*/12, /*rn=*/10, /*rm=*/11);
    } else {
        emit_add_x_reg(e, /*rd=*/12, /*rn=*/10, /*rm=*/11);
    }
    /* Re-sign-extend to 56 bits (in case we overflowed into bit 56). */
    emit_sbfx_x(e, /*rd=*/12, /*rn=*/12, 0, 56);

    /* Compute C/V/L flags into w5 (packed as newsr). */
    emit_addsub_flags(e, /*xorig=*/10, /*xsrc=*/11, /*xres=*/12,
                      /*is_sub=*/is_sub);

    if (!is_cmp) {
        emit_store_accu56(e, /*xaccu=*/12, /*which_ab=*/dst_ab, /*xtmp=*/8);
    }

    /* Clear V|C in SR and OR in the new C/V/L. */
    emit_sr_clear_vc_or_newsr(e, /*wnewsr=*/5);

    /* E/U/N/Z update via the shared shim. */
    emit_ccr_e_u_n_z(e, /*xaccu=*/12);
}

/*
 * AND / OR with a 24-bit baked immediate (A1-only operation).
 *   kind_li = DSP_JIT_LI_AND / OR
 *   dst_ab  = destination accumulator (0 = A, 1 = B)
 *   imm24   = baked pram[pc+1] low 24 bits
 *
 * Mirrors emu_and_x / emu_or_long:
 *   A1 |= imm24  (or &= for AND)
 *   SR clear N|Z|V; set N = new A1[23]; set Z = (A1 == 0).
 * Does NOT touch A0 / A2.
 */
static void emit_alu_long_imm_logical(ArmEmit *e, int kind_li,
                                      int dst_ab, uint32_t imm24)
{
    unsigned off_a1 = accu_off(dst_ab, 1);

    /* w5 = A1 (24 bits — upper 8 are don't-care because we mask
     * below). */
    emit_ldr_w_any(e, /*rd=*/5, /*rn=*/19, SCRATCH, off_a1);

    /* w6 = imm24 (materialised only once). For AND/OR the upper 8
     * bits of w6 don't matter because we mask the result. */
    emit_mov_imm32(e, /*rd=*/6, imm24);

    if (kind_li == DSP_JIT_LI_AND) {
        emit_and_w_reg(e, /*rd=*/5, /*rn=*/5, /*rm=*/6);
    } else {
        /* OR */
        emit_orr_w_reg(e, /*rd=*/5, /*rn=*/5, /*rm=*/6);
    }

    /* Mask to 24 bits and store back. */
    emit_ubfx_w(e, /*rd=*/5, /*rn=*/5, 0, 24);
    emit_str_w_any(e, /*rs=*/5, /*rn=*/19, SCRATCH, off_a1);

    /* Flags: N = bit 23 of new A1; Z = (new A1 == 0). */
    emit_ubfx_w(e, /*rd=*/6, /*rn=*/5, 23, 1);           /* w6 = N */
    emit_cmp_w_imm(e, /*rn=*/5, 0);
    emit_cset_w(e, /*rd=*/7, ARM_COND_EQ);               /* w7 = Z */

    /* Clear N|Z|V; OR in new N/Z. */
    emit_ldrh_any(e, /*rd=*/8, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/9,
                   (uint32_t)~((1u << DSP_SR_N) | (1u << DSP_SR_Z) |
                               (1u << DSP_SR_V))
                   & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/8, /*rn=*/8, /*rm=*/9);
    emit_bfi_x(e, /*rd=*/8, /*rn=*/6, DSP_SR_N, 1);
    emit_bfi_x(e, /*rd=*/8, /*rn=*/7, DSP_SR_Z, 1);
    emit_strh_any(e, /*rs=*/8, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * Dispatcher for long-immediate ALU ops. Classifies the handler;
 * if we have an inline emitter, materialise the baked imm from
 * pram[pc+1] and emit. Returns true if handled.
 *
 * Called from emit_instruction BEFORE falling through to the BLR
 * path, in parallel with emit_cf_call. See the write-up in
 * emit_instruction for how this composes.
 */
static bool emit_long_imm_call(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                               uint32_t inst, emu_func_t fn)
{
    int kind = dsp_jit_helper_classify_long_imm((void *)fn);
    if (kind == DSP_JIT_LI_NONE) {
        return false;
    }

    int dst_ab = (inst >> 3) & 1;
    uint32_t imm24 = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;

    switch (kind) {
    case DSP_JIT_LI_ADD:
    case DSP_JIT_LI_SUB:
    case DSP_JIT_LI_CMP:
        emit_alu_long_imm_arith(e, kind, dst_ab, imm24);
        break;
    case DSP_JIT_LI_AND:
    case DSP_JIT_LI_OR:
        emit_alu_long_imm_logical(e, kind, dst_ab, imm24);
        break;
    default:
        return false;
    }

    /*
     * All long-imm handlers are 2-word: cur_inst_len goes from 1
     * (preset by emit_instruction step 2) to 2 (the interp does
     * `dsp->cur_inst_len++`). Increment inline rather than
     * reusing emit_cf_inc_cur_inst_len — the CF helper is
     * defined later in the file and this emitter lives near the
     * ALU section, so inlining avoids a forward declaration.
     */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, 1);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    return true;
}

/*
 * LSR (A1-only shift right by 1). Matches emu_lsr_a/b in
 * dsp_emu.c.inc:1698-1716. SR: clears C|N|Z|V, sets C=orig[0],
 * Z=(new==0). N stays cleared (interp does NOT set N). No E/U
 * shim.
 */
static void emit_alu_lsr(ArmEmit *e, const AluVariant *v)
{
    unsigned off_a1 = accu_off(v->dst_ab, 1);

    /* w5 = A1. */
    emit_ldr_w_any(e, /*rd=*/5, /*rn=*/19, SCRATCH, off_a1);

    /* carry = orig[0]. */
    emit_ubfx_w(e, /*rd=*/6, /*rn=*/5, 0, 1);

    /* A1 = A1 >> 1. Logical — same as >>= 1 in interp since the
     * value held in dsp->registers[] is a 24-bit value stored in
     * a uint32_t, no sign bit at 31. */
    emit_lsr_w_imm(e, /*rd=*/5, /*rn=*/5, 1);
    emit_str_w_any(e, /*rs=*/5, /*rn=*/19, SCRATCH, off_a1);

    /* Z = (new == 0). */
    emit_cmp_w_imm(e, /*rn=*/5, 0);
    emit_cset_w(e, /*rd=*/8, ARM_COND_EQ);

    /* SR: clear C|N|Z|V, set C and Z (N stays cleared). */
    emit_ldrh_any(e, /*rd=*/9, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/10,
                   (uint32_t)~((1u << DSP_SR_C) | (1u << DSP_SR_N) |
                               (1u << DSP_SR_Z) | (1u << DSP_SR_V))
                   & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/9, /*rn=*/9, /*rm=*/10);

    emit_bfi_x(e, /*rd=*/9, /*rn=*/6, DSP_SR_C, 1);
    emit_bfi_x(e, /*rd=*/9, /*rn=*/8, DSP_SR_Z, 1);

    emit_strh_any(e, /*rs=*/9, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * Inline MPY / MPYR / MAC / MACR kernel.
 *
 * Collapses the interpreter's dsp_mul56 + optional dsp_add56 +
 * optional dsp_rnd56 chain into a small ARM64 sequence:
 *
 *   1. Load src1, src2 as signed 24-bit -> 32-bit W-regs.
 *   2. SMULL for signed 32x32 -> 64-bit product (actual 48-bit).
 *   3. LSL #1 to remove the "extra sign bit" (matches dsp_asl56(1)
 *      inside dsp_mul56).
 *   4. Optionally NEG for sign=minus.
 *   5. SBFX #0 #56 to sign-extend back to 64-bit under the JIT's
 *      "56-bit value sign-extended into X-reg" convention.
 *   6. MAC/MACR: load accu, ADD, compute V flag.
 *   7. MPYR/MACR: BLR dsp_jit_helper_rnd56 to apply convergent
 *      rounding (complex; shared helper).
 *   8. Store accu back.
 *   9. SR update:
 *        MAC/MACR : SR &= ~V; SR |= newsr & 0xfe;
 *        MPY/MPYR : SR &= ~V;
 *  10. E/U/N/Z via shared helper.
 */
static void emit_alu_mac(ArmEmit *e, const AluVariant *v)
{
    int is_mac  = (v->kind == ALU_KIND_MAC  || v->kind == ALU_KIND_MACR);
    int is_rnd  = (v->kind == ALU_KIND_MPYR || v->kind == ALU_KIND_MACR);
    int sign_m  = v->mac_sign;

    /* Load src1, src2 as signed 24-bit into W9, W10. (Using wider
     * allocation than prior ops because we need x11=prod, x12=accu
     * original, x13=accu result simultaneously.) */
    emit_ldr_w_any(e, /*rd=*/9,  /*rn=*/19, SCRATCH, OFF_REG(v->mac_src1_reg));
    emit_sbfx_x(e,  /*rd=*/9,  /*rn=*/9, 0, 24);
    emit_ldr_w_any(e, /*rd=*/10, /*rn=*/19, SCRATCH, OFF_REG(v->mac_src2_reg));
    emit_sbfx_x(e,  /*rd=*/10, /*rn=*/10, 0, 24);

    /* SMULL X11, W9, W10 — signed 32x32 -> 64. */
    emit_smull(e, /*rd=*/11, /*rn=*/9, /*rm=*/10);

    /* LSL #1 (matches dsp_asl56(dest, 1) inside dsp_mul56). */
    emit_lsl_x_imm(e, /*rd=*/11, /*rn=*/11, 1);

    /* Negate for sign=minus. */
    if (sign_m) {
        emit_neg_x(e, /*rd=*/11, /*rm=*/11);
    }

    /* Sign-extend to the JIT's 56-bit-in-X-reg convention. */
    emit_sbfx_x(e, /*rd=*/11, /*rn=*/11, 0, 56);

    int x_final = 11;  /* where the final value lives before store */
    int wnewsr  = 5;   /* holds new_sr for MAC/MACR, else unset */
    int mac_mode = is_mac;

    if (is_mac) {
        /* Load dest, compute ADD, compute overflow flag, mask C bit. */
        emit_load_accu56(e, /*xaccu=*/12, v->dst_ab, /*xtmp=*/8);
        emit_mov_x_reg(e, /*rd=*/13, /*rn=*/12);     /* save orig */
        emit_add_x_reg(e, /*rd=*/14, /*rn=*/12, /*rm=*/11);

        /* Compute flags per dsp_add56 but only use overflow (V & L).
         * MAC drops the C bit via `& 0xfe` on newsr. */
        emit_addsub_flags(e, /*xorig=*/13, /*xsrc=*/11, /*xres=*/14,
                          /*is_sub=*/0);
        /* emit_addsub_flags put carry at bit 0 of w5. MAC masks it
         * out (& 0xfe). Clear bit 0 of w5 with a UBFX+LSL pair. */
        emit_ubfx_w(e, /*rd=*/5, /*rn=*/5, 1, 15);
        emit_lsl_w_imm(e, /*rd=*/5, /*rn=*/5, 1);

        x_final = 14;
    }

    if (is_rnd) {
        /* BLR dsp_jit_helper_rnd56(dsp, packed) returns packed. */
        emit_mov_x_reg(e, /*rd=*/1, /*rn=*/x_final);
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/15,
                       (uint64_t)(uintptr_t)&dsp_jit_helper_rnd56);
        emit_blr(e, /*rn=*/15);
        emit_mov_x_reg(e, /*rd=*/x_final == 14 ? 14 : 11, /*rn=*/0);
    }

    /* Store result to accu. */
    emit_store_accu56(e, /*xaccu=*/x_final, v->dst_ab, /*xtmp=*/8);

    /* SR update: clear V; MAC also ORs newsr (with C bit already
     * cleared above). MPY/MPYR have no newsr, just clear V. */
    emit_ldrh_any(e, /*rd=*/6, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/7, (uint32_t)~(1u << DSP_SR_V) & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    if (mac_mode) {
        emit_orr_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/wnewsr);
    }
    emit_strh_any(e, /*rs=*/6, /*rn=*/19, SCRATCH, OFF_SR);

    /* E/U/N/Z on the final result. */
    emit_ccr_e_u_n_z(e, /*xaccu=*/x_final);
}

/*
 * AND / OR / EOR / NOT on A1 (or B1) only. Interpreter pattern
 * (dsp_emu.c.inc:966 and friends):
 *   A1 = A1 OP src;   // src is one of X0/Y0/X1/Y1 (raw register value)
 *   SR &= ~(N|Z|V);
 *   SR |= ((A1>>23) & 1) << N;
 *   SR |= (A1 == 0) << Z;
 *
 * For NOT, "src" is irrelevant (A1 = ~A1 with & BITMASK(24) applied).
 */
static void emit_alu_logical(ArmEmit *e, const AluVariant *v)
{
    int off_a1 = accu_off(v->dst_ab, 1);

    /* Load A1/B1 into w10. */
    emit_ldr_w_any(e, /*rd=*/10, /*rn=*/19, SCRATCH, off_a1);

    if (v->kind == ALU_KIND_NOT) {
        emit_mvn_w(e, /*rd=*/10, /*rm=*/10);
    } else {
        emit_ldr_w_any(e, /*rd=*/11, /*rn=*/19, SCRATCH, OFF_REG(v->src_reg));
        switch (v->kind) {
        case ALU_KIND_AND: emit_and_w_reg(e, 10, 10, 11); break;
        case ALU_KIND_OR:  emit_orr_w_reg(e, 10, 10, 11); break;
        case ALU_KIND_EOR: emit_eor_w_reg(e, 10, 10, 11); break;
        default: assert(!"bad logical kind"); return;
        }
    }

    /* Mask to 24 bits (matches interp's `& BITMASK(24)` and keeps
     * the invariant A1 has clean upper bits for subsequent ops). */
    emit_ubfx_w(e, /*rd=*/10, /*rn=*/10, 0, 24);

    /* Store back. */
    emit_str_w_any(e, /*rs=*/10, /*rn=*/19, SCRATCH, off_a1);

    /* SR: clear N|Z|V, set N = bit 23 of A1, Z = (A1 == 0). */
    emit_ldrh_any(e, /*rd=*/6, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/7, (uint32_t)~((1u << DSP_SR_N) | (1u << DSP_SR_Z) |
                                             (1u << DSP_SR_V)) & 0xFFFFu);
    emit_and_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);

    /* Extract N bit: (A1 >> 23) & 1 -> w7, shift into SR.N (bit 3). */
    emit_ubfx_w(e, /*rd=*/7, /*rn=*/10, 23, 1);
    emit_bfi_x(e, /*rd=*/6, /*rn=*/7, DSP_SR_N, 1);

    /* Z bit: set bit Z if w10 == 0. CBZ/CSET pattern via conditional
     * branch around the "set Z" store. */
    emit_cmp_w_imm(e, /*rn=*/10, 0);
    uint32_t *skip_z = e->buf;
    emit_bcond(e, ARM_COND_NE, 0);
    emit_mov_imm32(e, /*rd=*/7, 1u << DSP_SR_Z);
    emit_orr_w_reg(e, /*rd=*/6, /*rn=*/6, /*rm=*/7);
    patch_branch(skip_z, (int32_t)((uint8_t *)e->buf - (uint8_t *)skip_z));

    emit_strh_any(e, /*rs=*/6, /*rn=*/19, SCRATCH, OFF_SR);
}

/*
 * TFR family. Two shapes:
 *   - tfr_b_a / tfr_a_b (ALU_SRC_ACCU_A/B): 3-word copy, no flags.
 *   - tfr_X0/X1/Y0/Y1_{a,b}: zero A0/B0, copy X/Y into A1/B1, set
 *     A2/B2 to 0xff or 0 based on sign bit. No flags.
 */
static void emit_alu_tfr(ArmEmit *e, const AluVariant *v)
{
    if (v->src_form == ALU_SRC_ACCU_A || v->src_form == ALU_SRC_ACCU_B) {
        int src_ab = (v->src_form == ALU_SRC_ACCU_A) ? 0 : 1;
        /* 3-word copy. Assumes A and B never alias (always distinct). */
        emit_ldr_w_any(e, /*rd=*/10, /*rn=*/19, SCRATCH, accu_off(src_ab, 0));
        emit_str_w_any(e, /*rs=*/10, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 0));
        emit_ldr_w_any(e, /*rd=*/10, /*rn=*/19, SCRATCH, accu_off(src_ab, 1));
        emit_str_w_any(e, /*rs=*/10, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 1));
        emit_ldrb_any(e, /*rd=*/10, /*rn=*/19, SCRATCH, accu_off(src_ab, 2));
        emit_strb_any(e, /*rs=*/10, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 2));
        return;
    }

    /* X0/X1/Y0/Y1 -> A/B with sign-extend. */
    emit_ldr_w_any(e, /*rd=*/10, /*rn=*/19, SCRATCH, OFF_REG(v->src_reg));

    /* A0 / B0 = 0 */
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 0));
    /* A1 / B1 = src (keep full 32 bits — interp doesn't mask here). */
    emit_str_w_any(e, /*rs=*/10, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 1));

    /* A2 = (A1 bit 23) ? 0xff : 0. Compute via UBFX bit 23 + NEG
     * + mask to 8. */
    emit_ubfx_w(e, /*rd=*/7, /*rn=*/10, 23, 1);
    emit_neg_w(e, /*rd=*/7, /*rm=*/7);     /* 0 or 0xFFFFFFFF */
    emit_ubfx_w(e, /*rd=*/7, /*rn=*/7, 0, 8);
    emit_strb_any(e, /*rs=*/7, /*rn=*/19, SCRATCH, accu_off(v->dst_ab, 2));
}

/* --------------------------------------------------------------- *
 * ALU stats — total inlined vs BLR-fallback ALU ops. Printed by
 * XEMU_DSP_JIT_STATS on emulator exit so we can see what fraction
 * of ALU opcodes Phase 2 covers on the running game.
 *
 * Incremented at translate time (not execute time), so counts
 * "ALU ops emitted" in the code buffer rather than "ALU ops run".
 * That's enough to confirm inlining coverage on a given workload.
 * --------------------------------------------------------------- */
static uint64_t g_alu_inlined_count;
static uint64_t g_alu_fallback_count;

/*
 * Dispatch: emit the appropriate inline ALU kernel. Returns true
 * if it handled the op (caller emits nothing else); false means
 * "fall back to BLR alu" (caller emits the BLR).
 */
static bool emit_alu_inline(ArmEmit *e, const AluVariant *v)
{
    switch (v->kind) {
    case ALU_KIND_ADD:
    case ALU_KIND_SUB:
    case ALU_KIND_CMP:
    case ALU_KIND_CMPM:
        emit_alu_arith(e, v);
        return true;
    case ALU_KIND_TST:
        emit_alu_tst(e, v);
        return true;
    case ALU_KIND_CLR:
        emit_alu_clr(e, v);
        return true;
    case ALU_KIND_NEG:
        emit_alu_neg(e, v);
        return true;
    case ALU_KIND_ABS:
        emit_alu_abs(e, v);
        return true;
    case ALU_KIND_AND:
    case ALU_KIND_OR:
    case ALU_KIND_EOR:
    case ALU_KIND_NOT:
        emit_alu_logical(e, v);
        return true;
    case ALU_KIND_TFR:
        emit_alu_tfr(e, v);
        return true;
    case ALU_KIND_MPY:
    case ALU_KIND_MPYR:
    case ALU_KIND_MAC:
    case ALU_KIND_MACR:
        emit_alu_mac(e, v);
        return true;
    case ALU_KIND_ASL:
        emit_alu_asl(e, v);
        return true;
    case ALU_KIND_ASR:
        emit_alu_asr(e, v);
        return true;
    case ALU_KIND_LSL:
        emit_alu_lsl(e, v);
        return true;
    case ALU_KIND_LSR:
        emit_alu_lsr(e, v);
        return true;
    default:
        /* FALLBACK and MOVE fall through here. Caller BLRs the
         * handler (or skips entirely for MOVE). */
        return false;
    }
}

/*
 * Single entry point for all parmove stubs. Replaces the
 * old "emit_mov_x_reg + emit_mov_imm64 + emit_blr" trio with a
 * classify+inline-or-BLR dispatch.
 *
 * alu_op : the low 8 bits of inst (the opcodes_alu[] index).
 * alu    : the emu_func_t the parmove stub would have BLR'd. May
 *          be NULL for emu_move (caller is expected to skip that
 *          case already via dsp_jit_helper_alu_is_move).
 *
 * On ALU_KIND_FALLBACK or any classification we can't inline yet
 * (shifts), we emit the old BLR sequence — identical behaviour to
 * pre-Phase-2.
 */
static void emit_alu_call(ArmEmit *e, uint8_t alu_op, emu_func_t alu)
{
    if (alu == NULL) {
        /* Caller already filtered emu_move; nothing to emit. */
        return;
    }

    AluVariant v;
    alu_classify_opcode(alu_op, &v);

    if (v.kind != ALU_KIND_FALLBACK && v.kind != ALU_KIND_MOVE) {
        if (emit_alu_inline(e, &v)) {
            g_alu_inlined_count++;
            return;
        }
    }

    /* Fallback: BLR the existing C handler — identical to pre-
     * Phase-2 behaviour. */
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
    emit_blr(e, /*rn=*/1);
    g_alu_fallback_count++;
}

/* --------------------------------------------------------------- *
 * Parmove translators
 * --------------------------------------------------------------- */

/* Forward declarations so pm_2 can fall through to pm_3. */
static void emit_parmove_pm3(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc);

/*
 * emu_pm_0 — 0000_100d_00mm_mrrr: `S,x:ea  x0,D` or
 *           `S,y:ea  y0,D` (two simultaneous moves per parmove).
 *
 * memspace = bit 15, numreg = bit 16 (0=A, 1=B).
 * ea_mode  = bits 13:8.
 *
 * Pre-ALU captures: accu (A or B) and X0 or Y0.
 * Post-ALU writes:  memory[addr] = accu, A/B = x0y0 (with sign-ext).
 *
 * Callee-saved register allocation across the ALU BLR:
 *   x22 = addr (16-bit xy_addr)
 *   x23 = save_accu (limited 24-bit A/B value)
 *   x24 = save_xy0  (24-bit X0 or Y0 raw register value)
 */
static void emit_parmove_pm0(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    uint32_t memspace = (inst >> 15) & 1;
    uint32_t numreg   = (inst >> 16) & 1;   /* 0 = A, 1 = B */
    uint32_t value6   = (inst >> 8) & 0x3f;
    int dsp_ab = numreg ? DSP_REG_B : DSP_REG_A;
    int xy0_reg = (memspace == 0) ? DSP_REG_X0 : DSP_REG_Y0;

    /* addr into x22 (retour irrelevant for pm_0; it never uses imm form) */
    emit_calc_ea_inline(e, value6, /*out_addr_reg=*/22, /*want_retour=*/false,
                        dsp, pc);

    /* save_accu = A/B (limited, through pm_read_accu24) */
    emit_pm_read_reg(e, dsp_ab, /*value_reg=*/23);

    /* save_xy0 = X0 or Y0 (direct register load) */
    emit_ldr_w_any(e, /*rd=*/24, /*rn=*/19, SCRATCH, OFF_REG(xy0_reg));

    emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

    /* memory[addr] = save_accu */
    emit_mem_write_xy(e, (int)memspace, /*addr_reg=*/22, /*value_reg=*/23);

    /* A/B accu <- save_xy0 (with sign-ext to A2/B2) */
    /* pm_0 only targets A or B via this helper; the A/B branch in
     * emit_pm_write_reg ignores mask_to_width, so it's a don't-care. */
    emit_pm_write_reg(e, dsp_ab, /*value_reg=*/24, /*mask_to_width=*/false);
}

/*
 * emu_pm_1 — 0001_ffdf_w1mm_mrrr: two parmoves per instruction,
 * x:ea or y:ea on one side plus S2/D2 register-register on the
 * other. Direction for move 1 given by bit 15; memspace by bit 14.
 *
 * numreg1 mapping depends on memspace (compile-time):
 *   memspace=0 (X:): (inst>>18)&3 → 0/1/2/3 = X0/X1/A/B
 *   memspace=1 (Y:): (inst>>16)&3 → 0/1/2/3 = Y0/Y1/A/B
 *
 * numreg2 (S2 source, always A or B):
 *   memspace=0: DSP_REG_A + ((inst>>17)&1)
 *   memspace=1: DSP_REG_A + ((inst>>19)&1)
 *
 * numreg2 (D2 dest):
 *   memspace=0: DSP_REG_Y0 + ((inst>>16)&1)
 *   memspace=1: DSP_REG_X0 + ((inst>>18)&1)
 *
 * Register allocation:
 *   x22 = xy_addr
 *   x23 = save_1 (reg or mem content)
 *   x24 = save_2 (always from A/B via pm_read_accu24)
 */
static void emit_parmove_pm1(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    uint32_t value6   = (inst >> 8) & 0x3f;
    uint32_t memspace = (inst >> 14) & 1;
    bool     write_d  = (inst & (1u << 15)) != 0;

    int numreg1;
    if (memspace) {
        switch ((inst >> 16) & 3) {
        case 0: numreg1 = DSP_REG_Y0; break;
        case 1: numreg1 = DSP_REG_Y1; break;
        case 2: numreg1 = DSP_REG_A;  break;
        default: numreg1 = DSP_REG_B; break;
        }
    } else {
        switch ((inst >> 18) & 3) {
        case 0: numreg1 = DSP_REG_X0; break;
        case 1: numreg1 = DSP_REG_X1; break;
        case 2: numreg1 = DSP_REG_A;  break;
        default: numreg1 = DSP_REG_B; break;
        }
    }

    int s2_numreg = DSP_REG_A + (memspace ? ((inst >> 19) & 1)
                                          : ((inst >> 17) & 1));
    int d2_numreg = memspace ? (DSP_REG_X0 + ((inst >> 18) & 1))
                             : (DSP_REG_Y0 + ((inst >> 16) & 1));

    /* xy_addr into x22 with retour support (for the immediate-in-D1 form). */
    emit_calc_ea_inline(e, value6, /*out_addr_reg=*/22, /*want_retour=*/true,
                        dsp, pc);

    if (write_d) {
        /* save_1 = (retour ? xy_addr : memory[xy_addr]) */
        emit_ldr_w_imm(e, /*rd=*/0, /*rn=*/31, OFF_SP_SCRATCH1);
        uint32_t *to_mem = e->buf;
        emit_cbz_w(e, /*rn=*/0, 0);
        emit_mov_w_reg(e, /*rd=*/23, /*rn=*/22);
        uint32_t *to_after = e->buf;
        emit_b(e, 0);
        uint32_t *mem_label = e->buf;
        patch_branch(to_mem, (int32_t)((uint8_t *)mem_label -
                                       (uint8_t *)to_mem));
        emit_mem_read_xy(e, (int)memspace, /*addr_reg=*/22, /*value_reg=*/23);
        uint32_t *after_label = e->buf;
        patch_b(to_after, (int32_t)((uint8_t *)after_label -
                                    (uint8_t *)to_after));
    } else {
        emit_pm_read_reg(e, numreg1, /*value_reg=*/23);
    }

    /* save_2 = A/B via pm_read_accu24 */
    emit_pm_read_reg(e, s2_numreg, /*value_reg=*/24);

    emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

    /* Write D1 side. */
    if (write_d) {
        /* Matches the interpreter's A/B-split PLUS trailing
         * `dsp->registers[numreg1] = save_1` for bit-exact mirror. */
        if (numreg1 == DSP_REG_A || numreg1 == DSP_REG_B) {
            /* A/B branch does A0/A1/A2 split; mask flag is a don't-care. */
            emit_pm_write_reg(e, numreg1, /*value_reg=*/23,
                              /*mask_to_width=*/false);
        }
        /* Trailing unconditional registers[numreg1] = save_1. The
         * interpreter does this UNMASKED (`dsp->registers[numreg1]
         * = save_1;` in emu_pm_1's write-D1 path) regardless of the
         * destination's mask width, so we mirror that here.
         * For A/B this overwrites the scratch-slot-layout part of
         * registers[DSP_REG_A/B] (the 3-way split has already
         * happened above); for ordinary regs this is the only
         * write. For NULL / reserved indices we emit a zero-store
         * to keep bit-exact with the interpreter's `save & 0`. */
        emit_str_w_any(e, /*rs=*/23, /*rn=*/19, SCRATCH, OFF_REG(numreg1));
    } else {
        emit_mem_write_xy(e, (int)memspace, /*addr_reg=*/22, /*value_reg=*/23);
    }

    /* S2 -> D2: registers[d2_numreg] = save_2 (no mask needed; save_2
     * is already 24-bit and D2 is always X0/X1/Y0/Y1, all 24-bit). */
    emit_str_w_any(e, /*rs=*/24, /*rn=*/19, SCRATCH, OFF_REG(d2_numreg));
}

/* Forward: we call emit_parmove_pm5 from emit_parmove_pm4. */
static void emit_parmove_pm5(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc);

/*
 * emu_pm_4 — 0100_l0ll_wXaa_aaaa / 01dd_Xddd_wXmm_mrrr.
 *
 * Top-level discriminator: `(inst & 0xf40000) == 0x400000` selects
 * pm_4x (long-accu l:ea dual-word memory move); else fall through
 * to pm_5 (single x:/y: move). Check is compile-time.
 *
 * pm_4x itself is complex (8-case numreg decode + limit-aware
 * accu read + dual x:/y: memory I/O). We BLR a C shim for it
 * rather than inlining — it's infrequent enough that the per-op
 * BLR overhead is acceptable vs the ~300 extra lines of ARM64
 * translation that a full inline would need.
 */
static void emit_parmove_pm4(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    if ((inst & 0xf40000u) == 0x400000u) {
        /* pm_4x (long-accu l:ea). Full helper BLR — the helper
         * runs fetch + ALU + write internally, so we do NOT BLR
         * the ALU separately here. cur_inst / cur_inst_len /
         * instr_cycle were already set up by emit_parmove_stub. */
        (void)alu;  /* ALU call is inside the helper */
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/1,
            (uint64_t)(uintptr_t)&dsp_jit_helper_pm_4x);
        emit_blr(e, /*rn=*/1);
        return;
    }
    emit_parmove_pm5(e, inst, alu, dsp, pc);
}

/*
 * emu_pm_8 — 1wmm_eeff_WrrM_MRRR: dual simultaneous X and Y parmoves
 * with optional register-register halves. THE hot form for FIR
 * filter kernels (classic DSP56300 `x:(r0)+,x0  y:(r4)+,y0  mac ...`).
 *
 * Compile-time decoding:
 *   ea1:  5 bits at inst[12:8] + top-bit fixup if (ea1>>3)==0
 *   ea2:  bits 14:13 at ea2[1:0], bits 21:20 at ea2[4:3],
 *         bit 2 = !(ea1 bit 2), top-bit fixup same as ea1
 *   numreg1 = bits 19:18 → X0 / X1 / A / B
 *   numreg2 = bits 17:16 → Y0 / Y1 / A / B
 *   write_d1 = bit 15 (1 = mem→reg; 0 = reg→mem)
 *   write_d2 = bit 22 (same semantics, on the Y side)
 *
 * Register allocation across the ALU BLR:
 *   x22 = x_addr
 *   x23 = y_addr
 *   x24 = save_reg1
 *   x25 = save_reg2
 */
static void emit_parmove_pm8(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    uint32_t ea1 = (inst >> 8) & 0x1f;
    if ((ea1 >> 3) == 0) {
        ea1 |= (1u << 5);
    }
    uint32_t ea2 = (inst >> 13) & 0x3;
    ea2 |= ((inst >> 20) & 0x3) << 3;    /* bits 21:20 → bits 4:3 */
    if ((ea1 & (1u << 2)) == 0) {
        ea2 |= 1u << 2;
    }
    if ((ea2 >> 3) == 0) {
        ea2 |= (1u << 5);
    }

    int numreg1;
    switch ((inst >> 18) & 0x3) {
    case 0: numreg1 = DSP_REG_X0; break;
    case 1: numreg1 = DSP_REG_X1; break;
    case 2: numreg1 = DSP_REG_A;  break;
    default: numreg1 = DSP_REG_B; break;
    }

    int numreg2;
    switch ((inst >> 16) & 0x3) {
    case 0: numreg2 = DSP_REG_Y0; break;
    case 1: numreg2 = DSP_REG_Y1; break;
    case 2: numreg2 = DSP_REG_A;  break;
    default: numreg2 = DSP_REG_B; break;
    }

    bool write_d1 = (inst & (1u << 15)) != 0;
    bool write_d2 = (inst & (1u << 22)) != 0;

    /* Address computations — pm_8 never uses the retour flag
     * (both ea1 and ea2 always have their top bit forced on, so
     * they never select mode-6 absolute-immediate). */
    emit_calc_ea_inline(e, ea1, /*out_addr_reg=*/22, /*want_retour=*/false,
                        dsp, pc);
    emit_calc_ea_inline(e, ea2, /*out_addr_reg=*/23, /*want_retour=*/false,
                        dsp, pc);

    /* Fetch save_reg1. */
    if (write_d1) {
        emit_mem_read_xy(e, DSP_SPACE_X, /*addr_reg=*/22, /*value_reg=*/24);
    } else {
        emit_pm_read_reg(e, numreg1, /*value_reg=*/24);
    }

    /* Fetch save_reg2. */
    if (write_d2) {
        emit_mem_read_xy(e, DSP_SPACE_Y, /*addr_reg=*/23, /*value_reg=*/25);
    } else {
        emit_pm_read_reg(e, numreg2, /*value_reg=*/25);
    }

    emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

    /* Write first parmove. */
    if (write_d1) {
        /* Register destination (may be A/B accu split). The
         * interpreter's pm_8 write path does NOT have the
         * "else{} dsp->registers[numreg1] = save_1" quirk that
         * pm_1 does — the else branch is guarded correctly here. */
        if (numreg1 == DSP_REG_A || numreg1 == DSP_REG_B) {
            emit_pm_write_reg(e, numreg1, /*value_reg=*/24,
                              /*mask_to_width=*/false);
        } else {
            /* "dsp->registers[numreg1] = save_reg1" — unmasked in
             * the interpreter; we replicate that (numreg1 is always
             * X0/X1/Y0/Y1 in this branch, all 24-bit regs). */
            emit_str_w_any(e, /*rs=*/24, /*rn=*/19, SCRATCH, OFF_REG(numreg1));
        }
    } else {
        emit_mem_write_xy(e, DSP_SPACE_X, /*addr_reg=*/22, /*value_reg=*/24);
    }

    /* Write second parmove. */
    if (write_d2) {
        if (numreg2 == DSP_REG_A || numreg2 == DSP_REG_B) {
            emit_pm_write_reg(e, numreg2, /*value_reg=*/25,
                              /*mask_to_width=*/false);
        } else {
            emit_str_w_any(e, /*rs=*/25, /*rn=*/19, SCRATCH, OFF_REG(numreg2));
        }
    } else {
        emit_mem_write_xy(e, DSP_SPACE_Y, /*addr_reg=*/23, /*value_reg=*/25);
    }
}

/*
 * emu_pm_2 — four sub-cases discriminated by compile-time masks
 * on cur_inst:
 *   (inst & 0xFFFF00) == 0x200000  → NOP (just run ALU)
 *   (inst & 0xFFE000) == 0x204000  → R update only (calc_ea side effect)
 *   (inst & 0xFC0000) == 0x200000  → emu_pm_2_2 (reg-reg S,D)
 *   otherwise                       → emu_pm_3 fall-through (#xx,R)
 *
 * All four decode entirely at translate time — we pick one inline
 * expansion per instruction.
 */
static void emit_parmove_pm2_2(ArmEmit *e, uint32_t inst, emu_func_t alu,
                               dsp_core_t *dsp, uint32_t pc)
{
    (void)dsp; (void)pc;  /* no calc_ea in pm_2_2 */
    /* 0010 00ee eeed dddd S,D (reg-reg) */
    int srcreg = (int)((inst >> 13) & 0x1f);
    int dstreg = (int)((inst >> 8)  & 0x1f);

    emit_pm_read_reg(e, srcreg, /*value_reg=*/23);

    emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

    /* pm_2_2: interpreter masks with registers_mask[dstreg]
     * (dsp_emu.c.inc line 5398). */
    emit_pm_write_reg(e, dstreg, /*value_reg=*/23, /*mask_to_width=*/true);
}

static void emit_parmove_pm2(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    if ((inst & 0xffff00u) == 0x200000u) {
        /* NOP parmove — ALU only. */
        emit_alu_call(e, (uint8_t)(inst & 0xff), alu);
        return;
    }

    if ((inst & 0xffe000u) == 0x204000u) {
        /* R update — calc_ea with 5-bit ea_mode (mode 0-3 only,
         * since bit 12 = 0 in the mask). Output address is ignored
         * (interpreter passes &dummy). */
        uint32_t ea_mode = (inst >> 8) & 0x1f;
        emit_calc_ea_inline(e, ea_mode, /*out_addr_reg=*/22,
                            /*want_retour=*/false, dsp, pc);
        emit_alu_call(e, (uint8_t)(inst & 0xff), alu);
        return;
    }

    if ((inst & 0xfc0000u) == 0x200000u) {
        emit_parmove_pm2_2(e, inst, alu, dsp, pc);
        return;
    }

    /* Fall-through to pm_3 (literal-to-reg). */
    emit_parmove_pm3(e, inst, alu, dsp, pc);
}

/*
 * emu_pm_3  — 001d_dddd iiii_iiii #xx,R (literal into register).
 *
 * dstreg and the 8-bit literal are compile-time constants.
 * For 24-bit "wide" regs (X0/X1/Y0/Y1/A/B), the literal is
 * placed in bits 23:16 (shift left 16); for others the register's
 * natural width mask applies.
 *
 * The ALU (opcodes_alu[inst & 0xff]) runs BEFORE the register
 * write (interpreter order). For emu_move (opcode 0x00, no-op ALU)
 * the caller is expected to pass a NULL alu and we skip the BLR.
 */
static void emit_parmove_pm3(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    (void)dsp; (void)pc;  /* no calc_ea in pm_3 */
    uint32_t dstreg   = (inst >> 16) & 0x1f;
    uint32_t srcvalue = (inst >> 8)  & 0xff;

    /* Interpreter shifts srcvalue left 16 for wide destinations. */
    bool shift_left_16 =
        dstreg == DSP_REG_X0 || dstreg == DSP_REG_X1 ||
        dstreg == DSP_REG_Y0 || dstreg == DSP_REG_Y1 ||
        dstreg == DSP_REG_A  || dstreg == DSP_REG_B;
    uint32_t final_value = shift_left_16 ? (srcvalue << 16) : srcvalue;

    /* ALU first. */
    emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

    /* Load the final value into a scratch and write to destination.
     * pm_3: interpreter masks with registers_mask[dstreg]
     * (dsp_emu.c.inc line 5438). */
    emit_mov_imm32(e, /*rd=*/4, final_value);
    emit_pm_write_reg(e, dstreg, /*value_reg=*/4, /*mask_to_width=*/true);
}

/*
 * emu_pm_5 — single x:/y: memory + register move.
 *
 * Bit 15 of inst selects direction:
 *   bit15 = 1 → read memory into register (D)
 *   bit15 = 0 → read register, write to memory (S)
 *
 * Bit 14 selects addressing form:
 *   bit14 = 1 → ea via MMMRRR (calc_ea)
 *   bit14 = 0 → short absolute address in inst[13:8]
 *
 * memspace = (inst>>19) & 1 (0=X, 1=Y).
 * numreg   = ((inst>>16) & 7) | (((inst>>17) & 3) << 3) — 5-bit dest reg.
 *
 * Register allocation across the ALU BLR:
 *   x22 = xy_addr (needed for mem write path; for read-from-mem
 *                  path, addr is used immediately before ALU)
 *   x23 = value (source register value / loaded memory value)
 */
static void emit_parmove_pm5(ArmEmit *e, uint32_t inst, emu_func_t alu,
                             dsp_core_t *dsp, uint32_t pc)
{
    uint32_t value6   = (inst >> 8) & 0x3f;
    uint32_t memspace = (inst >> 19) & 1;
    /* Mirrors emu_pm_5: numreg low 3 bits from inst[18:16], high
     * 2 bits from inst[21:20] positioned at numreg[4:3]. */
    uint32_t numreg   = ((inst >> 16) & 7) | (((inst >> 20) & 3) << 3);
    bool     write_d  = (inst & (1u << 15)) != 0;   /* dir */
    bool     ea_form  = (inst & (1u << 14)) != 0;

    /*
     * Compute the address first. For ea_form=false the address is
     * just value6 (short absolute); we load as a constant. For
     * ea_form=true we go through emit_calc_ea_inline. We also need
     * retour for the "#xxxxxx,D" mode-6 path: in that case the
     * returned "addr" is actually an immediate 24-bit literal.
     *
     * For Phase 4a, we keep it simple: if ea_form and we might hit
     * mode 6 with immediate-literal semantics, we ask
     * emit_calc_ea_inline for retour and branch on it.
     */

    if (!ea_form) {
        /* Short absolute: xy_addr = value6, retour = 0 */
        emit_mov_imm32(e, /*rd=*/22, value6);
    } else {
        emit_calc_ea_inline(e, value6, /*out_addr_reg=*/22,
                            /*want_retour=*/true, dsp, pc);
    }

    if (write_d) {
        /*
         * Memory -> register. Fetch value first (possibly
         * immediate-literal via retour), then ALU, then write to
         * register.
         */

        if (ea_form) {
            /* Check retour; if set, value = xy_addr (immediate). */
            emit_ldr_w_imm(e, /*rd=*/0, /*rn=*/31, OFF_SP_SCRATCH1);
            uint32_t *to_mem = e->buf;
            emit_cbz_w(e, /*rn=*/0, 0);      /* patched to "mem-read" */

            /* Immediate case: value = xy_addr */
            emit_mov_w_reg(e, /*rd=*/23, /*rn=*/22);
            uint32_t *to_after = e->buf;
            emit_b(e, 0);

            uint32_t *mem_label = e->buf;
            patch_branch(to_mem, (int32_t)((uint8_t *)mem_label -
                                           (uint8_t *)to_mem));
            emit_mem_read_xy(e, (int)memspace, /*addr_reg=*/22,
                             /*value_reg=*/23);

            uint32_t *after_label = e->buf;
            patch_b(to_after, (int32_t)((uint8_t *)after_label -
                                        (uint8_t *)to_after));
        } else {
            /* Short-absolute: retour always 0 */
            emit_mem_read_xy(e, (int)memspace, /*addr_reg=*/22,
                             /*value_reg=*/23);
        }

        /* ALU */
        emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

        /* Write register. pm_5 (also reached via pm_4 fall-through)
         * masks with registers_mask[numreg] at dsp_emu.c.inc line
         * 5667 — without this, init-sentinel reads (0xCACACACA in
         * xram/yram) would leak into the 24-bit Y0 / Y1 / X0 / X1
         * slots. */
        emit_pm_write_reg(e, (int)numreg, /*value_reg=*/23,
                          /*mask_to_width=*/true);
    } else {
        /*
         * Register -> memory. Fetch value (accu-path for A/B),
         * then ALU, then write to memory.
         */
        emit_pm_read_reg(e, (int)numreg, /*value_reg=*/23);

        emit_alu_call(e, (uint8_t)(inst & 0xff), alu);

        emit_mem_write_xy(e, (int)memspace, /*addr_reg=*/22,
                          /*value_reg=*/23);
    }
}

/*
 * Shared per-op epilogue emitter. Called by both the non-parmove
 * stub (emit_instruction) and the parmove stubs (emit_parmove_pmN).
 *
 * Invoked AFTER the emu handler / parmove work has finished. Emits:
 *   5. postexecute_update_pc fast path (inline pc += cur_inst_len
 *      when no REP / DO loop active; else BLR x20 cached helper)
 *   6. postexecute_interrupts fast path (skip BLR when all
 *      interrupt fields are zero; else BLR x21 cached helper)
 *   7. num_inst += instr_cycle
 *   8-11. Block-exit checks: pending IRQ / loop_rep set /
 *      is_idle set / PC mismatch — all branch to the shared
 *      exit epilogue via the exits patch list.
 *
 * Clobbers: w0, w1, w2, x3 (SCRATCH). Preserves: x19-x25.
 */
/*
 * Hints the shared post-instruction epilogue can exploit to skip
 * runtime checks that are provably unreachable for the op just
 * emitted. Passed by the caller (emit_instruction /
 * emit_parmove_stub) based on static classification.
 *
 * `may_change_pc`
 *   false : the op we just emitted is known to leave dsp->pc at
 *           pc + cur_inst_len — parmove stubs (no parmove op
 *           branches) and inlined long-imm ALU ops fit this. The
 *           PC-mismatch exit check (step 11) is therefore
 *           unreachable and elided. Saves ~4 ARM64 insns per op.
 *   true  : we don't know what pc will be; emit the check.
 */
typedef struct EpilogueHints {
    bool may_change_pc;
} EpilogueHints;

static const EpilogueHints EPI_UNKNOWN = { .may_change_pc = true  };
static const EpilogueHints EPI_NO_PC   = { .may_change_pc = false };

static void emit_post_instruction_epilogue(ArmEmit *e, ExitPatchList *exits,
                                           uint32_t expected_next_pc,
                                           EpilogueHints hints)
{
    /*
     * Step 5: inline postexecute_update_pc fast path. The helper
     * does (a) REP state machine if loop_rep set; (b) pc +=
     * cur_inst_len; (c) DO loop end check if SR.LF set. Common
     * case (no REP, no DO) is just (b). Emit inline for the
     * common case.
     */
    {
        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_LOOP_REP);
        uint32_t *to_call_helper_1 = e->buf;
        emit_cbnz_w(e, 0, 0);       /* patched below */

        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_SR);
        uint32_t *to_call_helper_2 = e->buf;
        emit_tbnz_w(e, /*rt=*/0, DSP_SR_LF, 0);   /* patched below */

        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
        emit_ldr_w_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_PC);
        emit_add_w_reg(e, /*rd=*/1, /*rn=*/1, /*rm=*/0);
        emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_PC);

        uint32_t *to_after = e->buf;
        emit_b(e, 0);

        uint32_t *call_helper_label = e->buf;
        patch_branch(to_call_helper_1,
                     (int32_t)((uint8_t *)call_helper_label -
                               (uint8_t *)to_call_helper_1));
        /* TBNZ uses imm14 at bits 18:5 — custom patch. */
        {
            int32_t off = (int32_t)((uint8_t *)call_helper_label -
                                    (uint8_t *)to_call_helper_2);
            int32_t imm14 = off >> 2;
            assert(imm14 >= -(1 << 13) && imm14 < (1 << 13));
            uint32_t insn = *to_call_helper_2 & ~(0x3fffu << 5);
            insn |= ((uint32_t)imm14 & 0x3fff) << 5;
            *to_call_helper_2 = insn;
        }
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_blr(e, /*rn=*/20);    /* x20 = cached helper address */

        uint32_t *after_label = e->buf;
        patch_b(to_after, (int32_t)((uint8_t *)after_label -
                                    (uint8_t *)to_after));
    }

    /*
     * Step 6: inline postexecute_interrupts fast path. Helper is a
     * no-op when every interrupt-tracking field is zero; skip the
     * BLR in that common case. SR.T (trace bit) is NOT in the
     * quick check — xemu doesn't expose a DSP single-step debugger
     * so no production title sets it.
     */
    {
        emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_STATE);
        emit_ldrh_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_INTERRUPT_COUNTER);
        emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/2);
        emit_ldrh_any(e, /*rd=*/2, /*rn=*/19, SCRATCH,
                      OFF_INTERRUPT_PIPELINE_COUNT);
        emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/2);

        uint32_t *skip_site = e->buf;
        emit_cbz_w(e, /*rn=*/0, 0);

        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_blr(e, /*rn=*/21);    /* x21 = cached helper address */

        uint32_t *after_label = e->buf;
        patch_branch(skip_site,
                     (int32_t)((uint8_t *)after_label -
                               (uint8_t *)skip_site));
    }

    /* Step 7: num_inst += instr_cycle */
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
    emit_ldr_w_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_NUM_INST);
    emit_add_w_reg(e, /*rd=*/1, /*rn=*/1, /*rm=*/0);
    emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_NUM_INST);

    /* Step 8: exit-check: interrupt_counter (uint16_t). Kept as its
     * own branch — interrupt_counter is the primary signal for
     * "there's a pending interrupt, re-enter dispatcher so it can
     * hand off to the C interpreter for pipeline-accurate delivery". */
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_COUNTER);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /*
     * Steps 9 / 10 / 10b fused:
     *   loop_rep (u32)          — REP entered; block-boundary exit.
     *   is_idle (u8)            — STOP / WAIT executed.
     *   jit_exit_block_request  — self-mod: pram write invalidated
     *                             a block (possibly this one).
     *
     * All three are "exit-to-dispatcher" signals with identical
     * handling. Instead of three separate LDR + CBNZ pairs, OR
     * them into w0 and branch once. Saves two exit-patch entries
     * + two CBNZ instructions per op with no change in semantics
     * (a non-zero OR still triggers the same exit path). The
     * ordering of bytes in the OR is arbitrary since we're only
     * testing for non-zero.
     */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_LOOP_REP);
    emit_ldrb_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_IS_IDLE);
    emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);
    emit_ldrb_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_JIT_EXIT_BLOCK_REQ);
    emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /*
     * Step 11: exit-check: PC mismatch (branch taken by handler).
     * Skipped when the caller promises the op cannot change pc
     * (parmove stubs, inlined long-imm ALU, inlined ALU kernels).
     * Also skipped for terminator ops that exit via other means —
     * wait, no: terminators are exactly where pc CAN change, so
     * keep the check for them. The skip is safe only when
     * may_change_pc is false.
     *
     * Emission uses SUB + CBNZ rather than MOV imm + CMP + BCOND
     * — saves 1 ARM64 insn because expected_next_pc fits in imm12
     * (DSP PRAM is 4 KB, so pc < 0x1000; but the SUB can safely
     * take a larger imm12 anyway up to 0xFFF which is the max
     * valid pc). Non-zero SUB result means mismatch; CBNZ branches
     * to the exit path.
     */
    if (hints.may_change_pc) {
        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_PC);
        if (expected_next_pc <= 0xfff) {
            /* Fast path: expected_next_pc fits in ARM64 SUB's
             * imm12. `w0 = pc - expected_next_pc; cbnz w0, exit`
             * in 2 insns (vs the MOV imm32 + CMP + BCOND form
             * below). DSP PRAM is 4 KiB so pc_start + inst_len
             * normally stays inside 12 bits; blocks that translate
             * through the last PRAM word hit the edge case below. */
            emit_sub_w_imm(e, /*rd=*/0, /*rn=*/0, expected_next_pc);
            uint32_t *site = e->buf;
            emit_cbnz_w(e, 0, 0);
            record_exit_patch(exits, site);
        } else {
            /* Edge case (expected_next_pc > 0xfff — possible when
             * the block's last op is 2-word at pc == 0xffe). Fall
             * back to the full MOV imm32 + CMP + B.cond form. */
            emit_mov_imm32(e, /*rd=*/1, expected_next_pc);
            emit_cmp_w_reg(e, /*rn=*/0, /*rm=*/1);
            uint32_t *site = e->buf;
            emit_bcond(e, ARM_COND_NE, 0);
            record_exit_patch(exits, site);
        }
    }
}


/*
 * Emit a parmove stub: inst has inst >= 0x100000 (parallel-move form).
 *
 * Returns true if the parmove select is supported in this phase.
 * Returns false if the parmove should fall back to the interpreter
 * (translate_block's caller handles the bail).
 *
 * On success, emits:
 *   - set cur_inst / cur_inst_len=1 / instr_cycle=2
 *   - call the per-variant translator (emit_parmove_pmN)
 *   - call emit_post_instruction_epilogue
 */
/*
 * Compute the write-set contribution for a parmove instruction.
 * Called by emit_parmove_stub at translation time; purely static
 * decoding (no emitted code). The decoding mirrors the branching
 * inside each emit_parmove_pmN() exactly so we never under-estimate
 * what a stub might write to.
 */
static uint32_t parmove_write_set(uint32_t inst)
{
    uint32_t select = (inst >> 20) & 0xf;
    uint32_t ws = 0;

    /* Memory-write helpers. For any X-space write (memspace=X) we
     * conservatively include xram + mixbuffer + periph, because the
     * runtime address determines which one (addr < 0xC00 -> xram;
     * 0xC00..0xC1F -> mixbuffer; >= 0xFFFF80 -> peripheral). */
    const uint32_t X_ANY = DSP_JIT_WS_XRAM | DSP_JIT_WS_MIXBUFFER |
                           DSP_JIT_WS_PERIPH;
    const uint32_t Y_ANY = DSP_JIT_WS_YRAM;

    switch (select) {
    case 0: {
        /* pm_0: unconditionally writes memory (S = A/B -> mem, mem -> D).
         * memspace = bit 15. */
        uint32_t memspace = (inst >> 15) & 1;
        ws |= memspace ? Y_ANY : X_ANY;
        break;
    }
    case 1: {
        /* pm_1: writes memory only if W=0 (S1 -> x:ea or y:ea).
         * memspace = bit 14, W = bit 15. */
        bool write_mem = ((inst >> 15) & 1) == 0;
        if (write_mem) {
            uint32_t memspace = (inst >> 14) & 1;
            ws |= memspace ? Y_ANY : X_ANY;
        }
        break;
    }
    case 2:
    case 3:
        /* pm_2 family (NOP / R-update / reg-reg) and pm_3 (#xx,R)
         * are register-only: no memory side effects. */
        break;
    case 4: {
        /* pm_4: pm_4x (dual X+Y long-accu) or fall-through to pm_5.
         * pm_4x selector: (inst & 0xf40000) == 0x400000.
         * Both subvariants write memory only if W=0 (bit 15). */
        bool is_pm_4x = (inst & 0xf40000u) == 0x400000u;
        bool write_mem = ((inst >> 15) & 1) == 0;
        if (write_mem) {
            if (is_pm_4x) {
                /* l:ea writes BOTH x[addr] and y[addr] on the same
                 * cycle (dsp_emu.c.inc:5601-5602). */
                ws |= X_ANY | Y_ANY;
            } else {
                uint32_t memspace = (inst >> 19) & 1;
                ws |= memspace ? Y_ANY : X_ANY;
            }
        }
        break;
    }
    case 5:
    case 6:
    case 7: {
        /* pm_5: single x:/y: move. W=bit 15, memspace=bit 19. */
        bool write_mem = ((inst >> 15) & 1) == 0;
        if (write_mem) {
            uint32_t memspace = (inst >> 19) & 1;
            ws |= memspace ? Y_ANY : X_ANY;
        }
        break;
    }
    case 8:  case 9:  case 10: case 11:
    case 12: case 13: case 14: case 15: {
        /* pm_8: dual independent X+Y moves. Bit 15 = W for X side;
         * bit 22 = W for Y side. */
        if (((inst >> 15) & 1) == 0) {
            ws |= X_ANY;
        }
        if (((inst >> 22) & 1) == 0) {
            ws |= Y_ANY;
        }
        break;
    }
    default:
        ws = DSP_JIT_WS_ALL;
        break;
    }

    return ws;
}

static bool emit_parmove_stub(ArmEmit *e, ExitPatchList *exits,
                              uint32_t inst, emu_func_t alu,
                              uint32_t expected_next_pc,
                              uint32_t *out_write_set,
                              dsp_core_t *dsp, uint32_t pc)
{
    uint32_t select = (inst >> 20) & 0xf;

    /* Set cur_inst, cur_inst_len=1, instr_cycle=2. */
    emit_mov_imm32(e, /*rd=*/0, inst);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST);
    emit_movz_w(e, /*rd=*/0, 1, 0);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_movz_w(e, /*rd=*/0, 2, 0);
    emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);

    /* Skip the ALU BLR when opcodes_alu[0] (emu_move) is the
     * selected ALU — it's a no-op, and most pure-MOVE parmoves in
     * Xbox audio take that slot. */
    emu_func_t effective_alu = dsp_jit_helper_alu_is_move(alu) ? NULL : alu;

    switch (select) {
    case 0:
        /* 0000_100d 00mm_mrrr  S,x:ea  x0,D / S,y:ea  y0,D */
        emit_parmove_pm0(e, inst, effective_alu, dsp, pc);
        break;
    case 1:
        /* 0001_ffdf w1mm_mrrr  — x:ea/y:ea + reg-reg dual move */
        emit_parmove_pm1(e, inst, effective_alu, dsp, pc);
        break;
    case 2:
        /* 0010_XXXX XXXX_XXXX — pm_2 family (NOP / R-upd / S,D / #xx,R) */
        emit_parmove_pm2(e, inst, effective_alu, dsp, pc);
        break;
    case 3:
        /* 001d_dddd iiii_iiii #xx,R */
        emit_parmove_pm3(e, inst, effective_alu, dsp, pc);
        break;
    case 4:
        /* 0100_l0ll / 01dd_0ddd — pm_4 family (long-accu l:ea
         * or fall-through to pm_5). */
        emit_parmove_pm4(e, inst, effective_alu, dsp, pc);
        break;
    case 5:
    case 6:
    case 7:
        /* 01dd_Xddd w_mm_mrrr — single x:/y: move (pm_5 family) */
        emit_parmove_pm5(e, inst, effective_alu, dsp, pc);
        break;
    case 8:  case 9:  case 10: case 11:
    case 12: case 13: case 14: case 15:
        /* 1Xmm_eeff_WrrM_MRRR — dual x:/y: simultaneous move (pm_8).
         * This is the FIR / IIR filter kernel hot path. */
        emit_parmove_pm8(e, inst, effective_alu, dsp, pc);
        break;
    default:
        /* No unsupported variants remaining after Phase 4e. */
        return false;
    }

    /*
     * Parmove stubs: emit the full PC-mismatch exit check. Round-4
     * tried to skip it (parmove ALU variants shouldn't touch pc),
     * but in practice at least one parmove sub-class DOES end up
     * changing dsp->pc via its BLR fallback, and without the check
     * the block keeps running at the stale pc and eventually jumps
     * to pram garbage (e.g. 0x001000). Keep the check until we can
     * identify the offending handler and narrow EPI_NO_PC to the
     * truly-no-pc subset.
     */
    emit_post_instruction_epilogue(e, exits, expected_next_pc, EPI_UNKNOWN);
    if (out_write_set) {
        *out_write_set |= parmove_write_set(inst);
    }
    return true;
}

/* ============================================================== *
 * Phase 5a — inline control-flow handlers (JMP / JSR / BRA / BSR /
 * RTS / RTI / JCC / JSCC / BCC).
 *
 * Replaces the per-op `BLR emu_<cf>` round-trip for the common
 * static-target branches with inline ARM64 that mirrors the
 * interpreter's handler bit-exactly. Handlers not covered (jclr /
 * jset / jsclr / jsset / brclr / brset + the `_ea` / `_reg`
 * variants of the inlined ops) fall through to ALU_FALLBACK-style
 * BLR — zero behavioural change for those opcodes.
 *
 * Invariants used by every CF emitter:
 *   - At entry, emit_instruction has already set cur_inst = inst,
 *     cur_inst_len = 1, instr_cycle = 2. The CF emitter overrides
 *     these as the interp handler would.
 *   - dsp->pc is UNCHANGED at handler entry. The post-exec update
 *     adds cur_inst_len to pc; handlers that take a branch write
 *     pc = target AND cur_inst_len = 0 so the post-exec update is
 *     a no-op; the block exits via the PC-mismatch check in
 *     emit_post_instruction_epilogue.
 *   - All inlined CF ops are classified as terminators by
 *     dsp_jit_helper_is_terminator, so translate_block ends the
 *     block after emitting them (no block-internal fall-through
 *     worries).
 * ============================================================== */

/*
 * g_cf_inlined_count  : inlined via emit_cf_call (control-flow +
 *                       loop handlers). Credited from inside that
 *                       function, not here.
 *
 * g_cf_fallback_count : non-parmove handler that went to the BLR
 *                       fallback. The name is historical — after
 *                       round 3 it also covers ALU long-imm fall-
 *                       throughs (EOR_long has no interpreter
 *                       handler) and miscellaneous non-parallel
 *                       ops (movec, movem, movep, lua, tcc, andi,
 *                       ori, rep _ea/_aa/_reg, shifts _imm, ...).
 *                       Still reported as "cf_fallback" in the
 *                       stats output for continuity.
 */
static uint64_t g_cf_inlined_count;
static uint64_t g_cf_fallback_count;

/* Helper: pc = newpc, cur_inst_len = 0. Clobbers w0. */
static void emit_cf_set_pc_branch(ArmEmit *e, uint32_t newpc)
{
    emit_mov_imm32(e, /*rd=*/0, newpc);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_PC);
    emit_str_w_any(e, /*rs=*/31 /*WZR*/, /*rn=*/19, SCRATCH,
                   OFF_CUR_INST_LEN);
}

/* Helper: instr_cycle = `cycles`. Overrides rather than adds — the
 * total cycle count for a CF op is fixed at translate time (no
 * variant depends on dsp state). Clobbers w0. */
static void emit_cf_set_cycles(ArmEmit *e, unsigned cycles)
{
    emit_movz_w(e, /*rd=*/0, (uint16_t)cycles, 0);
    emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
}

/*
 * Helper: instr_cycle += `delta`. Used by CF emitters whose
 * per-op cycle cost depends on an EA-addressing mode side effect
 * (emu_calc_ea internally does `instr_cycle += 2` for modes 5 /
 * 6 / 7). Those paths must NOT use the SET helper — it would
 * silently clobber the +2 that calc_ea just added.
 *
 * Clobbers w0, w1.
 */
static void emit_cf_add_cycles(ArmEmit *e, unsigned delta)
{
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
    emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, delta);
    emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
}

/*
 * Helper: cur_inst_len += 1. Needed for bit-test EA variants in
 * the not-taken path — matches the interp's `++cur_inst_len` in
 * jclr_ea / jset_ea / jsclr_ea / jsset_ea, which preserves the
 * cur_inst_len++ side effect that emu_calc_ea does for mode 6.
 *
 * Writing a fixed "= 2" would be wrong for mode 6 (calc_ea bumps
 * it to 2 already; the handler's ++ then takes it to 3 to skip
 * BOTH the aa immediate word AND the branch target word).
 *
 * Clobbers w0, w1.
 */
static void emit_cf_inc_cur_inst_len(ArmEmit *e)
{
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_add_w_imm(e, /*rd=*/0, /*rn=*/0, 1);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
}

/* Emit: BLR dsp_jit_helper_stack_push(dsp, return_pc, SR). Clobbers
 * x0, w1, w2, x4, x30. */
static void emit_cf_stack_push(ArmEmit *e, uint32_t return_pc)
{
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);                /* x0 = dsp */
    emit_mov_imm32(e, /*rd=*/1, return_pc);                /* w1 = retpc */
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_SR); /* w2 = SR */
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_push);
    emit_blr(e, /*rn=*/4);
}

/* Emit: BLR dsp_jit_helper_stack_pop(dsp, &SP_SCRATCH0, &SP_SCRATCH1).
 * On return, *SP_SCRATCH0 = popped pc, *SP_SCRATCH1 = popped sr.
 * Clobbers x0, x1, x2, x4, x30. */
static void emit_cf_stack_pop(ArmEmit *e)
{
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_add_x_imm(e, /*rd=*/1, /*rn=*/31 /*SP*/, OFF_SP_SCRATCH0);
    emit_add_x_imm(e, /*rd=*/2, /*rn=*/31 /*SP*/, OFF_SP_SCRATCH1);
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_pop);
    emit_blr(e, /*rn=*/4);
}

/*
 * Emit the JSR/BSR-style conditional stack push:
 *   if (interrupt_state != LONG) stack_push(dsp, return_pc, SR);
 *   else                         interrupt_state = DISABLED;
 *
 * Pattern is identical for emu_jsr_imm / emu_bsr_imm / emu_bsr_long
 * (see dsp_emu.c.inc:7244-7261, 6340-6355, 6322-6337). emu_jscc /
 * emu_bcc don't take this path — they push unconditionally on the
 * "taken" branch and skip the push otherwise.
 *
 * Clobbers x0, w1, w2, x4, x30.
 */
static void emit_cf_cond_stack_push_jsr(ArmEmit *e, uint32_t return_pc)
{
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_STATE);
    emit_sub_w_imm(e, /*rd=*/0, /*rn=*/0, DSP_INTERRUPT_LONG);

    /* If interrupt_state == LONG (w0 == 0) after the sub, branch to
     * the "disable" leg. Else fall-through into the push. */
    uint32_t *to_disable = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    emit_cf_stack_push(e, return_pc);

    uint32_t *to_after = e->buf;
    emit_b(e, 0);

    /* LONG leg: interrupt_state = DSP_INTERRUPT_DISABLED */
    uint32_t *disable_label = e->buf;
    patch_branch(to_disable,
                 (int32_t)((uint8_t *)disable_label -
                           (uint8_t *)to_disable));
    emit_movz_w(e, /*rd=*/0, DSP_INTERRUPT_DISABLED, 0);
    emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_STATE);

    uint32_t *after_label = e->buf;
    /* Use patch_b (imm26 field) for the unconditional B above;
     * patch_branch is for the imm19-field CBZ / B.cond form and
     * would silently leave the low 5 bits of imm26 as zero. */
    patch_b(to_after,
            (int32_t)((uint8_t *)after_label -
                      (uint8_t *)to_after));
}

/*
 * Emit inline ARM64 that computes emu_calc_cc(cc_code) into w0.
 *
 * cc_code is known at translate time (it's a bitfield of the
 * instruction word), so each cc emits its own short sequence
 * (typically 3–7 ARM64 insns) with zero BLR / switch overhead.
 * Replaces the prior BLR-to-shim for every inline CF op that
 * needs a condition-code test.
 *
 * Each pair (CC/CS, GE/LT, NE/EQ, PL/MI, NN/NR, EC/ES, LC/LS,
 * GT/LE) differs only by a final boolean inversion of the same
 * boolean expression — implemented here by an EOR #1 tail when
 * `cc_code < 8`.
 *
 * Input / output register map:
 *   w0   — receives the 0/1 result (what the cbz test expects)
 *   w1   — scratch (N bit, U bit, Z bit, depending on case)
 *   w2   — scratch (V bit, E bit, depending on case)
 *
 * Clobbers w0, w1, w2. All other registers preserved. No BLR.
 *
 * Bit layout within SR (see dsp_cpu_regs.h):
 *   C=0 V=1 Z=2 N=3 U=4 E=5 L=6
 */
static void emit_cf_calc_cc(ArmEmit *e, uint32_t cc_code)
{
    /* Load SR into w0 first; each case extracts the needed bits. */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_SR);

    bool invert = (cc_code & 8) == 0;
    uint32_t base = cc_code & 7;

    switch (base) {
    case 0: /* CC (base, invert=true) / CS : result = SR.C */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_C, 1);
        break;
    case 1: /* GE / LT : result = (N ^ V) */
        emit_ubfx_w(e, /*rd=*/1, /*rn=*/0, DSP_SR_N, 1);
        emit_ubfx_w(e, /*rd=*/2, /*rn=*/0, DSP_SR_V, 1);
        emit_eor_w_reg(e, /*rd=*/0, /*rn=*/1, /*rm=*/2);
        break;
    case 2: /* NE / EQ : result = SR.Z */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_Z, 1);
        break;
    case 3: /* PL / MI : result = SR.N */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_N, 1);
        break;
    case 4: /* NN / NR : result = Z | (~U & ~E) = Z | ~(U | E) */
        emit_ubfx_w(e, /*rd=*/1, /*rn=*/0, DSP_SR_Z, 1);     /* w1 = Z */
        emit_ubfx_w(e, /*rd=*/2, /*rn=*/0, DSP_SR_U, 1);     /* w2 = U */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_E, 1);     /* w0 = E */
        emit_orr_w_reg(e, /*rd=*/2, /*rn=*/2, /*rm=*/0);     /* w2 = U|E */
        emit_eor_w_imm1(e, /*rd=*/2, /*rn=*/2);              /* w2 = ~(U|E) & 1 */
        emit_orr_w_reg(e, /*rd=*/0, /*rn=*/1, /*rm=*/2);     /* w0 = Z | ~(U|E) */
        break;
    case 5: /* EC / ES : result = SR.E */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_E, 1);
        break;
    case 6: /* LC / LS : result = SR.L */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_L, 1);
        break;
    case 7: /* GT / LE : result = Z | (N ^ V) */
        emit_ubfx_w(e, /*rd=*/1, /*rn=*/0, DSP_SR_N, 1);     /* w1 = N */
        emit_ubfx_w(e, /*rd=*/2, /*rn=*/0, DSP_SR_V, 1);     /* w2 = V */
        emit_eor_w_reg(e, /*rd=*/1, /*rn=*/1, /*rm=*/2);     /* w1 = N^V */
        emit_ubfx_w(e, /*rd=*/0, /*rn=*/0, DSP_SR_Z, 1);     /* w0 = Z */
        emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);     /* w0 = Z|(N^V) */
        break;
    default:
        /* Unreachable — base is guaranteed in [0,7]. */
        emit_mov_imm32(e, /*rd=*/0, 0);
        break;
    }

    if (invert) {
        emit_eor_w_imm1(e, /*rd=*/0, /*rn=*/0);
    }
}

/*
 * Decode a 9-bit PC-relative immediate from the low bits of an
 * emu_bra_imm / emu_bsr_imm / emu_bcc_imm instruction word:
 *   xxx = inst[4:0] | (inst[9:6] << 5)
 *   signed_xxx = sign-extend(xxx, 9 bits)
 * Returns a 32-bit signed offset.
 */
static int32_t cf_decode_signed_9(uint32_t inst)
{
    uint32_t xxx = (inst & 0x1f) + ((inst & (0xf << 6)) >> 1);
    /* sign-extend from bit 8 (9 bits total) */
    if (xxx & 0x100) {
        xxx |= ~0x1ffu;
    }
    return (int32_t)xxx;
}

/* JMP xxx (emu_jmp_imm). Unconditional absolute branch, 12-bit
 * absolute target from inst[11:0]. */
static void emit_cf_jmp_imm_op(ArmEmit *e, uint32_t inst)
{
    uint32_t newpc = inst & 0xfff;
    emit_cf_set_pc_branch(e, newpc);
    emit_cf_set_cycles(e, 4);
}

/* JSR xxx (emu_jsr_imm). Absolute 12-bit target; pushes return addr
 * unless a LONG interrupt is in progress. */
static void emit_cf_jsr_imm_op(ArmEmit *e, uint32_t pc, uint32_t inst)
{
    uint32_t newpc = inst & 0xfff;
    /* Return address is pc + cur_inst_len. emit_instruction sets
     * cur_inst_len = 1 pre-handler; for emu_jsr_imm (1-word), the
     * return address is pc + 1. */
    emit_cf_cond_stack_push_jsr(e, pc + 1);
    emit_cf_set_pc_branch(e, newpc);
    emit_cf_set_cycles(e, 4);
}

/* RTS (emu_rts). Pops (newpc, newsr), sets pc = newpc,
 * cur_inst_len = 0. Notably does NOT restore SR (RTI does). */
static void emit_cf_rts_op(ArmEmit *e)
{
    emit_cf_stack_pop(e);
    /* w1 = popped pc, w2 = popped sr (ignored for RTS) */
    emit_ldr_w_imm(e, /*rd=*/1, /*rn=*/31, OFF_SP_SCRATCH0);
    emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_PC);
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_cf_set_cycles(e, 4);
}

/* RTI (emu_rti). Like RTS + restores SR. */
static void emit_cf_rti_op(ArmEmit *e)
{
    emit_cf_stack_pop(e);
    emit_ldr_w_imm(e, /*rd=*/1, /*rn=*/31, OFF_SP_SCRATCH0);
    emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_PC);
    emit_ldr_w_imm(e, /*rd=*/2, /*rn=*/31, OFF_SP_SCRATCH1);
    emit_str_w_any(e, /*rs=*/2, /*rn=*/19, SCRATCH, OFF_SR);
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_cf_set_cycles(e, 4);
}

/* BRA xxx (emu_bra_imm). 9-bit signed PC-relative; no cycle adjust. */
static void emit_cf_bra_imm_op(ArmEmit *e, uint32_t pc, uint32_t inst)
{
    int32_t off = cf_decode_signed_9(inst);
    uint32_t newpc = (pc + (uint32_t)off) & 0xffffff;
    emit_cf_set_pc_branch(e, newpc);
    /* emu_bra_imm leaves instr_cycle at 2 (preset). */
}

/* BRA xxxx (emu_bra_long). 2-word; xxxx is the second word. pc +=
 * xxxx; pc &= 0xffffff. */
static void emit_cf_bra_long_op(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                                uint32_t inst)
{
    (void)inst;
    /* Bake the second word from pram. Self-modifying writes to
     * pram[pc+1] invalidate this block via dsp_jit_invalidate, so
     * the immediate stays consistent with the pram content the
     * interpreter would read at runtime. */
    uint32_t xxxx = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;
    uint32_t newpc = (pc + xxxx) & 0xffffff;
    emit_cf_set_pc_branch(e, newpc);
}

/* BSR xxx (emu_bsr_imm). 9-bit PC-rel, cond push, +2 cycles. */
static void emit_cf_bsr_imm_op(ArmEmit *e, uint32_t pc, uint32_t inst)
{
    int32_t off = cf_decode_signed_9(inst);
    uint32_t newpc = (pc + (uint32_t)off) & 0xffffff;
    emit_cf_cond_stack_push_jsr(e, pc + 1);
    emit_cf_set_pc_branch(e, newpc);
    emit_cf_set_cycles(e, 4);
}

/* BSR xxxx (emu_bsr_long). 2-word; +4 cycles. */
static void emit_cf_bsr_long_op(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                                uint32_t inst)
{
    (void)inst;
    uint32_t xxxx = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;
    uint32_t newpc = (pc + xxxx) & 0xffffff;
    /* Interp does cur_inst_len++ before the push, so the return
     * address is pc + 2 for bsr_long. */
    emit_cf_cond_stack_push_jsr(e, pc + 2);
    emit_cf_set_pc_branch(e, newpc);
    emit_cf_set_cycles(e, 6);
}

/*
 * JCC xxx (emu_jcc_imm). Conditional 12-bit absolute branch.
 * Taken: pc = newpc; cur_inst_len = 0.
 * Cycles: always +2 (regardless of taken/not-taken).
 */
static void emit_cf_jcc_imm_op(ArmEmit *e, uint32_t inst)
{
    uint32_t newpc = inst & 0xfff;
    uint32_t cc_code = (inst >> 12) & 0xf;
    emit_cf_calc_cc(e, cc_code);

    /* w0 = 0 → not taken; skip the branch write. */
    uint32_t *to_end = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    /* Taken: pc = newpc; cur_inst_len = 0. */
    emit_cf_set_pc_branch(e, newpc);

    uint32_t *end_label = e->buf;
    patch_branch(to_end, (int32_t)((uint8_t *)end_label -
                                   (uint8_t *)to_end));
    emit_cf_set_cycles(e, 4);
}

/*
 * JSCC xxx (emu_jscc_imm). Like JCC but also pushes (pc +
 * cur_inst_len, SR) when taken. For a 1-word jscc the return
 * address is pc + 1.
 */
static void emit_cf_jscc_imm_op(ArmEmit *e, uint32_t pc, uint32_t inst)
{
    uint32_t newpc = inst & 0xfff;
    uint32_t cc_code = (inst >> 12) & 0xf;
    emit_cf_calc_cc(e, cc_code);

    uint32_t *to_end = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    /* Taken: push, then set pc. */
    emit_cf_stack_push(e, pc + 1);
    emit_cf_set_pc_branch(e, newpc);

    uint32_t *end_label = e->buf;
    patch_branch(to_end, (int32_t)((uint8_t *)end_label -
                                   (uint8_t *)to_end));
    emit_cf_set_cycles(e, 4);
}

/* BCC xxx (emu_bcc_imm). 9-bit signed PC-rel, 1-word, no cycle
 * adjust. */
static void emit_cf_bcc_imm_op(ArmEmit *e, uint32_t pc, uint32_t inst)
{
    int32_t off = cf_decode_signed_9(inst);
    uint32_t newpc = (pc + (uint32_t)off) & 0xffffff;
    uint32_t cc_code = (inst >> 12) & 0xf;
    emit_cf_calc_cc(e, cc_code);

    uint32_t *to_end = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    emit_cf_set_pc_branch(e, newpc);

    uint32_t *end_label = e->buf;
    patch_branch(to_end, (int32_t)((uint8_t *)end_label -
                                   (uint8_t *)to_end));
    /* instr_cycle stays at the 2 preset by emit_instruction. */
}

/* BCC xxxx (emu_bcc_long). 2-word PC-rel; taken: cur_inst_len = 0,
 * pc += xxxx. Not-taken: cur_inst_len++ (to 2, handler increments
 * first). No cycle adjust. The block exits either way (terminator
 * + PC mismatch via the incremented length). */
static void emit_cf_bcc_long_op(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                                uint32_t inst)
{
    uint32_t xxxx = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;
    uint32_t newpc = (pc + xxxx) & 0xffffff;
    uint32_t cc_code = inst & 0xf;
    emit_cf_calc_cc(e, cc_code);
    /* w0 holds the cc result here. Park it in a callee-saved reg
     * (x22) so we can reuse w0 for the cur_inst_len store below
     * without losing the compare value. x22 is part of the
     * parmove save-slot pool; safe to clobber inside a CF op (no
     * parmove/ALU runs concurrently with a CF terminator). */
    emit_mov_w_reg(e, /*rd=*/22, /*rn=*/0);

    /* Mirror interp: set cur_inst_len = 2 unconditionally first
     * (the handler does this before the cc check — see
     * dsp_emu.c.inc:5974). Then if taken, override to 0. */
    emit_movz_w(e, /*rd=*/0, 2, 0);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    uint32_t *to_end = e->buf;
    emit_cbz_w(e, /*rn=*/22, 0);

    emit_cf_set_pc_branch(e, newpc);

    uint32_t *end_label = e->buf;
    patch_branch(to_end, (int32_t)((uint8_t *)end_label -
                                   (uint8_t *)to_end));
}

/* ============================================================== *
 * Phase 6 — inline loop handlers (REP / DO / DOR / ENDDO).
 *
 * Same design as Phase 5a's CF emitters: emit the interpreter's
 * logic inline, relying on emit_post_instruction_epilogue's
 * loop_rep / PC-mismatch / jit_exit_block_request checks to exit
 * the block cleanly. The REP/DO/ENDDO handlers themselves are
 * trivial register + stack updates; the loop-body iteration
 * arithmetic is entirely handled by the existing
 * dsp_postexecute_update_pc BLR in the epilogue (which the JIT
 * invokes for every op, including the loop body's own ops).
 *
 * None of these are chain candidates: REP sets loop_rep which
 * forces an exit to the C dispatcher after the single stub; DO/
 * DOR set SR.LF which the post-exec PC check reads each subsequent
 * iteration; ENDDO pops the loop stack — all semantics live in
 * post-exec, not in the instruction itself.
 * ============================================================== */

/* Common REP prologue: LCSAVE = LC; pc_on_rep = 1; loop_rep = 1.
 * Clobbers w0. */
static void emit_cf_rep_common_start(ArmEmit *e)
{
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LC));
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LCSAVE));
    emit_movz_w(e, /*rd=*/0, 1, 0);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_PC_ON_REP);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_LOOP_REP);
}

/* emu_rep_imm (single-word). LC = inst[15:8] | ((inst[3:0]) << 8). */
static void emit_cf_rep_imm_op(ArmEmit *e, uint32_t inst)
{
    uint32_t lc = ((inst >> 8) & 0xff) | ((inst & 0xf) << 8);
    emit_cf_rep_common_start(e);
    emit_mov_imm32(e, /*rd=*/0, lc);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LC));
    emit_cf_set_cycles(e, 4);
}

/* Common DO/DOR suffix: push(pc+2, SR); SR |= (1<<LF); LC = imm;
 * cycles = 6. Clobbers w0..w2, w4, x30.
 * `new_la_reg` : scratch W reg currently holding the computed LA
 *                value (not used here; LA is written before this
 *                helper by the caller).
 */
static void emit_cf_do_suffix(ArmEmit *e, uint32_t pc, uint32_t lc_imm)
{
    /* cur_inst_len++ — it was 1 after emit_instruction preset,
     * handler interprets the 2nd word so it bumps to 2. */
    emit_movz_w(e, /*rd=*/0, 2, 0);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    /* stack_push(pc + cur_inst_len = pc + 2, SR) */
    emit_cf_stack_push(e, pc + 2);

    /* SR |= (1 << DSP_SR_LF) ; DSP_SR_LF = 15 ; bit value = 0x8000 */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/1, 1u << DSP_SR_LF);
    emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_SR);

    /* LC = 12-bit imm */
    emit_mov_imm32(e, /*rd=*/0, lc_imm);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LC));

    emit_cf_set_cycles(e, 6);  /* preset 2 + handler += 4 = 6 */
}

/*
 * emu_do_imm (2-word).
 *   push(LA, LC)
 *   LA = pram[pc+1] & 0xffff
 *   cur_inst_len++
 *   push(pc+cur_inst_len, SR)
 *   SR |= LF
 *   LC = 12-bit imm from inst
 *   cycles += 4
 */
static void emit_cf_do_imm_op(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                              uint32_t inst)
{
    uint32_t lc = ((inst >> 8) & 0xff) | ((inst & 0xf) << 8);
    uint32_t la = (pc + 1 < DSP_PRAM_SIZE) ? (dsp->pram[pc + 1] & 0xffff) : 0;

    /* stack_push(LA, LC). Load values first. w1=LA, w2=LC. */
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_ldr_w_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LA));
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LC));
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_push);
    emit_blr(e, /*rn=*/4);

    /* LA = pram[pc+1] & 0xffff (baked immediate) */
    emit_mov_imm32(e, /*rd=*/0, la);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LA));

    emit_cf_do_suffix(e, pc, lc);
}

/*
 * emu_dor_imm (2-word). Same as DO except LA = (pc + xxxx) & 0xffff.
 */
static void emit_cf_dor_imm_op(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                               uint32_t inst)
{
    uint32_t lc = ((inst >> 8) & 0xff) | ((inst & 0xf) << 8);
    uint32_t xxxx = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;
    uint32_t la = (pc + xxxx) & 0xffff;

    /* Note the interp order differs from DO: cur_inst_len++ before
     * the first push. Mirror that by incrementing here first.
     * Matters only for any handler that observes cur_inst_len mid-
     * push (stack_push doesn't). */
    emit_movz_w(e, /*rd=*/0, 2, 0);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    /* push(LA, LC) */
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_ldr_w_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LA));
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LC));
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_push);
    emit_blr(e, /*rn=*/4);

    /* LA = (pc + xxxx) & 0xffff */
    emit_mov_imm32(e, /*rd=*/0, la);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LA));

    /* push(pc+cur_inst_len=pc+2, SR) */
    emit_cf_stack_push(e, pc + 2);

    /* SR |= LF */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/1, 1u << DSP_SR_LF);
    emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_SR);

    /* LC = 12-bit imm */
    emit_mov_imm32(e, /*rd=*/0, lc);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_REG(DSP_REG_LC));

    emit_cf_set_cycles(e, 6);
}

/*
 * emu_enddo:
 *   pop(&saved_pc, &saved_sr)     — saved_pc discarded
 *   SR = (SR & 0x7f) | (saved_sr & (1<<LF))
 *   pop(&LA, &LC)
 *
 * Cycle: the handler doesn't += anything, so stays at the preset 2.
 */
static void emit_cf_enddo_op(ArmEmit *e)
{
    /* First pop → [SCRATCH0, SCRATCH1] = (saved_pc, saved_sr). */
    emit_cf_stack_pop(e);

    /* w2 = saved_sr; w1 = saved_sr & (1<<LF). */
    emit_ldr_w_imm(e, /*rd=*/2, /*rn=*/31, OFF_SP_SCRATCH1);
    emit_mov_imm32(e, /*rd=*/1, 1u << DSP_SR_LF);
    emit_and_w_reg(e, /*rd=*/1, /*rn=*/2, /*rm=*/1);

    /* w0 = SR & 0x7f */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_imm32(e, /*rd=*/2, 0x7fu);
    emit_and_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/2);

    /* SR = w0 | w1 */
    emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/1);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_SR);

    /* Second pop: dsp_stack_pop(dsp, &LA_reg, &LC_reg). We can
     * pass the addresses of the register-file slots directly —
     * registers[] is at a known offset, each u32, so
     * x1 = x19 + OFF_REG(LA), x2 = x19 + OFF_REG(LC).
     *
     * Both offsets fit under 4095 for emit_add_x_imm (LA=0xFA,
     * LC=0xFE; BUG checker: OFF_REG(DSP_REG_LA) = 0x4 + 4*62 =
     * 0xFC so well within 0xfff). */
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_add_x_imm(e, /*rd=*/1, /*rn=*/19, OFF_REG(DSP_REG_LA));
    emit_add_x_imm(e, /*rd=*/2, /*rn=*/19, OFF_REG(DSP_REG_LC));
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_pop);
    emit_blr(e, /*rn=*/4);
}

/* ============================================================== *
 * Phase 5a — extended: _ea variants and bit-test families.
 *
 * These handlers share the same epilogue invariants as the _imm
 * CF emitters above (cur_inst_len / pc / cycle layout mirror the
 * interpreter bit-exactly), but their operand sources are dynamic:
 * _ea variants compute the target via emit_calc_ea_inline (fast
 * path for linear modes 0-4, BLR fallback for modes 5-7 with
 * cycle/cur_inst_len side effects). The bit-test family loads
 * the tested value either from X/Y memory (emit_mem_read_xy
 * handles linear-xram/yram inline and peripheral/out-of-range via
 * BLR) or from a register (simple LDR; A/B routed through
 * emit_pm_read_reg for scaling/limiting parity).
 *
 * Register allocation convention for these emitters:
 *   x22 — address (for mem-sourced) or baked value (for _reg)
 *   x23 — bit-test result (0 or 1)
 *   w0..w3, x30 — scratch (clobbered by any BLR)
 *
 * x22/x23 are callee-saved across BLRs; parmove stubs use
 * x22..x25 as save slots but CF ops are terminators, so no
 * parmove ever runs after / overlapping with them inside a block.
 * ============================================================== */

/* JMP ea (emu_jmp_ea). Target = calc_ea(inst[13:8]).
 *
 * Note on cycle accounting: emu_calc_ea internally adds +2 to
 * instr_cycle for EA modes 5 / 6 / 7 (see dsp_emu.c.inc:183 /
 * 191 / 200). The handler then adds its own +2. So we use
 * emit_cf_add_cycles here, not emit_cf_set_cycles — the latter
 * would silently discard calc_ea's side effect. Total cycle cost:
 * 4 for modes 0-4, 6 for modes 5-7. */
static void emit_cf_jmp_ea_op(ArmEmit *e, uint32_t inst,
                              dsp_core_t *dsp, uint32_t pc)
{
    uint32_t ea_mode = (inst >> 8) & 0x3f;
    /* w22 (x22) receives the 16-bit target address. */
    emit_calc_ea_inline(e, ea_mode, /*out_addr_reg=*/22, /*want_retour=*/false,
                        dsp, pc);
    emit_str_w_any(e, /*rs=*/22, /*rn=*/19, SCRATCH, OFF_PC);
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_cf_add_cycles(e, 2);  /* +2 on top of the preset 2 */
}

/* JSR ea (emu_jsr_ea). Like jsr_imm but target from calc_ea. */
static void emit_cf_jsr_ea_op(ArmEmit *e, uint32_t pc, uint32_t inst,
                              dsp_core_t *dsp)
{
    uint32_t ea_mode = (inst >> 8) & 0x3f;
    emit_calc_ea_inline(e, ea_mode, /*out_addr_reg=*/22, false, dsp, pc);
    /* Return address: pc + cur_inst_len. For jsr_ea, cur_inst_len
     * is 1 at handler entry (emu_calc_ea slow-path for mode 6 may
     * bump it; but in that case the immediate decoding happens
     * inside the slow helper, and cur_inst_len has been updated in
     * dsp_core_t already — so a fresh LDRH re-read gets the
     * post-calc-ea value. Match the interp by reading the live
     * cur_inst_len). */
    emit_ldrh_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_PC);
    emit_add_w_reg(e, /*rd=*/1, /*rn=*/2, /*rm=*/1);  /* w1 = pc + cur_inst_len */

    /* Conditional-push: if (interrupt_state != LONG) push(w1, SR); */
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_STATE);
    emit_sub_w_imm(e, /*rd=*/0, /*rn=*/0, DSP_INTERRUPT_LONG);
    uint32_t *to_disable = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    /* push(dsp, w1, SR) */
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_push);
    emit_blr(e, /*rn=*/4);
    uint32_t *to_after = e->buf;
    emit_b(e, 0);

    uint32_t *disable_label = e->buf;
    patch_branch(to_disable, (int32_t)((uint8_t *)disable_label -
                                       (uint8_t *)to_disable));
    emit_movz_w(e, /*rd=*/0, DSP_INTERRUPT_DISABLED, 0);
    emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_STATE);

    uint32_t *after_label = e->buf;
    patch_b(to_after, (int32_t)((uint8_t *)after_label -
                                (uint8_t *)to_after));

    /* pc = w22 (calc_ea output); cur_inst_len = 0. */
    emit_str_w_any(e, /*rs=*/22, /*rn=*/19, SCRATCH, OFF_PC);
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    /* +2 on top of preset 2 (and any +2 calc_ea already added for
     * modes 5-7) — see jmp_ea note above. */
    emit_cf_add_cycles(e, 2);
}

/* JCC ea (emu_jcc_ea). cc_code = inst[3:0] (differs from jcc_imm
 * which uses inst[15:12]). Target = calc_ea. */
static void emit_cf_jcc_ea_op(ArmEmit *e, uint32_t inst,
                              dsp_core_t *dsp, uint32_t pc)
{
    uint32_t ea_mode = (inst >> 8) & 0x3f;
    uint32_t cc_code = inst & 0xf;

    /* Compute newpc first (calc_ea can have cycle/cur_inst_len
     * side effects for mode 6; the interp evaluates calc_ea BEFORE
     * the cc check). */
    emit_calc_ea_inline(e, ea_mode, /*out_addr_reg=*/22, false, dsp, pc);

    /* Now compute cc_code result → w0. */
    emit_cf_calc_cc(e, cc_code);

    uint32_t *to_end = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    /* Taken: pc = w22, cur_inst_len = 0. */
    emit_str_w_any(e, /*rs=*/22, /*rn=*/19, SCRATCH, OFF_PC);
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    uint32_t *end_label = e->buf;
    patch_branch(to_end, (int32_t)((uint8_t *)end_label -
                                   (uint8_t *)to_end));
    /* +2 on top of preset 2 — plus any +2 calc_ea added for
     * modes 5-7 (interp's instr_cycle is always calc_ea's plus
     * the handler's +2, regardless of taken/not-taken). */
    emit_cf_add_cycles(e, 2);
}

/* JSCC ea (emu_jscc_ea). Like jcc_ea + push(pc+cur_inst_len, SR)
 * on taken. cc_code = inst[3:0]. */
static void emit_cf_jscc_ea_op(ArmEmit *e, uint32_t inst,
                               dsp_core_t *dsp, uint32_t pc)
{
    uint32_t ea_mode = (inst >> 8) & 0x3f;
    uint32_t cc_code = inst & 0xf;

    emit_calc_ea_inline(e, ea_mode, /*out_addr_reg=*/22, false, dsp, pc);
    emit_cf_calc_cc(e, cc_code);

    uint32_t *to_end = e->buf;
    emit_cbz_w(e, /*rn=*/0, 0);

    /* Taken: push(pc+cur_inst_len, SR); pc=w22; cur_inst_len=0. */
    emit_ldrh_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_PC);
    emit_add_w_reg(e, /*rd=*/1, /*rn=*/2, /*rm=*/1);  /* w1 = retpc */
    emit_ldr_w_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_SR);
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
    emit_mov_imm64(e, /*rd=*/4,
                   (uint64_t)(uintptr_t)&dsp_jit_helper_stack_push);
    emit_blr(e, /*rn=*/4);

    emit_str_w_any(e, /*rs=*/22, /*rn=*/19, SCRATCH, OFF_PC);
    emit_str_w_any(e, /*rs=*/31, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    uint32_t *end_label = e->buf;
    patch_branch(to_end, (int32_t)((uint8_t *)end_label -
                                   (uint8_t *)to_end));
    /* +2 on top of preset 2 + any +2 from calc_ea (see jmp_ea). */
    emit_cf_add_cycles(e, 2);
}

/*
 * Generic bit-test emitter covering 16 jclr/jset/jsclr/jsset +
 * 4 brclr/brset variants.
 *
 *   source_kind:
 *     0 (AA)  — addr = (inst>>8) & 0x3f, memspace = (inst>>6)&1
 *     1 (EA)  — addr = calc_ea((inst>>8) & 0x3f),
 *               memspace = (inst>>6) & 1
 *     2 (PP)  — addr = 0xffffc0 + ((inst>>8) & 0x3f),
 *               memspace = (inst>>6) & 1 (peripheral)
 *     3 (REG) — value = dsp->registers[(inst>>8) & 0x3f],
 *               or emu_pm_read_accu24 for A/B
 *   sense_set     : true  ⇒ branch when bit is SET (jset / brset
 *                          / jsset)
 *                   false ⇒ branch when bit is CLEAR (jclr / brclr
 *                          / jsclr)
 *   push_on_taken : true  ⇒ the "s" variants (jsclr / jsset) push
 *                          (pc+2, SR) before taking the branch.
 *                          brclr / brset and plain jclr / jset
 *                          have push_on_taken=false.
 *   pc_relative   : true  ⇒ brclr / brset. Target = (pc + xxxx) &
 *                          0xffffff.
 *                   false ⇒ jclr / jset / jsclr / jsset. Target =
 *                          pram[pc+1] (absolute 24-bit).
 *
 * All variants: +4 cycles, 2-word, not-taken bumps cur_inst_len to
 * 2. Bake the second word (xxxx or newaddr) at translate time;
 * pram writes that would change it invalidate this block via
 * dsp_jit_invalidate.
 */
static void emit_cf_bittest_generic(
    ArmEmit *e, dsp_core_t *dsp, uint32_t pc, uint32_t inst,
    int source_kind, bool sense_set,
    bool push_on_taken, bool pc_relative)
{
    uint32_t numbit = inst & 0x1f;
    uint32_t word2 = (pc + 1 < DSP_PRAM_SIZE) ? dsp->pram[pc + 1] : 0;
    uint32_t newpc = pc_relative
        ? ((pc + word2) & 0xffffff)
        : (word2 & 0xffffff);

    /* Load the value to test into w22. */
    if (source_kind == 3) {
        /* REG: direct register read, with A/B scaling via
         * emit_pm_read_reg. */
        uint32_t numreg = (inst >> 8) & 0x3f;
        emit_pm_read_reg(e, numreg, /*value_reg=*/22);
    } else {
        uint32_t memspace = (inst >> 6) & 1;
        int addr_reg = 22;

        if (source_kind == 0) {
            /* AA: 6-bit absolute (addr < 64, always linear). */
            uint32_t addr = (inst >> 8) & 0x3f;
            emit_mov_imm32(e, addr_reg, addr);
        } else if (source_kind == 1) {
            /* EA: calc_ea. For modes 5-7 this BLRs the C helper,
             * which itself may adjust cur_inst_len / instr_cycle. */
            uint32_t ea_mode = (inst >> 8) & 0x3f;
            emit_calc_ea_inline(e, ea_mode, addr_reg, false, dsp, pc);
        } else {
            /* PP: peripheral (0xffffc0 + off). */
            uint32_t pp_off = (inst >> 8) & 0x3f;
            emit_mov_imm32(e, addr_reg, 0xffffc0u + pp_off);
        }

        /* emit_mem_read_xy: fast path for addr < 0xc00 (AA case
         * always); BLR dsp56k_read_memory for PP / out-of-range EA.
         * w23 receives the 24-bit value. */
        emit_mem_read_xy(e, memspace, addr_reg, /*value_reg=*/23);
        /* Move value into w22 for uniform handling below. */
        emit_mov_w_reg(e, /*rd=*/22, /*rn=*/23);
    }

    /* Extract the test bit into w23. */
    emit_ubfx_w(e, /*rd=*/23, /*rn=*/22, numbit, 1);

    /*
     * instr_cycle += 4 (handler). For _ea mode 6 the calc_ea slow
     * path already added +2, so use the additive helper rather
     * than emit_cf_set_cycles — the latter would overwrite the
     * calc_ea side effect. Totals:
     *   _aa / _pp / _reg  : 2 preset + 0       + 4 handler = 6
     *   _ea modes 0-4     : 2       + 0       + 4         = 6
     *   _ea modes 5-7     : 2       + 2 calc  + 4         = 8
     */
    emit_cf_add_cycles(e, 4);

    /* Branch on w23: cbz for "clear" sense, cbnz for "set" sense. */
    uint32_t *to_not_taken = e->buf;
    if (sense_set) {
        emit_cbz_w(e, /*rn=*/23, 0);   /* not taken if bit == 0 */
    } else {
        emit_cbnz_w(e, /*rn=*/23, 0);  /* not taken if bit == 1 */
    }

    /* Taken path. */
    if (push_on_taken) {
        /* Interp pushes (pc+2, SR) — 2-word instruction, return
         * address is the word after the 2nd word. For _ea mode 6
         * the instruction is effectively 3 words; interp still
         * pushes pc+2 (the first word after the first-word +
         * pram[pc+1] aa immediate), which happens to coincide
         * with the branch target, so push(pc+2) matches interp
         * in all cases. */
        emit_cf_stack_push(e, pc + 2);
    }
    emit_cf_set_pc_branch(e, newpc);

    uint32_t *to_end = e->buf;
    emit_b(e, 0);

    /*
     * Not-taken path: interp does `++cur_inst_len`. For most
     * variants cur_inst_len is 1 at handler entry → 2 after the
     * ++. For _ea mode 6, calc_ea bumped cur_inst_len to 2, so
     * ++ takes it to 3 — skipping both the aa immediate word and
     * the branch-target word. Use the additive helper to preserve
     * this.
     */
    uint32_t *not_taken_label = e->buf;
    patch_branch(to_not_taken,
                 (int32_t)((uint8_t *)not_taken_label -
                           (uint8_t *)to_not_taken));
    emit_cf_inc_cur_inst_len(e);

    /* End label: both paths converge here (no more op-state stores
     * — the block epilogue's exit checks run next). */
    uint32_t *end_label = e->buf;
    patch_b(to_end, (int32_t)((uint8_t *)end_label -
                              (uint8_t *)to_end));
}

/*
 * CF dispatcher: classify the handler; if supported, emit the
 * inline kernel and return true. Otherwise return false so the
 * caller emits the original BLR fallback.
 *
 * `pc` is the translator-time PC of the instruction being emitted
 * (needed for PC-relative branches and jsr/bsr return-address
 * encoding). `dsp` is used to peek at pram for 2-word CF ops'
 * second word.
 */
static bool emit_cf_call(ArmEmit *e, dsp_core_t *dsp, uint32_t pc,
                         uint32_t inst, emu_func_t fn)
{
    int kind = dsp_jit_helper_classify_cf((void *)fn);
    switch (kind) {
    /* Immediate / absolute CF. */
    case DSP_JIT_CF_JMP_IMM:   emit_cf_jmp_imm_op(e, inst);           break;
    case DSP_JIT_CF_JSR_IMM:   emit_cf_jsr_imm_op(e, pc, inst);       break;
    case DSP_JIT_CF_RTS:       emit_cf_rts_op(e);                     break;
    case DSP_JIT_CF_RTI:       emit_cf_rti_op(e);                     break;
    case DSP_JIT_CF_BRA_IMM:   emit_cf_bra_imm_op(e, pc, inst);       break;
    case DSP_JIT_CF_BRA_LONG:  emit_cf_bra_long_op(e, dsp, pc, inst); break;
    case DSP_JIT_CF_BSR_IMM:   emit_cf_bsr_imm_op(e, pc, inst);       break;
    case DSP_JIT_CF_BSR_LONG:  emit_cf_bsr_long_op(e, dsp, pc, inst); break;
    case DSP_JIT_CF_JCC_IMM:   emit_cf_jcc_imm_op(e, inst);           break;
    case DSP_JIT_CF_JSCC_IMM:  emit_cf_jscc_imm_op(e, pc, inst);      break;
    case DSP_JIT_CF_BCC_IMM:   emit_cf_bcc_imm_op(e, pc, inst);       break;
    case DSP_JIT_CF_BCC_LONG:  emit_cf_bcc_long_op(e, dsp, pc, inst); break;
    /* Loop ops. */
    case DSP_JIT_CF_REP_IMM:   emit_cf_rep_imm_op(e, inst);           break;
    case DSP_JIT_CF_DO_IMM:    emit_cf_do_imm_op(e, dsp, pc, inst);   break;
    case DSP_JIT_CF_DOR_IMM:   emit_cf_dor_imm_op(e, dsp, pc, inst);  break;
    case DSP_JIT_CF_ENDDO:     emit_cf_enddo_op(e);                   break;
    /* _ea CF (calc_ea target). */
    case DSP_JIT_CF_JMP_EA:    emit_cf_jmp_ea_op(e, inst, dsp, pc);       break;
    case DSP_JIT_CF_JSR_EA:    emit_cf_jsr_ea_op(e, pc, inst, dsp);       break;
    case DSP_JIT_CF_JCC_EA:    emit_cf_jcc_ea_op(e, inst, dsp, pc);       break;
    case DSP_JIT_CF_JSCC_EA:   emit_cf_jscc_ea_op(e, inst, dsp, pc);      break;
    /* Bit-test absolute (jclr/jset/jsclr/jsset). */
    case DSP_JIT_CF_JCLR_AA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 0, false, false, false); break;
    case DSP_JIT_CF_JCLR_EA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 1, false, false, false); break;
    case DSP_JIT_CF_JCLR_PP:
        emit_cf_bittest_generic(e, dsp, pc, inst, 2, false, false, false); break;
    case DSP_JIT_CF_JCLR_REG:
        emit_cf_bittest_generic(e, dsp, pc, inst, 3, false, false, false); break;
    case DSP_JIT_CF_JSET_AA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 0, true, false, false); break;
    case DSP_JIT_CF_JSET_EA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 1, true, false, false); break;
    case DSP_JIT_CF_JSET_PP:
        emit_cf_bittest_generic(e, dsp, pc, inst, 2, true, false, false); break;
    case DSP_JIT_CF_JSET_REG:
        emit_cf_bittest_generic(e, dsp, pc, inst, 3, true, false, false); break;
    case DSP_JIT_CF_JSCLR_AA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 0, false, true, false); break;
    case DSP_JIT_CF_JSCLR_EA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 1, false, true, false); break;
    case DSP_JIT_CF_JSCLR_PP:
        emit_cf_bittest_generic(e, dsp, pc, inst, 2, false, true, false); break;
    case DSP_JIT_CF_JSCLR_REG:
        emit_cf_bittest_generic(e, dsp, pc, inst, 3, false, true, false); break;
    case DSP_JIT_CF_JSSET_AA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 0, true, true, false); break;
    case DSP_JIT_CF_JSSET_EA:
        emit_cf_bittest_generic(e, dsp, pc, inst, 1, true, true, false); break;
    case DSP_JIT_CF_JSSET_PP:
        emit_cf_bittest_generic(e, dsp, pc, inst, 2, true, true, false); break;
    case DSP_JIT_CF_JSSET_REG:
        emit_cf_bittest_generic(e, dsp, pc, inst, 3, true, true, false); break;
    /* Bit-test PC-relative (brclr/brset). */
    case DSP_JIT_CF_BRCLR_PP:
        emit_cf_bittest_generic(e, dsp, pc, inst, 2, false, false, true); break;
    case DSP_JIT_CF_BRCLR_REG:
        emit_cf_bittest_generic(e, dsp, pc, inst, 3, false, false, true); break;
    case DSP_JIT_CF_BRSET_PP:
        emit_cf_bittest_generic(e, dsp, pc, inst, 2, true, false, true); break;
    case DSP_JIT_CF_BRSET_REG:
        emit_cf_bittest_generic(e, dsp, pc, inst, 3, true, false, true); break;
    default:
        return false;
    }
    g_cf_inlined_count++;
    return true;
}

static bool emit_instruction(ArmEmit *e, ExitPatchList *exits,
                             dsp_core_t *dsp,
                             uint32_t pc, uint32_t inst, uint32_t inst_len,
                             emu_func_t emu_func, uint32_t expected_next_pc,
                             bool is_terminator,
                             uint32_t *out_write_set)
{
    /*
     * Always preset dsp->cur_inst = inst. Round-4 tried skipping
     * this store for handlers classified as inlinable, on the
     * theory that inlined emitters bake `inst` at translate time.
     * Turned out at least one inlined path still reads dsp->cur_inst
     * at runtime (likely an indirect BLR to a C helper we didn't
     * audit), so the skip caused the interpreter to later observe
     * stale instruction bits and assert on an unknown opcode. Keep
     * the unconditional preset until the offending reader is found.
     */
    emit_mov_imm32(e, /*rd=*/0, inst);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST);

    /* 2. dsp->cur_inst_len = 1. This is the interpreter's initial
     * value — handlers that read a second word do cur_inst_len++,
     * reaching 2; handlers that branch set it to 0. Translators
     * MUST NOT pre-set this to 2 even for known 2-word ops, or the
     * handler's ++ would leave it at 3. The `inst_len` parameter
     * is only used to compute the block's expected_next_pc and to
     * advance the translator's own pc cursor to the next real
     * instruction (skipping the immediate word of 2-word ops). */
    (void)inst_len;
    emit_movz_w(e, /*rd=*/0, 1, 0);
    emit_str_w_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);

    /* 3. dsp->instr_cycle = 2 (handlers will += more if needed) */
    emit_movz_w(e, /*rd=*/0, 2, 0);
    emit_strh_any(e, /*rs=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);

    /*
     * 4. Call the handler. Two inline paths are tried in order,
     * each bypassing the BLR to the C handler:
     *   (a) emit_cf_call: control-flow / loop ops.
     *   (b) emit_long_imm_call: ALU long-immediate ops
     *       (add_long / sub_long / cmp_long / and_long / or_long),
     *       where the 24-bit immediate is baked from pram[pc+1].
     * If neither path handles the op we fall through to the
     * generic BLR and bump the CF fallback counter. (Long-imm
     * fallbacks lump in with CF for the stats — they're both
     * "non-parmove handler not yet inlined". Splitting into a
     * separate counter is a cheap follow-up if ever needed.)
     */
    bool cf_inlined = emit_cf_call(e, dsp, pc, inst, emu_func);
    bool li_inlined = false;
    bool handler_inlined = cf_inlined;
    if (!handler_inlined) {
        li_inlined = emit_long_imm_call(e, dsp, pc, inst, emu_func);
        handler_inlined = li_inlined;
        if (li_inlined) {
            /* Long-imm ops are ALU — credit the inlined count. */
            g_alu_inlined_count++;
        }
    }
    if (!handler_inlined) {
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);              /* x0 = dsp */
        emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)emu_func);
        emit_blr(e, /*rn=*/1);
        g_cf_fallback_count++;
    }

    /*
     * PC-mismatch check hint:
     *   - Inlined CF/loop ops are terminators that actively change
     *     pc (branch taken) — keep the check so we exit on branch.
     *   - Inlined long-imm ALU ops never touch pc — skip the check.
     *   - BLR fallback: the handler MIGHT change pc (terminators
     *     like reset/stop/wait, or any future CF handler we
     *     haven't inlined yet) — keep the check.
     */
    /*
     * Always emit the PC-mismatch check. Round-4 tried to skip it
     * for inlined long-imm (they shouldn't touch dsp->pc), but in
     * practice some handler path does mutate pc in a way the
     * translator can't predict, and without the check the block
     * keeps running at the stale pc and eventually jumps to pram
     * garbage (e.g. op=0x001000 on startup). Keep the check until
     * the divergence is tracked down; the cost is minor now that
     * we use the SUB+CBNZ imm12 fast path.
     */
    (void)li_inlined;
    emit_post_instruction_epilogue(e, exits, expected_next_pc, EPI_UNKNOWN);

    /*
     * Write-set bookkeeping for the differential validator:
     *
     *  - Inlined CF / loop / long-imm ALU ops touch only pc /
     *    cur_inst_len / instr_cycle / SR / LA / LC / LCSAVE /
     *    pc_on_rep / loop_rep / stack / interrupt_state / the
     *    A/B accumulator registers — every one of these is in a
     *    dsp_state_diff range that is ALWAYS compared regardless
     *    of the WS bitmask, so the gated bits (xram / yram /
     *    pram / mixbuffer / periph) can stay zero. This drops
     *    the validator's per-block byte-compare from ~40 KB to
     *    ~2 KB for CF-dominated blocks, matching what parmove
     *    stubs already achieve via parmove_write_set().
     *
     *  - BLR-fallback paths still need WS_ALL because generic
     *    emu_* handlers (movep / movem / mem-write bit-tests
     *    for handlers we haven't inlined yet) can touch any
     *    region of dsp_core_t.
     */
    if (out_write_set && !handler_inlined) {
        *out_write_set |= DSP_JIT_WS_ALL;
    }

    /* Terminator instructions: the full stub ran so the handler and
     * post-update fired; we now return instead of trying to fall
     * through. */
    return !is_terminator;
}

/*
 * Stack frame layout (SP-relative, grows toward 0):
 *
 *     [SP+0  .. SP+3]   scratch word #0 (e.g. calc_ea addr out)
 *     [SP+4  .. SP+7]   scratch word #1
 *     [SP+8  .. SP+15]  padding (kept so SP stays 16-aligned)
 *     [SP+16 .. SP+23]  x24, x25
 *     [SP+24 .. SP+31]  x22, x23
 *     [SP+32 .. SP+39]  x20, x21 (cached helper addresses)
 *     [SP+40 .. SP+47]  x30, x19
 *     (SP at function entry)
 *
 * Total frame: 48 bytes. 16-byte aligned at every point.
 *
 * Register usage during a translated block:
 *   x19  — dsp pointer (pinned)
 *   x20  — cached &dsp_jit_helper_postexecute_update_pc
 *   x21  — cached &dsp_jit_helper_postexecute_interrupts
 *   x22-x25 — parmove save slots (source operands / addresses that
 *             must survive BLRs to helpers and opcodes_alu[])
 *   x3  — synthesized-address scratch (already used throughout Phase 1)
 *   x0-x2 — ABI-scratch work registers
 */

/*
 * Emit the shared exit label + epilogue. Restores every callee-saved
 * register pushed by the prologue and returns.
 */
static uint32_t *emit_epilogue(ArmEmit *e)
{
    uint32_t *label = e->buf;
    /* Deallocate 16-byte scratch area. */
    emit_u32(e, 0x910043ffu);   /* ADD SP, SP, #16 */
    /* Restore in reverse of prologue's push order. */
    emit_ldp_post(e, /*rt1=*/24, /*rt2=*/25, /*rn=*/31 /*SP*/, 16);
    emit_ldp_post(e, /*rt1=*/22, /*rt2=*/23, /*rn=*/31 /*SP*/, 16);
    emit_ldp_post(e, /*rt1=*/20, /*rt2=*/21, /*rn=*/31 /*SP*/, 16);
    emit_ldp_post(e, /*rt1=*/30, /*rt2=*/19, /*rn=*/31 /*SP*/, 16);
    emit_ret(e);
    return label;
}

/*
 * Block prologue. Saves LR + callee-saved regs x19/x20/x21/x22-x25
 * and initializes the cached helper pointers + the 16-byte scratch
 * slot used by parmove stubs' slow-path calc_ea BLR.
 */
static void emit_prologue(ArmEmit *e)
{
    /* SP must stay 16-byte-aligned throughout. Each STP with #-16
     * moves SP by -16 and stays aligned. */
    emit_stp_pre(e, /*rt1=*/30, /*rt2=*/19, /*rn=*/31 /*SP*/, -16);
    emit_stp_pre(e, /*rt1=*/20, /*rt2=*/21, /*rn=*/31 /*SP*/, -16);
    emit_stp_pre(e, /*rt1=*/22, /*rt2=*/23, /*rn=*/31 /*SP*/, -16);
    emit_stp_pre(e, /*rt1=*/24, /*rt2=*/25, /*rn=*/31 /*SP*/, -16);

    /* 16-byte scratch area for parmove slow-path helpers to write
     * their u32 outputs into. [SP+0..7] = two u32 words; [SP+8..15]
     * = padding (SP-alignment). */
    emit_u32(e, 0xd10043ffu);   /* SUB SP, SP, #16 */

    emit_mov_x_reg(e, /*rd=*/19, /*rn=*/0);   /* x19 = dsp */

    emit_mov_imm64(e, /*rd=*/20,
        (uint64_t)(uintptr_t)&dsp_jit_helper_postexecute_update_pc);
    emit_mov_imm64(e, /*rd=*/21,
        (uint64_t)(uintptr_t)&dsp_jit_helper_postexecute_interrupts);

    /* Clear self-mod exit-request flag at the start of each block.
     * dsp_jit_invalidate() sets it from inside handlers that write
     * to P-space; we read it in the per-op exit checks below to
     * bail out before running any stale instruction stub. Always
     * emitted — this is a JIT correctness flag, not diff-specific. */
    emit_strb_imm_zero(e, /*rn=*/19, OFF_JIT_EXIT_BLOCK_REQ);

    /* Clear diff-skip flag at block start. Set by handlers that
     * perform externally-visible side effects (e.g. DMA control
     * writes that scatter-gather Xbox host RAM) which the diff
     * harness cannot validate by interpreter replay. Only emitted
     * when diff mode is active so the non-diff (default) hot path
     * pays zero cost for this flag. Since g_jit_diff is set once
     * from parse_flags_once() and never changes, a block
     * translated while diff is off will never need the reset. */
    if (g_jit_diff) {
        emit_strb_imm_zero(e, /*rn=*/19, OFF_JIT_SKIP_DIFF);
    }
}

/* Shims implemented at the bottom of dsp_cpu.c (where the static
 * emu_* symbols and the opcodes[] table are in scope). */
emu_func_t dsp_jit_helper_lookup_emu(uint32_t inst);
uint32_t dsp_jit_helper_inst_length(uint32_t inst);
bool dsp_jit_helper_is_terminator(void *fn);

static DspJitBlock *translate_block(dsp_core_t *dsp, DspJitState *s,
                                    uint32_t pc_start)
{
    /* If any existing block covers pc_start, invalidate it first so we
     * never have two translated blocks with overlapping PC ranges. */
    if (s->pc_to_block[pc_start]) {
        DspJitBlock *old = s->pc_to_block[pc_start];
        for (uint32_t p = old->pc_start; p < old->pc_end && p < DSP_PRAM_SIZE; p++) {
            s->pc_to_block[p] = NULL;
        }
        old->entry = NULL;
        old->num_ops = 0;
    }

    /* If the code buffer is near-full, flush the cache.
     * Per-stub worst case is ~80 ARM64 instructions (~320 bytes)
     * once the fast-path branches are included; budget 512 bytes
     * per stub for safety. */
    size_t space_left = (s->code_buf + s->code_cap) - s->code_ptr;
    size_t worst_case = 256 + DSP_JIT_MAX_OPS_PER_BLOCK * 512;
    if (space_left < worst_case) {
        dsp_jit_invalidate_all(dsp);
    }

    ArmEmit e = {
        .buf = (uint32_t *)s->code_ptr,
        .buf_start = (uint32_t *)s->code_ptr,
        .buf_end = (uint32_t *)(s->code_buf + s->code_cap),
    };

    uint32_t *entry_ptr = e.buf;

    /* Toggle JIT pages writable on this thread while we emit. */
    qemu_thread_jit_write();

    emit_prologue(&e);

    ExitPatchList exits = { .count = 0 };

    uint32_t pc = pc_start;
    uint32_t pc_end = pc_start;
    int num_ops = 0;
    uint32_t write_set = 0;

    while (num_ops < DSP_JIT_MAX_OPS_PER_BLOCK && pc < DSP_PRAM_SIZE) {
        /*
         * Match the interpreter's read_memory_p behaviour: use the
         * full 32-bit pram word. Upper 8 bits are normally zero
         * (bootstrap masks them for the first 0x800 words), but if
         * a later DMA or write path leaves them non-zero, the
         * interpreter's dispatch keys on the full value. Masking
         * down to 24 bits here would make the JIT pick the
         * non-parmove path when the interpreter picks parmove
         * (for e.g. inst = 0x01000067: unmasked >= 0x100000 so
         * interp -> opcodes_parmove[0], masked = 0x000067 < 0x100000
         * so JIT -> non-parmove emu_*), and the resulting divergent
         * state would only show up after the first wrong op.
         */
        uint32_t inst = dsp->pram[pc];
        bool keep_going;

        if (inst >= 0x100000) {
            /* Parallel-move-bearing instruction (Phase 4). */
            emu_func_t alu = dsp_jit_helper_lookup_alu(inst);
            if (!alu) {
                /* ALU entry is emu_undefined — bail. */
                if (num_ops == 0) {
                    qemu_thread_jit_execute();
                    return NULL;
                }
                break;
            }

            uint32_t expected_next_pc = pc + 1;
            if (!emit_parmove_stub(&e, &exits, inst, alu, expected_next_pc,
                                   &write_set, dsp, pc)) {
                /* Variant not yet implemented: fall through to the
                 * interpreter for this op. */
                if (num_ops == 0) {
                    qemu_thread_jit_execute();
                    return NULL;
                }
                break;
            }

            s->pc_to_block[pc] = &s->blocks[pc_start];
            pc_end = pc + 1;
            pc = pc_end;
            num_ops++;
            keep_going = true;
        } else {
            emu_func_t emu = dsp_jit_helper_lookup_emu(inst);
            if (!emu) {
                if (num_ops == 0) {
                    qemu_thread_jit_execute();
                    return NULL;
                }
                break;
            }

            uint32_t inst_len = dsp_jit_helper_inst_length(inst);
            bool is_term = dsp_jit_helper_is_terminator((void *)emu);
            uint32_t expected_next_pc = pc + inst_len;

            keep_going = emit_instruction(&e, &exits, dsp, pc, inst,
                                          inst_len, emu, expected_next_pc,
                                          is_term, &write_set);

            for (uint32_t p = pc; p < pc + inst_len && p < DSP_PRAM_SIZE; p++) {
                s->pc_to_block[p] = &s->blocks[pc_start];
            }

            pc_end = pc + inst_len;
            pc = pc_end;
            num_ops++;
        }

        if (!keep_going) {
            break;
        }

        /* Defensive overflow check before next iteration (worst-case
         * per-op emit is ~80 ARM64 instructions = 320 bytes; leave
         * 512 bytes as comfortable headroom). */
        if ((e.buf_end - e.buf) < 128) {
            break;
        }
    }

    /* Emit shared exit/epilogue and patch pending exit branches. */
    uint32_t *exit_label = emit_epilogue(&e);
    patch_exits(&exits, exit_label);

    /* Advance bump allocator, flush I-cache, re-protect pages. */
    uint8_t *code_end = (uint8_t *)e.buf;
    jit_clear_icache(entry_ptr, code_end);
    s->code_ptr = code_end;
    qemu_thread_jit_execute();

    /* Install the block. */
    DspJitBlock *block = &s->blocks[pc_start];
    block->pc_start = pc_start;
    block->pc_end = pc_end;
    block->entry = (dsp_jit_entry_fn)entry_ptr;
    block->num_ops = num_ops;
    block->write_set = write_set;
    /* Fresh translation — must go through validation again. Written
     * atomically because the validator thread may read it. */
    qatomic_set(&block->diff_checked, 0);
    s->blocks_translated++;

    /* Optional hex dump of the emitted ARM64 block, for offline
     * disassembly and verification. Gated to avoid noise; set
     * XEMU_DSP_JIT_DUMP=1 to enable. */
    static bool dump_parsed, dump_enabled;
    if (!dump_parsed) {
        dump_parsed = true;
        const char *e = getenv("XEMU_DSP_JIT_DUMP");
        dump_enabled = (e && e[0] == '1');
    }
    if (dump_enabled) {
        fprintf(stderr,
                "[dsp-jit] block pc_start=0x%04x pc_end=0x%04x num_ops=%u "
                "entry=%p size=%zu bytes\n",
                pc_start, pc_end, num_ops, (void *)entry_ptr,
                (size_t)(code_end - (uint8_t *)entry_ptr));
        uint32_t *p = (uint32_t *)entry_ptr;
        uint32_t *pe = (uint32_t *)code_end;
        for (uint32_t *pi = p; pi < pe; pi += 4) {
            fprintf(stderr, "  %p:", (void *)pi);
            for (int k = 0; k < 4 && pi + k < pe; k++) {
                fprintf(stderr, " %08x", pi[k]);
            }
            fprintf(stderr, "\n");
        }
    }

    return block;
}

/* --------------------------------------------------------------- *
 * Public API
 * --------------------------------------------------------------- */

/* Forward decls — the validator helpers are defined below this
 * Public API block, but dsp_jit_init / dsp_jit_finalize need to
 * call them. */
static DiffQueue *diff_queue_create(dsp_core_t *owner);
static void       diff_queue_destroy(DiffQueue *q);

/* atexit-handler glue — needed because the normal xemu exit path
 * never calls dsp_destroy / dsp_jit_finalize. Definitions live
 * right after dsp_jit_finalize below. */
#define DSP_JIT_MAX_REGISTERED_CORES 2
static dsp_core_t *g_jit_registered_cores[DSP_JIT_MAX_REGISTERED_CORES];
static int g_jit_registered_count;
static bool g_jit_atexit_registered;
static void dsp_jit_stats_atexit_handler(void);

void dsp_jit_init(dsp_core_t *dsp)
{
    parse_flags_once();
    if (!g_jit_enabled) {
        return;
    }

    DspJitState *s = g_malloc0(sizeof(*s));
    s->code_buf = jit_code_alloc(DSP_JIT_CODE_BYTES);
    if (!s->code_buf) {
        fprintf(stderr, "xemu: DSP JIT: failed to mmap %u MiB code buffer; "
                        "JIT disabled for this core\n",
                (unsigned)(DSP_JIT_CODE_BYTES / (1024 * 1024)));
        g_free(s);
        return;
    }
    s->code_cap = DSP_JIT_CODE_BYTES;
    s->code_ptr = s->code_buf;

    if (g_jit_diff && !g_jit_diff_sync) {
        /* Async diff mode: spin up a validator worker thread with
         * its own ring of pre/post snapshots. See the DiffQueue /
         * diff_worker_fn commentary for the full design. If queue
         * creation fails (OOM / thread create refused), fall through
         * silently — the diff path auto-detects q == NULL and uses
         * sync mode per-block. */
        s->diff_q = diff_queue_create(dsp);
        if (!s->diff_q) {
            fprintf(stderr, "xemu: DSP JIT: async validator init "
                            "failed, falling back to sync diff mode\n");
        }
    }

    /* Stash on dsp_core via hidden slot. We use dsp->pram_opcache slot
     * 0xFFF0-ish? No — that would collide. Add a dedicated field via
     * the "jit_state" hook we add to dsp_core_t in this same commit. */
    dsp->jit_state = s;

    /*
     * Register this core with the atexit handler so
     * XEMU_DSP_JIT_STATS=1 prints on normal xemu quit. The Xbox
     * machine shutdown path (Cmd-Q / window close / SIGTERM) does
     * NOT call dsp_destroy — it just exits — so the stats dump
     * wired into dsp_jit_finalize would never fire otherwise.
     *
     * The registry is bounded (GP + EP = 2 cores max) and any
     * subsequent core going through dsp_jit_finalize cleanly
     * removes itself, so the atexit handler only sees live cores.
     */
    if (g_jit_registered_count < DSP_JIT_MAX_REGISTERED_CORES) {
        g_jit_registered_cores[g_jit_registered_count++] = dsp;
    }
    if (g_jit_stats && !g_jit_atexit_registered) {
        g_jit_atexit_registered = true;
        atexit(dsp_jit_stats_atexit_handler);
    }
}

/*
 * Dump the per-core + process-wide JIT counters to stderr. Used
 * from two call sites:
 *   1. dsp_jit_finalize — the clean-shutdown path (test-dsp binary).
 *   2. dsp_jit_stats_atexit_handler — the normal xemu exit path,
 *      which doesn't call dsp_destroy (Cmd-Q / signal / window
 *      close go through a plain exit() that never tears down the
 *      Xbox machine's MCPX APU).
 *
 * The per-core `printed` flag guards against double-printing when
 * both paths fire in the same process (test-dsp runs dsp_destroy
 * AND its atexit handler).
 */
static void dsp_jit_print_stats(dsp_core_t *dsp)
{
    DspJitState *s = (DspJitState *)dsp->jit_state;
    if (!s) {
        return;
    }
    if (s->stats_printed) {
        return;
    }
    s->stats_printed = true;

    if (s->diff_q) {
        fprintf(stderr,
                "xemu: DSP JIT validator (%s core) final: "
                "enqueued=%" PRIu64 " checked=%" PRIu64
                " dropped=%" PRIu64 " failures=%" PRIu64 "\n",
                dsp->is_gp ? "GP" : "EP",
                s->diff_q->enqueued, s->diff_q->checked,
                s->diff_q->dropped, s->diff_q->failures);
    }

    fprintf(stderr,
            "xemu: DSP JIT stats (%s core):\n"
            "  blocks_translated = %" PRIu64 "\n"
            "  blocks_executed   = %" PRIu64 "\n"
            "  cache_flushes     = %" PRIu64 "\n"
            "  fallbacks         = %" PRIu64 "\n"
            "  diff_ops_checked  = %" PRIu64 "\n"
            "  code_buf_used     = %zu bytes / %zu bytes\n"
            "  alu_inlined       = %" PRIu64
            " (%.1f%% of ALU ops)\n"
            "  alu_fallback      = %" PRIu64 "\n"
            "  cf_inlined        = %" PRIu64
            " (%.1f%% of emitted ops)\n"
            "  cf_fallback       = %" PRIu64 "\n",
            dsp->is_gp ? "GP" : "EP",
            s->blocks_translated, s->blocks_executed,
            s->cache_flushes, s->fallbacks,
            s->diff_ops_checked,
            (size_t)(s->code_ptr - s->code_buf), s->code_cap,
            g_alu_inlined_count,
            (g_alu_inlined_count + g_alu_fallback_count) == 0 ? 0.0 :
                100.0 * (double)g_alu_inlined_count /
                (double)(g_alu_inlined_count + g_alu_fallback_count),
            g_alu_fallback_count,
            g_cf_inlined_count,
            (g_cf_inlined_count + g_cf_fallback_count) == 0 ? 0.0 :
                100.0 * (double)g_cf_inlined_count /
                (double)(g_cf_inlined_count + g_cf_fallback_count),
            g_cf_fallback_count);
    fflush(stderr);
}

/*
 * atexit handler: iterate the registered live JIT cores and dump
 * per-core stats. The normal xemu shutdown path does not call
 * dsp_destroy, so without this hook the stats print wired into
 * dsp_jit_finalize never fires. Each core's stats_printed flag
 * guards against double-printing if dsp_destroy also ran (test
 * binary).
 */
static void dsp_jit_stats_atexit_handler(void)
{
    if (!g_jit_stats) {
        return;
    }
    for (int i = 0; i < g_jit_registered_count; i++) {
        dsp_core_t *d = g_jit_registered_cores[i];
        if (d && d->jit_state) {
            dsp_jit_print_stats(d);
        }
    }
}

void dsp_jit_finalize(dsp_core_t *dsp)
{
    DspJitState *s = (DspJitState *)dsp->jit_state;
    if (!s) {
        return;
    }

    if (g_jit_stats) {
        dsp_jit_print_stats(dsp);
    }

    /* Drain and stop the validator BEFORE freeing the queue: the
     * worker holds a pointer into s->diff_q->slots. */
    if (s->diff_q) {
        diff_queue_destroy(s->diff_q);
        s->diff_q = NULL;
    }

    /* Remove from the atexit registry so the handler (if it fires
     * after this) doesn't try to print a freed core. */
    for (int i = 0; i < g_jit_registered_count; i++) {
        if (g_jit_registered_cores[i] == dsp) {
            g_jit_registered_cores[i] = NULL;
            break;
        }
    }

    jit_code_free(s->code_buf, s->code_cap);
    g_free(s);
    dsp->jit_state = NULL;
}

void dsp_jit_invalidate(dsp_core_t *dsp, uint32_t addr)
{
    DspJitState *s = (DspJitState *)dsp->jit_state;
    if (!s || addr >= DSP_PRAM_SIZE) {
        return;
    }
    /*
     * Set the self-mod exit-request flag unconditionally. If we're
     * running under a JIT block right now (called from inside an
     * emu handler that wrote P-space), the current block's next
     * exit check will see this flag and exit early so the block
     * doesn't continue executing stubs with stale pram. If we're
     * outside the JIT (called from e.g. dsp_bootstrap), the flag
     * is harmlessly read + cleared at the start of the next block.
     */
    dsp->jit_exit_block_request = 1;

    DspJitBlock *b = s->pc_to_block[addr];
    if (!b) {
        return;
    }
    /* Evict all PCs covered by this block. */
    for (uint32_t p = b->pc_start; p < b->pc_end && p < DSP_PRAM_SIZE; p++) {
        s->pc_to_block[p] = NULL;
    }
    b->entry = NULL;
    b->num_ops = 0;
    /* Clear diff_checked so any in-flight validator entry for this
     * translation (same block pointer, different entry) can't race
     * and stamp the retranslated block as "validated" later. */
    qatomic_set(&b->diff_checked, 0);
}

void dsp_jit_invalidate_all(dsp_core_t *dsp)
{
    DspJitState *s = (DspJitState *)dsp->jit_state;
    if (!s) {
        return;
    }
    memset(s->pc_to_block, 0, sizeof(s->pc_to_block));
    memset(s->blocks, 0, sizeof(s->blocks));
    s->code_ptr = s->code_buf;
    s->cache_flushes++;
}

/* --------------------------------------------------------------- *
 * Differential validator — off-APU-thread infrastructure
 * --------------------------------------------------------------- */

/* Forward decl of the actual interpreter step — exposed from dsp_cpu.c. */
void dsp56k_execute_instruction(dsp_core_t *dsp);

/*
 * Peripheral shims for interpreter replay on a private dsp_core_t.
 *
 * The live dsp's read_peripheral / write_peripheral use
 * container_of(core, DSPState, core) to reach hardware state. If we
 * copy dsp_core_t bit-wise into a slot, those function pointers
 * still point at the real implementations — but container_of on the
 * copy would yield a bogus DSPState* and the real call would read
 * or mutate arbitrary memory. Rather than restoring the live state
 * after replay (which is what the old sync implementation did), we
 * swap the callbacks on the private copy to these stubs. If the
 * interp replay reaches one, it marks the slot as "skip compare" —
 * peripheral I/O is already flagged by the live-dsp DMA path via
 * core->jit_skip_diff_compare, so this is a belt-and-braces safety
 * check rather than the primary mechanism.
 */
static uint32_t diff_shim_read_peripheral(dsp_core_t *core, uint32_t address)
{
    (void)address;
    core->jit_skip_diff_compare = 1;
    return 0;
}

static void diff_shim_write_peripheral(dsp_core_t *core, uint32_t address,
                                       uint32_t value)
{
    (void)address;
    (void)value;
    core->jit_skip_diff_compare = 1;
}

/*
 * Byte-compare two dsp_core_t snapshots over their semantic state.
 * Returns offsetof of the first differing byte, or (size_t)-1 if
 * they match.
 *
 * `write_set` gates the memory-array ranges: if a block demonstrably
 * didn't write to xram, we skip comparing the 16 KB xram region
 * (similarly yram / pram / mixbuffer / periph). This is the
 * "narrowed compare" optimisation and cuts the typical FIR-kernel
 * compare from ~40 KB to ~2 KB.
 *
 * Always-excluded regions:
 *   - pram_opcache (interpreter-only cache; JIT doesn't populate it).
 *   - read_peripheral / write_peripheral function pointers (swapped
 *     to shims on the private copy; must differ by design).
 *   - jit_state pointer (our own bookkeeping).
 *   - jit_exit_block_request / jit_skip_diff_compare (flags we
 *     manipulate during the compare; compare them separately if
 *     needed).
 *   - disasm_* tail (debug/trace only).
 */
static size_t dsp_state_diff(const dsp_core_t *a, const dsp_core_t *b,
                             uint32_t write_set)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    struct diff_range { size_t off; size_t end; bool gated; uint32_t bit; };
    const struct diff_range ranges[] = {
        /* [0] Always: head through end of stack (is_gp..stack[1][15]). */
        { 0,
          offsetof(dsp_core_t, xram), false, 0 },
        /* [1] xram (WS_XRAM-gated) */
        { offsetof(dsp_core_t, xram),
          offsetof(dsp_core_t, yram), true, DSP_JIT_WS_XRAM },
        /* [2] yram (WS_YRAM-gated) */
        { offsetof(dsp_core_t, yram),
          offsetof(dsp_core_t, pram), true, DSP_JIT_WS_YRAM },
        /* [3] pram (WS_PRAM-gated) */
        { offsetof(dsp_core_t, pram),
          offsetof(dsp_core_t, pram_opcache), true, DSP_JIT_WS_PRAM },
        /* skip pram_opcache */
        /* [4] mixbuffer (WS_MIXBUFFER-gated) */
        { offsetof(dsp_core_t, mixbuffer),
          offsetof(dsp_core_t, periph), true, DSP_JIT_WS_MIXBUFFER },
        /* [5] periph (WS_PERIPH-gated) */
        { offsetof(dsp_core_t, periph),
          offsetof(dsp_core_t, loop_rep), true, DSP_JIT_WS_PERIPH },
        /* [6] Always: loop_rep through interrupt_is_pending */
        { offsetof(dsp_core_t, loop_rep),
          offsetof(dsp_core_t, read_peripheral), false, 0 },
        /* skip read_peripheral + write_peripheral function pointers */
        /* [7] Always: num_inst through cur_inst (end of runtime data) */
        { offsetof(dsp_core_t, num_inst),
          offsetof(dsp_core_t, str_disasm_memory), false, 0 },
        /* skip disasm_* tail */
    };

    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        if (ranges[r].gated && !(write_set & ranges[r].bit)) {
            continue;
        }
        for (size_t i = ranges[r].off; i < ranges[r].end; i++) {
            if (pa[i] != pb[i]) {
                return i;
            }
        }
    }
    return (size_t)-1;
}

/*
 * Diagnostic "window" around the block's starting PC used by the
 * failure path to show "did the block rewrite its own pram mid-
 * execution?" (the self-modifying-code diagnostic). Sized to cover
 * the longest translation we'd generate (DSP_JIT_MAX_OPS_PER_BLOCK
 * = 32, each up to 2 words). 64 words is plenty; the printed dump
 * only shows 10.
 */
#define DIFF_PRAM_WINDOW 64

/* Print a fully-formed DIFF FAILURE diagnostic given the two
 * mismatching snapshots plus the pre-block pram window we captured
 * before the interpreter replay started (so the "did pram change
 * during this block?" self-mod check is meaningful). Sync and
 * async paths share the same text. */
static void diff_report_failure(const uint32_t *pre_pram_window,
                                uint32_t pc_start,
                                const dsp_core_t *interp,
                                const dsp_core_t *jit,
                                uint32_t jit_cycles,
                                size_t diff_off)
{
    const char *field = "?";
    uint32_t field_idx = 0;
    uint32_t interp_word = 0;
    uint32_t jit_word = 0;
    if (diff_off >= offsetof(dsp_core_t, registers) &&
        diff_off <  offsetof(dsp_core_t, registers) + sizeof(((dsp_core_t*)0)->registers)) {
        field = "registers";
        field_idx = (uint32_t)((diff_off - offsetof(dsp_core_t, registers)) / 4);
        interp_word = interp->registers[field_idx];
        jit_word    = jit->registers[field_idx];
    } else if (diff_off >= offsetof(dsp_core_t, stack) &&
               diff_off <  offsetof(dsp_core_t, stack) + sizeof(((dsp_core_t*)0)->stack)) {
        field = "stack";
        uint32_t off_in = (uint32_t)(diff_off - offsetof(dsp_core_t, stack));
        field_idx = off_in / 4;
        interp_word = ((const uint32_t *)interp->stack)[field_idx];
        jit_word    = ((const uint32_t *)jit->stack)[field_idx];
    } else if (diff_off >= offsetof(dsp_core_t, xram) &&
               diff_off <  offsetof(dsp_core_t, xram) + sizeof(((dsp_core_t*)0)->xram)) {
        field = "xram";
        field_idx = (uint32_t)((diff_off - offsetof(dsp_core_t, xram)) / 4);
        interp_word = interp->xram[field_idx];
        jit_word    = jit->xram[field_idx];
    } else if (diff_off >= offsetof(dsp_core_t, yram) &&
               diff_off <  offsetof(dsp_core_t, yram) + sizeof(((dsp_core_t*)0)->yram)) {
        field = "yram";
        field_idx = (uint32_t)((diff_off - offsetof(dsp_core_t, yram)) / 4);
        interp_word = interp->yram[field_idx];
        jit_word    = jit->yram[field_idx];
    } else if (diff_off >= offsetof(dsp_core_t, pram) &&
               diff_off <  offsetof(dsp_core_t, pram) + sizeof(((dsp_core_t*)0)->pram)) {
        field = "pram";
        field_idx = (uint32_t)((diff_off - offsetof(dsp_core_t, pram)) / 4);
        interp_word = interp->pram[field_idx];
        jit_word    = jit->pram[field_idx];
    } else if (diff_off >= offsetof(dsp_core_t, periph) &&
               diff_off <  offsetof(dsp_core_t, periph) + sizeof(((dsp_core_t*)0)->periph)) {
        field = "periph";
        field_idx = (uint32_t)((diff_off - offsetof(dsp_core_t, periph)) / 4);
        interp_word = interp->periph[field_idx];
        jit_word    = jit->periph[field_idx];
    } else if (diff_off >= offsetof(dsp_core_t, mixbuffer) &&
               diff_off <  offsetof(dsp_core_t, mixbuffer) + sizeof(((dsp_core_t*)0)->mixbuffer)) {
        field = "mixbuffer";
        field_idx = (uint32_t)((diff_off - offsetof(dsp_core_t, mixbuffer)) / 4);
        interp_word = interp->mixbuffer[field_idx];
        jit_word    = jit->mixbuffer[field_idx];
    }

    fprintf(stderr,
            "xemu: DSP JIT DIFF FAILURE in block pc_start=0x%04x "
            "(jit_cycles=%u)\n"
            "  First differing byte at offsetof dsp_core_t = %zu "
            "(%s[%u]: interp=0x%06x jit=0x%06x)\n"
            "  (pc at entry = 0x%04x)\n"
            "  INTERP pc=0x%04x sr=0x%06x A2:A1:A0=%02x:%06x:%06x "
            "B2:B1:B0=%02x:%06x:%06x\n"
            "  JIT    pc=0x%04x sr=0x%06x A2:A1:A0=%02x:%06x:%06x "
            "B2:B1:B0=%02x:%06x:%06x\n"
            "  loop_rep interp=%u jit=%u  "
            "interrupt_counter interp=%u jit=%u\n",
            pc_start, jit_cycles, diff_off,
            field, field_idx, interp_word, jit_word,
            pc_start,
            interp->pc, interp->registers[DSP_REG_SR],
            interp->registers[DSP_REG_A2], interp->registers[DSP_REG_A1],
            interp->registers[DSP_REG_A0],
            interp->registers[DSP_REG_B2], interp->registers[DSP_REG_B1],
            interp->registers[DSP_REG_B0],
            jit->pc, jit->registers[DSP_REG_SR],
            jit->registers[DSP_REG_A2], jit->registers[DSP_REG_A1],
            jit->registers[DSP_REG_A0],
            jit->registers[DSP_REG_B2], jit->registers[DSP_REG_B1],
            jit->registers[DSP_REG_B0],
            interp->loop_rep, jit->loop_rep,
            interp->interrupt_counter, jit->interrupt_counter);

    fprintf(stderr, "  pram dump (block + next few words):\n");
    uint32_t pc0 = pc_start;
    uint32_t dump_len = 10;
    if (pc0 + dump_len > DSP_PRAM_SIZE) dump_len = DSP_PRAM_SIZE - pc0;
    if (dump_len > DIFF_PRAM_WINDOW) dump_len = DIFF_PRAM_WINDOW;
    bool self_mod = false;
    for (uint32_t i = 0; i < dump_len; i++) {
        uint32_t pre_w  = pre_pram_window[i];
        uint32_t post_w = jit->pram[pc0 + i];
        const char *marker = (pre_w != post_w) ? "  <-- SELF-MOD" : "";
        if (pre_w != post_w) self_mod = true;
        fprintf(stderr, "    pram[0x%04x]  pre=0x%08x post=0x%08x%s\n",
                pc0 + i, pre_w, post_w, marker);
    }
    if (self_mod) {
        fprintf(stderr,
            "  *** self-modifying code detected: the JIT block "
            "baked in the pre-write instruction words and kept\n"
            "      running its stale stubs after the handler "
            "rewrote pram.\n");
    }

    fprintf(stderr, "  Rn/Nn/Mn (at failure time):\n");
    for (int i = 0; i < 8; i++) {
        fprintf(stderr, "    R%d=%04x N%d=%04x M%d=%04x\n",
                i, interp->registers[DSP_REG_R0 + i] & 0xffff,
                i, interp->registers[DSP_REG_N0 + i] & 0xffff,
                i, interp->registers[DSP_REG_M0 + i] & 0xffff);
    }
}

/*
 * Validate one captured slot: run the interpreter on slot->pre up
 * to the same num_inst target the JIT reached in slot->post, then
 * compare. May be called inline on the APU thread (sync mode) or
 * on the validator thread (async mode) — the logic is identical.
 *
 * Returns true on success or skipped; false on divergence (caller
 * aborts the process in either mode).
 */
static bool diff_validate_slot(DiffSlot *slot, DiffQueue *q)
{
    if (slot->skip_compare) {
        /* JIT block hit DMA / non-replayable I/O. Validation meaningless. */
        return true;
    }

    /* Capture a small window of the pre-JIT pram starting at
     * pc_start BEFORE interp runs (which may mutate slot->pre.pram
     * if the block is self-modifying). Only used on divergence, by
     * the failure diagnostic. */
    uint32_t pre_pram_window[DIFF_PRAM_WINDOW];
    {
        uint32_t pc0 = slot->pc_start;
        uint32_t n = DIFF_PRAM_WINDOW;
        if (pc0 + n > DSP_PRAM_SIZE) {
            n = DSP_PRAM_SIZE - pc0;
        }
        memcpy(pre_pram_window, &slot->pre.pram[pc0], n * sizeof(uint32_t));
    }

    /* Swap the peripheral callbacks to shims so the interp replay
     * can't reach into a bogus DSPState*. */
    slot->pre.read_peripheral  = diff_shim_read_peripheral;
    slot->pre.write_peripheral = diff_shim_write_peripheral;
    slot->pre.jit_state        = NULL;  /* ignore our own bookkeeping */
    slot->pre.jit_skip_diff_compare = 0;
    slot->pre.jit_exit_block_request = 0;

    uint32_t interp_target = slot->num_inst_before + slot->jit_cycles;
    int guard = 4096;  /* enough for deepest legitimate block */
    while (slot->pre.num_inst < interp_target && guard-- > 0) {
        dsp56k_execute_instruction(&slot->pre);
        if (slot->pre.jit_skip_diff_compare) {
            /* Interp replay hit a peripheral; abort the compare. */
            return true;
        }
    }

    if (q) {
        q->checked++;
    }

    size_t diff_off = dsp_state_diff(&slot->pre, &slot->post,
                                     slot->write_set);
    if (diff_off == (size_t)-1) {
        return true;
    }

    if (q) {
        q->failures++;
    }
    diff_report_failure(pre_pram_window, slot->pc_start,
                        &slot->pre /* post-interp */,
                        &slot->post /* post-JIT */,
                        slot->jit_cycles, diff_off);
    return false;
}

/*
 * Validator worker thread. Pops published slots off the ring,
 * validates each one, and logs/aborts on divergence. Wakes on
 * cond signal; exits when q->exiting is set.
 */
static void *diff_worker_fn(void *opaque)
{
    DiffQueue *q = (DiffQueue *)opaque;

    for (;;) {
        qemu_mutex_lock(&q->mu);
        while (!q->exiting &&
               qatomic_load_acquire(&q->tail) ==
                   qatomic_load_acquire(&q->head)) {
            qemu_cond_wait(&q->cond, &q->mu);
        }
        bool exiting = q->exiting;
        qemu_mutex_unlock(&q->mu);

        /* Drain as many slots as are available, even when exiting,
         * so CI runs that set a small DIFF_MAX and then quit still
         * see any divergence the validator would have caught.
         * Acquire-load on tail pairs with the producer's
         * release-store in diff_queue_finish_push, ensuring we see
         * the fully-written slot contents. */
        while (qatomic_load_acquire(&q->tail) >
               qatomic_read(&q->head)) {
            uint64_t head = qatomic_read(&q->head);
            DiffSlot *slot = &q->slots[head % DSP_JIT_DIFF_SLOTS];

            DspJitBlock     *block       = slot->block;
            dsp_jit_entry_fn block_entry = slot->block_entry;
            bool             passed      = diff_validate_slot(slot, q);

            /* Mark the block as checked iff:
             *   (a) the compare passed (divergence would have
             *       aborted the process already);
             *   (b) the block has not been retranslated since
             *       enqueue (entry pointer still matches) —
             *       otherwise the new translation is different
             *       code and needs its own validation pass, which
             *       the producer's diff_checked=0 will trigger on
             *       its next call. Plain pointer read is atomic
             *       enough on ARM64 / macOS for this check. */
            if (passed && block && block->entry == block_entry) {
                qatomic_set(&block->diff_checked, 1);
            }

            /* Consumer-side advance: release-store so the producer
             * (if racing against a nearly-full ring) sees the slot
             * as free promptly. */
            qatomic_store_release(&q->head, head + 1);

            if (!passed) {
                /* Print final stats to aid post-mortem and abort.
                 * Flush stderr before abort() — macOS Xcode build
                 * configs can have unflushed output lost if the
                 * process terminates abruptly, and the divergence
                 * diagnostic is the ONE piece of information the
                 * user needs from a failed validation. */
                fprintf(stderr,
                        "xemu: DSP JIT validator (%s core): "
                        "checked=%" PRIu64 " failures=%" PRIu64
                        " dropped=%" PRIu64 " enqueued=%" PRIu64 "\n",
                        (q->owner && q->owner->is_gp) ? "GP" : "EP",
                        q->checked, q->failures, q->dropped, q->enqueued);
                fflush(stderr);
                abort();
            }
        }

        if (exiting) {
            break;
        }
    }
    return NULL;
}

/* Allocate and start the validator queue + worker. Returns NULL if
 * allocation or thread creation fails (diff mode then falls back to
 * sync, matching the XEMU_DSP_JIT_DIFF_SYNC=1 path). */
static DiffQueue *diff_queue_create(dsp_core_t *owner)
{
    DiffQueue *q = g_malloc0(sizeof(*q));
    q->slots = g_try_malloc0(sizeof(DiffSlot) * DSP_JIT_DIFF_SLOTS);
    if (!q->slots) {
        g_free(q);
        return NULL;
    }
    q->owner = owner;
    qemu_mutex_init(&q->mu);
    qemu_cond_init(&q->cond);
    qemu_thread_create(&q->worker, "mcpx.dsp_diff",
                       diff_worker_fn, q, QEMU_THREAD_JOINABLE);
    q->worker_started = true;
    return q;
}

static void diff_queue_destroy(DiffQueue *q)
{
    if (!q) {
        return;
    }
    if (q->worker_started) {
        qemu_mutex_lock(&q->mu);
        q->exiting = true;
        qemu_cond_signal(&q->cond);
        qemu_mutex_unlock(&q->mu);
        qemu_thread_join(&q->worker);
    }
    qemu_cond_destroy(&q->cond);
    qemu_mutex_destroy(&q->mu);
    g_free(q->slots);
    g_free(q);
}

/*
 * Async producer. Claims the next ring slot if space is available,
 * copies dsp into slot->pre, returns the slot pointer. On ring full
 * returns NULL and increments q->dropped — caller runs JIT without
 * enqueueing (the slot is effectively dropped from validation).
 *
 * Not thread-safe for multi-producer; fine here since only the APU
 * thread produces.
 */
static DiffSlot *diff_queue_begin_push(DiffQueue *q, const dsp_core_t *dsp)
{
    uint64_t tail = qatomic_read(&q->tail);
    /* Acquire-load on head pairs with the consumer's release-store,
     * so we see the slot as free as soon as the consumer finished
     * with it. */
    uint64_t head = qatomic_load_acquire(&q->head);
    if ((tail - head) >= DSP_JIT_DIFF_SLOTS) {
        q->dropped++;
        return NULL;
    }
    DiffSlot *slot = &q->slots[tail % DSP_JIT_DIFF_SLOTS];
    memcpy(&slot->pre, dsp, sizeof(*dsp));
    return slot;
}

static void diff_queue_finish_push(DiffQueue *q, DiffSlot *slot,
                                   const dsp_core_t *dsp,
                                   uint32_t pc_start, uint32_t num_inst_before,
                                   uint32_t jit_cycles, uint32_t write_set,
                                   bool skip_compare)
{
    memcpy(&slot->post, dsp, sizeof(*dsp));
    slot->pc_start        = pc_start;
    slot->num_inst_before = num_inst_before;
    slot->jit_cycles      = jit_cycles;
    slot->write_set       = write_set;
    slot->skip_compare    = skip_compare;
    q->enqueued++;

    /* Publish: release-store the new tail so the consumer sees the
     * filled slot. */
    uint64_t tail = qatomic_read(&q->tail);
    qatomic_store_release(&q->tail, tail + 1);

    /* Wake the worker. We use a signal under the mutex to avoid
     * missed wakeups, but only when the ring was empty before this
     * push — otherwise the worker is either running or about to
     * re-check the tail, so no wake needed. This keeps the common-
     * case producer path off the mutex entirely. */
    if (tail == qatomic_read(&q->head)) {
        qemu_mutex_lock(&q->mu);
        qemu_cond_signal(&q->cond);
        qemu_mutex_unlock(&q->mu);
    }
}

static unsigned int dsp_jit_execute_block_diff(dsp_core_t *dsp,
                                               DspJitState *s)
{
    DiffQueue *q = s->diff_q;
    /*
     * Three execution modes, picked per-block:
     *   - async + ring has space: copy pre into ring slot, run JIT,
     *     copy post into ring slot, publish. Worker thread validates
     *     later; APU thread pays 2 memcpys only.
     *   - async + ring full: drop validation for this block
     *     (increment q->dropped, still run the JIT normally). Keeps
     *     the APU thread fast under load spikes; validation becomes
     *     a sampler over the full run.
     *   - sync (XEMU_DSP_JIT_DIFF_SYNC=1 or no q): snapshot into a
     *     stack-local slot, run JIT, validate inline on the APU
     *     thread. Slow, for bring-up debugging only.
     */

    enum { MODE_ASYNC, MODE_DROP, MODE_SYNC } mode;
    DiffSlot  sync_slot;
    DiffSlot *slot = NULL;

    if (q && !g_jit_diff_sync) {
        slot = diff_queue_begin_push(q, dsp);
        mode = slot ? MODE_ASYNC : MODE_DROP;
    } else {
        slot = &sync_slot;
        memcpy(&slot->pre, dsp, sizeof(*dsp));
        mode = MODE_SYNC;
    }

    /* Run the JIT block on the live dsp. */
    DspJitBlock *b = s->pc_to_block[dsp->pc];
    if (!b || b->entry == NULL || b->pc_start != dsp->pc) {
        b = translate_block(dsp, s, dsp->pc);
        if (!b || !b->entry) {
            s->fallbacks++;
            /* Reserved ring slot (if any) is left un-published —
             * tail isn't advanced, so the slot is not visible to
             * the consumer and will be overwritten by the next push. */
            return 0;
        }
    }
    uint32_t pc_start         = b->pc_start;
    uint32_t write_set        = b->write_set;
    uint32_t num_inst_before  = dsp->num_inst;
    dsp_jit_entry_fn b_entry  = b->entry;   /* captured before run */
    b->entry(dsp);
    uint32_t jit_cycles = dsp->num_inst - num_inst_before;
    s->blocks_executed++;

    bool skip_compare = dsp->jit_skip_diff_compare;

    switch (mode) {
    case MODE_ASYNC:
        /* Publish and return — worker validates later off the APU
         * thread. APU thread continues with the next block
         * immediately. Mark the block as enqueued so any overlapping
         * re-entries while the worker is draining don't publish a
         * duplicate snapshot. */
        slot->block       = b;
        slot->block_entry = b_entry;
        diff_queue_finish_push(q, slot, dsp, pc_start, num_inst_before,
                               jit_cycles, write_set, skip_compare);
        qatomic_set(&b->diff_checked, 1);
        s->diff_ops_checked++;
        break;

    case MODE_DROP:
        /* Ring full; drop this block's validation. q->dropped was
         * already incremented by diff_queue_begin_push. Don't mark
         * the block — a later call can try again once the ring has
         * drained. */
        break;

    case MODE_SYNC:
        /* Sync: run the validator inline on the APU thread. No lock
         * release/re-acquire — the validator operates entirely on
         * the private copy (slot->pre / slot->post), so d->lock
         * stays held but nothing outside dsp_core_t is mutated. We
         * pass q == NULL to diff_validate_slot; it's defined to
         * skip the per-slot stats increments in that case. On
         * divergence the diagnostic from diff_report_failure has
         * already been printed; just abort. */
        memcpy(&slot->post, dsp, sizeof(*dsp));
        slot->pc_start        = pc_start;
        slot->num_inst_before = num_inst_before;
        slot->jit_cycles      = jit_cycles;
        slot->write_set       = write_set;
        slot->skip_compare    = skip_compare;
        slot->block           = b;
        slot->block_entry     = b_entry;
        s->diff_ops_checked++;

        if (!diff_validate_slot(slot, NULL)) {
            fflush(stderr);
            abort();
        }
        /* Sync mode validates inline: on success, the block is
         * proven correct for all future runs. Mark it so the peek
         * check takes the fast path next time. */
        if (b->entry == b_entry) {
            qatomic_set(&b->diff_checked, 1);
        }
        break;
    }

    return jit_cycles;
}

unsigned int dsp_jit_execute_block(dsp_core_t *dsp)
{
    DspJitState *s = (DspJitState *)dsp->jit_state;
    if (!s || dsp->pc >= DSP_PRAM_SIZE) {
        return 0;
    }

    if (g_jit_diff) {
        /* Per-translation validation gate: once a specific
         * translation has been validated (or is already queued for
         * validation), skip the diff path — same code runs every
         * time, validating once proves it. This is what keeps the
         * APU-thread cost bounded: total validations ≈ number of
         * unique block translations, not blocks-executed-per-sec.
         * Without this gate, continuous validation of a short
         * program loop drowns the APU thread in snapshot memcpys
         * and stalls the emulator's main thread (blocked on
         * d->lock via MCPX MMIO). */
        DspJitBlock *b_peek = s->pc_to_block[dsp->pc];
        if (b_peek && b_peek->entry != NULL &&
            b_peek->pc_start == dsp->pc &&
            qatomic_read(&b_peek->diff_checked)) {
            goto normal_path;
        }

        /* Hard cap: once the JIT has successfully enqueued
         * g_jit_diff_max blocks, turn off diff mode for the rest of
         * the session. Useful for CI that wants "validate first N
         * then run free"; usually unset because the per-translation
         * gate above already bounds the work. */
        if (g_jit_diff_max != 0 &&
            s->diff_ops_checked >= g_jit_diff_max) {
            if (s->diff_ops_checked == g_jit_diff_max) {
                fprintf(stderr,
                        "xemu: DSP JIT diff cap reached (%" PRIu64
                        " checks), disabling diff mode\n",
                        g_jit_diff_max);
                s->diff_ops_checked++; /* only print once */
            }
            goto normal_path;
        }
        /* Sampling: if N > 1, only diff-check roughly 1 of every N
         * not-yet-checked block executions. Recommended for
         * day-to-day runs (XEMU_DSP_JIT_DIFF=10 is a good default):
         * combined with the per-translation gate above it spreads
         * the validation bursts out over wall time, keeping APU-
         * thread latency well-bounded under any workload. */
        if (g_jit_diff_sample > 1) {
            s->diff_sample_counter++;
            if ((s->diff_sample_counter % g_jit_diff_sample) != 0) {
                goto normal_path;
            }
        }
        return dsp_jit_execute_block_diff(dsp, s);
    }
normal_path:;

    DspJitBlock *b = s->pc_to_block[dsp->pc];
    if (!b || b->entry == NULL || b->pc_start != dsp->pc) {
        /* Block miss: translate. On failure, caller falls back. */
        b = translate_block(dsp, s, dsp->pc);
        if (!b || !b->entry) {
            s->fallbacks++;
            return 0;
        }
    }

    uint32_t num_inst_before = dsp->num_inst;
    b->entry(dsp);
    s->blocks_executed++;
    return dsp->num_inst - num_inst_before;
}

#endif  /* DSP_JIT_SUPPORTED */
