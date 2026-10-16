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
    tcg_gen_op3(INDEX_op_com_f32, TCG_TYPE_I64,
                tcgv_i64_arg(ret), tcgv_f32_arg(arg1), tcgv_f32_arg(arg2));
}

void tcg_gen_com_f64(TCGv_i64 ret, TCGv_f64 arg1, TCGv_f64 arg2)
{
    tcg_gen_op3(INDEX_op_com_f64, TCG_TYPE_I64,
                tcgv_i64_arg(ret), tcgv_f64_arg(arg1), tcgv_f64_arg(arg2));
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
