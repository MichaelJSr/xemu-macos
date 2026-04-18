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

/* TBZ Wt, #bit, #imm14 */
G_GNUC_UNUSED static inline void emit_tbz_w(ArmEmit *e, int rt, int bit, int32_t off_bytes)
{
    int32_t imm14 = off_bytes >> 2;
    assert(imm14 >= -(1 << 13) && imm14 < (1 << 13));
    assert(bit >= 0 && bit < 32);
    emit_u32(e, 0x36000000u | ((uint32_t)(bit & 0x1f) << 19) |
                (((uint32_t)imm14 & 0x3fff) << 5) | (rt & 0x1f));
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
 * Emit calc_ea inline: fast path for modes 0-4 with linear Mn,
 * slow-path BLR otherwise. Leaves the 16-bit address in
 * out_addr_reg (which must be a callee-saved W register x22..x25
 * if the caller needs it preserved across subsequent BLRs).
 *
 * When `want_retour` is true, the retour flag (1 == immediate
 * literal returned in out_addr_reg instead of an address) is written
 * to [SP, #OFF_SP_SCRATCH1] so the caller can branch on it.
 *
 * out_addr_reg MUST NOT be 0, 1, 2, or 3 — those are used as
 * temporaries inside this emitter.
 *
 * Clobbers: w0, w1, w2, x3 (SCRATCH). Preserves: x19-x25 (except
 * out_addr_reg is written).
 */
static void emit_calc_ea_inline(ArmEmit *e, uint32_t ea_mode,
                                int out_addr_reg, bool want_retour)
{
    assert(out_addr_reg != 0 && out_addr_reg != 1 &&
           out_addr_reg != 2 && out_addr_reg != 3);
    uint32_t mode   = (ea_mode >> 3) & 7;
    uint32_t numreg = ea_mode & 7;

    if (mode >= 5) {
        /* Modes 5 (Rn+Nn), 6 (aa / 24-bit immediate, bumps
         * cur_inst_len + instr_cycle), and 7 (-(Rn)) always go
         * through the full helper: they either have cycle/len
         * side effects or are infrequent enough that inlining is
         * not worth the code growth. */
        emit_calc_ea_slow_call(e, ea_mode, out_addr_reg, want_retour);
        return;
    }

    uint32_t *to_slow = NULL;

    /* Modes 0-3 mutate Rn and thus require linear Mn == 0xFFFF.
     * Mode 4 is a pure read — skip the Mn check. */
    if (mode != 4) {
        emit_ldr_w_imm(e, /*rd=*/2, /*rn=*/19, OFF_M(numreg));
        emit_mov_imm32(e, /*rd=*/1, 0xFFFFu);
        emit_cmp_w_reg(e, /*rn=*/2, /*rm=*/1);
        to_slow = e->buf;
        emit_bcond(e, ARM_COND_NE, 0);  /* patched below */
    }

    /* Fast path: load Rn into out_addr_reg, compute Rn' per mode. */
    emit_ldr_w_imm(e, out_addr_reg, /*rn=*/19, OFF_R(numreg));

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
 * Parmove translators
 * --------------------------------------------------------------- */

/* Forward declarations so pm_2 can fall through to pm_3. */
static void emit_parmove_pm3(ArmEmit *e, uint32_t inst, emu_func_t alu);

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
static void emit_parmove_pm0(ArmEmit *e, uint32_t inst, emu_func_t alu)
{
    uint32_t memspace = (inst >> 15) & 1;
    uint32_t numreg   = (inst >> 16) & 1;   /* 0 = A, 1 = B */
    uint32_t value6   = (inst >> 8) & 0x3f;
    int dsp_ab = numreg ? DSP_REG_B : DSP_REG_A;
    int xy0_reg = (memspace == 0) ? DSP_REG_X0 : DSP_REG_Y0;

    /* addr into x22 (retour irrelevant for pm_0; it never uses imm form) */
    emit_calc_ea_inline(e, value6, /*out_addr_reg=*/22, /*want_retour=*/false);

    /* save_accu = A/B (limited, through pm_read_accu24) */
    emit_pm_read_reg(e, dsp_ab, /*value_reg=*/23);

    /* save_xy0 = X0 or Y0 (direct register load) */
    emit_ldr_w_any(e, /*rd=*/24, /*rn=*/19, SCRATCH, OFF_REG(xy0_reg));

    if (alu != NULL) {
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
        emit_blr(e, /*rn=*/1);
    }

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
static void emit_parmove_pm1(ArmEmit *e, uint32_t inst, emu_func_t alu)
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
    emit_calc_ea_inline(e, value6, /*out_addr_reg=*/22, /*want_retour=*/true);

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

    if (alu != NULL) {
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
        emit_blr(e, /*rn=*/1);
    }

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
static void emit_parmove_pm5(ArmEmit *e, uint32_t inst, emu_func_t alu);

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
static void emit_parmove_pm4(ArmEmit *e, uint32_t inst, emu_func_t alu)
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
    emit_parmove_pm5(e, inst, alu);
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
static void emit_parmove_pm8(ArmEmit *e, uint32_t inst, emu_func_t alu)
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
    emit_calc_ea_inline(e, ea1, /*out_addr_reg=*/22, /*want_retour=*/false);
    emit_calc_ea_inline(e, ea2, /*out_addr_reg=*/23, /*want_retour=*/false);

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

    if (alu != NULL) {
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
        emit_blr(e, /*rn=*/1);
    }

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
static void emit_parmove_pm2_2(ArmEmit *e, uint32_t inst, emu_func_t alu)
{
    /* 0010 00ee eeed dddd S,D (reg-reg) */
    int srcreg = (int)((inst >> 13) & 0x1f);
    int dstreg = (int)((inst >> 8)  & 0x1f);

    emit_pm_read_reg(e, srcreg, /*value_reg=*/23);

    if (alu != NULL) {
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
        emit_blr(e, /*rn=*/1);
    }

    /* pm_2_2: interpreter masks with registers_mask[dstreg]
     * (dsp_emu.c.inc line 5398). */
    emit_pm_write_reg(e, dstreg, /*value_reg=*/23, /*mask_to_width=*/true);
}

static void emit_parmove_pm2(ArmEmit *e, uint32_t inst, emu_func_t alu)
{
    if ((inst & 0xffff00u) == 0x200000u) {
        /* NOP parmove — ALU only. */
        if (alu != NULL) {
            emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
            emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
            emit_blr(e, /*rn=*/1);
        }
        return;
    }

    if ((inst & 0xffe000u) == 0x204000u) {
        /* R update — calc_ea with 5-bit ea_mode (mode 0-3 only,
         * since bit 12 = 0 in the mask). Output address is ignored
         * (interpreter passes &dummy). */
        uint32_t ea_mode = (inst >> 8) & 0x1f;
        emit_calc_ea_inline(e, ea_mode, /*out_addr_reg=*/22,
                            /*want_retour=*/false);
        if (alu != NULL) {
            emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
            emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
            emit_blr(e, /*rn=*/1);
        }
        return;
    }

    if ((inst & 0xfc0000u) == 0x200000u) {
        emit_parmove_pm2_2(e, inst, alu);
        return;
    }

    /* Fall-through to pm_3 (literal-to-reg). */
    emit_parmove_pm3(e, inst, alu);
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
static void emit_parmove_pm3(ArmEmit *e, uint32_t inst, emu_func_t alu)
{
    uint32_t dstreg   = (inst >> 16) & 0x1f;
    uint32_t srcvalue = (inst >> 8)  & 0xff;

    /* Interpreter shifts srcvalue left 16 for wide destinations. */
    bool shift_left_16 =
        dstreg == DSP_REG_X0 || dstreg == DSP_REG_X1 ||
        dstreg == DSP_REG_Y0 || dstreg == DSP_REG_Y1 ||
        dstreg == DSP_REG_A  || dstreg == DSP_REG_B;
    uint32_t final_value = shift_left_16 ? (srcvalue << 16) : srcvalue;

    /* ALU first. */
    if (alu != NULL) {
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
        emit_blr(e, /*rn=*/1);
    }

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
static void emit_parmove_pm5(ArmEmit *e, uint32_t inst, emu_func_t alu)
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
                            /*want_retour=*/true);
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
        if (alu != NULL) {
            emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
            emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
            emit_blr(e, /*rn=*/1);
        }

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

        if (alu != NULL) {
            emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
            emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)alu);
            emit_blr(e, /*rn=*/1);
        }

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
static void emit_post_instruction_epilogue(ArmEmit *e, ExitPatchList *exits,
                                           uint32_t expected_next_pc)
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

    /* Step 8: exit-check: interrupt_counter (uint16_t) */
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_COUNTER);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* Step 9: exit-check: loop_rep (uint32_t) */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_LOOP_REP);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* Step 10: exit-check: is_idle (bool, 1 byte) */
    emit_ldrb_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_IS_IDLE);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* Step 10b: exit-check: jit_exit_block_request (self-mod flag).
     * Set by dsp_jit_invalidate when a handler rewrote pram (possibly
     * our own); exit the block so the dispatcher re-translates from
     * the fresh pram. */
    emit_ldrb_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_JIT_EXIT_BLOCK_REQ);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* Step 11: exit-check: PC mismatch (branch taken by handler) */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_PC);
    emit_mov_imm32(e, /*rd=*/1, expected_next_pc);
    emit_cmp_w_reg(e, /*rn=*/0, /*rm=*/1);
    {
        uint32_t *site = e->buf;
        emit_bcond(e, ARM_COND_NE, 0);
        record_exit_patch(exits, site);
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
                              uint32_t *out_write_set)
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
        emit_parmove_pm0(e, inst, effective_alu);
        break;
    case 1:
        /* 0001_ffdf w1mm_mrrr  — x:ea/y:ea + reg-reg dual move */
        emit_parmove_pm1(e, inst, effective_alu);
        break;
    case 2:
        /* 0010_XXXX XXXX_XXXX — pm_2 family (NOP / R-upd / S,D / #xx,R) */
        emit_parmove_pm2(e, inst, effective_alu);
        break;
    case 3:
        /* 001d_dddd iiii_iiii #xx,R */
        emit_parmove_pm3(e, inst, effective_alu);
        break;
    case 4:
        /* 0100_l0ll / 01dd_0ddd — pm_4 family (long-accu l:ea
         * or fall-through to pm_5). */
        emit_parmove_pm4(e, inst, effective_alu);
        break;
    case 5:
    case 6:
    case 7:
        /* 01dd_Xddd w_mm_mrrr — single x:/y: move (pm_5 family) */
        emit_parmove_pm5(e, inst, effective_alu);
        break;
    case 8:  case 9:  case 10: case 11:
    case 12: case 13: case 14: case 15:
        /* 1Xmm_eeff_WrrM_MRRR — dual x:/y: simultaneous move (pm_8).
         * This is the FIR / IIR filter kernel hot path. */
        emit_parmove_pm8(e, inst, effective_alu);
        break;
    default:
        /* No unsupported variants remaining after Phase 4e. */
        return false;
    }

    emit_post_instruction_epilogue(e, exits, expected_next_pc);
    if (out_write_set) {
        *out_write_set |= parmove_write_set(inst);
    }
    return true;
}

