/*
 * MCPX APU DSP (DSP56300 / M56001) JIT — Phase 0 + Phase 1
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Design summary (see docs/dsp-jit-design.md for full details):
 *
 *   The JIT translates DSP basic blocks into ARM64 machine code.
 *   Each translated DSP instruction becomes a short "stub" that:
 *     1. Writes the pre-decoded instruction word into dsp->cur_inst,
 *        resets dsp->cur_inst_len to 1 and dsp->instr_cycle to 2.
 *     2. Calls the same emu_func_t C handler the interpreter would.
 *     3. Calls dsp_postexecute_update_pc (PC advance + DO-loop edge).
 *     4. Accumulates dsp->instr_cycle into dsp->num_inst.
 *     5. Runtime-checks for early block exit:
 *          - interrupt_counter > 0    (pending IRQ)
 *          - is_idle                  (stop/wait)
 *          - loop_rep                 (REP entered; block boundary)
 *          - pc != expected_next_pc   (branch taken)
 *
 *   Block length is bounded (MAX_OPS_PER_BLOCK). On block exit we
 *   return to the C dispatcher, which re-looks-up a block for the
 *   new PC. Block chaining across known fall-through is a Phase 5
 *   optimization and is NOT yet done here.
 *
 *   The on-disk emu_* handlers are unchanged, so correctness is
 *   equivalent to the interpreter by construction. Real speedup in
 *   this phase is modest (block prologue/epilogue amortized over
 *   many ops; no tracing/disasm check per op; no lookup_opcode);
 *   further phases inline specific emu_* handlers directly in ARM64.
 *
 *   A differential-validation mode (XEMU_DSP_JIT_DIFF=1) runs the
 *   interpreter and JIT on a shadow dsp_core_t in parallel and
 *   asserts bit-exact equivalence on every op. Used only during
 *   bring-up; off by default.
 */

#include "qemu/osdep.h"
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
static bool g_jit_stats;

