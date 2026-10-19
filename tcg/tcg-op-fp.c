#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "tcg/tcg-temp-internal.h"
#include "tcg/tcg-op-common.h"
#include "exec/translation-block.h"
#include "exec/plugin-gen.h"
#include "tcg-internal.h"
#include "tcg-has.h"

static void tcg_gen_op1_i32(TCGOpcode opc, TCGv_i32 a1)
{
    tcg_gen_op1(opc, TCG_TYPE_I32, tcgv_i32_arg(a1));
}

void tcg_gen_flcr(TCGv_i32 arg)
{
    tcg_gen_op1_i32(INDEX_op_flcr, arg);
}

/*
 * ld80f/st80f access [ptr + ofs]. The backends fold an 8-byte aligned
 * offset below 4 KiB into their loads and stores (aarch64: the LDR/LDRH
 * scaled immediates of the low and high words); any other is added here.
 */
static TCGv_ptr x80_addr(TCGv_ptr ptr, tcg_target_long *ofs)
{
    TCGv_ptr t;

    if (*ofs >= 0 && *ofs < 4096 && !(*ofs & 7)) {
        return ptr;
    }
    t = tcg_temp_new_ptr();
    tcg_gen_addi_ptr(t, ptr, *ofs);
    *ofs = 0;
    return t;
}

void tcg_gen_st80f_f32(TCGv_f32 arg, TCGv_ptr ptr, tcg_target_long ofs)
{
    ptr = x80_addr(ptr, &ofs);
    tcg_gen_op3(INDEX_op_st80f_f32, TCG_TYPE_F32, tcgv_f32_arg(arg),
                tcgv_ptr_arg(ptr), ofs);
}

void tcg_gen_st80f_f64(TCGv_f64 arg, TCGv_ptr ptr, tcg_target_long ofs)
{
    ptr = x80_addr(ptr, &ofs);
    tcg_gen_op3(INDEX_op_st80f_f64, TCG_TYPE_F64, tcgv_f64_arg(arg),
                tcgv_ptr_arg(ptr), ofs);
}

/* The double's 8 bytes at [ptr + ofs]: one STR Dt where the backend has it. */
void tcg_gen_st_f64(TCGv_f64 arg, TCGv_ptr ptr, tcg_target_long ofs)
{
    if (TCG_TARGET_HAS_st_f64) {
        tcg_gen_op3(INDEX_op_st_f64, TCG_TYPE_F64, tcgv_f64_arg(arg),
                    tcgv_ptr_arg(ptr), ofs);
    } else {
        TCGv_i64 t = tcg_temp_new_i64();
        tcg_gen_mov64f_i64(t, arg);
        tcg_gen_st_i64(t, ptr, ofs);
    }
}

/*
 * env[ofs] = arg, done only if the next guest access (qemu_ld/st, which
 * must follow at once) takes its slow path, the one way that access can
 * leave the TB: for state only a mid-block exit's restore reads. Without
 * backend support, an unconditional store.
 */
void tcg_gen_st_f64_slow(TCGv_f64 arg, tcg_target_long ofs)
{
    if (TCG_TARGET_HAS_st_f64_slow) {
        tcg_gen_op2(INDEX_op_st_f64_slow, TCG_TYPE_F64, tcgv_f64_arg(arg), ofs);
    } else {
        tcg_gen_st_f64(arg, tcg_env, ofs);
    }
}

/* The same for an integer value, stored with its own width. */
void tcg_gen_st_i32_slow(TCGv_i32 arg, tcg_target_long ofs)
{
    if (TCG_TARGET_HAS_st_f64_slow) {
        tcg_gen_op2(INDEX_op_st_f64_slow, TCG_TYPE_I32, tcgv_i32_arg(arg), ofs);
    } else {
        tcg_gen_st_i32(arg, tcg_env, ofs);
    }
}

