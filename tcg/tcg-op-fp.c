#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "tcg/tcg-temp-internal.h"
#include "tcg/tcg-op-common.h"
#include "exec/translation-block.h"
#include "exec/plugin-gen.h"
#include "tcg-internal.h"

static void tcg_gen_op1_i32(TCGOpcode opc, TCGv_i32 a1)
{
    tcg_gen_op1(opc, TCG_TYPE_I32, tcgv_i32_arg(a1));
}

void tcg_gen_flcr(TCGv_i32 arg)
{
    tcg_gen_op1_i32(INDEX_op_flcr, arg);
}

void tcg_gen_st80f_f32(TCGv_f32 arg, TCGv_ptr dst)
{
    tcg_gen_op2(INDEX_op_st80f_f32, TCG_TYPE_F32, tcgv_f32_arg(arg), tcgv_ptr_arg(dst));
}

void tcg_gen_st80f_f64(TCGv_f64 arg, TCGv_ptr dst)
{
    tcg_gen_op2(INDEX_op_st80f_f64, TCG_TYPE_F64, tcgv_f64_arg(arg), tcgv_ptr_arg(dst));
}

void tcg_gen_ld80f_f32(TCGv_f32 ret, TCGv_ptr src)
{
    tcg_gen_op2(INDEX_op_ld80f_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_ptr_arg(src));
}

void tcg_gen_ld80f_f64(TCGv_f64 ret, TCGv_ptr src)
{
    tcg_gen_op2(INDEX_op_ld80f_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_ptr_arg(src));
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

void tcg_gen_rint_f32(TCGv_f32 ret, TCGv_f32 arg)
{
    tcg_gen_op2(INDEX_op_rint_f32, TCG_TYPE_F32, tcgv_f32_arg(ret), tcgv_f32_arg(arg));
}

void tcg_gen_rint_f64(TCGv_f64 ret, TCGv_f64 arg)
{
    tcg_gen_op2(INDEX_op_rint_f64, TCG_TYPE_F64, tcgv_f64_arg(ret), tcgv_f64_arg(arg));
}

/*
 * Fused rint + cvt-to-int. If the backend supports the fused opcode we
 * emit a single TCG op (one intermediate FP temp saved, one less IR
 * node to schedule); otherwise we fall back to the standard rint +
 * cvt pair. The AArch64 backend accepts the fused form today and
 * lowers to FRINTI + FCVTZS, the same host instructions the pair path
 * would produce but routed through a single TCG op.
 */
void tcg_gen_rint_cvt_i32_f32(TCGv_i32 ret, TCGv_f32 arg)
{
    if (tcg_op_supported(INDEX_op_rint_cvt_i32_f32, TCG_TYPE_I32, 0)) {
        tcg_gen_op2(INDEX_op_rint_cvt_i32_f32, TCG_TYPE_I32,
                    tcgv_i32_arg(ret), tcgv_f32_arg(arg));
    } else {
        TCGv_f32 tmp = tcg_temp_new_f32();
        tcg_gen_rint_f32(tmp, arg);
        tcg_gen_cvt32f_i32(ret, tmp);
    }
}

void tcg_gen_rint_cvt_i32_f64(TCGv_i32 ret, TCGv_f64 arg)
{
    if (tcg_op_supported(INDEX_op_rint_cvt_i32_f64, TCG_TYPE_I32, 0)) {
        tcg_gen_op2(INDEX_op_rint_cvt_i32_f64, TCG_TYPE_I32,
                    tcgv_i32_arg(ret), tcgv_f64_arg(arg));
    } else {
        TCGv_f64 tmp = tcg_temp_new_f64();
        tcg_gen_rint_f64(tmp, arg);
        tcg_gen_cvt64f_i32(ret, tmp);
    }
}

void tcg_gen_rint_cvt_i64_f32(TCGv_i64 ret, TCGv_f32 arg)
{
    if (tcg_op_supported(INDEX_op_rint_cvt_i64_f32, TCG_TYPE_I64, 0)) {
        tcg_gen_op2(INDEX_op_rint_cvt_i64_f32, TCG_TYPE_I64,
                    tcgv_i64_arg(ret), tcgv_f32_arg(arg));
    } else {
        TCGv_f32 tmp = tcg_temp_new_f32();
        tcg_gen_rint_f32(tmp, arg);
        tcg_gen_cvt32f_i64(ret, tmp);
    }
}

void tcg_gen_rint_cvt_i64_f64(TCGv_i64 ret, TCGv_f64 arg)
{
    if (tcg_op_supported(INDEX_op_rint_cvt_i64_f64, TCG_TYPE_I64, 0)) {
        tcg_gen_op2(INDEX_op_rint_cvt_i64_f64, TCG_TYPE_I64,
                    tcgv_i64_arg(ret), tcgv_f64_arg(arg));
    } else {
        TCGv_f64 tmp = tcg_temp_new_f64();
        tcg_gen_rint_f64(tmp, arg);
        tcg_gen_cvt64f_i64(ret, tmp);
    }
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