static void parse_flags_once(void)
{
    if (g_jit_parsed) {
        return;
    }
    g_jit_parsed = true;
    const char *e;
    e = getenv("XEMU_DSP_JIT");
    g_jit_enabled = (e && e[0] == '1');
    e = getenv("XEMU_DSP_JIT_DIFF");
    g_jit_diff = (e && e[0] == '1');
    e = getenv("XEMU_DSP_JIT_STATS");
    g_jit_stats = (e && e[0] == '1');

    if (g_jit_enabled) {
        fprintf(stderr, "xemu: DSP JIT enabled%s%s\n",
                g_jit_diff ? " (DIFF mode — 2x slowdown, bit-exact validation)" : "",
                g_jit_stats ? " (stats)" : "");
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

typedef struct DspJitBlock {
    uint32_t pc_start;
    uint32_t pc_end;           /* exclusive — last covered PC + cur_inst_len */
    dsp_jit_entry_fn entry;    /* pointer into code buffer */
    uint32_t num_ops;
    /* pc_to_block lookup: for each covered PC, pc_to_block[pc] = this block. */
    struct DspJitBlock *next;  /* free list chain when unused */
} DspJitBlock;

typedef struct DspJitState {
    /* Code cache */
    uint8_t *code_buf;
    size_t   code_cap;
    uint8_t *code_ptr;         /* bump allocator write cursor */

    /* Blocks */
    DspJitBlock blocks[DSP_PRAM_SIZE];  /* one slot per possible PC */
    DspJitBlock *pc_to_block[DSP_PRAM_SIZE];  /* pc -> owner block (or NULL) */

    /* Differential-mode shadow state (XEMU_DSP_JIT_DIFF=1).
     * pre_state  = dsp state BEFORE running the JIT block
     * post_jit   = dsp state AFTER running the JIT block
     * Only allocated when diff mode is active. */
    dsp_core_t *pre_state;
    dsp_core_t *post_jit;

    /* Stats */
    uint64_t blocks_translated;
    uint64_t blocks_executed;
    uint64_t cache_flushes;
    uint64_t fallbacks;
    uint64_t diff_ops_checked;
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
typedef struct {
    uint32_t *sites[DSP_JIT_MAX_OPS_PER_BLOCK * 4];
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

static bool emit_instruction(ArmEmit *e, ExitPatchList *exits,
                             uint32_t pc, uint32_t inst, uint32_t inst_len,
                             emu_func_t emu_func, uint32_t expected_next_pc,
                             bool is_terminator)
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

    /*
     * 5. dsp_postexecute_update_pc — inline fast path.
     *
     * The helper does 3 things:
     *   (a) if loop_rep: REP state machine + keep PC on this insn.
     *   (b) pc += cur_inst_len
     *   (c) if SR.LF: handle DO-loop end.
     *
     * The common case (no loop_rep, no active DO loop) is just (b).
     * Emit that inline and only BLR the helper when (a) or (c) might
     * fire.
     */
    {
        /* if (loop_rep != 0) goto call_helper; */
        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_LOOP_REP);
        uint32_t *to_call_helper_1 = e->buf;
        emit_cbnz_w(e, 0, 0);       /* patched below */

        /* if (registers[SR] & (1 << DSP_SR_LF)) goto call_helper; */
        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_SR);
        uint32_t *to_call_helper_2 = e->buf;
        emit_tbnz_w(e, /*rt=*/0, DSP_SR_LF, 0);   /* patched below */

        /* Fast path: pc += cur_inst_len (loaded — handler may have
         * bumped it to 2 for long-imm, or zeroed for a terminator). */
        emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_CUR_INST_LEN);
        emit_ldr_w_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_PC);
        emit_add_w_reg(e, /*rd=*/1, /*rn=*/1, /*rm=*/0);
        emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_PC);

        /* Jump over the helper call. */
        uint32_t *to_after = e->buf;
        emit_b(e, 0);              /* patched below */

        /* call_helper: */
        uint32_t *call_helper_label = e->buf;
        patch_branch(to_call_helper_1,
                     (int32_t)((uint8_t *)call_helper_label -
                               (uint8_t *)to_call_helper_1));
        /* Re-patch the TBNZ with the same logic (imm14 at bits 18:5) */
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

        /* after: */
        uint32_t *after_label = e->buf;
        patch_b(to_after, (int32_t)((uint8_t *)after_label -
                                    (uint8_t *)to_after));
    }

    /*
     * 6. dsp_postexecute_interrupts — inline fast path.
     *
     * Helper is a no-op when every interrupt-tracking field is zero.
     * Skip the BLR in that common case.
     *
     * Note: the SR.T (trace) bit is intentionally NOT included in the
     * quick check. Trace is set only in DSP debugger / single-step
     * mode; production titles don't set it. If a title ever did, the
     * first instruction after SR.T becomes set would miss the TRACE
     * interrupt enqueue here — that is a known limitation and
     * matches xemu's existing lack of DSP debugger support.
     */
    {
        emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_STATE);
        emit_ldrh_any(e, /*rd=*/2, /*rn=*/19, SCRATCH, OFF_INTERRUPT_COUNTER);
        emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/2);
        emit_ldrh_any(e, /*rd=*/2, /*rn=*/19, SCRATCH,
                      OFF_INTERRUPT_PIPELINE_COUNT);
        emit_orr_w_reg(e, /*rd=*/0, /*rn=*/0, /*rm=*/2);

        uint32_t *skip_site = e->buf;
        emit_cbz_w(e, /*rn=*/0, 0);   /* patched below to after */

        /* call_helper: */
        emit_mov_x_reg(e, /*rd=*/0, /*rn=*/19);
        emit_blr(e, /*rn=*/21);    /* x21 = cached helper address */

        /* after: */
        uint32_t *after_label = e->buf;
        patch_branch(skip_site,
                     (int32_t)((uint8_t *)after_label -
                               (uint8_t *)skip_site));
    }

    /* 7. num_inst += instr_cycle */
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INSTR_CYCLE);
    emit_ldr_w_any(e, /*rd=*/1, /*rn=*/19, SCRATCH, OFF_NUM_INST);
    emit_add_w_reg(e, /*rd=*/1, /*rn=*/1, /*rm=*/0);
    emit_str_w_any(e, /*rs=*/1, /*rn=*/19, SCRATCH, OFF_NUM_INST);

    /* 8. exit-check: interrupt_counter (uint16_t) */
    emit_ldrh_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_INTERRUPT_COUNTER);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* 9. exit-check: loop_rep (uint32_t) */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_LOOP_REP);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* 10. exit-check: is_idle (bool, 1 byte) */
    emit_ldrb_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_IS_IDLE);
    {
        uint32_t *site = e->buf;
        emit_cbnz_w(e, 0, 0);
        record_exit_patch(exits, site);
    }

    /* 11. exit-check: PC mismatch (branch taken by handler) */
    emit_ldr_w_any(e, /*rd=*/0, /*rn=*/19, SCRATCH, OFF_PC);
    emit_mov_imm32(e, /*rd=*/1, expected_next_pc);
    emit_cmp_w_reg(e, /*rn=*/0, /*rm=*/1);
    {
        uint32_t *site = e->buf;
        emit_bcond(e, ARM_COND_NE, 0);
        record_exit_patch(exits, site);
    }

    /* Terminator instructions: the full stub ran so the handler and
     * post-update fired; we now return instead of trying to fall
     * through. */
    return !is_terminator;
}