void tcg_gen_st_i64_slow(TCGv_i64 arg, tcg_target_long ofs)
{
    if (TCG_TARGET_HAS_st_f64_slow) {
        tcg_gen_op2(INDEX_op_st_f64_slow, TCG_TYPE_I64, tcgv_i64_arg(arg), ofs);
    } else {
        tcg_gen_st_i64(arg, tcg_env, ofs);
    }
}

void tcg_gen_ld80f_f32(TCGv_f32 ret, TCGv_ptr ptr, tcg_target_long ofs)
{
    ptr = x80_addr(ptr, &ofs);
    tcg_gen_op3(INDEX_op_ld80f_f32, TCG_TYPE_F32, tcgv_f32_arg(ret),
                tcgv_ptr_arg(ptr), ofs);
}

void tcg_gen_ld80f_f64(TCGv_f64 ret, TCGv_ptr ptr, tcg_target_long ofs)
{
    ptr = x80_addr(ptr, &ofs);
    tcg_gen_op3(INDEX_op_ld80f_f64, TCG_TYPE_F64, tcgv_f64_arg(ret),
                tcgv_ptr_arg(ptr), ofs);
}

/*
 * ret = the double at [ptr + ofs_dbl] while the byte at [ptr + ofs_stale]
 * is set, else ld80f of [ptr + ofs_img] (target/i386 x87_wt_dbl). Only
 * where the backend has it (tcg_op_supported), with offsets its loads
 * take as immediates.
 */
void tcg_gen_ld80d_f64(TCGv_f64 ret, TCGv_ptr ptr, tcg_target_long ofs_img,
                       tcg_target_long ofs_stale, tcg_target_long ofs_dbl)
{
    tcg_debug_assert(TCG_TARGET_HAS_ld80d);
    tcg_debug_assert(ofs_img >= 0 && ofs_img < 4096 && !(ofs_img & 7));
    tcg_debug_assert(ofs_stale >= 0 && ofs_stale < 4096);
    tcg_debug_assert(ofs_dbl >= 0 && ofs_dbl < 32768 && !(ofs_dbl & 7));
    tcg_gen_op5(INDEX_op_ld80d_f64, TCG_TYPE_F64, tcgv_f64_arg(ret),
                tcgv_ptr_arg(ptr), ofs_img, ofs_stale, ofs_dbl);
}

void tcg_gen_abs_f32(TCGv_f32 ret, TCGv_f32 src)
{
    tcg_gen_op2(INDEX_op_abs_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(src));
}

void tcg_gen_abs_f64(TCGv_f64 ret, TCGv_f64 src)
{
    tcg_gen_op2(INDEX_op_abs_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(src));
}

void tcg_gen_add_f32(TCGv_f32 ret, TCGv_f32 arg1, TCGv_f32 arg2)
{
    tcg_gen_op3(INDEX_op_add_f32, TCG_TYPE_F32,
                tcgv_f32_arg(ret), tcgv_f32_arg(arg1), tcgv_f32_arg(arg2));
}

void tcg_gen_add_f64(TCGv_f64 ret, TCGv_f64 arg1, TCGv_f64 arg2)
{
    tcg_gen_op3(INDEX_op_add_f64, TCG_TYPE_F64,
                tcgv_f64_arg(ret), tcgv_f64_arg(arg1), tcgv_f64_arg(arg2));
}

void tcg_gen_chs_f32(TCGv_f32 ret, TCGv_f32 src)
{
    tcg_gen_op2(INDEX_op_chs_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(src));
}

void tcg_gen_chs_f64(TCGv_f64 ret, TCGv_f64 src)
{
    tcg_gen_op2(INDEX_op_chs_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(src));
}

void tcg_gen_com_f32(TCGv_i64 ret, TCGv_f32 arg1, TCGv_f32 arg2)
{
    tcg_gen_op4(INDEX_op_com_f32, TCG_TYPE_I64,
                tcgv_i64_arg(ret), tcgv_f32_arg(arg1), tcgv_f32_arg(arg2), 0);
}