static bool emit_instruction(ArmEmit *e, ExitPatchList *exits,
                             uint32_t pc, uint32_t inst, uint32_t inst_len,
                             emu_func_t emu_func, uint32_t expected_next_pc,
                             bool is_terminator,
                             uint32_t *out_write_set)
{
    (void)pc;

    /* 1. dsp->cur_inst = inst */
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

    /* 4. call emu_func(dsp) */
    emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);              /* x0 = dsp */
    emit_mov_imm64(e, /*rd=*/1, (uint64_t)(uintptr_t)emu_func);
    emit_blr(e, /*rn=*/1);

    emit_post_instruction_epilogue(e, exits, expected_next_pc);

    /* Generic emu_* handlers can do anything (movep peripherals,
     * movem to x/y memory, jsr/rts touch the stack, etc.). We don't
     * currently classify individual handlers, so mark the full
     * write-set. Parmove stubs (Phase 4) do precise tracking via
     * parmove_write_set(). */
    if (out_write_set) {
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
                                   &write_set)) {
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

            keep_going = emit_instruction(&e, &exits, pc, inst, inst_len,
                                          emu, expected_next_pc, is_term,
                                          &write_set);

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
}

void dsp_jit_finalize(dsp_core_t *dsp)
{
    DspJitState *s = (DspJitState *)dsp->jit_state;
    if (!s) {
        return;
    }

    /* Drain and stop the validator BEFORE freeing the queue: the
     * worker holds a pointer into s->diff_q->slots. */
    if (s->diff_q) {
        if (g_jit_stats) {
            fprintf(stderr,
                    "xemu: DSP JIT validator (%s core) final: "
                    "enqueued=%" PRIu64 " checked=%" PRIu64
                    " dropped=%" PRIu64 " failures=%" PRIu64 "\n",
                    dsp->is_gp ? "GP" : "EP",
                    s->diff_q->enqueued, s->diff_q->checked,
                    s->diff_q->dropped, s->diff_q->failures);
        }
        diff_queue_destroy(s->diff_q);
        s->diff_q = NULL;
    }

    if (g_jit_stats) {
        fprintf(stderr,
                "xemu: DSP JIT stats (%s core):\n"
                "  blocks_translated = %" PRIu64 "\n"
                "  blocks_executed   = %" PRIu64 "\n"
                "  cache_flushes     = %" PRIu64 "\n"
                "  fallbacks         = %" PRIu64 "\n"
                "  diff_ops_checked  = %" PRIu64 "\n"
                "  code_buf_used     = %zu bytes / %zu bytes\n",
                dsp->is_gp ? "GP" : "EP",
                s->blocks_translated, s->blocks_executed,
                s->cache_flushes, s->fallbacks,
                s->diff_ops_checked,
                (size_t)(s->code_ptr - s->code_buf), s->code_cap);
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