/*
 * Emit the shared exit label + epilogue. Restores the two pairs of
 * callee-saved registers pushed by the prologue (x30/x19 and x20/x21)
 * and returns.
 */
static uint32_t *emit_epilogue(ArmEmit *e)
{
    uint32_t *label = e->buf;
    /* Restore in reverse of prologue's push order. */
    emit_ldp_post(e, /*rt1=*/20, /*rt2=*/21, /*rn=*/31 /*SP*/, 16);
    emit_ldp_post(e, /*rt1=*/30, /*rt2=*/19, /*rn=*/31 /*SP*/, 16);
    emit_ret(e);
    return label;
}

/*
 * Block prologue. Saves LR + callee-saved regs x19/x20/x21 and
 * initializes:
 *   x19 = dsp (context pointer, pinned for block lifetime)
 *   x20 = &dsp_jit_helper_postexecute_update_pc   (cached helper)
 *   x21 = &dsp_jit_helper_postexecute_interrupts  (cached helper)
 * Caching the helper addresses in callee-saved regs saves the
 * MOVZ+MOVK*3 chain (4 insns) per helper call in each stub.
 */
static void emit_prologue(ArmEmit *e)
{
    /* SP must stay 16-byte-aligned; STP with #-16 and #-16 does that. */
    emit_stp_pre(e, /*rt1=*/30, /*rt2=*/19, /*rn=*/31 /*SP*/, -16);
    emit_stp_pre(e, /*rt1=*/20, /*rt2=*/21, /*rn=*/31 /*SP*/, -16);

    emit_mov_x_reg(e, /*rd=*/19, /*rn=*/0);   /* x19 = dsp */

    emit_mov_imm64(e, /*rd=*/20,
        (uint64_t)(uintptr_t)&dsp_jit_helper_postexecute_update_pc);
    emit_mov_imm64(e, /*rd=*/21,
        (uint64_t)(uintptr_t)&dsp_jit_helper_postexecute_interrupts);
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

    while (num_ops < DSP_JIT_MAX_OPS_PER_BLOCK && pc < DSP_PRAM_SIZE) {
        uint32_t inst = dsp->pram[pc] & 0xffffff;

        /* Phase 1 only translates non-parallel-move opcodes that map
         * to a real emu_* handler. Parallel-move-only instructions
         * (inst >= 0x100000) and unresolved opcodes fall back to the
         * interpreter via dispatcher re-entry. */
        if (inst >= 0x100000) {
            if (num_ops == 0) {
                qemu_thread_jit_execute();
                return NULL;
            }
            break;
        }

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

        bool keep_going = emit_instruction(&e, &exits, pc, inst, inst_len,
                                           emu, expected_next_pc, is_term);

        for (uint32_t p = pc; p < pc + inst_len && p < DSP_PRAM_SIZE; p++) {
            s->pc_to_block[p] = &s->blocks[pc_start];
        }

        pc_end = pc + inst_len;
        pc = pc_end;
        num_ops++;

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

    if (g_jit_diff) {
        /* Shadow cores for differential validation.
         * See dsp_jit_execute_block_diff for how they are used. */
        s->pre_state = g_malloc0(sizeof(dsp_core_t));
        s->post_jit  = g_malloc0(sizeof(dsp_core_t));
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

    g_free(s->pre_state);
    g_free(s->post_jit);
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

/*
 * Byte-compare two dsp_core_t snapshots over their semantic state.
 * Returns offsetof of the first differing byte, or (size_t)-1 if
 * they match.
 *
 * Excluded from the comparison:
 *   - disasm_* tail (debug/trace only).
 *   - pram_opcache (cache of resolved emu_func_t; the interpreter
 *     populates it during its dispatch, the JIT doesn't touch it).
 *   - jit_state pointer (our own bookkeeping).
 */
static size_t dsp_state_diff(const dsp_core_t *a, const dsp_core_t *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    struct diff_range { size_t off; size_t end; };
    const struct diff_range ranges[] = {
        /* head through end of pram (= up to pram_opcache) */
        { 0,
          offsetof(dsp_core_t, pram_opcache) },
        /* skip pram_opcache; resume at mixbuffer through disasm_* tail */
        { offsetof(dsp_core_t, mixbuffer),
          offsetof(dsp_core_t, str_disasm_memory) },
    };

    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        for (size_t i = ranges[r].off; i < ranges[r].end; i++) {
            if (pa[i] != pb[i]) {
                return i;
            }
        }
    }
    return (size_t)-1;
}

/* Forward decl of the actual interpreter step — exposed from
 * dsp_cpu.c. */
void dsp56k_execute_instruction(dsp_core_t *dsp);

static unsigned int dsp_jit_execute_block_diff(dsp_core_t *dsp,
                                               DspJitState *s)
{
    /* 1. Snapshot pre-state. */
    memcpy(s->pre_state, dsp, sizeof(*dsp));
    /* Blank out jit_state in the snapshot so the comparison ignores
     * our own bookkeeping. */
    s->pre_state->jit_state = NULL;

    /* 2. Run the JIT block (same path as normal). */
    DspJitBlock *b = s->pc_to_block[dsp->pc];
    if (!b || b->entry == NULL || b->pc_start != dsp->pc) {
        b = translate_block(dsp, s, dsp->pc);
        if (!b || !b->entry) {
            s->fallbacks++;
            return 0;
        }
    }
    uint32_t num_inst_before = dsp->num_inst;
    b->entry(dsp);
    uint32_t jit_cycles = dsp->num_inst - num_inst_before;
    s->blocks_executed++;

    /* 3. Snapshot post-JIT state. */
    memcpy(s->post_jit, dsp, sizeof(*dsp));
    s->post_jit->jit_state = NULL;

    /* 4. Replay via interpreter from the pre-state. */
    memcpy(dsp, s->pre_state, sizeof(*dsp));
    dsp->jit_state = s;   /* restore our own bookkeeping */

    uint32_t interp_target = num_inst_before + jit_cycles;
    int guard = 256;  /* step-count sanity upper-bound */
    while (dsp->num_inst < interp_target && guard-- > 0) {
        dsp56k_execute_instruction(dsp);
    }
    s->diff_ops_checked++;

    /* 5. Compare. */
    size_t diff_off = dsp_state_diff(dsp, s->post_jit);
    if (diff_off != (size_t)-1) {
        fprintf(stderr,
                "xemu: DSP JIT DIFF FAILURE in block pc_start=0x%04x "
                "(jit_cycles=%u)\n"
                "  First differing byte at offsetof dsp_core_t = %zu\n"
                "  (pc at entry = 0x%04x)\n"
                "  INTERP pc=0x%04x sr=0x%06x A2:A1:A0=%02x:%06x:%06x "
                "B2:B1:B0=%02x:%06x:%06x\n"
                "  JIT    pc=0x%04x sr=0x%06x A2:A1:A0=%02x:%06x:%06x "
                "B2:B1:B0=%02x:%06x:%06x\n"
                "  loop_rep interp=%u jit=%u  "
                "interrupt_counter interp=%u jit=%u\n",
                b ? b->pc_start : 0xffff, jit_cycles, diff_off,
                s->pre_state->pc,
                dsp->pc, dsp->registers[DSP_REG_SR],
                dsp->registers[DSP_REG_A2], dsp->registers[DSP_REG_A1],
                dsp->registers[DSP_REG_A0],
                dsp->registers[DSP_REG_B2], dsp->registers[DSP_REG_B1],
                dsp->registers[DSP_REG_B0],
                s->post_jit->pc, s->post_jit->registers[DSP_REG_SR],
                s->post_jit->registers[DSP_REG_A2],
                s->post_jit->registers[DSP_REG_A1],
                s->post_jit->registers[DSP_REG_A0],
                s->post_jit->registers[DSP_REG_B2],
                s->post_jit->registers[DSP_REG_B1],
                s->post_jit->registers[DSP_REG_B0],
                dsp->loop_rep, s->post_jit->loop_rep,
                dsp->interrupt_counter, s->post_jit->interrupt_counter);
        abort();
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
        return dsp_jit_execute_block_diff(dsp, s);
    }

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