void tcg_gen_com_f64(TCGv_i64 ret, TCGv_f64 arg1, TCGv_f64 arg2)
{
    tcg_gen_com_sh_f64(ret, arg1, arg2, 0);
}

/*
 * x86 ZF|PF|CF (0x40/0x04/0x01; unordered sets all three) of arg1 against
 * arg2, shifted left by sh: 0 for EFLAGS, 8 for the x87 C3/C2/C0 in fpus.
 */
void tcg_gen_com_sh_f64(TCGv_i64 ret, TCGv_f64 arg1, TCGv_f64 arg2,
                        unsigned sh)
{
    tcg_debug_assert(sh == 0 || sh == 8);
    tcg_gen_op4(INDEX_op_com_f64, TCG_TYPE_I64,
                tcgv_i64_arg(ret), tcgv_f64_arg(arg1), tcgv_f64_arg(arg2), sh);
}

void tcg_gen_cos_f32(TCGv_f32 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_cos_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_cos_f64(TCGv_f64 ret, TCGv_f64 arg)
{
    tcg_gen_op2(INDEX_op_cos_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(arg));
}

void tcg_gen_cvt32f_f64(TCGv_f64 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_cvt32f_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_cvt32f_i32(TCGv_i32 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_cvt32f_i32, TCG_TYPE_I32, tcgv_i32_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_cvt32f_i64(TCGv_i64 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_cvt32f_i64, TCG_TYPE_I64, tcgv_i64_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_cvt32i_f32(TCGv_f32 ret, TCGv_i32 arg)
{
    tcg_gen_op2(INDEX_op_cvt32i_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_i32_arg(arg));
}

void tcg_gen_cvt32i_f64(TCGv_f64 ret, TCGv_i32 arg)
{
    tcg_gen_op2(INDEX_op_cvt32i_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_i32_arg(arg));
}

void tcg_gen_cvt64f_f32(TCGv_f32 ret, TCGv_f64 arg)
{
    tcg_gen_op2(INDEX_op_cvt64f_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f64_arg(arg));
}

void tcg_gen_cvt64f_i32(TCGv_i32 ret, TCGv_f64 src)
{
    tcg_gen_op2(INDEX_op_cvt64f_i32, TCG_TYPE_I32, tcgv_i32_arg(ret), tcgv_f64_arg(src));
}

void tcg_gen_cvt64f_i64(TCGv_i64 ret, TCGv_f64 src)
{
    tcg_gen_op2(INDEX_op_cvt64f_i64, TCG_TYPE_I64, tcgv_i64_arg(ret), tcgv_f64_arg(src));
}

void tcg_gen_cvt64i_f32(TCGv_f32 ret, TCGv_i64 arg)
{
    tcg_gen_op2(INDEX_op_cvt64i_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_i64_arg(arg));
}

void tcg_gen_cvt64i_f64(TCGv_f64 ret, TCGv_i64 arg)
{
    tcg_gen_op2(INDEX_op_cvt64i_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_i64_arg(arg));
}

void tcg_gen_div_f32(TCGv_f32 ret, TCGv_f32 arg1, TCGv_f32 arg2)
{
    tcg_gen_op3(INDEX_op_div_f32, TCG_TYPE_F32,
                tcgv_f32_arg(ret), tcgv_f32_arg(arg1), tcgv_f32_arg(arg2));
}

void tcg_gen_div_f64(TCGv_f64 ret, TCGv_f64 arg1, TCGv_f64 arg2)
{
    tcg_gen_op3(INDEX_op_div_f64, TCG_TYPE_F64,
                tcgv_f64_arg(ret), tcgv_f64_arg(arg1), tcgv_f64_arg(arg2));
}

void tcg_gen_mov32f_i32(TCGv_i32 ret, TCGv_f32 src)
{
    tcg_gen_op2(INDEX_op_mov32f_i32, TCG_TYPE_I32, tcgv_i32_arg(ret), tcgv_f32_arg(src));
}

void tcg_gen_mov32i_f32(TCGv_f32 ret, TCGv_i32 arg)
{
    tcg_gen_op2(INDEX_op_mov32i_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_i32_arg(arg));
}

void tcg_gen_mov64f_i64(TCGv_i64 ret, TCGv_f64 src)
{
    tcg_gen_op2(INDEX_op_mov64f_i64, TCG_TYPE_I64, tcgv_i64_arg(ret), tcgv_f64_arg(src));
}

void tcg_gen_mov64i_f64(TCGv_f64 ret, TCGv_i64 arg)
{
    tcg_gen_op2(INDEX_op_mov64i_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_i64_arg(arg));
}

/*
 * ret = cond(c1, c2) ? v1 : v2 on doubles, compared in type (one FCSEL
 * after the compare on aarch64). Elsewhere the doubles go through GPRs.
 */
static void tcg_gen_movcond_f64_int(TCGCond cond, TCGType type, TCGv_f64 ret,
                                    TCGArg c1, TCGArg c2,
                                    TCGv_f64 v1, TCGv_f64 v2)
{
    if (TCG_TARGET_HAS_movcond_f64) {
        tcg_gen_op6(INDEX_op_movcond_f64, type, tcgv_f64_arg(ret), c1, c2,
                    tcgv_f64_arg(v1), tcgv_f64_arg(v2), cond);
    } else {
        TCGv_i64 a = tcg_temp_ebb_new_i64();
        TCGv_i64 b = tcg_temp_ebb_new_i64();

        tcg_gen_mov64f_i64(a, v1);
        tcg_gen_mov64f_i64(b, v2);
        if (type == TCG_TYPE_I32) {
            TCGv_i32 t = tcg_temp_ebb_new_i32();
            TCGv_i64 t64 = tcg_temp_ebb_new_i64();

            tcg_gen_op4(INDEX_op_setcond, TCG_TYPE_I32, tcgv_i32_arg(t),
                        c1, c2, cond);
            tcg_gen_extu_i32_i64(t64, t);
            tcg_gen_movcond_i64(TCG_COND_NE, a, t64, tcg_constant_i64(0),
                                a, b);
            tcg_temp_free_i32(t);
            tcg_temp_free_i64(t64);
        } else {
            tcg_gen_op6(INDEX_op_movcond, TCG_TYPE_I64, tcgv_i64_arg(a),
                        c1, c2, tcgv_i64_arg(a), tcgv_i64_arg(b), cond);
        }
        tcg_gen_mov64i_f64(ret, a);
        tcg_temp_free_i64(a);
        tcg_temp_free_i64(b);
    }
}

/*
 * ALWAYS and NEVER are plain moves. Their compare operands may be NULL (a
 * CCPrepare for a constant condition has no reg), so decide them before
 * anything converts c1 and c2 to args: a debug build asserts on NULL.
 */
static bool tcg_gen_movcond_f64_const(TCGCond cond, TCGv_f64 ret,
                                      TCGv_f64 v1, TCGv_f64 v2)
{
    if (cond == TCG_COND_ALWAYS || cond == TCG_COND_NEVER) {
        tcg_gen_mov_f64(ret, cond == TCG_COND_ALWAYS ? v1 : v2);
        return true;
    }
    return false;
}

void tcg_gen_movcond_i32_f64(TCGCond cond, TCGv_f64 ret, TCGv_i32 c1,
                             TCGv_i32 c2, TCGv_f64 v1, TCGv_f64 v2)
{
    if (!tcg_gen_movcond_f64_const(cond, ret, v1, v2)) {
        tcg_gen_movcond_f64_int(cond, TCG_TYPE_I32, ret, tcgv_i32_arg(c1),
                                tcgv_i32_arg(c2), v1, v2);
    }
}

void tcg_gen_movcond_i64_f64(TCGCond cond, TCGv_f64 ret, TCGv_i64 c1,
                             TCGv_i64 c2, TCGv_f64 v1, TCGv_f64 v2)
{
    if (!tcg_gen_movcond_f64_const(cond, ret, v1, v2)) {
        tcg_gen_movcond_f64_int(cond, TCG_TYPE_I64, ret, tcgv_i64_arg(c1),
                                tcgv_i64_arg(c2), v1, v2);
    }
}

void tcg_gen_mov_f32(TCGv_f32 ret, TCGv_f32 src)
{
    tcg_gen_op2(INDEX_op_mov_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(src));
}

void tcg_gen_mov_f64(TCGv_f64 ret, TCGv_f64 src)
{
    tcg_gen_op2(INDEX_op_mov_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(src));
}

void tcg_gen_mul_f32(TCGv_f32 ret, TCGv_f32 arg1, TCGv_f32 arg2)
{
    tcg_gen_op3(INDEX_op_mul_f32, TCG_TYPE_F32,
                tcgv_f32_arg(ret), tcgv_f32_arg(arg1), tcgv_f32_arg(arg2));
}

void tcg_gen_mul_f64(TCGv_f64 ret, TCGv_f64 arg1, TCGv_f64 arg2)
{
    tcg_gen_op3(INDEX_op_mul_f64, TCG_TYPE_F64,
                tcgv_f64_arg(ret), tcgv_f64_arg(arg1), tcgv_f64_arg(arg2));
}

/* Round to an integral value under the host rounding mode (rint). */
void tcg_gen_rint_f64(TCGv_f64 ret, TCGv_f64 arg)
{
    tcg_debug_assert(TCG_TARGET_HAS_rint_f64);
    tcg_gen_op2(INDEX_op_rint_f64, TCG_TYPE_F64, tcgv_f64_arg(ret),
                tcgv_f64_arg(arg));
}

/*
 * xemu: arg, except that a signalling NaN comes back quiet (quiet bit set,
 * sign and payload kept): what an x87 m64fp load reads back after its
 * double has been through floatx80 (i386 x87_wt_canon). aarch64: FMAX Dd,
 * Dn, Dn, exact only under FPCR.DN = FZ = 0 (and AH = 0), the vCPU's FPCR
 * outside the SSE helpers' bracket, which the m32fp loads' FCVT already
 * relies on. Hosts without it quiet the integer bits.
 */
void tcg_gen_quiet_f64(TCGv_f64 ret, TCGv_f64 arg)
{
    tcg_debug_assert(TCG_TARGET_HAS_quiet_f64);
    tcg_gen_op2(INDEX_op_quiet_f64, TCG_TYPE_F64, tcgv_f64_arg(ret),
                tcgv_f64_arg(arg));
}

void tcg_gen_sin_f32(TCGv_f32 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_sin_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_sin_f64(TCGv_f64 ret, TCGv_f64 arg)
{
    tcg_gen_op2(INDEX_op_sin_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(arg));
}

void tcg_gen_sqrt_f32(TCGv_f32 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_sqrt_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_sqrt_f64(TCGv_f64 ret, TCGv_f64 arg)
{
    tcg_gen_op2(INDEX_op_sqrt_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(arg));
}

void tcg_gen_sub_f32(TCGv_f32 ret, TCGv_f32 arg1, TCGv_f32 arg2)
{
    tcg_gen_op3(INDEX_op_sub_f32, TCG_TYPE_F32,
                tcgv_f32_arg(ret), tcgv_f32_arg(arg1), tcgv_f32_arg(arg2));
}

void tcg_gen_sub_f64(TCGv_f64 ret, TCGv_f64 arg1, TCGv_f64 arg2)
{
    tcg_gen_op3(INDEX_op_sub_f64, TCG_TYPE_F64,
                tcgv_f64_arg(ret), tcgv_f64_arg(arg1), tcgv_f64_arg(arg2));
}

/* x87_*: images at [ptr + ofs], the host x87 unit computing (x86 hosts). */
void tcg_gen_x87_arith(int op, TCGv_ptr a, tcg_target_long aofs,
                       TCGv_ptr b, tcg_target_long bofs)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op5(INDEX_op_x87_arith, TCG_TYPE_I64, tcgv_ptr_arg(a),
                tcgv_ptr_arg(b), aofs, bofs, op);
}

void tcg_gen_x87_arith_i32(int op, int type, TCGv_ptr a, tcg_target_long ofs,
                           TCGv_i32 val)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op4(INDEX_op_x87_arith_m, TCG_TYPE_I64, tcgv_ptr_arg(a),
                tcgv_i32_arg(val), ofs, op | type << 3);
}

void tcg_gen_x87_arith_i64(int op, int type, TCGv_ptr a, tcg_target_long ofs,
                           TCGv_i64 val)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op4(INDEX_op_x87_arith_m, TCG_TYPE_I64, tcgv_ptr_arg(a),
                tcgv_i64_arg(val), ofs, op | type << 3);
}

void tcg_gen_x87_unary(int kind, TCGv_ptr a, tcg_target_long ofs)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op3(INDEX_op_x87_unary, TCG_TYPE_I64, tcgv_ptr_arg(a), ofs, kind);
}

void tcg_gen_x87_ld_i32(int type, TCGv_ptr d, tcg_target_long ofs,
                        TCGv_i32 val)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op4(INDEX_op_x87_ld, TCG_TYPE_I64, tcgv_ptr_arg(d),
                tcgv_i32_arg(val), ofs, type);
}

void tcg_gen_x87_ld_i64(int type, TCGv_ptr d, tcg_target_long ofs,
                        TCGv_i64 val)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op4(INDEX_op_x87_ld, TCG_TYPE_I64, tcgv_ptr_arg(d),
                tcgv_i64_arg(val), ofs, type);
}

void tcg_gen_x87_st_i32(TCGv_i32 ret, int type, TCGv_ptr s,
                        tcg_target_long ofs)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_debug_assert(type != TCG_X87_M64F && type != TCG_X87_M64I);
    tcg_gen_op4(INDEX_op_x87_st, TCG_TYPE_I64, tcgv_i32_arg(ret),
                tcgv_ptr_arg(s), ofs, type);
}

void tcg_gen_x87_st_i64(TCGv_i64 ret, int type, TCGv_ptr s,
                        tcg_target_long ofs)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_debug_assert(type == TCG_X87_M64F || type == TCG_X87_M64I);
    tcg_gen_op4(INDEX_op_x87_st, TCG_TYPE_I64, tcgv_i64_arg(ret),
                tcgv_ptr_arg(s), ofs, type);
}

void tcg_gen_x87_cmp(TCGv_i32 ret, TCGv_ptr a, tcg_target_long aofs,
                     TCGv_ptr b, tcg_target_long bofs)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op5(INDEX_op_x87_cmp, TCG_TYPE_I64, tcgv_i32_arg(ret),
                tcgv_ptr_arg(a), tcgv_ptr_arg(b), aofs, bofs);
}

void tcg_gen_x87_cmp_i32(TCGv_i32 ret, int type, TCGv_ptr a,
                         tcg_target_long ofs, TCGv_i32 val)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op5(INDEX_op_x87_cmp_m, TCG_TYPE_I64, tcgv_i32_arg(ret),
                tcgv_ptr_arg(a), tcgv_i32_arg(val), ofs, type);
}

void tcg_gen_x87_cmp_i64(TCGv_i32 ret, int type, TCGv_ptr a,
                         tcg_target_long ofs, TCGv_i64 val)
{
    tcg_debug_assert(TCG_TARGET_HAS_x87);
    tcg_gen_op5(INDEX_op_x87_cmp_m, TCG_TYPE_I64, tcgv_i32_arg(ret),
                tcgv_ptr_arg(a), tcgv_i64_arg(val), ofs, type);
}
