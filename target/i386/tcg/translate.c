/*
 *  i386 translation
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"

#include "qemu/host-utils.h"
#include "cpu.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/translation-block.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg-op-gvec.h"
#include "exec/translator.h"
#include "exec/target_page.h"
#include "fpu/softfloat.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "helper-tcg.h"
#include "decode-new.h"

#include "exec/log.h"
#include "exec/icount.h"

static int g_use_hard_fpu;
static int g_use_hard_fpu_inline;
#if defined(XBOX)
static int g_x87_wt; /* write-through inline x87 (x87_wt_*) */
int xemu_sse_vfp; /* inline packed-single add/sub/mul/div (gen_fp_sse_inline) */
#endif

#if defined(XBOX) && (defined(__x86_64__) || defined(__aarch64__))
#include "ui/xemu-settings.h"
#define MAP_GEN_HELPER_SOFT_HARD(name) \
    (g_use_hard_fpu ? gen_helper_##name##__hard : gen_helper_##name##__soft)
#define gen_helper_flds_FT0       MAP_GEN_HELPER_SOFT_HARD(flds_FT0)
#define gen_helper_fldl_FT0       MAP_GEN_HELPER_SOFT_HARD(fldl_FT0)
#define gen_helper_fildl_FT0      MAP_GEN_HELPER_SOFT_HARD(fildl_FT0)
#define gen_helper_flds_ST0       MAP_GEN_HELPER_SOFT_HARD(flds_ST0)
#define gen_helper_fldl_ST0       MAP_GEN_HELPER_SOFT_HARD(fldl_ST0)
#define gen_helper_fildl_ST0      MAP_GEN_HELPER_SOFT_HARD(fildl_ST0)
#define gen_helper_fildll_ST0     MAP_GEN_HELPER_SOFT_HARD(fildll_ST0)
#define gen_helper_fsts_ST0       MAP_GEN_HELPER_SOFT_HARD(fsts_ST0)
#define gen_helper_fstl_ST0       MAP_GEN_HELPER_SOFT_HARD(fstl_ST0)
#define gen_helper_fist_ST0       MAP_GEN_HELPER_SOFT_HARD(fist_ST0)
#define gen_helper_fistl_ST0      MAP_GEN_HELPER_SOFT_HARD(fistl_ST0)
#define gen_helper_fistll_ST0     MAP_GEN_HELPER_SOFT_HARD(fistll_ST0)
#define gen_helper_fistt_ST0      MAP_GEN_HELPER_SOFT_HARD(fistt_ST0)
#define gen_helper_fisttl_ST0     MAP_GEN_HELPER_SOFT_HARD(fisttl_ST0)
#define gen_helper_fisttll_ST0    MAP_GEN_HELPER_SOFT_HARD(fisttll_ST0)
#define gen_helper_fldt_ST0       MAP_GEN_HELPER_SOFT_HARD(fldt_ST0)
#define gen_helper_fstt_ST0       MAP_GEN_HELPER_SOFT_HARD(fstt_ST0)
#define gen_helper_fpush          MAP_GEN_HELPER_SOFT_HARD(fpush)
#define gen_helper_fpop           MAP_GEN_HELPER_SOFT_HARD(fpop)
#define gen_helper_ffree_STN      MAP_GEN_HELPER_SOFT_HARD(ffree_STN)
#define gen_helper_fmov_ST0_FT0   MAP_GEN_HELPER_SOFT_HARD(fmov_ST0_FT0)
#define gen_helper_fmov_FT0_STN   MAP_GEN_HELPER_SOFT_HARD(fmov_FT0_STN)
#define gen_helper_fmov_ST0_STN   MAP_GEN_HELPER_SOFT_HARD(fmov_ST0_STN)
#define gen_helper_fmov_STN_ST0   MAP_GEN_HELPER_SOFT_HARD(fmov_STN_ST0)
#define gen_helper_fxchg_ST0_STN  MAP_GEN_HELPER_SOFT_HARD(fxchg_ST0_STN)
#define gen_helper_fcom_ST0_FT0   MAP_GEN_HELPER_SOFT_HARD(fcom_ST0_FT0)
#define gen_helper_fucom_ST0_FT0  MAP_GEN_HELPER_SOFT_HARD(fucom_ST0_FT0)
#define gen_helper_fcomi_ST0_FT0  MAP_GEN_HELPER_SOFT_HARD(fcomi_ST0_FT0)
#define gen_helper_fucomi_ST0_FT0 MAP_GEN_HELPER_SOFT_HARD(fucomi_ST0_FT0)
#define gen_helper_fadd_ST0_FT0   MAP_GEN_HELPER_SOFT_HARD(fadd_ST0_FT0)
#define gen_helper_fmul_ST0_FT0   MAP_GEN_HELPER_SOFT_HARD(fmul_ST0_FT0)
#define gen_helper_fsub_ST0_FT0   MAP_GEN_HELPER_SOFT_HARD(fsub_ST0_FT0)
#define gen_helper_fsubr_ST0_FT0  MAP_GEN_HELPER_SOFT_HARD(fsubr_ST0_FT0)
#define gen_helper_fdiv_ST0_FT0   MAP_GEN_HELPER_SOFT_HARD(fdiv_ST0_FT0)
#define gen_helper_fdivr_ST0_FT0  MAP_GEN_HELPER_SOFT_HARD(fdivr_ST0_FT0)
#define gen_helper_fadd_STN_ST0   MAP_GEN_HELPER_SOFT_HARD(fadd_STN_ST0)
#define gen_helper_fmul_STN_ST0   MAP_GEN_HELPER_SOFT_HARD(fmul_STN_ST0)
#define gen_helper_fsub_STN_ST0   MAP_GEN_HELPER_SOFT_HARD(fsub_STN_ST0)
#define gen_helper_fsubr_STN_ST0  MAP_GEN_HELPER_SOFT_HARD(fsubr_STN_ST0)
#define gen_helper_fdiv_STN_ST0   MAP_GEN_HELPER_SOFT_HARD(fdiv_STN_ST0)
#define gen_helper_fdivr_STN_ST0  MAP_GEN_HELPER_SOFT_HARD(fdivr_STN_ST0)
#define gen_helper_fchs_ST0       MAP_GEN_HELPER_SOFT_HARD(fchs_ST0)
#define gen_helper_fabs_ST0       MAP_GEN_HELPER_SOFT_HARD(fabs_ST0)
#define gen_helper_fxam_ST0       MAP_GEN_HELPER_SOFT_HARD(fxam_ST0)
#define gen_helper_fld1_ST0       MAP_GEN_HELPER_SOFT_HARD(fld1_ST0)
#define gen_helper_fldl2t_ST0     MAP_GEN_HELPER_SOFT_HARD(fldl2t_ST0)
#define gen_helper_fldl2e_ST0     MAP_GEN_HELPER_SOFT_HARD(fldl2e_ST0)
#define gen_helper_fldpi_ST0      MAP_GEN_HELPER_SOFT_HARD(fldpi_ST0)
#define gen_helper_fldlg2_ST0     MAP_GEN_HELPER_SOFT_HARD(fldlg2_ST0)
#define gen_helper_fldln2_ST0     MAP_GEN_HELPER_SOFT_HARD(fldln2_ST0)
#define gen_helper_fldz_ST0       MAP_GEN_HELPER_SOFT_HARD(fldz_ST0)
#define gen_helper_fldz_FT0       MAP_GEN_HELPER_SOFT_HARD(fldz_FT0)
#define gen_helper_fnstsw         MAP_GEN_HELPER_SOFT_HARD(fnstsw)
#define gen_helper_fldcw          MAP_GEN_HELPER_SOFT_HARD(fldcw)
#define gen_helper_fclex          MAP_GEN_HELPER_SOFT_HARD(fclex)
#define gen_helper_fwait          MAP_GEN_HELPER_SOFT_HARD(fwait)
#define gen_helper_fninit         MAP_GEN_HELPER_SOFT_HARD(fninit)
#define gen_helper_fbld_ST0       MAP_GEN_HELPER_SOFT_HARD(fbld_ST0)
#define gen_helper_fbst_ST0       MAP_GEN_HELPER_SOFT_HARD(fbst_ST0)
#define gen_helper_f2xm1          MAP_GEN_HELPER_SOFT_HARD(f2xm1)
#define gen_helper_fyl2x          MAP_GEN_HELPER_SOFT_HARD(fyl2x)
#define gen_helper_fptan          MAP_GEN_HELPER_SOFT_HARD(fptan)
#define gen_helper_fpatan         MAP_GEN_HELPER_SOFT_HARD(fpatan)
#define gen_helper_fxtract        MAP_GEN_HELPER_SOFT_HARD(fxtract)
#define gen_helper_fprem1         MAP_GEN_HELPER_SOFT_HARD(fprem1)
#define gen_helper_fprem          MAP_GEN_HELPER_SOFT_HARD(fprem)
#define gen_helper_fyl2xp1        MAP_GEN_HELPER_SOFT_HARD(fyl2xp1)
#define gen_helper_fsqrt          MAP_GEN_HELPER_SOFT_HARD(fsqrt)
#define gen_helper_fsincos        MAP_GEN_HELPER_SOFT_HARD(fsincos)
#define gen_helper_frndint        MAP_GEN_HELPER_SOFT_HARD(frndint)
#define gen_helper_fscale         MAP_GEN_HELPER_SOFT_HARD(fscale)
#define gen_helper_fsin           MAP_GEN_HELPER_SOFT_HARD(fsin)
#define gen_helper_fcos           MAP_GEN_HELPER_SOFT_HARD(fcos)
#define gen_helper_fstenv         MAP_GEN_HELPER_SOFT_HARD(fstenv)
#define gen_helper_fldenv         MAP_GEN_HELPER_SOFT_HARD(fldenv)
#define gen_helper_fsave          MAP_GEN_HELPER_SOFT_HARD(fsave)
#define gen_helper_frstor         MAP_GEN_HELPER_SOFT_HARD(frstor)
#endif /* defined(XBOX) && (defined(__x86_64__) || defined(__aarch64__)) */

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

/* Fixes for Windows namespace pollution.  */
#undef IN
#undef OUT

#define PREFIX_REPZ   0x01
#define PREFIX_REPNZ  0x02
#define PREFIX_LOCK   0x04
#define PREFIX_DATA   0x08
#define PREFIX_ADR    0x10
#define PREFIX_VEX    0x20
#define PREFIX_REX    0x40

#ifdef TARGET_X86_64
# define ctztl  ctz64
# define clztl  clz64
#else
# define ctztl  ctz32
# define clztl  clz32
#endif

/* For a switch indexed by MODRM, match all memory operands for a given OP.  */
#define CASE_MODRM_MEM_OP(OP) \
    case (0 << 6) | (OP << 3) | 0 ... (0 << 6) | (OP << 3) | 7: \
    case (1 << 6) | (OP << 3) | 0 ... (1 << 6) | (OP << 3) | 7: \
    case (2 << 6) | (OP << 3) | 0 ... (2 << 6) | (OP << 3) | 7

#define CASE_MODRM_OP(OP) \
    case (0 << 6) | (OP << 3) | 0 ... (0 << 6) | (OP << 3) | 7: \
    case (1 << 6) | (OP << 3) | 0 ... (1 << 6) | (OP << 3) | 7: \
    case (2 << 6) | (OP << 3) | 0 ... (2 << 6) | (OP << 3) | 7: \
    case (3 << 6) | (OP << 3) | 0 ... (3 << 6) | (OP << 3) | 7

//#define MACRO_TEST   1

/* global register indexes */
static TCGv cpu_cc_dst, cpu_cc_src, cpu_cc_src2;
static TCGv cpu_eip;
static TCGv_i32 cpu_cc_op;
static TCGv cpu_regs[CPU_NB_REGS];
static TCGv cpu_seg_base[6];
static TCGv_i64 cpu_bndl[4];
static TCGv_i64 cpu_bndu[4];
static TCGv_i32 fpstt;

typedef struct TCGv_fp_d *TCGv_fp;

typedef struct DisasContext {
    DisasContextBase base;

    target_ulong pc;       /* pc = eip + cs_base */
    target_ulong cs_base;  /* base of CS segment */
    target_ulong pc_save;

    MemOp aflag;
    MemOp dflag;

    int8_t override; /* -1 if no override, else R_CS, R_DS, etc */
    uint8_t prefix;

    bool has_modrm;
    uint8_t modrm;

#ifndef CONFIG_USER_ONLY
    uint8_t cpl;   /* code priv level */
    uint8_t iopl;  /* i/o priv level */
#endif
    uint8_t vex_l;  /* vex vector length */
    uint8_t vex_v;  /* vex vvvv register, without 1's complement.  */
    uint8_t popl_esp_hack; /* for correct popl with esp base handling */
    uint8_t rip_offset; /* only used in x86_64, but left for simplicity */

#ifdef TARGET_X86_64
    uint8_t rex_r;
    uint8_t rex_x;
    uint8_t rex_b;
#endif
    bool vex_w; /* used by AVX even on 32-bit processors */
    bool jmp_opt; /* use direct block chaining for direct jumps */
#if defined(XBOX)
    bool xemu_ret_exit; /* current DISAS_JUMP terminator is a near ret */
    bool xemu_fldcw_exit; /* current DISAS_JUMP terminator follows fldcw */
    /* x87 census / deferral (target/i386/tcg/xemu-x87.h) */
    uint8_t xemu_x87_run;       /* x87 insns since the last cache flush */
    CPUX86State *xemu_env;      /* code-byte peeks (xemu_idle_loop_kind) */
    bool xemu_fip_pend;         /* deferred FIP/FCS update outstanding */
    bool xemu_fdp_pend;         /* deferred FDP/FDS update outstanding */
    TCGv xemu_fip_val;
    TCGv xemu_fdp_val;
    TCGv_i32 xemu_fdp_sel;
    target_ulong xemu_fip_pc;   /* pc of the insn whose FIP is deferred */
    int xemu_fds_seg;           /* segment env->fpds was last stored from */
#endif
    bool cc_op_dirty;

    CCOp cc_op;  /* current CC operation */
    int mem_index; /* select memory access functions */
    uint32_t flags; /* all execution flags */
    int cpuid_features;
    int cpuid_ext_features;
    int cpuid_ext2_features;
    int cpuid_ext3_features;
    int cpuid_7_0_ebx_features;
    int cpuid_7_0_ecx_features;
    int cpuid_7_1_eax_features;
    int cpuid_xsave_features;

    /* TCG local temps */
    TCGv cc_srcT;
    TCGv A0;
    TCGv T0;
    TCGv T1;

    /* TCG local register indexes (only used inside old micro ops) */
    TCGv_i32 tmp2_i32;
    TCGv_i64 tmp1_i64;

    sigjmp_buf jmpbuf;
    TCGOp *prev_insn_start;
    TCGOp *prev_insn_end;

    /* Floating point */
    bool flcr_set;
    int fpstt_delta;
    TCGv_fp fpregs[8];
    TCGv_fp ft0;
#if defined(XBOX)
    /* Write-through x87: canonical register values (x87_wt_*). */
    TCGv_f64 wt_st[8];
    TCGv_f64 wt_ft0;
    /*
     * Deferred stores (x87_wt_pend): wt_st slots whose st80f waits for the
     * next flush, the fpstt_delta that bit 0 of env->xemu_x87_shadow maps
     * to (wt_base, valid while wt_epoch), the pending set at the start of
     * this insn as recorded in insn_start word 2 (wt_insn_pend), and
     * whether a flush inside this insn ended deferral for the rest of it.
     */
    uint8_t wt_pend;
    uint8_t wt_insn_pend;
    bool wt_epoch;
    bool wt_nodefer;
    int wt_base;
    /*
     * wt_st slots whose env image is, or once stored will be, st80f of the
     * cached double (x87_wt_exact). A slot read with ld80f may hold more
     * than a double (fldt, fild m64, fldpi, a softfloat helper's result).
     */
    uint8_t wt_exact;
    /* The call being emitted reads only its arguments (gen_pure_call_*). */
    bool pure_call;
#endif
} DisasContext;

/*
 * Point EIP to next instruction before ending translation.
 * For instructions that can change hflags.
 */
#define DISAS_EOB_NEXT         DISAS_TARGET_0

/*
 * Point EIP to next instruction and set HF_INHIBIT_IRQ if not
 * already set.  For instructions that activate interrupt shadow.
 */
#define DISAS_EOB_INHIBIT_IRQ  DISAS_TARGET_1

/*
 * Return to the main loop; EIP might have already been updated
 * but even in that case do not use lookup_and_goto_ptr().
 */
#define DISAS_EOB_ONLY         DISAS_TARGET_2

/*
 * EIP has already been updated.  For jumps that wish to use
 * lookup_and_goto_ptr()
 */
#define DISAS_JUMP             DISAS_TARGET_3

/*
 * EIP has already been updated.  Use updated value of
 * EFLAGS.TF to determine singlestep trap (SYSCALL/SYSRET).
 */
#define DISAS_EOB_RECHECK_TF   DISAS_TARGET_4

/* The environment in which user-only runs is constrained. */
#ifdef CONFIG_USER_ONLY
#define PE(S)     true
#define CPL(S)    3
#define IOPL(S)   0
#define SVME(S)   false
#define GUEST(S)  false
#else
#define PE(S)     (((S)->flags & HF_PE_MASK) != 0)
#define CPL(S)    ((S)->cpl)
#define IOPL(S)   ((S)->iopl)
#define SVME(S)   (((S)->flags & HF_SVME_MASK) != 0)
#define GUEST(S)  (((S)->flags & HF_GUEST_MASK) != 0)
#endif
#if defined(CONFIG_USER_ONLY) && defined(TARGET_X86_64)
#define VM86(S)   false
#define CODE32(S) true
#define SS32(S)   true
#define ADDSEG(S) false
#else
#define VM86(S)   (((S)->flags & HF_VM_MASK) != 0)
#define CODE32(S) (((S)->flags & HF_CS32_MASK) != 0)
#define SS32(S)   (((S)->flags & HF_SS32_MASK) != 0)
#define ADDSEG(S) (((S)->flags & HF_ADDSEG_MASK) != 0)
#endif
#if !defined(TARGET_X86_64)
#define CODE64(S) false
#elif defined(CONFIG_USER_ONLY)
#define CODE64(S) true
#else
#define CODE64(S) (((S)->flags & HF_CS64_MASK) != 0)
#endif
#if defined(CONFIG_USER_ONLY) || defined(TARGET_X86_64)
#define LMA(S)    (((S)->flags & HF_LMA_MASK) != 0)
#else
#define LMA(S)    false
#endif

#ifdef TARGET_X86_64
#define REX_PREFIX(S)  (((S)->prefix & PREFIX_REX) != 0)
#define REX_W(S)       ((S)->vex_w)
#define REX_R(S)       ((S)->rex_r + 0)
#define REX_X(S)       ((S)->rex_x + 0)
#define REX_B(S)       ((S)->rex_b + 0)
#else
#define REX_PREFIX(S)  false
#define REX_W(S)       false
#define REX_R(S)       0
#define REX_X(S)       0
#define REX_B(S)       0
#endif

/*
 * Many system-only helpers are not reachable for user-only.
 * Define stub generators here, so that we need not either sprinkle
 * ifdefs through the translator, nor provide the helper function.
 */
#define STUB_HELPER(NAME, ...) \
    static inline void gen_helper_##NAME(__VA_ARGS__) \
    { qemu_build_not_reached(); }

#ifdef CONFIG_USER_ONLY
STUB_HELPER(clgi, TCGv_env env)
STUB_HELPER(flush_page, TCGv_env env, TCGv addr)
STUB_HELPER(inb, TCGv ret, TCGv_env env, TCGv_i32 port)
STUB_HELPER(inw, TCGv ret, TCGv_env env, TCGv_i32 port)
STUB_HELPER(inl, TCGv ret, TCGv_env env, TCGv_i32 port)
STUB_HELPER(monitor, TCGv_env env, TCGv addr)
STUB_HELPER(mwait, TCGv_env env, TCGv_i32 pc_ofs)
STUB_HELPER(outb, TCGv_env env, TCGv_i32 port, TCGv_i32 val)
STUB_HELPER(outw, TCGv_env env, TCGv_i32 port, TCGv_i32 val)
STUB_HELPER(outl, TCGv_env env, TCGv_i32 port, TCGv_i32 val)
STUB_HELPER(stgi, TCGv_env env)
STUB_HELPER(svm_check_intercept, TCGv_env env, TCGv_i32 type)
STUB_HELPER(vmload, TCGv_env env, TCGv_i32 aflag)
STUB_HELPER(vmmcall, TCGv_env env)
STUB_HELPER(vmrun, TCGv_env env, TCGv_i32 aflag, TCGv_i32 pc_ofs)
STUB_HELPER(vmsave, TCGv_env env, TCGv_i32 aflag)
STUB_HELPER(write_crN, TCGv_env env, TCGv_i32 reg, TCGv val)
#endif

static void gen_jmp_rel(DisasContext *s, MemOp ot, int diff, int tb_num);
static void gen_jmp_rel_csize(DisasContext *s, int diff, int tb_num);
static void gen_exception_gpf(DisasContext *s);

/* i386 shift ops */
enum {
    OP_ROL,
    OP_ROR,
    OP_RCL,
    OP_RCR,
    OP_SHL,
    OP_SHR,
    OP_SHL1, /* undocumented */
    OP_SAR = 7,
};

enum {
    JCC_O,
    JCC_B,
    JCC_Z,
    JCC_BE,
    JCC_S,
    JCC_P,
    JCC_L,
    JCC_LE,
};

enum {
    USES_CC_DST  = 1,
    USES_CC_SRC  = 2,
    USES_CC_SRC2 = 4,
    USES_CC_SRCT = 8,
};

/* Bit set if the global variable is live after setting CC_OP to X.  */
static const uint8_t cc_op_live_[] = {
    [CC_OP_DYNAMIC] = USES_CC_DST | USES_CC_SRC | USES_CC_SRC2,
    [CC_OP_EFLAGS] = USES_CC_SRC,
    [CC_OP_MULB ... CC_OP_MULQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_ADDB ... CC_OP_ADDQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_ADCB ... CC_OP_ADCQ] = USES_CC_DST | USES_CC_SRC | USES_CC_SRC2,
    [CC_OP_SUBB ... CC_OP_SUBQ] = USES_CC_DST | USES_CC_SRC | USES_CC_SRCT,
    [CC_OP_SBBB ... CC_OP_SBBQ] = USES_CC_DST | USES_CC_SRC | USES_CC_SRC2,
    [CC_OP_LOGICB ... CC_OP_LOGICQ] = USES_CC_DST,
    [CC_OP_INCB ... CC_OP_INCQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_DECB ... CC_OP_DECQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_SHLB ... CC_OP_SHLQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_SARB ... CC_OP_SARQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_BMILGB ... CC_OP_BMILGQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_BLSIB ... CC_OP_BLSIQ] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_ADCX] = USES_CC_DST | USES_CC_SRC,
    [CC_OP_ADOX] = USES_CC_SRC | USES_CC_SRC2,
    [CC_OP_ADCOX] = USES_CC_DST | USES_CC_SRC | USES_CC_SRC2,
    [CC_OP_POPCNT] = USES_CC_DST,
};

static uint8_t cc_op_live(CCOp op)
{
    uint8_t result;
    assert(op >= 0 && op < ARRAY_SIZE(cc_op_live_));

    /*
     * Check that the array is fully populated.  A zero entry would correspond
     * to a fixed value of EFLAGS, which can be obtained with CC_OP_EFLAGS
     * as well.
     */
    result = cc_op_live_[op];
    assert(result);
    return result;
}

static void set_cc_op_1(DisasContext *s, CCOp op, bool dirty)
{
    int dead;

    if (s->cc_op == op) {
        return;
    }

    /* Discard CC computation that will no longer be used.  */
    dead = cc_op_live(s->cc_op) & ~cc_op_live(op);
    if (dead & USES_CC_DST) {
        tcg_gen_discard_tl(cpu_cc_dst);
    }
    if (dead & USES_CC_SRC) {
        tcg_gen_discard_tl(cpu_cc_src);
    }
    if (dead & USES_CC_SRC2) {
        tcg_gen_discard_tl(cpu_cc_src2);
    }
    if (dead & USES_CC_SRCT) {
        tcg_gen_discard_tl(s->cc_srcT);
    }

    if (dirty && s->cc_op == CC_OP_DYNAMIC) {
        tcg_gen_discard_i32(cpu_cc_op);
    }
    s->cc_op_dirty = dirty;
    s->cc_op = op;
}

static void set_cc_op(DisasContext *s, CCOp op)
{
    /*
     * The DYNAMIC setting is translator only, everything else
     * will be spilled later.
     */
    set_cc_op_1(s, op, op != CC_OP_DYNAMIC);
}

static void assume_cc_op(DisasContext *s, CCOp op)
{
    set_cc_op_1(s, op, false);
}

static void gen_update_cc_op(DisasContext *s)
{
    if (s->cc_op_dirty) {
        tcg_gen_movi_i32(cpu_cc_op, s->cc_op);
        s->cc_op_dirty = false;
    }
}

#ifdef TARGET_X86_64

#define NB_OP_SIZES 4

#else /* !TARGET_X86_64 */

#define NB_OP_SIZES 3

#endif /* !TARGET_X86_64 */

#if HOST_BIG_ENDIAN
#define REG_B_OFFSET (sizeof(target_ulong) - 1)
#define REG_H_OFFSET (sizeof(target_ulong) - 2)
#define REG_W_OFFSET (sizeof(target_ulong) - 2)
#define REG_L_OFFSET (sizeof(target_ulong) - 4)
#define REG_LH_OFFSET (sizeof(target_ulong) - 8)
#else
#define REG_B_OFFSET 0
#define REG_H_OFFSET 1
#define REG_W_OFFSET 0
#define REG_L_OFFSET 0
#define REG_LH_OFFSET 4
#endif

/* In instruction encodings for byte register accesses the
 * register number usually indicates "low 8 bits of register N";
 * however there are some special cases where N 4..7 indicates
 * [AH, CH, DH, BH], ie "bits 15..8 of register N-4". Return
 * true for this special case, false otherwise.
 */
static inline bool byte_reg_is_xH(DisasContext *s, int reg)
{
    /* Any time the REX prefix is present, byte registers are uniform */
    if (reg < 4 || REX_PREFIX(s)) {
        return false;
    }
    return true;
}

/* Select the size of a push/pop operation.  */
static inline MemOp mo_pushpop(DisasContext *s, MemOp ot)
{
    if (CODE64(s)) {
        return ot == MO_16 ? MO_16 : MO_64;
    } else {
        return ot;
    }
}

/* Select the size of the stack pointer.  */
static inline MemOp mo_stacksize(DisasContext *s)
{
    return CODE64(s) ? MO_64 : SS32(s) ? MO_32 : MO_16;
}

/* Compute the result of writing t0 to the OT-sized register REG.
 *
 * If DEST is NULL, store the result into the register and return the
 * register's TCGv.
 *
 * If DEST is not NULL, store the result into DEST and return the
 * register's TCGv.
 */
static TCGv gen_op_deposit_reg_v(DisasContext *s, MemOp ot, int reg, TCGv dest, TCGv t0)
{
    switch(ot) {
    case MO_8:
        if (byte_reg_is_xH(s, reg)) {
            dest = dest ? dest : cpu_regs[reg - 4];
            tcg_gen_deposit_tl(dest, cpu_regs[reg - 4], t0, 8, 8);
            return cpu_regs[reg - 4];
        }
        dest = dest ? dest : cpu_regs[reg];
        tcg_gen_deposit_tl(dest, cpu_regs[reg], t0, 0, 8);
        break;
    case MO_16:
        dest = dest ? dest : cpu_regs[reg];
        tcg_gen_deposit_tl(dest, cpu_regs[reg], t0, 0, 16);
        break;
    case MO_32:
        /* For x86_64, this sets the higher half of register to zero.
           For i386, this is equivalent to a mov. */
        dest = dest ? dest : cpu_regs[reg];
        tcg_gen_ext32u_tl(dest, t0);
        break;
#ifdef TARGET_X86_64
    case MO_64:
        dest = dest ? dest : cpu_regs[reg];
        tcg_gen_mov_tl(dest, t0);
        break;
#endif
    default:
        g_assert_not_reached();
    }
    return cpu_regs[reg];
}

static void gen_op_mov_reg_v(DisasContext *s, MemOp ot, int reg, TCGv t0)
{
    gen_op_deposit_reg_v(s, ot, reg, NULL, t0);
}

static inline
void gen_op_mov_v_reg(DisasContext *s, MemOp ot, TCGv t0, int reg)
{
    if (ot == MO_8 && byte_reg_is_xH(s, reg)) {
        tcg_gen_shri_tl(t0, cpu_regs[reg - 4], 8);
    } else {
        tcg_gen_mov_tl(t0, cpu_regs[reg]);
    }
}

static void gen_add_A0_im(DisasContext *s, int val)
{
    tcg_gen_addi_tl(s->A0, s->A0, val);
    if (!CODE64(s)) {
        tcg_gen_ext32u_tl(s->A0, s->A0);
    }
}

static inline void gen_op_jmp_v(DisasContext *s, TCGv dest)
{
    tcg_gen_mov_tl(cpu_eip, dest);
    s->pc_save = -1;
}

static inline void gen_op_add_reg(DisasContext *s, MemOp size, int reg, TCGv val)
{
    /* Using cpu_regs[reg] does not work for xH registers.  */
    assert(size >= MO_16);
    if (size == MO_16) {
        TCGv temp = tcg_temp_new();
        tcg_gen_add_tl(temp, cpu_regs[reg], val);
        gen_op_mov_reg_v(s, size, reg, temp);
    } else {
        tcg_gen_add_tl(cpu_regs[reg], cpu_regs[reg], val);
        tcg_gen_ext_tl(cpu_regs[reg], cpu_regs[reg], size);
    }
}

static inline
void gen_op_add_reg_im(DisasContext *s, MemOp size, int reg, int32_t val)
{
    gen_op_add_reg(s, size, reg, tcg_constant_tl(val));
}

static inline void gen_op_ld_v(DisasContext *s, int idx, TCGv t0, TCGv a0)
{
    tcg_gen_qemu_ld_tl(t0, a0, s->mem_index, idx | MO_LE);
}

static inline void gen_op_st_v(DisasContext *s, int idx, TCGv t0, TCGv a0)
{
    tcg_gen_qemu_st_tl(t0, a0, s->mem_index, idx | MO_LE);
}

static void gen_update_eip_next(DisasContext *s)
{
    assert(s->pc_save != -1);
    if (tb_cflags(s->base.tb) & CF_PCREL) {
        tcg_gen_addi_tl(cpu_eip, cpu_eip, s->pc - s->pc_save);
    } else if (CODE64(s)) {
        tcg_gen_movi_tl(cpu_eip, s->pc);
    } else {
        tcg_gen_movi_tl(cpu_eip, (uint32_t)(s->pc - s->cs_base));
    }
    s->pc_save = s->pc;
}

static void gen_update_eip_cur(DisasContext *s)
{
    assert(s->pc_save != -1);
    if (tb_cflags(s->base.tb) & CF_PCREL) {
        tcg_gen_addi_tl(cpu_eip, cpu_eip, s->base.pc_next - s->pc_save);
    } else if (CODE64(s)) {
        tcg_gen_movi_tl(cpu_eip, s->base.pc_next);
    } else {
        tcg_gen_movi_tl(cpu_eip, (uint32_t)(s->base.pc_next - s->cs_base));
    }
    s->pc_save = s->base.pc_next;
}

static int cur_insn_len(DisasContext *s)
{
    return s->pc - s->base.pc_next;
}

static TCGv_i32 cur_insn_len_i32(DisasContext *s)
{
    return tcg_constant_i32(cur_insn_len(s));
}

static TCGv_i32 eip_next_i32(DisasContext *s)
{
    assert(s->pc_save != -1);
    /*
     * This function has two users: lcall_real (always 16-bit mode), and
     * iret_protected (16, 32, or 64-bit mode).  IRET only uses the value
     * when EFLAGS.NT is set, which is illegal in 64-bit mode, which is
     * why passing a 32-bit value isn't broken.  To avoid using this where
     * we shouldn't, return -1 in 64-bit mode so that execution goes into
     * the weeds quickly.
     */
    if (CODE64(s)) {
        return tcg_constant_i32(-1);
    }
    if (tb_cflags(s->base.tb) & CF_PCREL) {
        TCGv_i32 ret = tcg_temp_new_i32();
        tcg_gen_trunc_tl_i32(ret, cpu_eip);
        tcg_gen_addi_i32(ret, ret, s->pc - s->pc_save);
        return ret;
    } else {
        return tcg_constant_i32(s->pc - s->cs_base);
    }
}

#if defined(XBOX)
void xemu_i386_ras_flush(CPUState *cpu)
{
    CPUX86State *env = cpu_env(cpu);
    memset(env->xemu_retc, 0, sizeof(env->xemu_retc));
    memset(env->xemu_retc_pages, 0, sizeof(env->xemu_retc_pages));
}

/*
 * Per-page counterpart of the flush above, called wherever the jump cache
 * drops one page (INVLPG, ranged TLB flushes; both also drop the page before,
 * for TBs that span into it). The memo is keyed by virtual eip and, like the
 * jump cache, never checks the physical page, so its entries for a page must
 * not outlive a remap of that page.
 */
void xemu_i386_ras_flush_page(CPUState *cpu, vaddr page_addr)
{
    CPUX86State *env = cpu_env(cpu);
    unsigned bit;
    uint64_t mask;
    bool shared = false;

    /* The memo's key is the 32-bit linear pc (cs_base + eip, both uint32_t):
     * truncate first, so the previous-page flush of linear page 0 (passed
     * as a wrapped 64-bit vaddr) still matches page 0xfffff000. */
    page_addr = (uint32_t)page_addr & TARGET_PAGE_MASK;
    bit = xemu_retc_page_bit(page_addr);
    mask = 1ull << (bit % 64);
    if (!(env->xemu_retc_pages[bit / 64] & mask)) {
        return;
    }
    for (int i = 0; i < XEMU_RETC_SIZE; i++) {
        struct XemuRetcEntry *e = &env->xemu_retc[i];
        vaddr page;

        if (!e->tb) {
            continue;
        }
        page = (vaddr)(uint32_t)(e->cs_base + e->eip) & TARGET_PAGE_MASK;
        if (page == page_addr) {
            e->tb = NULL;
        } else if (xemu_retc_page_bit(page) == bit) {
            shared = true;
        }
    }
    if (!shared) {
        env->xemu_retc_pages[bit / 64] &= ~mask;
    }
}
#endif

static TCGv eip_next_tl(DisasContext *s)
{
    assert(s->pc_save != -1);
    if (tb_cflags(s->base.tb) & CF_PCREL) {
        TCGv ret = tcg_temp_new();
        tcg_gen_addi_tl(ret, cpu_eip, s->pc - s->pc_save);
        return ret;
    } else if (CODE64(s)) {
        return tcg_constant_tl(s->pc);
    } else {
        return tcg_constant_tl((uint32_t)(s->pc - s->cs_base));
    }
}

static TCGv eip_cur_tl(DisasContext *s)
{
    assert(s->pc_save != -1);
    if (tb_cflags(s->base.tb) & CF_PCREL) {
        TCGv ret = tcg_temp_new();
        tcg_gen_addi_tl(ret, cpu_eip, s->base.pc_next - s->pc_save);
        return ret;
    } else if (CODE64(s)) {
        return tcg_constant_tl(s->base.pc_next);
    } else {
        return tcg_constant_tl((uint32_t)(s->base.pc_next - s->cs_base));
    }
}

/* Compute SEG:REG into DEST.  SEG is selected from the override segment
   (OVR_SEG) and the default segment (DEF_SEG).  OVR_SEG may be -1 to
   indicate no override.  */
static void gen_lea_v_seg_dest(DisasContext *s, MemOp aflag, TCGv dest, TCGv a0,
                               int def_seg, int ovr_seg)
{
    switch (aflag) {
#ifdef TARGET_X86_64
    case MO_64:
        if (ovr_seg < 0) {
            tcg_gen_mov_tl(dest, a0);
            return;
        }
        break;
#endif
    case MO_32:
        /* 32 bit address */
        if (ovr_seg < 0 && ADDSEG(s)) {
            ovr_seg = def_seg;
        }
        if (ovr_seg < 0) {
            tcg_gen_ext32u_tl(dest, a0);
            return;
        }
        break;
    case MO_16:
        /* 16 bit address */
        tcg_gen_ext16u_tl(dest, a0);
        a0 = dest;
        if (ovr_seg < 0) {
            if (ADDSEG(s)) {
                ovr_seg = def_seg;
            } else {
                return;
            }
        }
        break;
    default:
        g_assert_not_reached();
    }

    if (ovr_seg >= 0) {
        TCGv seg = cpu_seg_base[ovr_seg];

        if (aflag == MO_64) {
            tcg_gen_add_tl(dest, a0, seg);
        } else if (CODE64(s)) {
            tcg_gen_ext32u_tl(dest, a0);
            tcg_gen_add_tl(dest, dest, seg);
        } else {
            tcg_gen_add_tl(dest, a0, seg);
            tcg_gen_ext32u_tl(dest, dest);
        }
    }
}

static void gen_lea_v_seg(DisasContext *s, TCGv a0,
                          int def_seg, int ovr_seg)
{
    gen_lea_v_seg_dest(s, s->aflag, s->A0, a0, def_seg, ovr_seg);
}

static inline void gen_string_movl_A0_ESI(DisasContext *s)
{
    gen_lea_v_seg(s, cpu_regs[R_ESI], R_DS, s->override);
}

static inline void gen_string_movl_A0_EDI(DisasContext *s)
{
    gen_lea_v_seg(s, cpu_regs[R_EDI], R_ES, -1);
}

static TCGv gen_ext_tl(TCGv dst, TCGv src, MemOp size, bool sign)
{
    if (size == MO_TL) {
        return src;
    }
    if (!dst) {
        dst = tcg_temp_new();
    }
    tcg_gen_ext_tl(dst, src, size | (sign ? MO_SIGN : 0));
    return dst;
}

static void gen_op_j_ecx(DisasContext *s, TCGCond cond, TCGLabel *label1)
{
    TCGv tmp = gen_ext_tl(NULL, cpu_regs[R_ECX], s->aflag, false);

    tcg_gen_brcondi_tl(cond, tmp, 0, label1);
}

static inline void gen_op_jz_ecx(DisasContext *s, TCGLabel *label1)
{
    gen_op_j_ecx(s, TCG_COND_EQ, label1);
}

static inline void gen_op_jnz_ecx(DisasContext *s, TCGLabel *label1)
{
    gen_op_j_ecx(s, TCG_COND_NE, label1);
}

static void gen_set_hflag(DisasContext *s, uint32_t mask)
{
    if ((s->flags & mask) == 0) {
        TCGv_i32 t = tcg_temp_new_i32();
        tcg_gen_ld_i32(t, tcg_env, offsetof(CPUX86State, hflags));
        tcg_gen_ori_i32(t, t, mask);
        tcg_gen_st_i32(t, tcg_env, offsetof(CPUX86State, hflags));
        s->flags |= mask;
    }
}

static void gen_reset_hflag(DisasContext *s, uint32_t mask)
{
    if (s->flags & mask) {
        TCGv_i32 t = tcg_temp_new_i32();
        tcg_gen_ld_i32(t, tcg_env, offsetof(CPUX86State, hflags));
        tcg_gen_andi_i32(t, t, ~mask);
        tcg_gen_st_i32(t, tcg_env, offsetof(CPUX86State, hflags));
        s->flags &= ~mask;
    }
}

static void gen_set_eflags(DisasContext *s, target_ulong mask)
{
    TCGv t = tcg_temp_new();

    tcg_gen_ld_tl(t, tcg_env, offsetof(CPUX86State, eflags));
    tcg_gen_ori_tl(t, t, mask);
    tcg_gen_st_tl(t, tcg_env, offsetof(CPUX86State, eflags));
}

static void gen_reset_eflags(DisasContext *s, target_ulong mask)
{
    TCGv t = tcg_temp_new();

    tcg_gen_ld_tl(t, tcg_env, offsetof(CPUX86State, eflags));
    tcg_gen_andi_tl(t, t, ~mask);
    tcg_gen_st_tl(t, tcg_env, offsetof(CPUX86State, eflags));
}

static void gen_helper_in_func(MemOp ot, TCGv v, TCGv_i32 n)
{
    switch (ot) {
    case MO_8:
        gen_helper_inb(v, tcg_env, n);
        break;
    case MO_16:
        gen_helper_inw(v, tcg_env, n);
        break;
    case MO_32:
        gen_helper_inl(v, tcg_env, n);
        break;
    default:
        g_assert_not_reached();
    }
}

static void gen_helper_out_func(MemOp ot, TCGv_i32 v, TCGv_i32 n)
{
    switch (ot) {
    case MO_8:
        gen_helper_outb(tcg_env, v, n);
        break;
    case MO_16:
        gen_helper_outw(tcg_env, v, n);
        break;
    case MO_32:
        gen_helper_outl(tcg_env, v, n);
        break;
    default:
        g_assert_not_reached();
    }
}

/*
 * Validate that access to [port, port + 1<<ot) is allowed.
 * Raise #GP, or VMM exit if not.
 */
static bool gen_check_io(DisasContext *s, MemOp ot, TCGv_i32 port,
                         uint32_t svm_flags)
{
#ifdef CONFIG_USER_ONLY
    /*
     * We do not implement the ioperm(2) syscall, so the TSS check
     * will always fail.
     */
    gen_exception_gpf(s);
    return false;
#else
    if (PE(s) && (CPL(s) > IOPL(s) || VM86(s))) {
        gen_helper_check_io(tcg_env, port, tcg_constant_i32(1 << ot));
    }
    if (GUEST(s)) {
        gen_update_cc_op(s);
        gen_update_eip_cur(s);
        if (s->prefix & (PREFIX_REPZ | PREFIX_REPNZ)) {
            svm_flags |= SVM_IOIO_REP_MASK;
        }
        svm_flags |= 1 << (SVM_IOIO_SIZE_SHIFT + ot);
        gen_helper_svm_check_io(tcg_env, port,
                                tcg_constant_i32(svm_flags),
                                cur_insn_len_i32(s));
    }
    return true;
#endif
}

static void gen_movs(DisasContext *s, MemOp ot, TCGv dshift)
{
    gen_string_movl_A0_ESI(s);
    gen_op_ld_v(s, ot, s->T0, s->A0);
    gen_string_movl_A0_EDI(s);
    gen_op_st_v(s, ot, s->T0, s->A0);

    gen_op_add_reg(s, s->aflag, R_ESI, dshift);
    gen_op_add_reg(s, s->aflag, R_EDI, dshift);
}

/*
 * Bracket a helper that reads only its arguments and cannot fault
 * (cc_compute_all, cc_compute_c: TCG_CALL_NO_RWG_SE, no env): its call
 * skips gen_bb_epilogue's x87 flush, which exists for C code that reads
 * or writes env's x87 state or the FPCR. The deferred stores, the cached
 * doubles and the FIP run stay as they were, as across inline code.
 */
static void gen_pure_call_begin(DisasContext *s)
{
#if defined(XBOX)
    s->pure_call = true;
#endif
}

static void gen_pure_call_end(DisasContext *s)
{
#if defined(XBOX)
    s->pure_call = false;
#endif
}

/* compute all eflags to reg */
static void gen_mov_eflags(DisasContext *s, TCGv reg)
{
    TCGv dst, src1, src2;
    TCGv_i32 cc_op;
    int live, dead;

    if (s->cc_op == CC_OP_EFLAGS) {
        tcg_gen_mov_tl(reg, cpu_cc_src);
        return;
    }

    dst = cpu_cc_dst;
    src1 = cpu_cc_src;
    src2 = cpu_cc_src2;

    /* Take care to not read values that are not live.  */
    live = cc_op_live(s->cc_op) & ~USES_CC_SRCT;
    dead = live ^ (USES_CC_DST | USES_CC_SRC | USES_CC_SRC2);
    if (dead) {
        TCGv zero = tcg_constant_tl(0);
        if (dead & USES_CC_DST) {
            dst = zero;
        }
        if (dead & USES_CC_SRC) {
            src1 = zero;
        }
        if (dead & USES_CC_SRC2) {
            src2 = zero;
        }
    }

    if (s->cc_op != CC_OP_DYNAMIC) {
        cc_op = tcg_constant_i32(s->cc_op);
    } else {
        cc_op = cpu_cc_op;
    }
    gen_pure_call_begin(s);
    gen_helper_cc_compute_all(reg, dst, src1, src2, cc_op);
    gen_pure_call_end(s);
}

/* compute all eflags to cc_src */
static void gen_compute_eflags(DisasContext *s)
{
    gen_mov_eflags(s, cpu_cc_src);
    set_cc_op(s, CC_OP_EFLAGS);
}

typedef struct CCPrepare {
    TCGCond cond;
    TCGv reg;
    TCGv reg2;
    target_ulong imm;
    bool use_reg2;
    bool no_setcond;
} CCPrepare;

static CCPrepare gen_prepare_sign_nz(TCGv src, MemOp size)
{
    if (size == MO_TL) {
        return (CCPrepare) { .cond = TCG_COND_LT, .reg = src };
    } else {
        return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = src,
                             .imm = 1ull << ((8 << size) - 1) };
    }
}

static CCPrepare gen_prepare_val_nz(TCGv src, MemOp size, bool eqz)
{
    if (size == MO_TL) {
        return (CCPrepare) { .cond = eqz ? TCG_COND_EQ : TCG_COND_NE,
                             .reg = src };
    } else {
        return (CCPrepare) { .cond = eqz ? TCG_COND_TSTEQ : TCG_COND_TSTNE,
                             .imm = MAKE_64BIT_MASK(0, 8 << size),
                             .reg = src };
    }
}

/* compute eflags.C, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_c(DisasContext *s, TCGv reg)
{
    MemOp size;

    switch (s->cc_op) {
    case CC_OP_SUBB ... CC_OP_SUBQ:
        /* (DATA_TYPE)CC_SRCT < (DATA_TYPE)CC_SRC */
        size = s->cc_op - CC_OP_SUBB;
        tcg_gen_ext_tl(s->cc_srcT, s->cc_srcT, size);
        tcg_gen_ext_tl(cpu_cc_src, cpu_cc_src, size);
        return (CCPrepare) { .cond = TCG_COND_LTU, .reg = s->cc_srcT,
                             .reg2 = cpu_cc_src, .use_reg2 = true };

    case CC_OP_ADDB ... CC_OP_ADDQ:
        /* (DATA_TYPE)CC_DST < (DATA_TYPE)CC_SRC */
        size = cc_op_size(s->cc_op);
        tcg_gen_ext_tl(cpu_cc_dst, cpu_cc_dst, size);
        tcg_gen_ext_tl(cpu_cc_src, cpu_cc_src, size);
        return (CCPrepare) { .cond = TCG_COND_LTU, .reg = cpu_cc_dst,
                             .reg2 = cpu_cc_src, .use_reg2 = true };

    case CC_OP_LOGICB ... CC_OP_LOGICQ:
    case CC_OP_POPCNT:
        return (CCPrepare) { .cond = TCG_COND_NEVER };

    case CC_OP_INCB ... CC_OP_INCQ:
    case CC_OP_DECB ... CC_OP_DECQ:
        return (CCPrepare) { .cond = TCG_COND_NE, .reg = cpu_cc_src,
                             .no_setcond = true };

    case CC_OP_SHLB ... CC_OP_SHLQ:
        /* (CC_SRC >> (DATA_BITS - 1)) & 1 */
        size = cc_op_size(s->cc_op);
        return gen_prepare_sign_nz(cpu_cc_src, size);

    case CC_OP_MULB ... CC_OP_MULQ:
        return (CCPrepare) { .cond = TCG_COND_NE,
                             .reg = cpu_cc_src };

    case CC_OP_BMILGB ... CC_OP_BMILGQ:
        size = cc_op_size(s->cc_op);
        return gen_prepare_val_nz(cpu_cc_src, size, true);

    case CC_OP_BLSIB ... CC_OP_BLSIQ:
        size = cc_op_size(s->cc_op);
        return gen_prepare_val_nz(cpu_cc_src, size, false);

    case CC_OP_ADCX:
    case CC_OP_ADCOX:
        return (CCPrepare) { .cond = TCG_COND_NE, .reg = cpu_cc_dst,
                             .no_setcond = true };

    case CC_OP_EFLAGS:
    case CC_OP_SARB ... CC_OP_SARQ:
        /* CC_SRC & 1 */
        return (CCPrepare) { .cond = TCG_COND_TSTNE,
                             .reg = cpu_cc_src, .imm = CC_C };

    default:
       /* The need to compute only C from CC_OP_DYNAMIC is important
          in efficiently implementing e.g. INC at the start of a TB.  */
       gen_update_cc_op(s);
       if (!reg) {
           reg = tcg_temp_new();
       }
       gen_pure_call_begin(s);
       gen_helper_cc_compute_c(reg, cpu_cc_dst, cpu_cc_src,
                               cpu_cc_src2, cpu_cc_op);
       gen_pure_call_end(s);
       return (CCPrepare) { .cond = TCG_COND_NE, .reg = reg,
                            .no_setcond = true };
    }
}

/* compute eflags.P, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_p(DisasContext *s, TCGv reg)
{
    switch (s->cc_op) {
    case CC_OP_DYNAMIC:
        gen_compute_eflags(s);
        /* FALLTHRU */
    case CC_OP_EFLAGS:
    case CC_OP_ADCX:
    case CC_OP_ADOX:
    case CC_OP_ADCOX:
        return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = cpu_cc_src,
                             .imm = CC_P };
    case CC_OP_BMILGB ... CC_OP_BMILGQ:
    case CC_OP_BLSIB ... CC_OP_BLSIQ:
    case CC_OP_POPCNT:
        return (CCPrepare) { .cond = TCG_COND_NEVER };
    default:
        /*
         * Even parity of the result's low byte, without the helper: fold
         * the high nibble into the low one, then pick that bit of 0x9669
         * (bit n is set iff n has an even number of bits set).
         */
        if (!reg || reg == cpu_cc_dst) {
            reg = tcg_temp_new();
        }
        tcg_gen_shri_tl(reg, cpu_cc_dst, 4);
        tcg_gen_xor_tl(reg, reg, cpu_cc_dst);
        tcg_gen_andi_tl(reg, reg, 0xf);
        tcg_gen_shr_tl(reg, tcg_constant_tl(0x9669), reg);
        return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = reg, .imm = 1 };
    }
}

/* compute eflags.S, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_s(DisasContext *s, TCGv reg)
{
    switch (s->cc_op) {
    case CC_OP_DYNAMIC:
        gen_compute_eflags(s);
        /* FALLTHRU */
    case CC_OP_EFLAGS:
    case CC_OP_ADCX:
    case CC_OP_ADOX:
    case CC_OP_ADCOX:
        return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = cpu_cc_src,
                             .imm = CC_S };
    case CC_OP_POPCNT:
        return (CCPrepare) { .cond = TCG_COND_NEVER };
    default:
        return gen_prepare_sign_nz(cpu_cc_dst, cc_op_size(s->cc_op));
    }
}

/* compute eflags.O, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_o(DisasContext *s, TCGv reg)
{
    switch (s->cc_op) {
    case CC_OP_ADOX:
    case CC_OP_ADCOX:
        return (CCPrepare) { .cond = TCG_COND_NE, .reg = cpu_cc_src2,
                             .no_setcond = true };
    case CC_OP_LOGICB ... CC_OP_LOGICQ:
    case CC_OP_POPCNT:
        return (CCPrepare) { .cond = TCG_COND_NEVER };
    case CC_OP_MULB ... CC_OP_MULQ:
        return (CCPrepare) { .cond = TCG_COND_NE, .reg = cpu_cc_src };
    default:
        gen_compute_eflags(s);
        return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = cpu_cc_src,
                             .imm = CC_O };
    }
}

/* compute eflags.Z, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_z(DisasContext *s, TCGv reg)
{
    switch (s->cc_op) {
    case CC_OP_EFLAGS:
    case CC_OP_ADCX:
    case CC_OP_ADOX:
    case CC_OP_ADCOX:
        return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = cpu_cc_src,
                             .imm = CC_Z };
    case CC_OP_DYNAMIC:
        gen_update_cc_op(s);
        if (!reg) {
            reg = tcg_temp_new();
        }
        gen_helper_cc_compute_nz(reg, cpu_cc_dst, cpu_cc_src, cpu_cc_op);
        return (CCPrepare) { .cond = TCG_COND_EQ, .reg = reg, .imm = 0 };
    case CC_OP_POPCNT:
        return (CCPrepare) { .cond = TCG_COND_EQ, .reg = cpu_cc_dst };
    default:
        {
            MemOp size = cc_op_size(s->cc_op);
            return gen_prepare_val_nz(cpu_cc_dst, size, true);
        }
    }
}

/* return how to compute jump opcode 'b'.  'reg' can be clobbered
 * if needed; it may be used for CCPrepare.reg if that will
 * provide more freedom in the translation of a subsequent setcond. */
static CCPrepare gen_prepare_cc(DisasContext *s, int b, TCGv reg)
{
    int inv, jcc_op, cond;
    MemOp size;
    CCPrepare cc;

    inv = b & 1;
    jcc_op = (b >> 1) & 7;

    switch (s->cc_op) {
    case CC_OP_SUBB ... CC_OP_SUBQ:
        /* We optimize relational operators for the cmp/jcc case.  */
        size = cc_op_size(s->cc_op);
        switch (jcc_op) {
        case JCC_BE:
            tcg_gen_ext_tl(s->cc_srcT, s->cc_srcT, size);
            tcg_gen_ext_tl(cpu_cc_src, cpu_cc_src, size);
            cc = (CCPrepare) { .cond = TCG_COND_LEU, .reg = s->cc_srcT,
                               .reg2 = cpu_cc_src, .use_reg2 = true };
            break;
        case JCC_L:
            cond = TCG_COND_LT;
            goto fast_jcc_l;
        case JCC_LE:
            cond = TCG_COND_LE;
        fast_jcc_l:
            tcg_gen_ext_tl(s->cc_srcT, s->cc_srcT, size | MO_SIGN);
            tcg_gen_ext_tl(cpu_cc_src, cpu_cc_src, size | MO_SIGN);
            cc = (CCPrepare) { .cond = cond, .reg = s->cc_srcT,
                               .reg2 = cpu_cc_src, .use_reg2 = true };
            break;

        default:
            goto slow_jcc;
        }
        break;

    case CC_OP_LOGICB ... CC_OP_LOGICQ:
        /* Mostly used for test+jump */
        size = s->cc_op - CC_OP_LOGICB;
        switch (jcc_op) {
        case JCC_BE:
            /* CF = 0, becomes jz/je */
            jcc_op = JCC_Z;
            goto slow_jcc;
        case JCC_L:
            /* OF = 0, becomes js/jns */
            jcc_op = JCC_S;
            goto slow_jcc;
        case JCC_LE:
            /* SF or ZF, becomes signed <= 0 */
            tcg_gen_ext_tl(cpu_cc_dst, cpu_cc_dst, size | MO_SIGN);
            cc = (CCPrepare) { .cond = TCG_COND_LE, .reg = cpu_cc_dst };
            break;
        default:
            goto slow_jcc;
        }
        break;

    default:
    slow_jcc:
        /* This actually generates good code for JC, JZ and JS.  */
        switch (jcc_op) {
        case JCC_O:
            cc = gen_prepare_eflags_o(s, reg);
            break;
        case JCC_B:
            cc = gen_prepare_eflags_c(s, reg);
            break;
        case JCC_Z:
            cc = gen_prepare_eflags_z(s, reg);
            break;
        case JCC_BE:
            gen_compute_eflags(s);
            cc = (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = cpu_cc_src,
                               .imm = CC_Z | CC_C };
            break;
        case JCC_S:
            cc = gen_prepare_eflags_s(s, reg);
            break;
        case JCC_P:
            cc = gen_prepare_eflags_p(s, reg);
            break;
        case JCC_L:
            gen_compute_eflags(s);
            if (!reg || reg == cpu_cc_src) {
                reg = tcg_temp_new();
            }
            tcg_gen_addi_tl(reg, cpu_cc_src, CC_O - CC_S);
            cc = (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = reg,
                               .imm = CC_O };
            break;
        default:
        case JCC_LE:
            gen_compute_eflags(s);
            if (!reg || reg == cpu_cc_src) {
                reg = tcg_temp_new();
            }
            tcg_gen_addi_tl(reg, cpu_cc_src, CC_O - CC_S);
            cc = (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = reg,
                               .imm = CC_O | CC_Z };
            break;
        }
        break;
    }

    if (inv) {
        cc.cond = tcg_invert_cond(cc.cond);
    }
    return cc;
}

static void gen_neg_setcc(DisasContext *s, int b, TCGv reg)
{
    CCPrepare cc = gen_prepare_cc(s, b, reg);

    if (cc.no_setcond) {
        if (cc.cond == TCG_COND_EQ) {
            tcg_gen_addi_tl(reg, cc.reg, -1);
        } else {
            tcg_gen_neg_tl(reg, cc.reg);
        }
        return;
    }

    if (cc.use_reg2) {
        tcg_gen_negsetcond_tl(cc.cond, reg, cc.reg, cc.reg2);
    } else {
        tcg_gen_negsetcondi_tl(cc.cond, reg, cc.reg, cc.imm);
    }
}

static void gen_setcc(DisasContext *s, int b, TCGv reg)
{
    CCPrepare cc = gen_prepare_cc(s, b, reg);

    if (cc.no_setcond) {
        if (cc.cond == TCG_COND_EQ) {
            tcg_gen_xori_tl(reg, cc.reg, 1);
        } else {
            tcg_gen_mov_tl(reg, cc.reg);
        }
        return;
    }

    if (cc.use_reg2) {
        tcg_gen_setcond_tl(cc.cond, reg, cc.reg, cc.reg2);
    } else {
        tcg_gen_setcondi_tl(cc.cond, reg, cc.reg, cc.imm);
    }
}

static inline void gen_compute_eflags_c(DisasContext *s, TCGv reg)
{
    gen_setcc(s, JCC_B << 1, reg);
}

/* generate a conditional jump to label 'l1' according to jump opcode
   value 'b'. In the fast case, T0 is guaranteed not to be used. */
static inline void gen_jcc_noeob(DisasContext *s, int b, TCGLabel *l1)
{
    CCPrepare cc = gen_prepare_cc(s, b, NULL);

    if (cc.use_reg2) {
        tcg_gen_brcond_tl(cc.cond, cc.reg, cc.reg2, l1);
    } else {
        tcg_gen_brcondi_tl(cc.cond, cc.reg, cc.imm, l1);
    }
}

/* Generate a conditional jump to label 'l1' according to jump opcode
   value 'b'. In the fast case, T0 is guaranteed not to be used.
   One or both of the branches will call gen_jmp_rel, so ensure
   cc_op is clean.  */
static inline void gen_jcc(DisasContext *s, int b, TCGLabel *l1)
{
    CCPrepare cc = gen_prepare_cc(s, b, NULL);

    /*
     * Note that this must be _after_ gen_prepare_cc, because it can change
     * the cc_op to CC_OP_EFLAGS (because it's CC_OP_DYNAMIC or because
     * it's cheaper to just compute the flags)!
     */
    gen_update_cc_op(s);
    if (cc.use_reg2) {
        tcg_gen_brcond_tl(cc.cond, cc.reg, cc.reg2, l1);
    } else {
        tcg_gen_brcondi_tl(cc.cond, cc.reg, cc.imm, l1);
    }
}

static void gen_stos(DisasContext *s, MemOp ot, TCGv dshift)
{
    gen_string_movl_A0_EDI(s);
    gen_op_st_v(s, ot, s->T0, s->A0);
    gen_op_add_reg(s, s->aflag, R_EDI, dshift);
}

static void gen_lods(DisasContext *s, MemOp ot, TCGv dshift)
{
    gen_string_movl_A0_ESI(s);
    gen_op_ld_v(s, ot, s->T0, s->A0);
    gen_op_mov_reg_v(s, ot, R_EAX, s->T0);
    gen_op_add_reg(s, s->aflag, R_ESI, dshift);
}

static void gen_scas(DisasContext *s, MemOp ot, TCGv dshift)
{
    gen_string_movl_A0_EDI(s);
    gen_op_ld_v(s, ot, s->T1, s->A0);
    tcg_gen_mov_tl(cpu_cc_src, s->T1);
    tcg_gen_mov_tl(s->cc_srcT, s->T0);
    tcg_gen_sub_tl(cpu_cc_dst, s->T0, s->T1);
    set_cc_op(s, CC_OP_SUBB + ot);

    gen_op_add_reg(s, s->aflag, R_EDI, dshift);
}

static void gen_cmps(DisasContext *s, MemOp ot, TCGv dshift)
{
    gen_string_movl_A0_EDI(s);
    gen_op_ld_v(s, ot, s->T1, s->A0);
    gen_string_movl_A0_ESI(s);
    gen_op_ld_v(s, ot, s->T0, s->A0);
    tcg_gen_mov_tl(cpu_cc_src, s->T1);
    tcg_gen_mov_tl(s->cc_srcT, s->T0);
    tcg_gen_sub_tl(cpu_cc_dst, s->T0, s->T1);
    set_cc_op(s, CC_OP_SUBB + ot);

    gen_op_add_reg(s, s->aflag, R_ESI, dshift);
    gen_op_add_reg(s, s->aflag, R_EDI, dshift);
}

static void gen_bpt_io(DisasContext *s, TCGv_i32 t_port, int ot)
{
    if (s->flags & HF_IOBPT_MASK) {
#ifdef CONFIG_USER_ONLY
        /* user-mode cpu should not be in IOBPT mode */
        g_assert_not_reached();
#else
        TCGv_i32 t_size = tcg_constant_i32(1 << ot);
        TCGv t_next = eip_next_tl(s);
        gen_helper_bpt_io(tcg_env, t_port, t_size, t_next);
#endif /* CONFIG_USER_ONLY */
    }
}

static void gen_ins(DisasContext *s, MemOp ot, TCGv dshift)
{
    TCGv_i32 port = tcg_temp_new_i32();

    gen_string_movl_A0_EDI(s);
    /* Note: we must do this dummy write first to be restartable in
       case of page fault. */
    tcg_gen_movi_tl(s->T0, 0);
    gen_op_st_v(s, ot, s->T0, s->A0);
    tcg_gen_trunc_tl_i32(port, cpu_regs[R_EDX]);
    tcg_gen_andi_i32(port, port, 0xffff);
    gen_helper_in_func(ot, s->T0, port);
    gen_op_st_v(s, ot, s->T0, s->A0);
    gen_op_add_reg(s, s->aflag, R_EDI, dshift);
    gen_bpt_io(s, port, ot);
}

static void gen_outs(DisasContext *s, MemOp ot, TCGv dshift)
{
    TCGv_i32 port = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();

    gen_string_movl_A0_ESI(s);
    gen_op_ld_v(s, ot, s->T0, s->A0);

    tcg_gen_trunc_tl_i32(port, cpu_regs[R_EDX]);
    tcg_gen_andi_i32(port, port, 0xffff);
    tcg_gen_trunc_tl_i32(value, s->T0);
    gen_helper_out_func(ot, port, value);
    gen_op_add_reg(s, s->aflag, R_ESI, dshift);
    gen_bpt_io(s, port, ot);
}

#define REP_MAX 65535

#if defined(XBOX)
/*
 * xemu: shortest REP MOVS/STOS (in elements) handed to helper_xemu_rep_bulk;
 * below it the call costs more host instructions than the loop (heavy-f1:
 * 4, 12 and 32 all save less than 8).
 */
#define REP_BULK_MIN 8
#endif

static void do_gen_rep(DisasContext *s, MemOp ot, TCGv dshift,
                       void (*fn)(DisasContext *s, MemOp ot, TCGv dshift),
                       bool is_repz_nz)
{
    TCGLabel *last = gen_new_label();
    TCGLabel *loop = gen_new_label();
    TCGLabel *done = gen_new_label();

    target_ulong cx_mask = MAKE_64BIT_MASK(0, 8 << s->aflag);
    TCGv cx_next = tcg_temp_new();

    /*
     * Check if we must translate a single iteration only.  Normally, HF_RF_MASK
     * would also limit translation blocks to one instruction, so that gen_eob
     * can reset the flag; here however RF is set throughout the repetition, so
     * we can plow through until CX/ECX/RCX is zero.
     *
     * xemu: icount counts every iteration as an instruction upstream, which
     * sends each one through the TCG main loop; Xbox games memcpy and memset
     * with REP MOVSD/STOSD all the time, so under icount that was the largest
     * single cost of deterministic mode. Loop here instead: the REP counts as
     * one instruction per TB entry (and again every REP_MAX iterations), which
     * is just as deterministic, only coarser in virtual time.
     */
#if defined(XBOX)
    bool can_loop =
        (!(tb_cflags(s->base.tb) & CF_SINGLE_STEP)
	 && !(s->flags & (HF_TF_MASK | HF_INHIBIT_IRQ_MASK)));
#else
    bool can_loop =
        (!(tb_cflags(s->base.tb) & (CF_USE_ICOUNT | CF_SINGLE_STEP))
	 && !(s->flags & (HF_TF_MASK | HF_INHIBIT_IRQ_MASK)));
#endif
    bool had_rf = s->flags & HF_RF_MASK;

    /*
     * Even if EFLAGS.RF was set on entry (such as if we're on the second or
     * later iteration and an exception or interrupt happened), force gen_eob()
     * not to clear the flag.  We do that ourselves after the last iteration.
     */
    s->flags &= ~HF_RF_MASK;

    /*
     * For CMPS/SCAS, the CC_OP after a memory fault could come from either
     * the previous instruction or the string instruction; but because we
     * arrange to keep CC_OP up to date all the time, just mark the whole
     * insn as CC_OP_DYNAMIC.
     *
     * It's not a problem to do this even for instructions that do not
     * modify the flags, so do it unconditionally.
     */
    gen_update_cc_op(s);
    tcg_set_insn_start_param(s->base.insn_start, 1, CC_OP_DYNAMIC);

#if defined(XBOX)
    /*
     * Copy or fill what it can as host memmove/memset first. It stops where
     * the loop below must take over, with ECX/ESI/EDI as the loop would
     * have left them there, and never past one of the loop's TB exits.
     */
    if (can_loop && (fn == gen_movs || fn == gen_stos) &&
        s->aflag == MO_32 && !ADDSEG(s) && s->override < 0) {
        TCGLabel *loop_only = gen_new_label();

        tcg_gen_brcondi_tl(TCG_COND_LTU, cpu_regs[R_ECX], REP_BULK_MIN,
                           loop_only);
        gen_helper_xemu_rep_bulk(tcg_env,
                                 tcg_constant_i32(ot | (fn == gen_stos) << 2 |
                                                  s->mem_index << 3));
        gen_set_label(loop_only);
    }
#endif

    /* Any iteration at all?  */
    tcg_gen_brcondi_tl(TCG_COND_TSTEQ, cpu_regs[R_ECX], cx_mask, done);

    /*
     * From now on we operate on the value of CX/ECX/RCX that will be written
     * back, which is stored in cx_next.  There can be no carry, so we can zero
     * extend here if needed and not do any expensive deposit operations later.
     */
    tcg_gen_subi_tl(cx_next, cpu_regs[R_ECX], 1);
#ifdef TARGET_X86_64
    if (s->aflag == MO_32) {
        tcg_gen_ext32u_tl(cx_next, cx_next);
        cx_mask = ~0;
    }
#endif

    /*
     * The last iteration is handled outside the loop, so that cx_next
     * can never underflow.
     */
    if (can_loop) {
        tcg_gen_brcondi_tl(TCG_COND_TSTEQ, cx_next, cx_mask, last);
    }

    gen_set_label(loop);
    fn(s, ot, dshift);
    tcg_gen_mov_tl(cpu_regs[R_ECX], cx_next);
    gen_update_cc_op(s);

    /* Leave if REP condition fails.  */
    if (is_repz_nz) {
        int nz = (s->prefix & PREFIX_REPNZ) ? 1 : 0;
        gen_jcc_noeob(s, (JCC_Z << 1) | (nz ^ 1), done);
        /* gen_prepare_eflags_z never changes cc_op.  */
	assert(!s->cc_op_dirty);
    }

    if (can_loop) {
        tcg_gen_subi_tl(cx_next, cx_next, 1);
        tcg_gen_brcondi_tl(TCG_COND_TSTNE, cx_next, REP_MAX, loop);
        tcg_gen_brcondi_tl(TCG_COND_TSTEQ, cx_next, cx_mask, last);
    }

    /*
     * Traps or interrupts set RF_MASK if they happen after any iteration
     * but the last.  Set it here before giving the main loop a chance to
     * execute.  (For faults, seg_helper.c sets the flag as usual).
     */
    if (!had_rf) {
        gen_set_eflags(s, RF_MASK);
    }

    /* Go to the main loop but reenter the same instruction.  */
    gen_jmp_rel_csize(s, -cur_insn_len(s), 0);

    if (can_loop) {
        /*
         * The last iteration needs no conditional jump, even if is_repz_nz,
         * because the repeats are ending anyway.
         */
        gen_set_label(last);
        set_cc_op(s, CC_OP_DYNAMIC);
        fn(s, ot, dshift);
        tcg_gen_mov_tl(cpu_regs[R_ECX], cx_next);
        gen_update_cc_op(s);
    }

    /* CX/ECX/RCX is zero, or REPZ/REPNZ broke the repetition.  */
    gen_set_label(done);
    set_cc_op(s, CC_OP_DYNAMIC);
    if (had_rf) {
        gen_reset_eflags(s, RF_MASK);
    }
    gen_jmp_rel_csize(s, 0, 1);
}

static void do_gen_string(DisasContext *s, MemOp ot,
                          void (*fn)(DisasContext *s, MemOp ot, TCGv dshift),
                          bool is_repz_nz)
{
    TCGv dshift = tcg_temp_new();
    tcg_gen_ld32s_tl(dshift, tcg_env, offsetof(CPUX86State, df));
    tcg_gen_shli_tl(dshift, dshift, ot);

    if (s->prefix & (PREFIX_REPZ | PREFIX_REPNZ)) {
        do_gen_rep(s, ot, dshift, fn, is_repz_nz);
    } else {
        fn(s, ot, dshift);
    }
}

static void gen_repz(DisasContext *s, MemOp ot,
                     void (*fn)(DisasContext *s, MemOp ot, TCGv dshift))
{
    do_gen_string(s, ot, fn, false);
}

static void gen_repz_nz(DisasContext *s, MemOp ot,
                        void (*fn)(DisasContext *s, MemOp ot, TCGv dshift))
{
    do_gen_string(s, ot, fn, true);
}

/*
 * An x87 register image as a base and a constant offset, which folds into
 * the loads and stores that use it: ST(i) is one ADD (extended register)
 * on aarch64, env + ((fpstt + i) & 7) * 16 at offset fpregs[0].
 */
typedef struct X87Ptr {
    TCGv_ptr base;
    intptr_t ofs;
} X87Ptr;

/* (fpstt + opreg) & 7; fpstt itself is always 0..7. */
static TCGv_i32 gen_st_index(int opreg)
{
    TCGv_i32 idx;

    if (opreg == 0) {
        return fpstt;
    }
    idx = tcg_temp_new_i32();
    tcg_gen_addi_i32(idx, fpstt, opreg);
    tcg_gen_andi_i32(idx, idx, 7);
    return idx;
}

static X87Ptr gen_stn_ptr(int opreg)
{
    TCGv_ptr base = tcg_temp_new_ptr();
    tcg_gen_add_zext_shl_ptr(base, tcg_env, gen_st_index(opreg), 4);
    return (X87Ptr){ base, offsetof(CPUX86State, fpregs[0].d) };
}

static X87Ptr gen_ft0_ptr(void)
{
    return (X87Ptr){ tcg_env, offsetof(CPUX86State, ft0) };
}

/* env->fptags[(env->fpstt + opreg) & 7] = value */
static void gen_set_fptag_stn(int opreg, int value)
{
    TCGv_ptr p = tcg_temp_new_ptr();
    tcg_gen_add_zext_shl_ptr(p, tcg_env, gen_st_index(opreg), 0);
    tcg_gen_st8_i32(tcg_constant_i32(value), p,
                    offsetof(CPUX86State, fptags[0]));
}

#if defined(XBOX)
/*
 * With the inline x87 cache off (the helper path, which both modes run),
 * the ops that only move bits in env -- push/pop, fnstsw, register moves
 * and fxch, fchs/fabs, fld1/fldz, ffree -- are emitted as TCG code instead
 * of helper calls. They write the same env bytes the helpers do, at the
 * same point in the instruction.
 */
static bool x87_lean(void)
{
    return !g_use_hard_fpu_inline;
}

/* Copy one 16-byte x87 register image (floatx80 and its padding). */
static void x87_gen_copy(X87Ptr dst, X87Ptr src)
{
    TCGv_i64 lo = tcg_temp_new_i64();
    TCGv_i64 hi = tcg_temp_new_i64();
    tcg_gen_ld_i64(lo, src.base, src.ofs);
    tcg_gen_ld_i64(hi, src.base, src.ofs + 8);
    tcg_gen_st_i64(lo, dst.base, dst.ofs);
    tcg_gen_st_i64(hi, dst.base, dst.ofs + 8);
}

/* Store a floatx80 constant (helper_fld1/fldz: ST0 = floatx80_one ...). */
static void x87_gen_const(X87Ptr dst, uint16_t high, uint64_t low)
{
    tcg_gen_st_i64(tcg_constant_i64(low), dst.base, dst.ofs);
    tcg_gen_st16_i32(tcg_constant_i32(high), dst.base, dst.ofs + 8);
}

/* floatx80_chs / floatx80_abs on the sign/exponent halfword. */
static void x87_gen_sign(X87Ptr p, bool chs)
{
    TCGv_i32 hi = tcg_temp_new_i32();
    tcg_gen_ld16u_i32(hi, p.base, p.ofs + 8);
    if (chs) {
        tcg_gen_xori_i32(hi, hi, 0x8000);
    } else {
        tcg_gen_andi_i32(hi, hi, 0x7fff);
    }
    tcg_gen_st16_i32(hi, p.base, p.ofs + 8);
}
#else
static bool x87_lean(void)
{
    return false;
}
#endif

static bool fpu_using_double_precision(DisasContext *s)
{
    /*
     * XXX: Currently emulating double extended precision with double precision
     * when using hard floats.
     */
    return s->flags & HF_FPU_PC_MASK;
}

static void gen_movi_f32(DisasContext *s, TCGv_f32 ret, float arg)
{
    tcg_gen_mov32i_f32(ret, tcg_constant_i32(*(uint32_t *)&arg));
}

static void gen_movi_f64(DisasContext *s, TCGv_f64 ret, double arg)
{
    tcg_gen_mov64i_f64(ret, tcg_constant_i64(*(uint64_t *)&arg));
}

#if defined(XBOX)
/*
 * XEMU_INV_PROF (d): cheap translate-time census counters (defined in
 * accel/tcg/tb-maint.c). Declared here in the fork's function-local extern
 * idiom because the private header lives in the accel/tcg source dir, not on
 * the target/ include path.
 */
extern bool xemu_inv_prof_on(void);
extern uint64_t xemu_inv_gototb_emitted;
extern uint64_t xemu_inv_jcprobe_emitted;
extern uint64_t xemu_inv_retmemo_emitted;
/* Cross-page direct-chaining campaign (XEMU_XPAGE_*;
 * docs/Finished/xpage-design.md, accel/tcg/xemu-xpage.[ch]). Same
 * function-local extern idiom. */
extern bool xemu_xpage_refute_on(void);
extern bool xemu_xpage_prof_on(void);
extern uint64_t xemu_xpage_taken_kernel;
extern uint64_t xemu_xpage_taken_low;
extern uint64_t xemu_xpage_emitted;
#endif

#include "xemu-x87.h"

static void gen_flcr(DisasContext *s)
{
    /* TODO: Oversynchronized */
    if (s->flcr_set) {
        return;
    }

    TCGv_i32 v = tcg_temp_new_i32();
    tcg_gen_ld16u_i32(v, tcg_env, offsetof(CPUX86State, fpuc));
    tcg_gen_andi_i32(v, v, 0xc00);
    tcg_gen_shli_i32(v, v, 3);
    tcg_gen_ori_i32(v, v, 0x1f80);
    tcg_gen_flcr(v);
    s->flcr_set = true;
}

static void gen_mov32f_i64(TCGv_i64 ret, TCGv_f32 arg)
{
    TCGv_f64 t = tcg_temp_new_f64();
    tcg_gen_cvt32f_f64(t, arg);
    tcg_gen_mov64f_i64(ret, t);
}

static void gen_mov32f_i32(TCGv_i32 ret, TCGv_f32 arg)
{
    tcg_gen_mov32f_i32(ret, arg);
}

static void gen_mov32i_f64(TCGv_f64 ret, TCGv_i32 arg)
{
    TCGv_f32 t = tcg_temp_new_f32();
    tcg_gen_mov32i_f32(t, arg);
    tcg_gen_cvt32f_f64(ret, t);
}

static void gen_mov32i_f32(TCGv_f32 ret, TCGv_i32 arg)
{
    tcg_gen_mov32i_f32(ret, arg);
}

static void gen_mov64f_i32(TCGv_i32 ret, TCGv_f64 arg)
{
    TCGv_f32 t = tcg_temp_new_f32();
    tcg_gen_cvt64f_f32(t, arg);
    tcg_gen_mov32f_i32(ret, t);
}

static void gen_mov64f_i64(TCGv_i64 ret, TCGv_f64 arg)
{
    tcg_gen_mov64f_i64(ret, arg);
}

static void gen_mov64i_f32(TCGv_f32 ret, TCGv_i64 arg)
{
    TCGv_f64 t = tcg_temp_new_f64();
    tcg_gen_mov64i_f64(t, arg);
    tcg_gen_cvt64f_f32(ret, t);
}

static void gen_mov64i_f64(TCGv_f64 ret, TCGv_i64 arg)
{
    tcg_gen_mov64i_f64(ret, arg);
}

#define PREC 32
#include "ops_fpu.h"
#undef PREC

#define PREC 64
#include "ops_fpu.h"
#undef PREC

#define fp_pc_wrapper(f) \
    (fpu_using_double_precision(s) ? glue(f, _f64) : glue(f, _f32))

#if defined(XBOX)
static void x87_wt_flush(DisasContext *s);
#endif

static void gen_flush_fp(DisasContext *s)
{
    fp_pc_wrapper(flush_fp_regs)(s);
#if defined(XBOX)
    /*
     * Reached from gen_bb_epilogue() (every BB_END op and the top of every
     * tcg_gen_callN except the argument-only pure calls, which read no env
     * state: gen_pure_call_begin) and from i386_tr_tb_stop(), so this is
     * also the point where a deferred exception-pointer update must land:
     * no consumer of env->fpip/fpcs/fpdp/fpds can run without passing
     * through here first.
     */
    xemu_x87_flush_ptrs(s);
    xemu_x87_close_run(s);
    s->xemu_fds_seg = -1;
    /* Write-through: land the deferred stores, then drop the copies. */
    x87_wt_flush(s);
#endif
    s->fpstt_delta = 0;
    s->flcr_set = false;
#if defined(XBOX)
    for (int i = 0; i < 8; i++) {
        s->wt_st[i] = NULL;
    }
    s->wt_ft0 = NULL;
    s->wt_exact = 0;
#endif
}

#if defined(XBOX)
/*
 * Exact write-through inline x87 (aarch64 hosts whose backend has FP ops).
 *
 * The aarch64 __hard helpers (fpu_helper.c) read each operand from its
 * 80-bit image with floatx80_to_double_fast, compute one double operation
 * under the live FPCR, and write double_to_floatx80_fast of the result.
 * This path does exactly that inline, without the call:
 *  - every result reaches env by the next flush (gen_flush_fp), as the
 *    helper would have written it; a mid-block exit before then
 *    (cpu_io_recompile under icount, a guest #PF) lands it from the
 *    shadow double (x87_wt_pend);
 *  - the conversions are the backend's ld80f/st80f, which match the C
 *    conversions bit for bit;
 *  - each register's double, as the next helper would read it back, is
 *    kept in a temp until the next flush (gen_flush_fp), so a chain of
 *    x87 instructions does not reconvert. st80f is exact, so
 *    that is the computed result itself; only a double loaded from memory
 *    can be a signalling NaN, which reads back quieted (x87_wt_canon);
 *  - the FPCR is never written: the helpers round with whatever it holds.
 * Only blocks with precision control double/extended (HF_FPU_PC) use it:
 * single precision rounds results to float (pack_arm64) and keeps the
 * helpers. FT0 temps from memory operands are consumed in the same
 * instruction by an inline op, so they are not stored to env->ft0.
 */
static bool x87_wt(DisasContext *s)
{
    return g_x87_wt && fpu_using_double_precision(s);
}

/*
 * canon = floatx80_to_double_fast(double_to_floatx80_fast(bits)): bits
 * itself, but a NaN comes back with the quiet bit set.
 */
static void x87_wt_canon(TCGv_i64 canon, TCGv_i64 bits)
{
    TCGv_i64 mag = tcg_temp_new_i64();
    TCGv_i64 q = tcg_temp_new_i64();

    tcg_gen_andi_i64(mag, bits, INT64_MAX);
    tcg_gen_ori_i64(q, bits, 1ull << 51);
    tcg_gen_movcond_i64(TCG_COND_GTU, canon, mag,
                        tcg_constant_i64(0x7FF0000000000000ull), q, bits);
}

static TCGv_f64 x87_wt_from_bits(TCGv_i64 bits)
{
    TCGv_f64 v = tcg_temp_new_f64();
    tcg_gen_mov64i_f64(v, bits);
    return v;
}

/*
 * ST(i) as the helpers read it. ld80f/st80f are the backend's inline
 * floatx80_to_double_fast/double_to_floatx80_fast (tcg-target.c.inc), which
 * match the C bit for bit.
 *
 * The cached double is converted once, under the FPCR at fill time, and a
 * value beyond double's range rounds by FPCR.RMode. A helper would convert
 * it under the FPCR at use time. The two agree because FPCR changes only
 * inside helpers (update_fp_status, the SSE brackets, which restore it on
 * leave) and every helper call flushes wt_st (tcg_gen_callN ->
 * gen_bb_epilogue -> gen_flush_fp), except the argument-only pure calls
 * (gen_pure_call_begin), which leave the FPCR alone.
 */
static TCGv_f64 x87_wt_get_st(DisasContext *s, int i)
{
    int r = (s->fpstt_delta + i) & 7;
    TCGv_f64 *slot = &s->wt_st[r];
    if (!*slot) {
        X87Ptr p = gen_stn_ptr(i);
        *slot = tcg_temp_new_f64();
        tcg_gen_ld80f_f64(*slot, p.base, p.ofs);
        s->wt_exact &= ~(1 << r);
    }
    return *slot;
}

/*
 * Whether ST(i)'s image is st80f of its cached double. For two such
 * operands the double compare orders them exactly as the helpers'
 * floatx80_compare_quiet orders the images: st80f is exact and monotonic
 * (subnormals come out normalised, a NaN quiet), so the 80-bit integer
 * fast path and softfloat agree with FCMP, unordered included.
 */
static bool x87_wt_exact(DisasContext *s, int i)
{
    int r = (s->fpstt_delta + i) & 7;
    return s->wt_st[r] && (s->wt_exact & (1 << r));
}

static void x87_wt_set_exact(DisasContext *s, int r, bool on)
{
    s->wt_exact = (s->wt_exact & ~(1 << r)) | (on << r);
}

static TCGv_f64 x87_wt_get_ft0(DisasContext *s)
{
    if (!s->wt_ft0) {
        X87Ptr p = gen_ft0_ptr();
        s->wt_ft0 = tcg_temp_new_f64();
        tcg_gen_ld80f_f64(s->wt_ft0, p.base, p.ofs);
    }
    return s->wt_ft0;
}

/*
 * Deferred stores. A result's st80f waits for the next flush (gen_flush_fp:
 * every helper call except the argument-only pure calls (gen_pure_call_begin),
 * every BB_END op and the TB end), so a slot written again
 * before then, which most x87 chains do (fld, fmul, fadd... on one ST0),
 * converts once. Only an overwrite cancels a store: a popped slot is still
 * stored, since FSAVE/FXSAVE expose empty registers.
 *
 * A mid-block exit (a guest fault, cpu_io_recompile) must still leave env
 * as the eager stores would. So every deferred value is also stored as a
 * double in env->xemu_x87_shadow (one STR), insn_start word 2 names the
 * slots pending at the start of each insn, and x86_restore_state_to_opc
 * converts those (xemu_x87_restore) with double_to_floatx80_fast, st80f's
 * C twin. Shadow slot b is physical register (pend_top + b) & 7, pend_top
 * being the fpstt stored when the first store of a flush interval was
 * deferred. A flush inside an insn that started with stores pending zeroes
 * pend_top, so an exit later in that insn (a helper's fault) lands nothing
 * over what the flush and the helper wrote, and the rest of the insn
 * stores eagerly.
 *
 * Invariants: a pending slot always has its double in wt_st; and env's
 * image of a pending slot is stale, so TCG code that reads or writes the
 * raw image (x87_gen_copy, fxch, the sign ops, the constants) goes through
 * the cached double or x87_wt_sync first. Helpers need nothing: their call
 * flushes, and the pure calls that skip the flush read only their
 * arguments.
 */
static uint8_t x87_wt_rel_pend(DisasContext *s)
{
    uint8_t rel = 0;

    for (int r = 0; r < 8; r++) {
        if (s->wt_pend & (1 << r)) {
            rel |= 1 << ((r - s->wt_base) & 7);
        }
    }
    return rel;
}

static bool x87_wt_pending(DisasContext *s, int slot)
{
    return s->wt_pend & (1 << slot);
}

/* Store wt_st slot `slot` now, as the eager path did. */
static void x87_wt_store(DisasContext *s, int slot)
{
    X87Ptr p = gen_stn_ptr((slot - s->fpstt_delta) & 7);

    tcg_gen_st80f_f64(s->wt_st[slot], p.base, p.ofs);
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_st80f);
    }
}

/* Make env's image of a slot current before TCG code reads it raw. */
static void x87_wt_sync(DisasContext *s, int slot)
{
    if (x87_wt_pending(s, slot)) {
        x87_wt_store(s, slot);
        s->wt_pend &= ~(1 << slot);
    }
}

/* Defer slot `slot`'s store of r. */
static void x87_wt_pend(DisasContext *s, int slot, TCGv_f64 r)
{
    if (!s->wt_epoch) {
        TCGv_i32 top = tcg_temp_new_i32();

        tcg_gen_ori_i32(top, fpstt, 8);
        tcg_gen_st8_i32(top, tcg_env,
                        offsetof(CPUX86State, xemu_x87_pend_top));
        s->wt_epoch = true;
        s->wt_base = s->fpstt_delta;
    }
    tcg_gen_st_f64(r, tcg_env,
                   offsetof(CPUX86State,
                            xemu_x87_shadow[(slot - s->wt_base) & 7]));
    s->wt_st[slot] = r;
    s->wt_pend |= 1 << slot;
    s->wt_exact |= 1 << slot;
    if (unlikely(xemu_x87_refute_on())) {
        xemu_x87_gen_poison(gen_stn_ptr((slot - s->fpstt_delta) & 7));
    }
}

/*
 * The flush's half: store every pending slot. Called from gen_flush_fp
 * before it drops wt_st and fpstt_delta.
 */
static void x87_wt_flush(DisasContext *s)
{
    if (s->wt_pend) {
        for (int r = 0; r < 8; r++) {
            if (s->wt_pend & (1 << r)) {
                tcg_debug_assert(s->wt_st[r]);
                x87_wt_store(s, r);
            }
        }
        s->wt_pend = 0;
        if (unlikely(xemu_x87_refute_on())) {
            xemu_x87_gen_poison_scan(&xemu_x87_wt_flush_viol);
        }
    }
    if (s->wt_insn_pend && !s->wt_nodefer) {
        tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env,
                        offsetof(CPUX86State, xemu_x87_pend_top));
    }
    s->wt_nodefer = s->wt_insn_pend != 0;
    s->wt_epoch = false;
}

/*
 * Refuter, at the top of every insn: exactly the pending slots hold the
 * poison, their shadows hold their doubles, and pend_top maps the shadow
 * slots onto them, so a mid-block exit here would land exactly them.
 */
static void x87_wt_boundary_check(DisasContext *s)
{
    if (!xemu_x87_refute_on() || !x87_wt(s)) {
        return;
    }
    for (int r = 0; r < 8; r++) {
        bool pend = x87_wt_pending(s, r);

        xemu_x87_gen_poison_check(&xemu_x87_wt_bnd_viol,
                                  gen_stn_ptr((r - s->fpstt_delta) & 7), pend);
        if (pend) {
            TCGv_i64 want = tcg_temp_new_i64();
            TCGv_i64 was = tcg_temp_new_i64();

            tcg_gen_mov64f_i64(want, s->wt_st[r]);
            tcg_gen_ld_i64(was, tcg_env,
                           offsetof(CPUX86State,
                                    xemu_x87_shadow[(r - s->wt_base) & 7]));
            xemu_x87_gen_ne_count_i64(&xemu_x87_wt_bnd_viol, want, was);
        }
    }
    xemu_x87_gen_poison_check(&xemu_x87_wt_bnd_viol, gen_ft0_ptr(), false);
    if (s->wt_pend) {
        TCGv_i32 want = tcg_temp_new_i32();
        TCGv_i32 was = tcg_temp_new_i32();

        tcg_gen_addi_i32(want, fpstt, s->wt_base - s->fpstt_delta);
        tcg_gen_andi_i32(want, want, 7);
        tcg_gen_ori_i32(want, want, 8);
        tcg_gen_ld8u_i32(was, tcg_env,
                         offsetof(CPUX86State, xemu_x87_pend_top));
        xemu_x87_gen_ne_count_i32(&xemu_x87_wt_bnd_viol, want, was);
    }
    xemu_gen_counter_inc(&xemu_x87_wt_bnd_checks);
}

/*
 * ST(i) = r, as a helper writes double_to_floatx80_fast(result). r reads
 * back unchanged: it is computed by the host (fadd, fcvt, scvtf...), which
 * never returns a signalling NaN.
 */
static void x87_wt_set_st(DisasContext *s, int i, TCGv_f64 r)
{
    int slot = (s->fpstt_delta + i) & 7;

    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_writes);
    }
    if (s->wt_nodefer) {
        s->wt_st[slot] = r;
        s->wt_exact |= 1 << slot;
        x87_wt_store(s, slot);
        return;
    }
    x87_wt_pend(s, slot, r);
}

/* ST(i) = a qword from memory; st80f stores it quieted as well. */
static void x87_wt_set_st_bits(DisasContext *s, int i, TCGv_i64 bits)
{
    TCGv_i64 canon = tcg_temp_new_i64();
    x87_wt_canon(canon, bits);
    x87_wt_set_st(s, i, x87_wt_from_bits(canon));
}

/* FT0 from a qword memory operand: only the canonical value, no env store. */
static void x87_wt_set_ft0_bits(DisasContext *s, TCGv_i64 bits)
{
    TCGv_i64 canon = tcg_temp_new_i64();
    x87_wt_canon(canon, bits);
    s->wt_ft0 = x87_wt_from_bits(canon);
}

/* a op b for the arith ops' operand order; NULL for the compares. */
static TCGv_f64 x87_wt_arith(int op, TCGv_f64 a, TCGv_f64 b)
{
    TCGv_f64 r = tcg_temp_new_f64();
    switch (op) {
    case 0:
        tcg_gen_add_f64(r, a, b);
        break;
    case 1:
        tcg_gen_mul_f64(r, a, b);
        break;
    case 4:
        tcg_gen_sub_f64(r, a, b);
        break;
    case 5:
        tcg_gen_sub_f64(r, b, a);
        break;
    case 6:
        tcg_gen_div_f64(r, a, b);
        break;
    case 7:
        tcg_gen_div_f64(r, b, a);
        break;
    default:
        g_assert_not_reached();
    }
    return r;
}

/* fpus C3/C2/C0 from a compare's ZF|PF|CF (tcg_gen_com_f64). */
static void x87_wt_fpus_cc(TCGv_i64 res)
{
    TCGv_i64 fpus = tcg_temp_new_i64();

    tcg_gen_andi_i64(res, res, 0x45);   /* ZF|PF|CF = C3|C2|C0 at 6,2,0 */
    tcg_gen_shli_i64(res, res, 8);
    tcg_gen_ld16u_i64(fpus, tcg_env, offsetof(CPUX86State, fpus));
    tcg_gen_andi_i64(fpus, fpus, ~0x4500);
    tcg_gen_or_i64(fpus, fpus, res);
    tcg_gen_st16_i64(fpus, tcg_env, offsetof(CPUX86State, fpus));
}

/* helper_fcom_ST0_FT0: fpus C3/C2/C0 from a double compare. */
static void x87_wt_fcom(TCGv_f64 a, TCGv_f64 b)
{
    TCGv_i64 res = tcg_temp_new_i64();

    tcg_gen_com_f64(res, a, b);
    x87_wt_fpus_cc(res);
}

/*
 * ZF|PF|CF of ST0 against ST(i) for fucom* and fcomi* (helper_fucom_ST0_FT0,
 * helper_f(u)comi_ST0_FT0, with FT0 = ST(i)), or NULL where the helper
 * must run. The __hard helpers raise no exception flags
 * (merge_exception_flags is empty under USE_HARD_FPU), so neither does
 * this, quiet or not. fcomi* compares floatx80_to_double_fast of both
 * images, which is what ld80f gives: always inline. fucom* orders the
 * 80-bit images themselves (floatx80_compare_quiet__hard), so it runs
 * inline only when both images are st80f of their doubles (x87_wt_exact):
 * a register read from env may hold bits a double has not.
 */
static TCGv_i64 x87_wt_com_st(DisasContext *s, int i, bool quiet)
{
    TCGv_i64 res;

    if (!x87_wt(s) ||
        (quiet && !(x87_wt_exact(s, 0) && x87_wt_exact(s, i)))) {
        if (x87_wt(s) && unlikely(xemu_x87_census_on())) {
            xemu_gen_counter_inc(&xemu_x87_wt_cmp_helper);
        }
        return NULL;
    }
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_cmp_inline);
    }
    res = tcg_temp_new_i64();
    tcg_gen_com_f64(res, x87_wt_get_st(s, 0), x87_wt_get_st(s, i));
    return res;
}

/* fucom/fucomp/fucompp: false if the helper must run. */
static bool gen_fucom_wt(DisasContext *s, int i)
{
    TCGv_i64 res = x87_wt_com_st(s, i, true);

    if (!res) {
        return false;
    }
    x87_wt_fpus_cc(res);
    return true;
}

/*
 * f(u)comi(p): ZF/PF/CF from the compare, the other flags as they were
 * (cpu_cc_compute_all), CC_OP_EFLAGS; false if the helper must run.
 * gen_compute_eflags' cc_compute_all call is a pure call: no flush.
 */
static bool gen_fcomi_wt(DisasContext *s, int i, bool quiet)
{
    TCGv_i64 res = x87_wt_com_st(s, i, quiet);
    TCGv t;

    if (!res) {
        return false;
    }
    gen_compute_eflags(s);
    t = tcg_temp_new();
    tcg_gen_trunc_i64_tl(t, res);       /* com_f64 sets no other bit */
    tcg_gen_andi_tl(cpu_cc_src, cpu_cc_src, ~(CC_Z | CC_P | CC_C));
    tcg_gen_or_tl(cpu_cc_src, cpu_cc_src, t);
    return true;
}
#else
static bool x87_wt(DisasContext *s)
{
    return false;
}

static bool gen_fucom_wt(DisasContext *s, int i)
{
    return false;
}

static bool gen_fcomi_wt(DisasContext *s, int i, bool quiet)
{
    return false;
}
#endif

/*
 * Ugly macros to handle soft FPU helper generation
 */
#define GEN_HELPER_FALLBACK_v_v(func) do { \
        if (!g_use_hard_fpu_inline) { \
            gen_helper_ ## func(tcg_env); \
            return; \
        }} while(0)

#define GEN_HELPER_FALLBACK_v_i(func, arg) do { \
        if (!g_use_hard_fpu_inline) { \
            gen_helper_ ## func(tcg_env, tcg_constant_i32(arg)); \
            return; \
        }} while(0)

#define GEN_HELPER_FALLBACK_v_T(func, arg) do { \
        if (!g_use_hard_fpu_inline) { \
            gen_helper_ ## func(tcg_env, arg); \
            return; \
        }} while(0)

#define GEN_HELPER_FALLBACK_T_v(func, arg) do { \
        if (!g_use_hard_fpu_inline) { \
            gen_helper_ ## func(arg, tcg_env); \
            return; \
        }} while(0)

static void gen_fpush(DisasContext *s)
{
    if (!x87_lean()) {
        GEN_HELPER_FALLBACK_v_v(fpush);
    }

    tcg_gen_subi_i32(fpstt, fpstt, 1);
    tcg_gen_andi_i32(fpstt, fpstt, 7);
    gen_set_fptag_stn(0, 0); /* validate stack entry */

    s->fpstt_delta -= 1;
}

static void gen_fpop(DisasContext *s)
{
    if (!x87_lean()) {
        GEN_HELPER_FALLBACK_v_v(fpop);
    }

    gen_set_fptag_stn(0, 1); /* invalidate stack entry */
    tcg_gen_addi_i32(fpstt, fpstt, 1);
    tcg_gen_andi_i32(fpstt, fpstt, 7);

    fp_pc_wrapper(gen_fpop)(s);
    s->fpstt_delta += 1;
}

/* FT0 = ST(i) for a helper (fucom*, fcomi*), which reads env->ft0. */
static void gen_fmov_FT0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    if (x87_lean()) {
        int src = (s->fpstt_delta + st_index) & 7;
        if (x87_wt(s)) {
            x87_wt_sync(s, src);
        }
        x87_gen_copy(gen_ft0_ptr(), gen_stn_ptr(st_index));
        if (x87_wt(s)) {
            s->wt_ft0 = s->wt_st[src];
        }
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_i(fmov_FT0_STN, st_index);
    fp_pc_wrapper(gen_fmov_FT0_STN)(s, st_index);
}

/*
 * FT0 = ST(i) for gen_helper_fp_arith_ST0_FT0 and gen_fcom_ST0_FT0, which
 * write-through blocks run inline on the double: env->ft0 is not read, so
 * it is not written (as for the memory forms).
 */
static void gen_fload_FT0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        s->wt_ft0 = x87_wt_get_st(s, st_index);
        return;
    }
#endif
    gen_fmov_FT0_STN(s, st_index);
}

#if defined(XBOX)
/*
 * ST(d) = ST(s) as raw 16-byte copies. A deferred source has a stale
 * image, so its double is copied instead, deferred as well: st80f of the
 * same double gives the bytes the source will get.
 */
static void x87_lean_move(DisasContext *s, int d, int src_i)
{
    int dst = (s->fpstt_delta + d) & 7;
    int src = (s->fpstt_delta + src_i) & 7;

    if (x87_wt(s) && x87_wt_pending(s, src)) {
        x87_wt_pend(s, dst, s->wt_st[src]);
        return;
    }
    x87_gen_copy(gen_stn_ptr(d), gen_stn_ptr(src_i));
    if (x87_wt(s)) {
        s->wt_st[dst] = s->wt_st[src];
        s->wt_pend &= ~(1 << dst);
        x87_wt_set_exact(s, dst, s->wt_exact & (1 << src));
    }
}
#endif

static void gen_fmov_ST0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    if (x87_lean()) {
        x87_lean_move(s, 0, st_index);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_i(fmov_ST0_STN, st_index);
    fp_pc_wrapper(gen_fmov_ST0_STN)(s, st_index);
}

static void gen_fmov_STN_ST0(DisasContext *s, int st_index)
{
#if defined(XBOX)
    if (x87_lean()) {
        x87_lean_move(s, st_index, 0);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_i(fmov_STN_ST0, st_index);
    fp_pc_wrapper(gen_fmov_STN_ST0)(s, st_index);
}

static void gen_ffree_STN(DisasContext *s, int st_index)
{
    if (!x87_lean()) {
        GEN_HELPER_FALLBACK_v_i(ffree_STN, st_index);
    }

    /*
     * Inline: a single byte write into the tag word; no register spill,
     * no env fpus change (ffree only marks the tag slot empty).
     */
    gen_set_fptag_stn(st_index, 1);
}

static void gen_fxchg_ST0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    if (x87_lean()) {
        int a = s->fpstt_delta & 7;
        int b = (s->fpstt_delta + st_index) & 7;
        bool pa = x87_wt(s) && x87_wt_pending(s, a);
        bool pb = x87_wt(s) && x87_wt_pending(s, b);

        if (pa || pb) {
            /*
             * A deferred side takes the other's double (deferred again);
             * a current side's image moves raw, read before the deferred
             * side is overwritten.
             */
            TCGv_f64 va = s->wt_st[a], vb = s->wt_st[b];
            bool ea = s->wt_exact & (1 << a), eb = s->wt_exact & (1 << b);
            if (!pb) {
                x87_gen_copy(gen_stn_ptr(0), gen_stn_ptr(st_index));
                s->wt_pend &= ~(1 << a);
                s->wt_st[a] = vb;
                x87_wt_set_exact(s, a, eb);
            }
            if (!pa) {
                x87_gen_copy(gen_stn_ptr(st_index), gen_stn_ptr(0));
                s->wt_pend &= ~(1 << b);
                s->wt_st[b] = va;
                x87_wt_set_exact(s, b, ea);
            }
            if (pb) {
                x87_wt_pend(s, a, vb);
            }
            if (pa) {
                x87_wt_pend(s, b, va);
            }
            return;
        }

        X87Ptr p0 = gen_stn_ptr(0);
        X87Ptr pn = gen_stn_ptr(st_index);
        TCGv_i64 lo0 = tcg_temp_new_i64(), hi0 = tcg_temp_new_i64();
        TCGv_i64 lon = tcg_temp_new_i64(), hin = tcg_temp_new_i64();
        tcg_gen_ld_i64(lo0, p0.base, p0.ofs);
        tcg_gen_ld_i64(hi0, p0.base, p0.ofs + 8);
        tcg_gen_ld_i64(lon, pn.base, pn.ofs);
        tcg_gen_ld_i64(hin, pn.base, pn.ofs + 8);
        tcg_gen_st_i64(lo0, pn.base, pn.ofs);
        tcg_gen_st_i64(hi0, pn.base, pn.ofs + 8);
        tcg_gen_st_i64(lon, p0.base, p0.ofs);
        tcg_gen_st_i64(hin, p0.base, p0.ofs + 8);
        if (x87_wt(s)) {
            TCGv_f64 t = s->wt_st[a];
            bool ea = s->wt_exact & (1 << a);
            s->wt_st[a] = s->wt_st[b];
            s->wt_st[b] = t;
            x87_wt_set_exact(s, a, s->wt_exact & (1 << b));
            x87_wt_set_exact(s, b, ea);
        }
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_i(fxchg_ST0_STN, st_index);

    /* Ensure ST0, STN are loaded */
    if (fpu_using_double_precision(s)) {
        get_stn_f64(s, 0);
        get_stn_f64(s, st_index);
    } else {
        get_stn_f32(s, 0);
        get_stn_f32(s, st_index);
    }
    TCGv_fp i = s->fpregs[(s->fpstt_delta + 0) & 7];
    s->fpregs[(s->fpstt_delta + 0) & 7] =
        s->fpregs[(s->fpstt_delta + st_index) & 7];
    s->fpregs[(s->fpstt_delta + st_index) & 7] = i;
}

static void gen_enter_mmx(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(enter_mmx);

    gen_flush_fp((DisasContext *)tcg_ctx->disas_ctx);

    tcg_gen_movi_i32(fpstt, 0);

    TCGv_i32 v = tcg_constant_i32(0);
    for (int i = 0; i < 8; i++) {
        tcg_gen_st8_i32(v, tcg_env, offsetof(CPUX86State, fptags[0]) + i);
    }
}

#if defined(XBOX)
/* (double) of a float32 memory operand, as float32_to_floatx80__hard. */
static TCGv_f64 x87_wt_f32_to_f64(TCGv_i32 arg)
{
    TCGv_f32 t = tcg_temp_new_f32();
    TCGv_f64 d = tcg_temp_new_f64();
    tcg_gen_mov32i_f32(t, arg);
    tcg_gen_cvt32f_f64(d, t);
    return d;
}

/* (double) of an int32, exact: int32_to_floatx80__hard. */
static TCGv_f64 x87_wt_i32_to_f64(TCGv_i32 arg)
{
    TCGv_f64 d = tcg_temp_new_f64();
    tcg_gen_cvt32i_f64(d, arg);
    return d;
}
#endif

static void gen_flds_FT0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        s->wt_ft0 = x87_wt_f32_to_f64(arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_T(flds_FT0, arg);
    fp_pc_wrapper(gen_flds_FT0)(s, arg);
}

static void gen_flds_ST0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        TCGv_f64 d = x87_wt_f32_to_f64(arg);
        gen_fpush(s);
        x87_wt_set_st(s, 0, d);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_T(flds_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_flds_ST0)(s, arg);
}

static void gen_fldl_FT0(DisasContext *s, TCGv_i64 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        x87_wt_set_ft0_bits(s, arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_T(fldl_FT0, arg);
    fp_pc_wrapper(gen_fldl_FT0)(s, arg);
}

static void gen_fldl_ST0(DisasContext *s, TCGv_i64 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        gen_fpush(s);
        x87_wt_set_st_bits(s, 0, arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_T(fldl_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_fldl_ST0)(s, arg);
}

static void gen_fildl_FT0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        s->wt_ft0 = x87_wt_i32_to_f64(arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_T(fildl_FT0, arg);
    fp_pc_wrapper(gen_fildl_FT0)(s, arg);
}

static void gen_fildl_ST0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        TCGv_f64 d = x87_wt_i32_to_f64(arg);
        gen_fpush(s);
        x87_wt_set_st(s, 0, d);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_T(fildl_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_fildl_ST0)(s, arg);
}

static void gen_fildll_ST0(DisasContext *s, TCGv_i64 arg)
{
    GEN_HELPER_FALLBACK_v_T(fildll_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_fildll_ST0)(s, arg);
}

static void gen_fsts_ST0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        /* (float) of the double, under the live FPCR */
        TCGv_f32 t = tcg_temp_new_f32();
        tcg_gen_cvt64f_f32(t, x87_wt_get_st(s, 0));
        tcg_gen_mov32f_i32(arg, t);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_T_v(fsts_ST0, arg);
    fp_pc_wrapper(gen_fsts_ST0)(s, arg);
}

static void gen_fstl_ST0(DisasContext *s, TCGv_i64 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        tcg_gen_mov64f_i64(arg, x87_wt_get_st(s, 0));
        return;
    }
#endif
    GEN_HELPER_FALLBACK_T_v(fstl_ST0, arg);
    fp_pc_wrapper(gen_fstl_ST0)(s, arg);
}

static void gen_fistl_ST0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_T_v(fistl_ST0, arg);
    fp_pc_wrapper(gen_fistl_ST0)(s, arg);
}

static void gen_fistll_ST0(DisasContext *s, TCGv_i64 arg)
{
    GEN_HELPER_FALLBACK_T_v(fistll_ST0, arg);
    fp_pc_wrapper(gen_fistll_ST0)(s, arg);
}

#if defined(XBOX)
/*
 * fchs/fabs of a deferred ST0: st80f copies the sign through on every path
 * (double_to_floatx80_fast), so the sign op on the double gives the bytes
 * the image sign op would. FNEG/FABS only touch the sign bit (FPCR.AH is
 * never set), and the result reads back unchanged.
 */
static bool x87_wt_sign_pending(DisasContext *s, bool chs)
{
    int slot = s->fpstt_delta & 7;
    TCGv_f64 r;

    if (!x87_wt(s) || !x87_wt_pending(s, slot)) {
        return false;
    }
    r = tcg_temp_new_f64();
    if (chs) {
        tcg_gen_chs_f64(r, s->wt_st[slot]);
    } else {
        tcg_gen_abs_f64(r, s->wt_st[slot]);
    }
    x87_wt_pend(s, slot, r);
    return true;
}
#endif

static void gen_fchs_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_lean()) {
        if (x87_wt_sign_pending(s, true)) {
            return;
        }
        x87_gen_sign(gen_stn_ptr(0), true);
        /*
         * Reload rather than negate the cached double: the conversion does
         * not commute with the sign for an 80-bit value beyond double range
         * under RP/RM (inf on one side, DBL_MAX on the other), nor for the
         * fixed-sign default NaN of an unnormal or pseudo-NaN.
         */
        s->wt_st[s->fpstt_delta & 7] = NULL;
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_v(fchs_ST0);
    fp_pc_wrapper(gen_fchs_ST0)(s);
}

static void gen_fabs_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_lean()) {
        if (x87_wt_sign_pending(s, false)) {
            return;
        }
        x87_gen_sign(gen_stn_ptr(0), false);
        s->wt_st[s->fpstt_delta & 7] = NULL;   /* as in gen_fchs_ST0 */
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_v(fabs_ST0);
    fp_pc_wrapper(gen_fabs_ST0)(s);
}

static void gen_fsqrt(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fsqrt);
    fp_pc_wrapper(gen_fsqrt)(s);
}

static void gen_clear_fpus_c2(DisasContext *s)
{
    TCGv_i32 v = tcg_temp_new_i32();
    tcg_gen_ld16u_i32(v, tcg_env, offsetof(CPUX86State, fpus));
    tcg_gen_andi_i32(v, v, ~0x400); /* C2 <-- 0 */
    tcg_gen_st16_i32(v, tcg_env, offsetof(CPUX86State, fpus));
}

static void gen_fsin(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fsin);
    fp_pc_wrapper(gen_fsin)(s);
    gen_clear_fpus_c2(s); /* FIXME: Does not check range correctly */
}

static void gen_fcos(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fcos);
    fp_pc_wrapper(gen_fcos)(s);
    gen_clear_fpus_c2(s); /* FIXME: Does not check range correctly */
}

static void gen_helper_fp_arith_ST0_FT0(DisasContext *s, int op)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        TCGv_f64 a = x87_wt_get_st(s, 0);
        TCGv_f64 b = x87_wt_get_ft0(s);
        if (op == 2 || op == 3) {
            x87_wt_fcom(a, b);
        } else {
            /* 4 fsub ST0-FT0, 5 fsubr FT0-ST0, 6 fdiv, 7 fdivr */
            x87_wt_set_st(s, 0, x87_wt_arith(op, a, b));
        }
        return;
    }
#endif
    if (g_use_hard_fpu_inline) {
        fp_pc_wrapper(gen_helper_fp_arith_ST0_FT0)(s, op);
    } else {
        switch (op) {
        case 0:
            gen_helper_fadd_ST0_FT0(tcg_env);
            break;
        case 1:
            gen_helper_fmul_ST0_FT0(tcg_env);
            break;
        case 2:
            gen_helper_fcom_ST0_FT0(tcg_env);
            break;
        case 3:
            gen_helper_fcom_ST0_FT0(tcg_env);
            break;
        case 4:
            gen_helper_fsub_ST0_FT0(tcg_env);
            break;
        case 5:
            gen_helper_fsubr_ST0_FT0(tcg_env);
            break;
        case 6:
            gen_helper_fdiv_ST0_FT0(tcg_env);
            break;
        case 7:
            gen_helper_fdivr_ST0_FT0(tcg_env);
            break;
        }
    }
}

static void gen_fcom_ST0_FT0(DisasContext *s)
{
    gen_helper_fp_arith_ST0_FT0(s, 2);
}

#if defined(XBOX)
/*
 * fcmov on the write-through path: ST0 = jcc(b) ? ST0 : ST(i), selected
 * without a branch. A branch is a BB_END op, whose flush lands every
 * deferred store and drops the cached doubles, so the fld/fld/fucomi/fcmov
 * min/max idiom converted both loads every element. The select re-defers
 * ST0 with either double, which leaves the image the move would only when
 * both images are st80f of their doubles (x87_wt_exact); otherwise this
 * branches on the condition it computed. False only off the path.
 */
static bool gen_fcmov_wt(DisasContext *s, int b, int opreg)
{
    TCGv c;
    TCGLabel *l1;

    if (!x87_wt(s)) {
        return false;
    }
    /* First, so that a flushing call in it cannot outdate the check. */
    c = tcg_temp_new();
    gen_setcc(s, b, c);
    if (x87_wt_exact(s, 0) && x87_wt_exact(s, opreg)) {
        TCGv_i64 c64 = tcg_temp_new_i64();
        TCGv_i64 keep = tcg_temp_new_i64();
        TCGv_i64 mov = tcg_temp_new_i64();

        if (unlikely(xemu_x87_census_on())) {
            xemu_gen_counter_inc(&xemu_x87_wt_fcmov_inline);
        }
        tcg_gen_extu_tl_i64(c64, c);
        tcg_gen_mov64f_i64(keep, x87_wt_get_st(s, 0));
        tcg_gen_mov64f_i64(mov, x87_wt_get_st(s, opreg));
        tcg_gen_movcond_i64(TCG_COND_NE, keep, c64, tcg_constant_i64(0),
                            keep, mov);
        x87_wt_set_st(s, 0, x87_wt_from_bits(keep));
        return true;
    }
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_fcmov_branch);
    }
    l1 = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_NE, c, 0, l1);
    gen_fmov_ST0_STN(s, opreg);
    gen_set_label(l1);
    return true;
}
#else
static bool gen_fcmov_wt(DisasContext *s, int b, int opreg)
{
    return false;
}
#endif

/* fcomi/fucomi ST0, ST(i): EFLAGS ZF/PF/CF, CC_OP_EFLAGS. */
static void gen_fcomi_ST0_STN(DisasContext *s, int opreg, bool quiet)
{
    if (gen_fcomi_wt(s, opreg, quiet)) {
        return;
    }
    gen_update_cc_op(s);
    gen_fmov_FT0_STN(s, opreg);
    if (quiet) {
        gen_helper_fucomi_ST0_FT0(tcg_env);
    } else {
        gen_helper_fcomi_ST0_FT0(tcg_env);
    }
    assume_cc_op(s, CC_OP_EFLAGS);
}

static void gen_fnstsw_inline(DisasContext *s, TCGv_i32 result)
{
    TCGv_i32 fpus = tcg_temp_new_i32();
    tcg_gen_ld16u_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
    tcg_gen_andi_i32(fpus, fpus, ~0x3800);
    TCGv_i32 top = tcg_temp_new_i32();
    tcg_gen_andi_i32(top, fpstt, 0x7);
    tcg_gen_shli_i32(top, top, 11);
    tcg_gen_or_i32(result, fpus, top);
}

/* NOTE the exception in "r" op ordering */
static void gen_helper_fp_arith_STN_ST0(DisasContext *s, int op, int opreg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        /*
         * op 4 is fsubr_STN_ST0 (ST0 - STi), 5 fsub_STN_ST0 (STi - ST0),
         * 6 fdivr_STN_ST0, 7 fdiv_STN_ST0: the reverse of the ST0_FT0
         * order with STi in the first operand, hence op ^ 1.
         */
        TCGv_f64 a = x87_wt_get_st(s, opreg);
        TCGv_f64 b = x87_wt_get_st(s, 0);
        x87_wt_set_st(s, opreg, x87_wt_arith(op >= 4 ? op ^ 1 : op, a, b));
        return;
    }
#endif
    if (g_use_hard_fpu_inline) {
        fp_pc_wrapper(gen_helper_fp_arith_STN_ST0)(s, op, opreg);
    } else {
        TCGv_i32 tmp = tcg_constant_i32(opreg);

        switch (op) {
        case 0:
            gen_helper_fadd_STN_ST0(tcg_env, tmp);
            break;
        case 1:
            gen_helper_fmul_STN_ST0(tcg_env, tmp);
            break;
        case 4:
            gen_helper_fsubr_STN_ST0(tcg_env, tmp);
            break;
        case 5:
            gen_helper_fsub_STN_ST0(tcg_env, tmp);
            break;
        case 6:
            gen_helper_fdivr_STN_ST0(tcg_env, tmp);
            break;
        case 7:
            gen_helper_fdiv_STN_ST0(tcg_env, tmp);
            break;
        }
    }
}

static void gen_fld1_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_lean()) {
        x87_gen_const(gen_stn_ptr(0), 0x3fff, 0x8000000000000000ULL);
        if (x87_wt(s)) {
            s->wt_st[s->fpstt_delta & 7] =
                x87_wt_from_bits(tcg_constant_i64(0x3FF0000000000000ull));
            s->wt_pend &= ~(1 << (s->fpstt_delta & 7));
            s->wt_exact |= 1 << (s->fpstt_delta & 7);
        }
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_v(fld1_ST0);
    fp_pc_wrapper(gen_fld1_ST0)(s);
}

static void gen_fldz_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_lean()) {
        x87_gen_const(gen_stn_ptr(0), 0, 0);
        if (x87_wt(s)) {
            s->wt_st[s->fpstt_delta & 7] =
                x87_wt_from_bits(tcg_constant_i64(0));
            s->wt_pend &= ~(1 << (s->fpstt_delta & 7));
            s->wt_exact |= 1 << (s->fpstt_delta & 7);
        }
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_v(fldz_ST0);
    fp_pc_wrapper(gen_fldz_ST0)(s);
    /* FIXME: Set tag word */
}

static void gen_fldz_FT0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_lean()) {
        x87_gen_const(gen_ft0_ptr(), 0, 0);
        if (x87_wt(s)) {
            s->wt_ft0 = x87_wt_from_bits(tcg_constant_i64(0));
        }
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_v(fldz_FT0);
    fp_pc_wrapper(gen_fldz_FT0)(s);
}

static void gen_exception(DisasContext *s, int trapno)
{
    gen_update_cc_op(s);
    gen_update_eip_cur(s);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(trapno));
    s->base.is_jmp = DISAS_NORETURN;
}

/* Generate #UD for the current instruction.  The assumption here is that
   the instruction is known, but it isn't allowed in the current cpu mode.  */
static void gen_illegal_opcode(DisasContext *s)
{
    gen_exception(s, EXCP06_ILLOP);
}

/* Generate #GP for the current instruction. */
static void gen_exception_gpf(DisasContext *s)
{
    gen_exception(s, EXCP0D_GPF);
}

/* Check for cpl == 0; if not, raise #GP and return false. */
static bool check_cpl0(DisasContext *s)
{
    if (CPL(s) == 0) {
        return true;
    }
    gen_exception_gpf(s);
    return false;
}

/* XXX: add faster immediate case */
static TCGv gen_shiftd_rm_T1(DisasContext *s, MemOp ot,
                             bool is_right, TCGv count)
{
    target_ulong mask = (ot == MO_64 ? 63 : 31);
    TCGv cc_src = tcg_temp_new();
    TCGv tmp = tcg_temp_new();
    TCGv hishift;

    switch (ot) {
    case MO_16:
        /* Note: we implement the Intel behaviour for shift count > 16.
           This means "shrdw C, B, A" shifts A:B:A >> C.  Build the B:A
           portion by constructing it as a 32-bit value.  */
        if (is_right) {
            tcg_gen_deposit_tl(tmp, s->T0, s->T1, 16, 16);
            tcg_gen_mov_tl(s->T1, s->T0);
            tcg_gen_mov_tl(s->T0, tmp);
        } else {
            tcg_gen_deposit_tl(s->T1, s->T0, s->T1, 16, 16);
        }
        /*
         * If TARGET_X86_64 defined then fall through into MO_32 case,
         * otherwise fall through default case.
         */
    case MO_32:
#ifdef TARGET_X86_64
        /* Concatenate the two 32-bit values and use a 64-bit shift.  */
        tcg_gen_subi_tl(tmp, count, 1);
        if (is_right) {
            tcg_gen_concat_tl_i64(s->T0, s->T0, s->T1);
            tcg_gen_shr_i64(cc_src, s->T0, tmp);
            tcg_gen_shr_i64(s->T0, s->T0, count);
        } else {
            tcg_gen_concat_tl_i64(s->T0, s->T1, s->T0);
            tcg_gen_shl_i64(cc_src, s->T0, tmp);
            tcg_gen_shl_i64(s->T0, s->T0, count);
            tcg_gen_shri_i64(cc_src, cc_src, 32);
            tcg_gen_shri_i64(s->T0, s->T0, 32);
        }
        break;
#endif
    default:
        hishift = tcg_temp_new();
        tcg_gen_subi_tl(tmp, count, 1);
        if (is_right) {
            tcg_gen_shr_tl(cc_src, s->T0, tmp);

            /* mask + 1 - count = mask - tmp = mask ^ tmp */
            tcg_gen_xori_tl(hishift, tmp, mask);
            tcg_gen_shr_tl(s->T0, s->T0, count);
            tcg_gen_shl_tl(s->T1, s->T1, hishift);
        } else {
            tcg_gen_shl_tl(cc_src, s->T0, tmp);

            /* mask + 1 - count = mask - tmp = mask ^ tmp */
            tcg_gen_xori_tl(hishift, tmp, mask);
            tcg_gen_shl_tl(s->T0, s->T0, count);
            tcg_gen_shr_tl(s->T1, s->T1, hishift);

            if (ot == MO_16) {
                /* Only needed if count > 16, for Intel behaviour.  */
                tcg_gen_shri_tl(tmp, s->T1, 1);
                tcg_gen_or_tl(cc_src, cc_src, tmp);
            }
        }
        tcg_gen_movcond_tl(TCG_COND_EQ, s->T1,
                           count, tcg_constant_tl(0),
                           tcg_constant_tl(0), s->T1);
        tcg_gen_or_tl(s->T0, s->T0, s->T1);
        break;
    }

    return cc_src;
}

#define X86_MAX_INSN_LENGTH 15

static uint64_t advance_pc(CPUX86State *env, DisasContext *s, int num_bytes)
{
    uint64_t pc = s->pc;

    /* This is a subsequent insn that crosses a page boundary.  */
    if (s->base.num_insns > 1 &&
        !translator_is_same_page(&s->base, s->pc + num_bytes - 1)) {
        siglongjmp(s->jmpbuf, 2);
    }

    s->pc += num_bytes;
    if (unlikely(cur_insn_len(s) > X86_MAX_INSN_LENGTH)) {
        /* If the instruction's 16th byte is on a different page than the 1st, a
         * page fault on the second page wins over the general protection fault
         * caused by the instruction being too long.
         * This can happen even if the operand is only one byte long!
         */
        if (((s->pc - 1) ^ (pc - 1)) & TARGET_PAGE_MASK) {
            (void)translator_ldub(env, &s->base,
                                  (s->pc - 1) & TARGET_PAGE_MASK);
        }
        siglongjmp(s->jmpbuf, 1);
    }

    return pc;
}

static inline uint8_t x86_ldub_code(CPUX86State *env, DisasContext *s)
{
    return translator_ldub(env, &s->base, advance_pc(env, s, 1));
}

static inline uint16_t x86_lduw_code(CPUX86State *env, DisasContext *s)
{
    return translator_lduw(env, &s->base, advance_pc(env, s, 2));
}

static inline uint32_t x86_ldl_code(CPUX86State *env, DisasContext *s)
{
    return translator_ldl(env, &s->base, advance_pc(env, s, 4));
}

#ifdef TARGET_X86_64
static inline uint64_t x86_ldq_code(CPUX86State *env, DisasContext *s)
{
    return translator_ldq(env, &s->base, advance_pc(env, s, 8));
}
#endif

/* Decompose an address.  */

static AddressParts gen_lea_modrm_0(CPUX86State *env, DisasContext *s,
                                    int modrm, bool is_vsib)
{
    int def_seg, base, index, scale, mod, rm;
    target_long disp;
    bool havesib;

    def_seg = R_DS;
    index = -1;
    scale = 0;
    disp = 0;

    mod = (modrm >> 6) & 3;
    rm = modrm & 7;
    base = rm | REX_B(s);

    if (mod == 3) {
        /* Normally filtered out earlier, but including this path
           simplifies multi-byte nop, as well as bndcl, bndcu, bndcn.  */
        goto done;
    }

    switch (s->aflag) {
    case MO_64:
    case MO_32:
        havesib = 0;
        if (rm == 4) {
            int code = x86_ldub_code(env, s);
            scale = (code >> 6) & 3;
            index = ((code >> 3) & 7) | REX_X(s);
            if (index == 4 && !is_vsib) {
                index = -1;  /* no index */
            }
            base = (code & 7) | REX_B(s);
            havesib = 1;
        }

        switch (mod) {
        case 0:
            if ((base & 7) == 5) {
                base = -1;
                disp = (int32_t)x86_ldl_code(env, s);
                if (CODE64(s) && !havesib) {
                    base = -2;
                    disp += s->pc + s->rip_offset;
                }
            }
            break;
        case 1:
            disp = (int8_t)x86_ldub_code(env, s);
            break;
        default:
        case 2:
            disp = (int32_t)x86_ldl_code(env, s);
            break;
        }

        /* For correct popl handling with esp.  */
        if (base == R_ESP && s->popl_esp_hack) {
            disp += s->popl_esp_hack;
        }
        if (base == R_EBP || base == R_ESP) {
            def_seg = R_SS;
        }
        break;

    case MO_16:
        if (mod == 0) {
            if (rm == 6) {
                base = -1;
                disp = x86_lduw_code(env, s);
                break;
            }
        } else if (mod == 1) {
            disp = (int8_t)x86_ldub_code(env, s);
        } else {
            disp = (int16_t)x86_lduw_code(env, s);
        }

        switch (rm) {
        case 0:
            base = R_EBX;
            index = R_ESI;
            break;
        case 1:
            base = R_EBX;
            index = R_EDI;
            break;
        case 2:
            base = R_EBP;
            index = R_ESI;
            def_seg = R_SS;
            break;
        case 3:
            base = R_EBP;
            index = R_EDI;
            def_seg = R_SS;
            break;
        case 4:
            base = R_ESI;
            break;
        case 5:
            base = R_EDI;
            break;
        case 6:
            base = R_EBP;
            def_seg = R_SS;
            break;
        default:
        case 7:
            base = R_EBX;
            break;
        }
        break;

    default:
        g_assert_not_reached();
    }

 done:
    return (AddressParts){ def_seg, base, index, scale, disp };
}

/* Compute the address, with a minimum number of TCG ops.  */
static TCGv gen_lea_modrm_1(DisasContext *s, AddressParts a, bool is_vsib)
{
    TCGv ea = NULL;

    if (a.index >= 0 && !is_vsib) {
        if (a.scale == 0) {
            ea = cpu_regs[a.index];
        } else {
            tcg_gen_shli_tl(s->A0, cpu_regs[a.index], a.scale);
            ea = s->A0;
        }
        if (a.base >= 0) {
            tcg_gen_add_tl(s->A0, ea, cpu_regs[a.base]);
            ea = s->A0;
        }
    } else if (a.base >= 0) {
        ea = cpu_regs[a.base];
    }
    if (!ea) {
        if (tb_cflags(s->base.tb) & CF_PCREL && a.base == -2) {
            /* With cpu_eip ~= pc_save, the expression is pc-relative. */
            tcg_gen_addi_tl(s->A0, cpu_eip, a.disp - s->pc_save);
        } else {
            tcg_gen_movi_tl(s->A0, a.disp);
        }
        ea = s->A0;
    } else if (a.disp != 0) {
        tcg_gen_addi_tl(s->A0, ea, a.disp);
        ea = s->A0;
    }

    return ea;
}

/* Used for BNDCL, BNDCU, BNDCN.  */
static void gen_bndck(DisasContext *s, X86DecodedInsn *decode,
                      TCGCond cond, TCGv_i64 bndv)
{
    TCGv ea = gen_lea_modrm_1(s, decode->mem, false);
    TCGv_i32 t32 = tcg_temp_new_i32();
    TCGv_i64 t64 = tcg_temp_new_i64();

    tcg_gen_extu_tl_i64(t64, ea);
    if (!CODE64(s)) {
        tcg_gen_ext32u_i64(t64, t64);
    }
    tcg_gen_setcond_i64(cond, t64, t64, bndv);
    tcg_gen_extrl_i64_i32(t32, t64);
    gen_helper_bndck(tcg_env, t32);
}

/* generate modrm load of memory or register. */
static void gen_ld_modrm(DisasContext *s, X86DecodedInsn *decode, MemOp ot)
{
    int modrm = s->modrm;
    int mod, rm;

    mod = (modrm >> 6) & 3;
    rm = (modrm & 7) | REX_B(s);
    if (mod == 3) {
        gen_op_mov_v_reg(s, ot, s->T0, rm);
    } else {
        gen_lea_modrm(s, decode);
        gen_op_ld_v(s, ot, s->T0, s->A0);
    }
}

/* generate modrm store of memory or register. */
static void gen_st_modrm(DisasContext *s, X86DecodedInsn *decode, MemOp ot)
{
    int modrm = s->modrm;
    int mod, rm;

    mod = (modrm >> 6) & 3;
    rm = (modrm & 7) | REX_B(s);
    if (mod == 3) {
        gen_op_mov_reg_v(s, ot, rm, s->T0);
    } else {
        gen_lea_modrm(s, decode);
        gen_op_st_v(s, ot, s->T0, s->A0);
    }
}

static target_ulong insn_get_addr(CPUX86State *env, DisasContext *s, MemOp ot)
{
    target_ulong ret;

    switch (ot) {
    case MO_8:
        ret = x86_ldub_code(env, s);
        break;
    case MO_16:
        ret = x86_lduw_code(env, s);
        break;
    case MO_32:
        ret = x86_ldl_code(env, s);
        break;
#ifdef TARGET_X86_64
    case MO_64:
        ret = x86_ldq_code(env, s);
        break;
#endif
    default:
        g_assert_not_reached();
    }
    return ret;
}

static inline uint32_t insn_get(CPUX86State *env, DisasContext *s, MemOp ot)
{
    uint32_t ret;

    switch (ot) {
    case MO_8:
        ret = x86_ldub_code(env, s);
        break;
    case MO_16:
        ret = x86_lduw_code(env, s);
        break;
    case MO_32:
#ifdef TARGET_X86_64
    case MO_64:
#endif
        ret = x86_ldl_code(env, s);
        break;
    default:
        g_assert_not_reached();
    }
    return ret;
}

static target_long insn_get_signed(CPUX86State *env, DisasContext *s, MemOp ot)
{
    target_long ret;

    switch (ot) {
    case MO_8:
        ret = (int8_t) x86_ldub_code(env, s);
        break;
    case MO_16:
        ret = (int16_t) x86_lduw_code(env, s);
        break;
    case MO_32:
        ret = (int32_t) x86_ldl_code(env, s);
        break;
#ifdef TARGET_X86_64
    case MO_64:
        ret = x86_ldq_code(env, s);
        break;
#endif
    default:
        g_assert_not_reached();
    }
    return ret;
}

static void gen_conditional_jump_labels(DisasContext *s, target_long diff,
                                        TCGLabel *not_taken, TCGLabel *taken)
{
    if (not_taken) {
        gen_set_label(not_taken);
    }
    gen_jmp_rel_csize(s, 0, 1);

    gen_set_label(taken);
    gen_jmp_rel(s, s->dflag, diff, 0);
}

static void gen_cmovcc(DisasContext *s, int b, TCGv dest, TCGv src)
{
    CCPrepare cc = gen_prepare_cc(s, b, NULL);

    if (!cc.use_reg2) {
        cc.reg2 = tcg_constant_tl(cc.imm);
    }

    tcg_gen_movcond_tl(cc.cond, dest, cc.reg, cc.reg2, src, dest);
}

static void gen_op_movl_seg_real(DisasContext *s, X86Seg seg_reg, TCGv seg)
{
    TCGv selector = tcg_temp_new();
#if defined(XBOX)
    /*
     * The real-mode segment load writes segs[].selector inline, with no
     * helper call to flush behind. A deferred FDP/FCS update reads the
     * selector at its flush point, so land it before the value moves, and
     * forget which selector env->fpds holds.
     */
    xemu_x87_flush_ptrs(s);
    s->xemu_fds_seg = -1;
#endif
    tcg_gen_ext16u_tl(selector, seg);
    tcg_gen_st32_tl(selector, tcg_env,
                    offsetof(CPUX86State,segs[seg_reg].selector));
    tcg_gen_shli_tl(cpu_seg_base[seg_reg], selector, 4);
}

/* move SRC to seg_reg and compute if the CPU state may change. Never
   call this function with seg_reg == R_CS */
static void gen_movl_seg(DisasContext *s, X86Seg seg_reg, TCGv src, bool inhibit_irq)
{
    if (PE(s) && !VM86(s)) {
        TCGv_i32 sel = tcg_temp_new_i32();

        tcg_gen_trunc_tl_i32(sel, src);
        gen_helper_load_seg(tcg_env, tcg_constant_i32(seg_reg), sel);

        /*
         * For moves to SS, the SS32 flag may change. For CODE32 only, changes
         * to SS, DS and ES may change the ADDSEG flags.
         */
        if (seg_reg == R_SS || (CODE32(s) && seg_reg < R_FS)) {
            s->base.is_jmp = DISAS_EOB_NEXT;
        }
    } else {
        gen_op_movl_seg_real(s, seg_reg, src);
    }

    /*
     * For MOV or POP to SS (but not LSS) translation must always
     * stop as a special handling must be done to disable hardware
     * interrupts for the next instruction.
     *
     * This is the last instruction, so it's okay to overwrite
     * HF_TF_MASK; the next TB will start with the flag set.
     *
     * DISAS_EOB_INHIBIT_IRQ is a superset of DISAS_EOB_NEXT which
     * might have been set above.
     */
    if (inhibit_irq) {
        s->base.is_jmp = DISAS_EOB_INHIBIT_IRQ;
        s->flags &= ~HF_TF_MASK;
    }
}

static void gen_far_call(DisasContext *s)
{
    TCGv_i32 new_cs = tcg_temp_new_i32();
    tcg_gen_trunc_tl_i32(new_cs, s->T1);
    if (PE(s) && !VM86(s)) {
        gen_helper_lcall_protected(tcg_env, new_cs, s->T0,
                                   tcg_constant_i32(s->dflag - 1),
                                   eip_next_tl(s));
    } else {
        TCGv_i32 new_eip = tcg_temp_new_i32();
        tcg_gen_trunc_tl_i32(new_eip, s->T0);
        gen_helper_lcall_real(tcg_env, new_cs, new_eip,
                              tcg_constant_i32(s->dflag - 1),
                              eip_next_i32(s));
    }
    s->base.is_jmp = DISAS_JUMP;
}

static void gen_far_jmp(DisasContext *s)
{
    if (PE(s) && !VM86(s)) {
        TCGv_i32 new_cs = tcg_temp_new_i32();
        tcg_gen_trunc_tl_i32(new_cs, s->T1);
        gen_helper_ljmp_protected(tcg_env, new_cs, s->T0,
                                  eip_next_tl(s));
    } else {
        gen_op_movl_seg_real(s, R_CS, s->T1);
        gen_op_jmp_v(s, s->T0);
    }
    s->base.is_jmp = DISAS_JUMP;
}

static void gen_svm_check_intercept(DisasContext *s, uint32_t type)
{
    /* no SVM activated; fast case */
    if (likely(!GUEST(s))) {
        return;
    }
    gen_helper_svm_check_intercept(tcg_env, tcg_constant_i32(type));
}

static inline void gen_stack_update(DisasContext *s, int addend)
{
    gen_op_add_reg_im(s, mo_stacksize(s), R_ESP, addend);
}

static void gen_lea_ss_ofs(DisasContext *s, TCGv dest, TCGv src, target_ulong offset)
{
    if (offset) {
        tcg_gen_addi_tl(dest, src, offset);
        src = dest;
    }
    gen_lea_v_seg_dest(s, mo_stacksize(s), dest, src, R_SS, -1);
}

/* Generate a push. It depends on ss32, addseg and dflag.  */
static void gen_push_v(DisasContext *s, TCGv val)
{
    MemOp d_ot = mo_pushpop(s, s->dflag);
    MemOp a_ot = mo_stacksize(s);
    int size = 1 << d_ot;
    TCGv new_esp = tcg_temp_new();

    tcg_gen_subi_tl(new_esp, cpu_regs[R_ESP], size);

    /* Now reduce the value to the address size and apply SS base.  */
    gen_lea_ss_ofs(s, s->A0, new_esp, 0);
    gen_op_st_v(s, d_ot, val, s->A0);
    gen_op_mov_reg_v(s, a_ot, R_ESP, new_esp);
}

/* two step pop is necessary for precise exceptions */
static MemOp gen_pop_T0(DisasContext *s)
{
    MemOp d_ot = mo_pushpop(s, s->dflag);

    gen_lea_ss_ofs(s, s->T0, cpu_regs[R_ESP], 0);
    gen_op_ld_v(s, d_ot, s->T0, s->T0);

    return d_ot;
}

static inline void gen_pop_update(DisasContext *s, MemOp ot)
{
    gen_stack_update(s, 1 << ot);
}

static void gen_pusha(DisasContext *s)
{
    MemOp d_ot = s->dflag;
    int size = 1 << d_ot;
    int i;

    for (i = 0; i < 8; i++) {
        gen_lea_ss_ofs(s, s->A0, cpu_regs[R_ESP], (i - 8) * size);
        gen_op_st_v(s, d_ot, cpu_regs[7 - i], s->A0);
    }

    gen_stack_update(s, -8 * size);
}

static void gen_popa(DisasContext *s)
{
    MemOp d_ot = s->dflag;
    int size = 1 << d_ot;
    int i;

    for (i = 0; i < 8; i++) {
        /* ESP is not reloaded */
        if (7 - i == R_ESP) {
            continue;
        }
        gen_lea_ss_ofs(s, s->A0, cpu_regs[R_ESP], i * size);
        gen_op_ld_v(s, d_ot, s->T0, s->A0);
        gen_op_mov_reg_v(s, d_ot, 7 - i, s->T0);
    }

    gen_stack_update(s, 8 * size);
}

static void gen_enter(DisasContext *s, int esp_addend, int level)
{
    MemOp d_ot = mo_pushpop(s, s->dflag);
    MemOp a_ot = mo_stacksize(s);
    int size = 1 << d_ot;

    /* Push BP; compute FrameTemp into T1.  */
    tcg_gen_subi_tl(s->T1, cpu_regs[R_ESP], size);
    gen_lea_ss_ofs(s, s->A0, s->T1, 0);
    gen_op_st_v(s, d_ot, cpu_regs[R_EBP], s->A0);

    level &= 31;
    if (level != 0) {
        int i;
        if (level > 1) {
            TCGv fp = tcg_temp_new();

            /* Copy level-1 pointers from the previous frame.  */
            for (i = 1; i < level; ++i) {
                gen_lea_ss_ofs(s, s->A0, cpu_regs[R_EBP], -size * i);
                gen_op_ld_v(s, d_ot, fp, s->A0);

                gen_lea_ss_ofs(s, s->A0, s->T1, -size * i);
                gen_op_st_v(s, d_ot, fp, s->A0);
            }
        }

        /* Push the current FrameTemp as the last level.  */
        gen_lea_ss_ofs(s, s->A0, s->T1, -size * level);
        gen_op_st_v(s, d_ot, s->T1, s->A0);
    }

    /* Copy the FrameTemp value to EBP.  */
    gen_op_mov_reg_v(s, d_ot, R_EBP, s->T1);

    /* Compute the final value of ESP.  */
    tcg_gen_subi_tl(s->T1, s->T1, esp_addend + size * level);
    gen_op_mov_reg_v(s, a_ot, R_ESP, s->T1);
}

static void gen_leave(DisasContext *s)
{
    MemOp d_ot = mo_pushpop(s, s->dflag);
    MemOp a_ot = mo_stacksize(s);

    gen_lea_ss_ofs(s, s->A0, cpu_regs[R_EBP], 0);
    gen_op_ld_v(s, d_ot, s->T0, s->A0);

    tcg_gen_addi_tl(s->T1, cpu_regs[R_EBP], 1 << d_ot);

    gen_op_mov_reg_v(s, d_ot, R_EBP, s->T0);
    gen_op_mov_reg_v(s, a_ot, R_ESP, s->T1);
}

/* Similarly, except that the assumption here is that we don't decode
   the instruction at all -- either a missing opcode, an unimplemented
   feature, or just a bogus instruction stream.  */
static void gen_unknown_opcode(CPUX86State *env, DisasContext *s)
{
    gen_illegal_opcode(s);

    if (qemu_loglevel_mask(LOG_UNIMP)) {
        FILE *logfile = qemu_log_trylock();
        if (logfile) {
            target_ulong pc = s->base.pc_next, end = s->pc;

            fprintf(logfile, "ILLOPC: " TARGET_FMT_lx ":", pc);
            for (; pc < end; ++pc) {
                fprintf(logfile, " %02x", translator_ldub(env, &s->base, pc));
            }
            fprintf(logfile, "\n");
            qemu_log_unlock(logfile);
        }
    }
}

/* an interrupt is different from an exception because of the
   privilege checks */
static void gen_interrupt(DisasContext *s, uint8_t intno)
{
    gen_update_cc_op(s);
    gen_update_eip_cur(s);
    gen_helper_raise_interrupt(tcg_env, tcg_constant_i32(intno),
                               cur_insn_len_i32(s));
    s->base.is_jmp = DISAS_NORETURN;
}

/* Clear BND registers during legacy branches.  */
static void gen_bnd_jmp(DisasContext *s)
{
    /* Clear the registers only if BND prefix is missing, MPX is enabled,
       and if the BNDREGs are known to be in use (non-zero) already.
       The helper itself will check BNDPRESERVE at runtime.  */
    if ((s->prefix & PREFIX_REPNZ) == 0
        && (s->flags & HF_MPX_EN_MASK) != 0
        && (s->flags & HF_MPX_IU_MASK) != 0) {
        gen_helper_bnd_jmp(tcg_env);
    }
}

/*
 * Generate an end of block, including common tasks such as generating
 * single step traps, resetting the RF flag, and handling the interrupt
 * shadow.
 */
#if defined(XBOX)
/*
 * Inline near-return memo probe (phase 2). Emitted at ret exits ahead
 * of the helper fallback. Validation against the ret site's
 * translate-time context constants is exact: nothing between TB entry
 * and its ret exit changes cs_base/flags/cflags, so the runtime
 * context at this exit equals this TB's own translation context. The
 * live tb->cflags load catches CF_INVALID (invalidation); flushes
 * clear entries (wholesale or per page), and empty entries fail the tb != NULL
 * check. Misses fall through to the helper, which also owns every
 * special case (breakpoints, single-step, logging, epilogue return).
 */
static void xemu_gen_inline_ret_memo(DisasContext *s)
{
    TCGLabel *slow = gen_new_label();
    TCGv_i32 eip = tcg_temp_new_i32();
    TCGv_i32 idx = tcg_temp_new_i32();
    TCGv_i32 t32 = tcg_temp_new_i32();
    TCGv_ptr ep = tcg_temp_new_ptr();
    TCGv_ptr tbp = tcg_temp_new_ptr();
    TCGv_ptr tcp = tcg_temp_new_ptr();

    tcg_gen_mov_i32(eip, cpu_eip);
    tcg_gen_muli_i32(idx, eip, (int32_t)2654435761u);
    tcg_gen_shri_i32(idx, idx, 32 - XEMU_RETC_BITS);
    tcg_gen_shli_i32(idx, idx, 5); /* 32-byte entry stride */
    tcg_gen_ext_i32_ptr(ep, idx);
    tcg_gen_add_ptr(ep, ep, tcg_env);

    tcg_gen_ld_i32(t32, ep,
                   offsetof(CPUX86State, xemu_retc) +
                   offsetof(struct XemuRetcEntry, eip));
    tcg_gen_brcond_i32(TCG_COND_NE, t32, eip, slow);
    tcg_gen_ld_i32(t32, ep,
                   offsetof(CPUX86State, xemu_retc) +
                   offsetof(struct XemuRetcEntry, flags));
    tcg_gen_brcondi_i32(TCG_COND_NE, t32, (int32_t)s->base.tb->flags, slow);
    tcg_gen_ld_i32(t32, ep,
                   offsetof(CPUX86State, xemu_retc) +
                   offsetof(struct XemuRetcEntry, cs_base));
    tcg_gen_brcondi_i32(TCG_COND_NE, t32, (int32_t)s->cs_base, slow);
    tcg_gen_ld_ptr(tbp, ep,
                   offsetof(CPUX86State, xemu_retc) +
                   offsetof(struct XemuRetcEntry, tb));
    tcg_gen_brcondi_ptr(TCG_COND_EQ, tbp, 0, slow);
    tcg_gen_ld_i32(t32, tbp, offsetof(TranslationBlock, cflags));
    tcg_gen_brcondi_i32(TCG_COND_NE, t32,
                        (int32_t)tb_cflags(s->base.tb), slow);

    tcg_gen_ld_ptr(tcp, tbp, offsetof(TranslationBlock, tc) +
                             offsetof(struct tb_tc, ptr));
    tcg_gen_xemu_goto_ptr(tcp);

    gen_set_label(slow);
}
#endif

static void
gen_eob(DisasContext *s, int mode)
{
    bool inhibit_reset;

    gen_update_cc_op(s);

    /* If several instructions disable interrupts, only the first does it.  */
    inhibit_reset = false;
    if (s->flags & HF_INHIBIT_IRQ_MASK) {
        gen_reset_hflag(s, HF_INHIBIT_IRQ_MASK);
        inhibit_reset = true;
    } else if (mode == DISAS_EOB_INHIBIT_IRQ) {
        gen_set_hflag(s, HF_INHIBIT_IRQ_MASK);
    }

    if (s->flags & HF_RF_MASK) {
        gen_reset_eflags(s, RF_MASK);
    }
    if (mode == DISAS_EOB_RECHECK_TF) {
        gen_helper_rechecking_single_step(tcg_env);
        tcg_gen_exit_tb(NULL, 0);
    } else if (s->flags & HF_TF_MASK) {
        gen_helper_single_step(tcg_env);
    } else if (mode == DISAS_JUMP &&
               /* give irqs a chance to happen */
               !inhibit_reset) {
#if defined(XBOX)
        if (unlikely(xemu_inv_prof_on())) {
            if (s->xemu_ret_exit) {
                xemu_inv_retmemo_emitted++;
            } else {
                xemu_inv_jcprobe_emitted++;
            }
        }
        /*
         * The inline probes accept a target whose cflags equal this TB's.
         * A one-shot TB (cpu_io_recompile, SMC: CF_NOIRQ plus a count) must
         * not reach another one that way: it skips the icount budget check.
         * Under -d nochain (CF_NO_GOTO_TB) every exit goes through the
         * helper, so -d exec,nochain logs every TB.
         */
        bool xemu_probe_ok =
            !(tb_cflags(s->base.tb) &
              (CF_NO_GOTO_TB | CF_NO_GOTO_PTR | CF_NOIRQ | CF_COUNT_MASK));
        if (s->xemu_ret_exit) {
            s->xemu_ret_exit = false;
            if (xemu_probe_ok) {
                xemu_gen_inline_ret_memo(s);
            }
            tcg_gen_xemu_lookup_ret_and_goto_ptr();
        } else {
            if (xemu_probe_ok) {
                TCGv_i32 lin = tcg_temp_new_i32();
                TCGv_i32 flags =
                    tcg_constant_i32((int32_t)s->base.tb->flags);
                tcg_gen_addi_i32(lin, cpu_eip, (int32_t)s->cs_base);
                if (s->xemu_fldcw_exit) {
                    s->xemu_fldcw_exit = false;
                    /*
                     * fldcw rewrote HF_FPU_RC/PC (cpu_set_fpuc). Every
                     * other flag bit is still this TB's, so take those
                     * two from env->hflags and the rest as a constant.
                     */
                    const uint32_t fpu = HF_FPU_RC_MASK | HF_FPU_PC_MASK;
                    flags = tcg_temp_new_i32();
                    tcg_gen_ld_i32(flags, tcg_env,
                                   offsetof(CPUX86State, hflags));
                    tcg_gen_andi_i32(flags, flags, fpu);
                    tcg_gen_ori_i32(flags, flags,
                                    (int32_t)(s->base.tb->flags & ~fpu));
                }
                tcg_gen_xemu_jc_probe_and_goto_ptr(
                    lin,
                    offsetof(X86CPU, parent_obj.tb_jmp_cache) -
                        offsetof(X86CPU, env),
                    (uint64_t)s->cs_base, flags, tb_cflags(s->base.tb));
            }
            tcg_gen_lookup_and_goto_ptr();
        }
#else
        tcg_gen_lookup_and_goto_ptr();
#endif
    } else {
        tcg_gen_exit_tb(NULL, 0);
    }

    s->base.is_jmp = DISAS_NORETURN;
}

/* Jump to eip+diff, truncating the result to OT. */
static void gen_jmp_rel(DisasContext *s, MemOp ot, int diff, int tb_num)
{
    bool use_goto_tb = s->jmp_opt;
    target_ulong mask = -1;
    target_ulong new_pc = s->pc + diff;
    target_ulong new_eip = new_pc - s->cs_base;

    assert(!s->cc_op_dirty);

    /* In 64-bit mode, operand size is fixed at 64 bits. */
    if (!CODE64(s)) {
        if (ot == MO_16) {
            mask = 0xffff;
            if (tb_cflags(s->base.tb) & CF_PCREL && CODE32(s)) {
                use_goto_tb = false;
            }
        } else {
            mask = 0xffffffff;
        }
    }
    new_eip &= mask;

#if defined(XBOX)
    /*
     * A cross-page DIRECT jump: gen_jmp_rel only ever emits direct relative
     * jumps, so any goto_tb candidate whose target is off the source page is
     * exactly the "other" exit-census slice this campaign targets. Captured
     * before the CF_PCREL block below can clear use_goto_tb.
     */
    bool xpage_cpd = use_goto_tb && !translator_is_same_page(&s->base, new_pc);
#endif

    if (tb_cflags(s->base.tb) & CF_PCREL) {
        tcg_gen_addi_tl(cpu_eip, cpu_eip, new_pc - s->pc_save);
        /*
         * If we can prove the branch does not leave the page and we have
         * no extra masking to apply (data16 branch in code32, see above),
         * then we have also proven that the addition does not wrap.
         */
        if (!use_goto_tb || !translator_is_same_page(&s->base, new_pc)) {
            tcg_gen_andi_tl(cpu_eip, cpu_eip, mask);
#if defined(XBOX)
            /*
             * Cross-page relaxation: the andi above keeps cpu_eip wrap-correct
             * regardless, so only the refuter forbids the direct chain
             * (translator_use_goto_tb agrees below). Always on (broad, since
             * 2026-07-12); see docs/Finished/xpage-design.md.
             */
            if (xemu_xpage_refute_on()) {
                use_goto_tb = false;
            }
#else
            use_goto_tb = false;
#endif
        }
    } else if (!CODE64(s)) {
        new_pc = (uint32_t)(new_eip + s->cs_base);
    }

#if defined(XBOX)
    /*
     * Gate-0 taken counter + Gate-1 refuter, emitted into the source TB body
     * so they run on every taken cross-page-direct exit whether or not it is
     * chained. Under CF_PCREL cpu_eip already holds the target here (addi +
     * wrap andi), so the refuter helper can read env->eip.
     */
    if (xpage_cpd) {
        if (unlikely(xemu_xpage_prof_on())) {
            /* Masked 32-bit linear target: kernel identity window
             * (>= 0x80000000) vs low, wrap-safe. */
            uint64_t *cntp = (((uint64_t)new_pc & 0xffffffffULL) >= 0x80000000ULL)
                ? &xemu_xpage_taken_kernel : &xemu_xpage_taken_low;
            TCGv_ptr cnt = tcg_constant_ptr(cntp);
            TCGv_i64 tcnt = tcg_temp_new_i64();
            xemu_xpage_emitted++;
            tcg_gen_ld_i64(tcnt, cnt, 0);
            tcg_gen_addi_i64(tcnt, tcnt, 1);
            tcg_gen_st_i64(tcnt, cnt, 0);
        }
        if (unlikely(xemu_xpage_refute_on()) &&
            (tb_cflags(s->base.tb) & CF_PCREL)) {
            gen_helper_xemu_xpage_refute(tcg_env, tcg_constant_i64(s->cs_base));
        }
    }
#endif

#if defined(XBOX)
    /*
     * A target in the last X86_MAX_INSN_LENGTH - 1 bytes of its page can
     * start with an insn that crosses into the next page, and
     * cpu_exec_loop never chains into a TB that spans two pages (its
     * tb_page_addr1 guard), so a goto_tb slot to it would leave through
     * the dispatch loop every time. Reach such targets through gen_eob's
     * jump-cache probe and helper_lookup_tb_ptr instead: both may enter a
     * spanning TB, because tlb_flush_page also clears the jump cache for
     * the page before. This covers the fall-through out of a TB that
     * stopped before a page-crossing insn too (i386_tr_translate_insn,
     * case 2).
     */
    if (((uint32_t)new_pc & ~TARGET_PAGE_MASK) >
        TARGET_PAGE_SIZE - X86_MAX_INSN_LENGTH) {
        use_goto_tb = false;
    }
#endif

    if (use_goto_tb && translator_use_goto_tb(&s->base, new_pc)) {
        /* jump to same page: we can use a direct jump */
#if defined(XBOX)
        if (xpage_cpd) {
            /*
             * Relaxed cross-page slot emitted: mark the source so
             * tb_add_jump registers every dest it links in the backstop
             * registry (tb-maint.c), regardless of the phys-page
             * comparison — a virtual-alias target can share the source's
             * phys page and would evade a phys-only test. This flag is
             * the only thing that registers same-page dests; same-page
             * slots from non-emitters stay chained across flushes, as
             * upstream's do.
             */
            s->base.tb->xemu_xpage_reg |= XEMU_XPAGE_TB_CROSS_EMITTER;
        }
        if (unlikely(xemu_inv_prof_on())) {
            xemu_inv_gototb_emitted++;
        }
#endif
        tcg_gen_goto_tb(tb_num);
        if (!(tb_cflags(s->base.tb) & CF_PCREL)) {
            tcg_gen_movi_tl(cpu_eip, new_eip);
        }
        tcg_gen_exit_tb(s->base.tb, tb_num);
        s->base.is_jmp = DISAS_NORETURN;
    } else {
        if (!(tb_cflags(s->base.tb) & CF_PCREL)) {
            tcg_gen_movi_tl(cpu_eip, new_eip);
        }
        if (s->jmp_opt) {
            gen_eob(s, DISAS_JUMP);   /* jump to another page */
        } else {
            gen_eob(s, DISAS_EOB_ONLY);  /* exit to main loop */
        }
    }
}

/* Jump to eip+diff, truncating to the current code size. */
static void gen_jmp_rel_csize(DisasContext *s, int diff, int tb_num)
{
    /* CODE64 ignores the OT argument, so we need not consider it. */
    gen_jmp_rel(s, CODE32(s) ? MO_32 : MO_16, diff, tb_num);
}

static inline void gen_ldq_env_A0(DisasContext *s, int offset)
{
    tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0, s->mem_index, MO_LEUQ);
    tcg_gen_st_i64(s->tmp1_i64, tcg_env, offset);
}

static inline void gen_stq_env_A0(DisasContext *s, int offset)
{
    tcg_gen_ld_i64(s->tmp1_i64, tcg_env, offset);
    tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0, s->mem_index, MO_LEUQ);
}

static inline void gen_ldo_env_A0(DisasContext *s, int offset, bool align)
{
    MemOp atom = (s->cpuid_ext_features & CPUID_EXT_AVX
                  ? MO_ATOM_IFALIGN : MO_ATOM_IFALIGN_PAIR);
    MemOp mop = MO_128 | MO_LE | atom | (align ? MO_ALIGN_16 : 0);
    int mem_index = s->mem_index;
    TCGv_i128 t = tcg_temp_new_i128();

    tcg_gen_qemu_ld_i128(t, s->A0, mem_index, mop);
    tcg_gen_st_i128(t, tcg_env, offset);
}

static inline void gen_sto_env_A0(DisasContext *s, int offset, bool align)
{
    MemOp atom = (s->cpuid_ext_features & CPUID_EXT_AVX
                  ? MO_ATOM_IFALIGN : MO_ATOM_IFALIGN_PAIR);
    MemOp mop = MO_128 | MO_LE | atom | (align ? MO_ALIGN_16 : 0);
    int mem_index = s->mem_index;
    TCGv_i128 t = tcg_temp_new_i128();

    tcg_gen_ld_i128(t, tcg_env, offset);
    tcg_gen_qemu_st_i128(t, s->A0, mem_index, mop);
}

static void gen_ldy_env_A0(DisasContext *s, int offset, bool align)
{
    MemOp mop = MO_128 | MO_LE | MO_ATOM_IFALIGN_PAIR;
    int mem_index = s->mem_index;
    TCGv_i128 t0 = tcg_temp_new_i128();
    TCGv_i128 t1 = tcg_temp_new_i128();
    TCGv a0_hi = tcg_temp_new();

    tcg_gen_qemu_ld_i128(t0, s->A0, mem_index, mop | (align ? MO_ALIGN_32 : 0));
    tcg_gen_addi_tl(a0_hi, s->A0, 16);
    tcg_gen_qemu_ld_i128(t1, a0_hi, mem_index, mop);

    tcg_gen_st_i128(t0, tcg_env, offset + offsetof(YMMReg, YMM_X(0)));
    tcg_gen_st_i128(t1, tcg_env, offset + offsetof(YMMReg, YMM_X(1)));
}

static void gen_sty_env_A0(DisasContext *s, int offset, bool align)
{
    MemOp mop = MO_128 | MO_LE | MO_ATOM_IFALIGN_PAIR;
    int mem_index = s->mem_index;
    TCGv_i128 t = tcg_temp_new_i128();
    TCGv a0_hi = tcg_temp_new();

    tcg_gen_ld_i128(t, tcg_env, offset + offsetof(YMMReg, YMM_X(0)));
    tcg_gen_qemu_st_i128(t, s->A0, mem_index, mop | (align ? MO_ALIGN_32 : 0));
    tcg_gen_addi_tl(a0_hi, s->A0, 16);
    tcg_gen_ld_i128(t, tcg_env, offset + offsetof(YMMReg, YMM_X(1)));
    tcg_gen_qemu_st_i128(t, a0_hi, mem_index, mop);
}

#if defined(XBOX) && defined(CONFIG_SYSTEM_ONLY)
/*
 * Deterministic mode (icount): the Xbox kernel's idle thread never executes
 * HLT. KiIdleLoop spins on "sti; nop; nop; cli", checks the DPC list and
 * the next-thread pointer, and goes round again; every STI ends a TB, so
 * under icount each idle virtual microsecond costs hundreds of trips through
 * the TCG loop and idle time, not game work, bounds fast-forward. Both exit
 * conditions only change in interrupt handlers, so while they are false and
 * nothing is pending, halting until the next interrupt (helper_xemu_ki_idle)
 * is the same as spinning until it, and icount then warps virtual time to
 * the next timer deadline at once.
 *
 * Matched on the loop's bytes, both forms the kernel has:
 *   P: sti; nop; nop; cli; cmp ebp,[ebp]; je J
 *   J == P                                    -> DPC list only
 *   J: cmp dword [ebx+0x2c],0; je P           -> DPC list and NextThread
 * Every byte must sit on the STI's page.
 */
enum { XEMU_IDLE_NONE, XEMU_IDLE_DPC, XEMU_IDLE_DPC_THREAD };

static bool xemu_idle_bytes_match(DisasContext *s, vaddr pc,
                                  const uint8_t *want, int len)
{
    for (int i = 0; i < len; i++) {
        if (want[i] != 0xff &&
            translator_ldub(s->xemu_env, &s->base, pc + i) != want[i]) {
            return false;
        }
    }
    return true;
}

static int xemu_idle_loop_kind(DisasContext *s)
{
    static const uint8_t head[] = {
        0x90, 0x90, 0xfa, 0x3b, 0x6d, 0x00, 0x74, 0xff,
    };
    static const uint8_t tail[] = { 0x83, 0x7b, 0x2c, 0x00, 0x74, 0xff };
    vaddr sti = s->pc - 1;
    vaddr page = sti & TARGET_PAGE_MASK;

    if (!icount_enabled() ||
        ((s->pc + sizeof(head) - 1) & TARGET_PAGE_MASK) != page) {
        return XEMU_IDLE_NONE;
    }
    /*
     * The TB ends at the STI, so whatever this returns rests on bytes its
     * ihash doesn't cover: keep it out of the recycler and drop it at every
     * load (tb-maint.c).
     */
    s->base.tb->xemu_xpage_reg |= XEMU_TB_PEEKS;
    if (!xemu_idle_bytes_match(s, s->pc, head, sizeof(head))) {
        return XEMU_IDLE_NONE;
    }

    vaddr j = s->pc + sizeof(head) +
              (int8_t)translator_ldub(s->xemu_env, &s->base,
                                      s->pc + sizeof(head) - 1);
    if (j == sti) {
        return XEMU_IDLE_DPC;
    }
    if ((j & TARGET_PAGE_MASK) != page ||
        ((j + sizeof(tail) - 1) & TARGET_PAGE_MASK) != page ||
        !xemu_idle_bytes_match(s, j, tail, sizeof(tail))) {
        return XEMU_IDLE_NONE;
    }
    vaddr back = j + sizeof(tail) +
                 (int8_t)translator_ldub(s->xemu_env, &s->base,
                                         j + sizeof(tail) - 1);
    return back == sti ? XEMU_IDLE_DPC_THREAD : XEMU_IDLE_NONE;
}
#endif

#include "emit.c.inc"

static void gen_x87(DisasContext *s, X86DecodedInsn *decode)
{
    bool update_fip = true;
    int b = decode->b;
    int modrm = s->modrm;
    int mod, rm, op;

    if (s->flags & (HF_EM_MASK | HF_TS_MASK)) {
        /* if CR0.EM or CR0.TS are set, generate an FPU exception */
        /* XXX: what to do if illegal op ? */
        gen_exception(s, EXCP07_PREX);
        return;
    }
#if defined(XBOX)
    if (s->xemu_x87_run < UINT8_MAX) {
        s->xemu_x87_run++;
    }
#endif
    mod = (modrm >> 6) & 3;
    rm = modrm & 7;
    op = ((b & 7) << 3) | ((modrm >> 3) & 7);
    if (mod != 3) {
        /* memory op */
        TCGv ea = gen_lea_modrm_1(s, decode->mem, false);
        TCGv last_addr = tcg_temp_new();
        bool update_fdp = true;

        tcg_gen_mov_tl(last_addr, ea);
        gen_lea_v_seg(s, ea, decode->mem.def_seg, s->override);

        switch (op) {
        case 0x00 ... 0x07: /* fxxxs */
        case 0x10 ... 0x17: /* fixxxl */
        case 0x20 ... 0x27: /* fxxxl */
        case 0x30 ... 0x37: /* fixxx */
            {
                int op1;
                op1 = op & 7;

                switch (op >> 4) {
                case 0:
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    gen_flds_FT0(s, s->tmp2_i32);
                    break;
                case 1:
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    gen_fildl_FT0(s, s->tmp2_i32);
                    break;
                case 2:
                    tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    gen_fldl_FT0(s, s->tmp1_i64);
                    break;
                case 3:
                default:
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LESW);
                    gen_fildl_FT0(s, s->tmp2_i32);
                    break;
                }

                gen_helper_fp_arith_ST0_FT0(s, op1);
                if (op1 == 3) {
                    /* fcomp needs pop */
                    gen_fpop(s);
                }
            }
            break;
        case 0x08: /* flds */
        case 0x0a: /* fsts */
        case 0x0b: /* fstps */
        case 0x18 ... 0x1b: /* fildl, fisttpl, fistl, fistpl */
        case 0x28 ... 0x2b: /* fldl, fisttpll, fstl, fstpl */
        case 0x38 ... 0x3b: /* filds, fisttps, fists, fistps */
            switch (op & 7) {
            case 0:
                switch (op >> 4) {
                case 0:
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    gen_flds_ST0(s, s->tmp2_i32);
                    break;
                case 1:
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    gen_fildl_ST0(s, s->tmp2_i32);
                    break;
                case 2:
                    tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    gen_fldl_ST0(s, s->tmp1_i64);
                    break;
                case 3:
                default:
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LESW);
                    gen_fildl_ST0(s, s->tmp2_i32);
                    break;
                }
                break;
            case 1:
                /* XXX: the corresponding CPUID bit must be tested ! */
                switch (op >> 4) {
                case 1:
                    gen_helper_fisttl_ST0(s->tmp2_i32, tcg_env);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    break;
                case 2:
                    gen_helper_fisttll_ST0(s->tmp1_i64, tcg_env);
                    tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    break;
                case 3:
                default:
                    gen_helper_fistt_ST0(s->tmp2_i32, tcg_env);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUW);
                    break;
                }
                gen_fpop(s);
                break;
            default:
                switch (op >> 4) {
                case 0:
                    gen_fsts_ST0(s, s->tmp2_i32);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    break;
                case 1:
                    gen_fistl_ST0(s, s->tmp2_i32);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    break;
                case 2:
                    gen_fstl_ST0(s, s->tmp1_i64);
                    tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    break;
                case 3:
                default:
                    gen_helper_fist_ST0(s->tmp2_i32, tcg_env);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUW);
                    break;
                }
                if ((op & 7) == 3) {
                    gen_fpop(s);
                }
                break;
            }
            break;
        case 0x0c: /* fldenv mem */
            gen_helper_fldenv(tcg_env, s->A0,
                              tcg_constant_i32(s->dflag - 1));
            update_fip = update_fdp = false;
            gen_update_eip_next(s);
            gen_eob(s, DISAS_EOB_ONLY);
            break;
        case 0x0d: /* fldcw mem */
            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                s->mem_index, MO_LEUW);
            gen_helper_fldcw(tcg_env, s->tmp2_i32);
            update_fip = update_fdp = false;
            gen_update_eip_next(s);
#if defined(XBOX)
            /*
             * The new RC/PC are part of the next TB's flags, so no
             * goto_tb; gen_eob's probe compares them as computed at
             * runtime, and helper_lookup_tb_ptr reads them from env.
             */
            s->xemu_fldcw_exit = true;
            gen_eob(s, DISAS_JUMP);
#else
            gen_eob(s, DISAS_EOB_ONLY);
#endif
            break;
        case 0x0e: /* fnstenv mem */
            gen_helper_fstenv(tcg_env, s->A0,
                              tcg_constant_i32(s->dflag - 1));
            update_fip = update_fdp = false;
            break;
        case 0x0f: /* fnstcw mem */
            /*
             * Helper body is "return env->fpuc"; inline to a single
             * host ld16u. Matches the existing gen_fnstsw_inline
             * pattern and avoids a call-frame setup per uncommon but
             * not-free FPU control-word read.
             */
            tcg_gen_ld16u_i32(s->tmp2_i32, tcg_env,
                              offsetof(CPUX86State, fpuc));
            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                s->mem_index, MO_LEUW);
            update_fip = update_fdp = false;
            break;
        case 0x1d: /* fldt mem */
            gen_helper_fldt_ST0(tcg_env, s->A0);
            break;
        case 0x1f: /* fstpt mem */
            gen_helper_fstt_ST0(tcg_env, s->A0);
            gen_fpop(s);
            break;
        case 0x2c: /* frstor mem */
            gen_helper_frstor(tcg_env, s->A0,
                              tcg_constant_i32(s->dflag - 1));
            update_fip = update_fdp = false;
            /*
             * The restored control word may change precision control,
             * which the write-through x87 path reads from the TB flags.
             */
            if (g_x87_wt) {
                gen_update_eip_next(s);
                gen_eob(s, DISAS_EOB_ONLY);
            }
            break;
        case 0x2e: /* fnsave mem */
            gen_helper_fsave(tcg_env, s->A0,
                             tcg_constant_i32(s->dflag - 1));
            update_fip = update_fdp = false;
            break;
        case 0x2f: /* fnstsw mem */
            if (x87_lean()) {
                gen_fnstsw_inline(s, s->tmp2_i32);
            } else {
                gen_helper_fnstsw(s->tmp2_i32, tcg_env);
            }
            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                s->mem_index, MO_LEUW);
            update_fip = update_fdp = false;
            break;
        case 0x3c: /* fbld */
            gen_helper_fbld_ST0(tcg_env, s->A0);
            break;
        case 0x3e: /* fbstp */
            gen_helper_fbst_ST0(tcg_env, s->A0);
            gen_fpop(s);
            break;
        case 0x3d: /* fildll */
            tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                s->mem_index, MO_LEUQ);
            gen_fildll_ST0(s, s->tmp1_i64);
            break;
        case 0x3f: /* fistpll */
            gen_fistll_ST0(s, s->tmp1_i64);
            tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                s->mem_index, MO_LEUQ);
            gen_fpop(s);
            break;
        default:
            goto illegal_op;
        }

        if (update_fdp) {
            int last_seg = s->override >= 0 ? s->override : decode->mem.def_seg;
            bool emit_now = true;
            bool emit_fds = true;

#if defined(XBOX)
            if (unlikely(xemu_x87_census_on())) {
                xemu_gen_counter_inc(&xemu_x87_fdp_legacy);
            }
            if (xemu_x87_fdp_eager()) {
                emit_fds = xemu_x87_fds_needed(s, last_seg);
            } else {
                xemu_x87_record_fdp(s, last_seg, last_addr);
                emit_now = xemu_x87_refute_on();
            }
#endif
            if (emit_now) {
                if (emit_fds) {
                    tcg_gen_ld_i32(s->tmp2_i32, tcg_env,
                                   offsetof(CPUX86State,
                                            segs[last_seg].selector));
                    tcg_gen_st16_i32(s->tmp2_i32, tcg_env,
                                     offsetof(CPUX86State, fpds));
                }
                tcg_gen_st_tl(last_addr, tcg_env,
                              offsetof(CPUX86State, fpdp));
            }
        }
    } else {
        /* register float ops */
        int opreg = rm;

        switch (op) {
        case 0x08: /* fld sti */
            gen_fpush(s);
            gen_fmov_ST0_STN(s, (opreg + 1) & 7);
            break;
        case 0x09: /* fxchg sti */
        case 0x29: /* fxchg4 sti, undocumented op */
        case 0x39: /* fxchg7 sti, undocumented op */
            gen_fxchg_ST0_STN(s, opreg);
            break;
        case 0x0a: /* grp d9/2 */
            switch (rm) {
            case 0: /* fnop */
                /*
                 * check exceptions (FreeBSD FPU probe)
                 * needs to be treated as I/O because of ferr_irq
                 */
                translator_io_start(&s->base);
                gen_helper_fwait(tcg_env);
                update_fip = false;
                break;
            default:
                goto illegal_op;
            }
            break;
        case 0x0c: /* grp d9/4 */
            switch (rm) {
            case 0: /* fchs */
                gen_fchs_ST0(s);
                break;
            case 1: /* fabs */
                gen_fabs_ST0(s);
                break;
            case 4: /* ftst */
                gen_fldz_FT0(s);
                gen_fcom_ST0_FT0(s);
                break;
            case 5: /* fxam */
                gen_helper_fxam_ST0(tcg_env);
                break;
            default:
                goto illegal_op;
            }
            break;
        case 0x0d: /* grp d9/5 */
            {
                switch (rm) {
                case 0:
                    gen_fpush(s);
                    gen_fld1_ST0(s);
                    break;
                case 1:
                    gen_fpush(s);
                    gen_helper_fldl2t_ST0(tcg_env);
                    break;
                case 2:
                    gen_fpush(s);
                    gen_helper_fldl2e_ST0(tcg_env);
                    break;
                case 3:
                    gen_fpush(s);
                    gen_helper_fldpi_ST0(tcg_env);
                    break;
                case 4:
                    gen_fpush(s);
                    gen_helper_fldlg2_ST0(tcg_env);
                    break;
                case 5:
                    gen_fpush(s);
                    gen_helper_fldln2_ST0(tcg_env);
                    break;
                case 6:
                    gen_fpush(s);
                    gen_fldz_ST0(s);
                    break;
                default:
                    goto illegal_op;
                }
            }
            break;
        case 0x0e: /* grp d9/6 */
            switch (rm) {
            case 0: /* f2xm1 */
                gen_helper_f2xm1(tcg_env);
                break;
            case 1: /* fyl2x */
                gen_helper_fyl2x(tcg_env);
                break;
            case 2: /* fptan */
                gen_helper_fptan(tcg_env);
                break;
            case 3: /* fpatan */
                gen_helper_fpatan(tcg_env);
                break;
            case 4: /* fxtract */
                gen_helper_fxtract(tcg_env);
                break;
            case 5: /* fprem1 */
                gen_helper_fprem1(tcg_env);
                break;
            case 6: /* fdecstp */
            {
                tcg_gen_subi_i32(fpstt, fpstt, 1);
                tcg_gen_andi_i32(fpstt, fpstt, 7);
                s->fpstt_delta -= 1;
                TCGv_i32 fpus_tmp = tcg_temp_new_i32();
                tcg_gen_ld16u_i32(fpus_tmp, tcg_env, offsetof(CPUX86State, fpus));
                tcg_gen_andi_i32(fpus_tmp, fpus_tmp, ~0x4700);
                tcg_gen_st16_i32(fpus_tmp, tcg_env, offsetof(CPUX86State, fpus));
                break;
            }
            default:
            case 7: /* fincstp */
            {
                tcg_gen_addi_i32(fpstt, fpstt, 1);
                tcg_gen_andi_i32(fpstt, fpstt, 7);
                s->fpstt_delta += 1;
                TCGv_i32 fpus_tmp = tcg_temp_new_i32();
                tcg_gen_ld16u_i32(fpus_tmp, tcg_env, offsetof(CPUX86State, fpus));
                tcg_gen_andi_i32(fpus_tmp, fpus_tmp, ~0x4700);
                tcg_gen_st16_i32(fpus_tmp, tcg_env, offsetof(CPUX86State, fpus));
                break;
            }
            }
            break;
        case 0x0f: /* grp d9/7 */
            switch (rm) {
            case 0: /* fprem */
                gen_helper_fprem(tcg_env);
                break;
            case 1: /* fyl2xp1 */
                gen_helper_fyl2xp1(tcg_env);
                break;
            case 2: /* fsqrt */
                gen_fsqrt(s);
                break;
            case 3: /* fsincos */
                gen_helper_fsincos(tcg_env);
                break;
            case 4: /* frndint */
                gen_helper_frndint(tcg_env);
                break;
            case 5: /* fscale */
                gen_helper_fscale(tcg_env);
                break;
            case 6: /* fsin */
                gen_fsin(s);
                break;
            default:
            case 7: /* fcos */
                gen_fcos(s);
                break;
            }
            break;
        case 0x00: case 0x01: case 0x04 ... 0x07: /* fxxx st, sti */
        case 0x20: case 0x21: case 0x24 ... 0x27: /* fxxx sti, st */
        case 0x30: case 0x31: case 0x34 ... 0x37: /* fxxxp sti, st */
            {
                int op1;

                op1 = op & 7;
                if (op >= 0x20) {
                    gen_helper_fp_arith_STN_ST0(s, op1, opreg);
                    if (op >= 0x30) {
                        gen_fpop(s);
                    }
                } else {
                    gen_fload_FT0_STN(s, opreg);
                    gen_helper_fp_arith_ST0_FT0(s, op1);
                }
            }
            break;
        case 0x02: /* fcom */
        case 0x22: /* fcom2, undocumented op */
            gen_fload_FT0_STN(s, opreg);
            gen_fcom_ST0_FT0(s);
            break;
        case 0x03: /* fcomp */
        case 0x23: /* fcomp3, undocumented op */
        case 0x32: /* fcomp5, undocumented op */
            gen_fload_FT0_STN(s, opreg);
            gen_fcom_ST0_FT0(s);
            gen_fpop(s);
            break;
        case 0x15: /* da/5 */
            switch (rm) {
            case 1: /* fucompp */
                if (!gen_fucom_wt(s, 1)) {
                    gen_fmov_FT0_STN(s, 1);
                    gen_helper_fucom_ST0_FT0(tcg_env);
                }
                gen_fpop(s);
                gen_fpop(s);
                break;
            default:
                goto illegal_op;
            }
            break;
        case 0x1c:
            switch (rm) {
            case 0: /* feni (287 only, just do nop here) */
                break;
            case 1: /* fdisi (287 only, just do nop here) */
                break;
            case 2: /* fclex */
                gen_helper_fclex(tcg_env);
                update_fip = false;
                break;
            case 3: /* fninit */
                gen_helper_fninit(tcg_env);
                update_fip = false;
                gen_update_eip_next(s);
                gen_eob(s, DISAS_EOB_ONLY);
                break;
            case 4: /* fsetpm (287 only, just do nop here) */
                break;
            default:
                goto illegal_op;
            }
            break;
        case 0x1d: /* fucomi */
        case 0x1e: /* fcomi */
            if (!(s->cpuid_features & CPUID_CMOV)) {
                goto illegal_op;
            }
            gen_fcomi_ST0_STN(s, opreg, op == 0x1d);
            break;
        case 0x28: /* ffree sti */
            gen_ffree_STN(s, opreg);
            break;
        case 0x2a: /* fst sti */
            gen_fmov_STN_ST0(s, opreg);
            break;
        case 0x2b: /* fstp sti */
        case 0x0b: /* fstp1 sti, undocumented op */
        case 0x3a: /* fstp8 sti, undocumented op */
        case 0x3b: /* fstp9 sti, undocumented op */
            gen_fmov_STN_ST0(s, opreg);
            gen_fpop(s);
            break;
        case 0x2c: /* fucom st(i) */
        case 0x2d: /* fucomp st(i) */
            if (!gen_fucom_wt(s, opreg)) {
                gen_fmov_FT0_STN(s, opreg);
                gen_helper_fucom_ST0_FT0(tcg_env);
            }
            if (op == 0x2d) {
                gen_fpop(s);
            }
            break;
        case 0x33: /* de/3 */
            switch (rm) {
            case 1: /* fcompp */
                gen_fload_FT0_STN(s, 1);
                gen_fcom_ST0_FT0(s);
                gen_fpop(s);
                gen_fpop(s);
                break;
            default:
                goto illegal_op;
            }
            break;
        case 0x38: /* ffreep sti, undocumented op */
            gen_ffree_STN(s, opreg);
            gen_fpop(s);
            break;
        case 0x3c: /* df/4 */
            switch (rm) {
            case 0:
                if (x87_lean()) {
                    gen_fnstsw_inline(s, s->tmp2_i32);
                } else {
                    gen_helper_fnstsw(s->tmp2_i32, tcg_env);
                }
                tcg_gen_extu_i32_tl(s->T0, s->tmp2_i32);
                gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
                break;
            default:
                goto illegal_op;
            }
            break;
        case 0x3d: /* fucomip */
        case 0x3e: /* fcomip */
            if (!(s->cpuid_features & CPUID_CMOV)) {
                goto illegal_op;
            }
            gen_fcomi_ST0_STN(s, opreg, op == 0x3d);
            gen_fpop(s);
            break;
        case 0x10 ... 0x13: /* fcmovxx */
        case 0x18 ... 0x1b:
            {
                int op1;
                TCGLabel *l1;
                static const uint8_t fcmov_cc[8] = {
                    (JCC_B << 1),
                    (JCC_Z << 1),
                    (JCC_BE << 1),
                    (JCC_P << 1),
                };

                if (!(s->cpuid_features & CPUID_CMOV)) {
                    goto illegal_op;
                }
                op1 = fcmov_cc[op & 3] | (((op >> 3) & 1) ^ 1);
                if (gen_fcmov_wt(s, op1, opreg)) {
                    break;
                }
                l1 = gen_new_label();
                gen_jcc_noeob(s, op1, l1);
                gen_fmov_ST0_STN(s, opreg);
                gen_set_label(l1);
            }
            break;
        default:
            goto illegal_op;
        }
    }

    if (update_fip) {
        bool emit_now = true;

#if defined(XBOX)
        if (unlikely(xemu_x87_census_on())) {
            xemu_gen_counter_inc(&xemu_x87_fip_legacy);
        }
        xemu_x87_record_fip(s, eip_cur_tl(s));
        emit_now = xemu_x87_refute_on();
#endif
        if (emit_now) {
            tcg_gen_ld_i32(s->tmp2_i32, tcg_env,
                           offsetof(CPUX86State, segs[R_CS].selector));
            tcg_gen_st16_i32(s->tmp2_i32, tcg_env,
                             offsetof(CPUX86State, fpcs));
            tcg_gen_st_tl(eip_cur_tl(s),
                          tcg_env, offsetof(CPUX86State, fpip));
        }
    }
    return;

 illegal_op:
    gen_illegal_opcode(s);
}

static void gen_multi0F(DisasContext *s, X86DecodedInsn *decode)
{
    int prefixes = s->prefix;
    MemOp dflag = s->dflag;
    int b = decode->b + 0x100;
    int modrm = s->modrm;
    MemOp ot;
    int reg, rm, mod, op;

    /* now check op code */
    switch (b) {
    case 0x1c7: /* RDSEED, RDPID with f3 prefix */
        mod = (modrm >> 6) & 3;
        switch ((modrm >> 3) & 7) {
        case 7:
            if (mod != 3 ||
                (s->prefix & PREFIX_REPNZ)) {
                goto illegal_op;
            }
            if (s->prefix & PREFIX_REPZ) {
                if (!(s->cpuid_7_0_ecx_features & CPUID_7_0_ECX_RDPID)) {
                    goto illegal_op;
                }
                gen_helper_rdpid(s->T0, tcg_env);
                rm = (modrm & 7) | REX_B(s);
                gen_op_mov_reg_v(s, dflag, rm, s->T0);
                break;
            } else {
                if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_RDSEED)) {
                    goto illegal_op;
                }
                goto do_rdrand;
            }

        case 6: /* RDRAND */
            if (mod != 3 ||
                (s->prefix & (PREFIX_REPZ | PREFIX_REPNZ)) ||
                !(s->cpuid_ext_features & CPUID_EXT_RDRAND)) {
                goto illegal_op;
            }
        do_rdrand:
            translator_io_start(&s->base);
            gen_helper_rdrand(s->T0, tcg_env);
            rm = (modrm & 7) | REX_B(s);
            gen_op_mov_reg_v(s, dflag, rm, s->T0);
            assume_cc_op(s, CC_OP_EFLAGS);
            break;

        default:
            goto illegal_op;
        }
        break;

    case 0x100:
        mod = (modrm >> 6) & 3;
        op = (modrm >> 3) & 7;
        switch(op) {
        case 0: /* sldt */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_LDTR_READ);
            tcg_gen_ld32u_tl(s->T0, tcg_env,
                             offsetof(CPUX86State, ldt.selector));
            ot = mod == 3 ? dflag : MO_16;
            gen_st_modrm(s, decode, ot);
            break;
        case 2: /* lldt */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (check_cpl0(s)) {
                gen_svm_check_intercept(s, SVM_EXIT_LDTR_WRITE);
                gen_ld_modrm(s, decode, MO_16);
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                gen_helper_lldt(tcg_env, s->tmp2_i32);
            }
            break;
        case 1: /* str */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_TR_READ);
            tcg_gen_ld32u_tl(s->T0, tcg_env,
                             offsetof(CPUX86State, tr.selector));
            ot = mod == 3 ? dflag : MO_16;
            gen_st_modrm(s, decode, ot);
            break;
        case 3: /* ltr */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (check_cpl0(s)) {
                gen_svm_check_intercept(s, SVM_EXIT_TR_WRITE);
                gen_ld_modrm(s, decode, MO_16);
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                gen_helper_ltr(tcg_env, s->tmp2_i32);
            }
            break;
        case 4: /* verr */
        case 5: /* verw */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            gen_ld_modrm(s, decode, MO_16);
            gen_update_cc_op(s);
            if (op == 4) {
                gen_helper_verr(tcg_env, s->T0);
            } else {
                gen_helper_verw(tcg_env, s->T0);
            }
            assume_cc_op(s, CC_OP_EFLAGS);
            break;
        default:
            goto illegal_op;
        }
        break;

    case 0x101:
        switch (modrm) {
        CASE_MODRM_MEM_OP(0): /* sgdt */
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_GDTR_READ);
            gen_lea_modrm(s, decode);
            tcg_gen_ld32u_tl(s->T0,
                             tcg_env, offsetof(CPUX86State, gdt.limit));
            gen_op_st_v(s, MO_16, s->T0, s->A0);
            gen_add_A0_im(s, 2);
            tcg_gen_ld_tl(s->T0, tcg_env, offsetof(CPUX86State, gdt.base));
            /*
             * NB: Despite a confusing description in Intel CPU documentation,
             *     all 32-bits are written regardless of operand size.
             */
            gen_op_st_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            break;

        case 0xc8: /* monitor */
            if (!(s->cpuid_ext_features & CPUID_EXT_MONITOR) || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_lea_v_seg(s, cpu_regs[R_EAX], R_DS, s->override);
            gen_helper_monitor(tcg_env, s->A0);
            break;

        case 0xc9: /* mwait */
            if (!(s->cpuid_ext_features & CPUID_EXT_MONITOR) || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_mwait(tcg_env, cur_insn_len_i32(s));
            s->base.is_jmp = DISAS_NORETURN;
            break;

        case 0xca: /* clac */
            if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_SMAP)
                || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_reset_eflags(s, AC_MASK);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xcb: /* stac */
            if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_SMAP)
                || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_set_eflags(s, AC_MASK);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(1): /* sidt */
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_IDTR_READ);
            gen_lea_modrm(s, decode);
            tcg_gen_ld32u_tl(s->T0, tcg_env, offsetof(CPUX86State, idt.limit));
            gen_op_st_v(s, MO_16, s->T0, s->A0);
            gen_add_A0_im(s, 2);
            tcg_gen_ld_tl(s->T0, tcg_env, offsetof(CPUX86State, idt.base));
            /*
             * NB: Despite a confusing description in Intel CPU documentation,
             *     all 32-bits are written regardless of operand size.
             */
            gen_op_st_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            break;

        case 0xd0: /* xgetbv */
            if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                || (s->prefix & (PREFIX_DATA | PREFIX_REPZ | PREFIX_REPNZ))) {
                goto illegal_op;
            }
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_xgetbv(s->tmp1_i64, tcg_env, s->tmp2_i32);
            tcg_gen_extr_i64_tl(cpu_regs[R_EAX], cpu_regs[R_EDX], s->tmp1_i64);
            break;

        case 0xd1: /* xsetbv */
            if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                || (s->prefix & (PREFIX_DATA | PREFIX_REPZ | PREFIX_REPNZ))) {
                goto illegal_op;
            }
            gen_svm_check_intercept(s, SVM_EXIT_XSETBV);
            if (!check_cpl0(s)) {
                break;
            }
            tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                  cpu_regs[R_EDX]);
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_xsetbv(tcg_env, s->tmp2_i32, s->tmp1_i64);
            /* End TB because translation flags may change.  */
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xd8: /* VMRUN */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            /*
             * Reloads INHIBIT_IRQ mask as well as TF and RF with guest state.
             * The usual gen_eob() handling is performed on vmexit after
             * host state is reloaded.
             */
            gen_helper_vmrun(tcg_env, tcg_constant_i32(s->aflag - 1),
                             cur_insn_len_i32(s));
            tcg_gen_exit_tb(NULL, 0);
            s->base.is_jmp = DISAS_NORETURN;
            break;

        case 0xd9: /* VMMCALL */
            if (!SVME(s)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmmcall(tcg_env);
            break;

        case 0xda: /* VMLOAD */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmload(tcg_env, tcg_constant_i32(s->aflag - 1));
            break;

        case 0xdb: /* VMSAVE */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmsave(tcg_env, tcg_constant_i32(s->aflag - 1));
            break;

        case 0xdc: /* STGI */
            if ((!SVME(s) && !(s->cpuid_ext3_features & CPUID_EXT3_SKINIT))
                || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_helper_stgi(tcg_env);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xdd: /* CLGI */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_clgi(tcg_env);
            break;

        case 0xde: /* SKINIT */
            if ((!SVME(s) && !(s->cpuid_ext3_features & CPUID_EXT3_SKINIT))
                || !PE(s)) {
                goto illegal_op;
            }
            gen_svm_check_intercept(s, SVM_EXIT_SKINIT);
            /* If not intercepted, not implemented -- raise #UD. */
            goto illegal_op;

        case 0xdf: /* INVLPGA */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_INVLPGA);
            if (s->aflag == MO_64) {
                tcg_gen_mov_tl(s->A0, cpu_regs[R_EAX]);
            } else {
                tcg_gen_ext32u_tl(s->A0, cpu_regs[R_EAX]);
            }
            gen_helper_flush_page(tcg_env, s->A0);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(2): /* lgdt */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_GDTR_WRITE);
            gen_lea_modrm(s, decode);
            gen_op_ld_v(s, MO_16, s->T1, s->A0);
            gen_add_A0_im(s, 2);
            gen_op_ld_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            if (dflag == MO_16) {
                tcg_gen_andi_tl(s->T0, s->T0, 0xffffff);
            }
            tcg_gen_st_tl(s->T0, tcg_env, offsetof(CPUX86State, gdt.base));
            tcg_gen_st32_tl(s->T1, tcg_env, offsetof(CPUX86State, gdt.limit));
            break;

        CASE_MODRM_MEM_OP(3): /* lidt */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_IDTR_WRITE);
            gen_lea_modrm(s, decode);
            gen_op_ld_v(s, MO_16, s->T1, s->A0);
            gen_add_A0_im(s, 2);
            gen_op_ld_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            if (dflag == MO_16) {
                tcg_gen_andi_tl(s->T0, s->T0, 0xffffff);
            }
            tcg_gen_st_tl(s->T0, tcg_env, offsetof(CPUX86State, idt.base));
            tcg_gen_st32_tl(s->T1, tcg_env, offsetof(CPUX86State, idt.limit));
            break;

        CASE_MODRM_OP(4): /* smsw */
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_READ_CR0);
            tcg_gen_ld_tl(s->T0, tcg_env, offsetof(CPUX86State, cr[0]));
            /*
             * In 32-bit mode, the higher 16 bits of the destination
             * register are undefined.  In practice CR0[31:0] is stored
             * just like in 64-bit mode.
             */
            mod = (modrm >> 6) & 3;
            ot = (mod != 3 ? MO_16 : s->dflag);
            gen_st_modrm(s, decode, ot);
            break;
        case 0xee: /* rdpkru */
            if (s->prefix & (PREFIX_DATA | PREFIX_REPZ | PREFIX_REPNZ)) {
                goto illegal_op;
            }
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_rdpkru(s->tmp1_i64, tcg_env, s->tmp2_i32);
            tcg_gen_extr_i64_tl(cpu_regs[R_EAX], cpu_regs[R_EDX], s->tmp1_i64);
            break;
        case 0xef: /* wrpkru */
            if (s->prefix & (PREFIX_DATA | PREFIX_REPZ | PREFIX_REPNZ)) {
                goto illegal_op;
            }
            tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                  cpu_regs[R_EDX]);
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_wrpkru(tcg_env, s->tmp2_i32, s->tmp1_i64);
            break;

        CASE_MODRM_OP(6): /* lmsw */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_WRITE_CR0);
            gen_ld_modrm(s, decode, MO_16);
            /*
             * Only the 4 lower bits of CR0 are modified.
             * PE cannot be set to zero if already set to one.
             */
            tcg_gen_ld_tl(s->T1, tcg_env, offsetof(CPUX86State, cr[0]));
            tcg_gen_andi_tl(s->T0, s->T0, 0xf);
            tcg_gen_andi_tl(s->T1, s->T1, ~0xe);
            tcg_gen_or_tl(s->T0, s->T0, s->T1);
            gen_helper_write_crN(tcg_env, tcg_constant_i32(0), s->T0);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(7): /* invlpg */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_INVLPG);
            gen_lea_modrm(s, decode);
            gen_helper_flush_page(tcg_env, s->A0);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xf8: /* swapgs */
#ifdef TARGET_X86_64
            if (CODE64(s)) {
                if (check_cpl0(s)) {
                    tcg_gen_mov_tl(s->T0, cpu_seg_base[R_GS]);
                    tcg_gen_ld_tl(cpu_seg_base[R_GS], tcg_env,
                                  offsetof(CPUX86State, kernelgsbase));
                    tcg_gen_st_tl(s->T0, tcg_env,
                                  offsetof(CPUX86State, kernelgsbase));
                }
                break;
            }
#endif
            goto illegal_op;

        case 0xf9: /* rdtscp */
            if (!(s->cpuid_ext2_features & CPUID_EXT2_RDTSCP)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            translator_io_start(&s->base);
            gen_helper_rdtsc(tcg_env);
            gen_helper_rdpid(s->T0, tcg_env);
            gen_op_mov_reg_v(s, dflag, R_ECX, s->T0);
            break;

        default:
            goto illegal_op;
        }
        break;

    case 0x11a:
        if (s->flags & HF_MPX_EN_MASK) {
            mod = (modrm >> 6) & 3;
            reg = ((modrm >> 3) & 7) | REX_R(s);
            if (prefixes & PREFIX_REPZ) {
                /* bndcl */
                if (reg >= 4
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                gen_bndck(s, decode, TCG_COND_LTU, cpu_bndl[reg]);
            } else if (prefixes & PREFIX_REPNZ) {
                /* bndcu */
                if (reg >= 4
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                TCGv_i64 notu = tcg_temp_new_i64();
                tcg_gen_not_i64(notu, cpu_bndu[reg]);
                gen_bndck(s, decode, TCG_COND_GTU, notu);
            } else if (prefixes & PREFIX_DATA) {
                /* bndmov -- from reg/mem */
                if (reg >= 4 || s->aflag == MO_16) {
                    goto illegal_op;
                }
                if (mod == 3) {
                    int reg2 = (modrm & 7) | REX_B(s);
                    if (reg2 >= 4) {
                        goto illegal_op;
                    }
                    if (s->flags & HF_MPX_IU_MASK) {
                        tcg_gen_mov_i64(cpu_bndl[reg], cpu_bndl[reg2]);
                        tcg_gen_mov_i64(cpu_bndu[reg], cpu_bndu[reg2]);
                    }
                } else {
                    gen_lea_modrm(s, decode);
                    if (CODE64(s)) {
                        tcg_gen_qemu_ld_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                        tcg_gen_addi_tl(s->A0, s->A0, 8);
                        tcg_gen_qemu_ld_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                        tcg_gen_addi_tl(s->A0, s->A0, 4);
                        tcg_gen_qemu_ld_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                    }
                    /* bnd registers are now in-use */
                    gen_set_hflag(s, HF_MPX_IU_MASK);
                }
            } else if (mod != 3) {
                /* bndldx */
                AddressParts a = decode->mem;
                if (reg >= 4
                    || s->aflag == MO_16
                    || a.base < -1) {
                    goto illegal_op;
                }
                if (a.base >= 0) {
                    tcg_gen_addi_tl(s->A0, cpu_regs[a.base], a.disp);
                } else {
                    tcg_gen_movi_tl(s->A0, 0);
                }
                gen_lea_v_seg(s, s->A0, a.def_seg, s->override);
                if (a.index >= 0) {
                    tcg_gen_mov_tl(s->T0, cpu_regs[a.index]);
                } else {
                    tcg_gen_movi_tl(s->T0, 0);
                }
                if (CODE64(s)) {
                    gen_helper_bndldx64(cpu_bndl[reg], tcg_env, s->A0, s->T0);
                    tcg_gen_ld_i64(cpu_bndu[reg], tcg_env,
                                   offsetof(CPUX86State, mmx_t0.MMX_Q(0)));
                } else {
                    gen_helper_bndldx32(cpu_bndu[reg], tcg_env, s->A0, s->T0);
                    tcg_gen_ext32u_i64(cpu_bndl[reg], cpu_bndu[reg]);
                    tcg_gen_shri_i64(cpu_bndu[reg], cpu_bndu[reg], 32);
                }
                gen_set_hflag(s, HF_MPX_IU_MASK);
            }
        }
        break;
    case 0x11b:
        if (s->flags & HF_MPX_EN_MASK) {
            mod = (modrm >> 6) & 3;
            reg = ((modrm >> 3) & 7) | REX_R(s);
            if (mod != 3 && (prefixes & PREFIX_REPZ)) {
                /* bndmk */
                if (reg >= 4
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                AddressParts a = decode->mem;
                if (a.base >= 0) {
                    tcg_gen_extu_tl_i64(cpu_bndl[reg], cpu_regs[a.base]);
                    if (!CODE64(s)) {
                        tcg_gen_ext32u_i64(cpu_bndl[reg], cpu_bndl[reg]);
                    }
                } else if (a.base == -1) {
                    /* no base register has lower bound of 0 */
                    tcg_gen_movi_i64(cpu_bndl[reg], 0);
                } else {
                    /* rip-relative generates #ud */
                    goto illegal_op;
                }
                tcg_gen_not_tl(s->A0, gen_lea_modrm_1(s, decode->mem, false));
                if (!CODE64(s)) {
                    tcg_gen_ext32u_tl(s->A0, s->A0);
                }
                tcg_gen_extu_tl_i64(cpu_bndu[reg], s->A0);
                /* bnd registers are now in-use */
                gen_set_hflag(s, HF_MPX_IU_MASK);
                break;
            } else if (prefixes & PREFIX_REPNZ) {
                /* bndcn */
                if (reg >= 4
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                gen_bndck(s, decode, TCG_COND_GTU, cpu_bndu[reg]);
            } else if (prefixes & PREFIX_DATA) {
                /* bndmov -- to reg/mem */
                if (reg >= 4 || s->aflag == MO_16) {
                    goto illegal_op;
                }
                if (mod == 3) {
                    int reg2 = (modrm & 7) | REX_B(s);
                    if (reg2 >= 4) {
                        goto illegal_op;
                    }
                    if (s->flags & HF_MPX_IU_MASK) {
                        tcg_gen_mov_i64(cpu_bndl[reg2], cpu_bndl[reg]);
                        tcg_gen_mov_i64(cpu_bndu[reg2], cpu_bndu[reg]);
                    }
                } else {
                    gen_lea_modrm(s, decode);
                    if (CODE64(s)) {
                        tcg_gen_qemu_st_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                        tcg_gen_addi_tl(s->A0, s->A0, 8);
                        tcg_gen_qemu_st_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                    } else {
                        tcg_gen_qemu_st_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                        tcg_gen_addi_tl(s->A0, s->A0, 4);
                        tcg_gen_qemu_st_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                    }
                }
            } else if (mod != 3) {
                /* bndstx */
                AddressParts a = decode->mem;
                if (reg >= 4
                    || s->aflag == MO_16
                    || a.base < -1) {
                    goto illegal_op;
                }
                if (a.base >= 0) {
                    tcg_gen_addi_tl(s->A0, cpu_regs[a.base], a.disp);
                } else {
                    tcg_gen_movi_tl(s->A0, 0);
                }
                gen_lea_v_seg(s, s->A0, a.def_seg, s->override);
                if (a.index >= 0) {
                    tcg_gen_mov_tl(s->T0, cpu_regs[a.index]);
                } else {
                    tcg_gen_movi_tl(s->T0, 0);
                }
                if (CODE64(s)) {
                    gen_helper_bndstx64(tcg_env, s->A0, s->T0,
                                        cpu_bndl[reg], cpu_bndu[reg]);
                } else {
                    gen_helper_bndstx32(tcg_env, s->A0, s->T0,
                                        cpu_bndl[reg], cpu_bndu[reg]);
                }
            }
        }
        break;
    default:
        g_assert_not_reached();
    }
    return;
 illegal_op:
    gen_illegal_opcode(s);
}

#include "decode-new.c.inc"

void tcg_x86_init(void)
{
    static const char reg_names[CPU_NB_REGS][4] = {
#ifdef TARGET_X86_64
        [R_EAX] = "rax",
        [R_EBX] = "rbx",
        [R_ECX] = "rcx",
        [R_EDX] = "rdx",
        [R_ESI] = "rsi",
        [R_EDI] = "rdi",
        [R_EBP] = "rbp",
        [R_ESP] = "rsp",
        [8]  = "r8",
        [9]  = "r9",
        [10] = "r10",
        [11] = "r11",
        [12] = "r12",
        [13] = "r13",
        [14] = "r14",
        [15] = "r15",
#else
        [R_EAX] = "eax",
        [R_EBX] = "ebx",
        [R_ECX] = "ecx",
        [R_EDX] = "edx",
        [R_ESI] = "esi",
        [R_EDI] = "edi",
        [R_EBP] = "ebp",
        [R_ESP] = "esp",
#endif
    };
    static const char eip_name[] = {
#ifdef TARGET_X86_64
        "rip"
#else
        "eip"
#endif
    };
    static const char seg_base_names[6][8] = {
        [R_CS] = "cs_base",
        [R_DS] = "ds_base",
        [R_ES] = "es_base",
        [R_FS] = "fs_base",
        [R_GS] = "gs_base",
        [R_SS] = "ss_base",
    };
    static const char bnd_regl_names[4][8] = {
        "bnd0_lb", "bnd1_lb", "bnd2_lb", "bnd3_lb"
    };
    static const char bnd_regu_names[4][8] = {
        "bnd0_ub", "bnd1_ub", "bnd2_ub", "bnd3_ub"
    };
    int i;

    cpu_cc_op = tcg_global_mem_new_i32(tcg_env,
                                       offsetof(CPUX86State, cc_op), "cc_op");
    cpu_cc_dst = tcg_global_mem_new(tcg_env, offsetof(CPUX86State, cc_dst),
                                    "cc_dst");
    cpu_cc_src = tcg_global_mem_new(tcg_env, offsetof(CPUX86State, cc_src),
                                    "cc_src");
    cpu_cc_src2 = tcg_global_mem_new(tcg_env, offsetof(CPUX86State, cc_src2),
                                     "cc_src2");
    cpu_eip = tcg_global_mem_new(tcg_env, offsetof(CPUX86State, eip), eip_name);

    for (i = 0; i < CPU_NB_REGS; ++i) {
        cpu_regs[i] = tcg_global_mem_new(tcg_env,
                                         offsetof(CPUX86State, regs[i]),
                                         reg_names[i]);
    }

    for (i = 0; i < 6; ++i) {
        cpu_seg_base[i]
            = tcg_global_mem_new(tcg_env,
                                 offsetof(CPUX86State, segs[i].base),
                                 seg_base_names[i]);
    }

    for (i = 0; i < 4; ++i) {
        cpu_bndl[i]
            = tcg_global_mem_new_i64(tcg_env,
                                     offsetof(CPUX86State, bnd_regs[i].lb),
                                     bnd_regl_names[i]);
        cpu_bndu[i]
            = tcg_global_mem_new_i64(tcg_env,
                                     offsetof(CPUX86State, bnd_regs[i].ub),
                                     bnd_regu_names[i]);
    }

    fpstt = tcg_global_mem_new_i32(tcg_env,
                                   offsetof(CPUX86State, fpstt), "fpstt");

#if defined(XBOX) && (defined(__x86_64__) || defined(__aarch64__))
    g_use_hard_fpu = g_config.perf.hard_fpu;
    /*
     * The old inline x87 cache stays off. It was gated on
     * TCG_TARGET_HAS_fpu, which is private to tcg/ and so never defined
     * here: it has not run since 3eb1b2de10 (2026-08-09). It is not exact
     * against the helpers (dead push/pop values never reach env, f32
     * caching, FUCOM as a double compare...) and loses cached values on a
     * mid-block exit (cpu_io_recompile under icount, a guest #PF).
     */
    g_use_hard_fpu_inline = 0;
#if defined(__aarch64__)
    /*
     * The write-through path (x87_wt_*) instead matches the aarch64
     * double-based hard helpers bit for bit. Not on x86_64 hosts, whose
     * hard helpers compute in 80-bit long double. The backend must have
     * FP ops (Windows ARM64 does not).
     */
    g_x87_wt = g_use_hard_fpu &&
               tcg_op_supported(INDEX_op_add_f64, TCG_TYPE_I64, 0);
    /*
     * Packed-single add/sub/mul/div as host vector FP ops, bit for bit
     * the NEON helpers (same instructions, same FPCR) in TBs where those
     * would not touch FPCR: MXCSR default (HF_SSE_DFL) and x87 RC nearest.
     * Not under the XEMU_SSE_HOST=2 self-check, which checks the helpers.
     */
    xemu_sse_vfp = g_use_hard_fpu && xemu_sse_host_mode() == 1 &&
                   tcg_op_supported(INDEX_op_fmul_vec, TCG_TYPE_V128, 0);
#endif
#endif
}

static void i386_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    CPUX86State *env = cpu_env(cpu);
    uint32_t flags = dc->base.tb->flags;
    uint32_t cflags = tb_cflags(dc->base.tb);
    int cpl = (flags >> HF_CPL_SHIFT) & 3;
    int iopl = (flags >> IOPL_SHIFT) & 3;

    dc->cs_base = dc->base.tb->cs_base;
    dc->pc_save = dc->base.pc_next;
    dc->flags = flags;
#if defined(XBOX)
    dc->xemu_env = env;
#endif
#ifndef CONFIG_USER_ONLY
    dc->cpl = cpl;
    dc->iopl = iopl;
#endif

    /* We make some simplifying assumptions; validate they're correct. */
    g_assert(PE(dc) == ((flags & HF_PE_MASK) != 0));
    g_assert(CPL(dc) == cpl);
    g_assert(IOPL(dc) == iopl);
    g_assert(VM86(dc) == ((flags & HF_VM_MASK) != 0));
    g_assert(CODE32(dc) == ((flags & HF_CS32_MASK) != 0));
    g_assert(CODE64(dc) == ((flags & HF_CS64_MASK) != 0));
    g_assert(SS32(dc) == ((flags & HF_SS32_MASK) != 0));
    g_assert(LMA(dc) == ((flags & HF_LMA_MASK) != 0));
    g_assert(ADDSEG(dc) == ((flags & HF_ADDSEG_MASK) != 0));
    g_assert(SVME(dc) == ((flags & HF_SVME_MASK) != 0));
    g_assert(GUEST(dc) == ((flags & HF_GUEST_MASK) != 0));

    dc->cc_op = CC_OP_DYNAMIC;
    dc->cc_op_dirty = false;
    /* select memory access functions */
    dc->mem_index = cpu_mmu_index(cpu, false);
    dc->cpuid_features = env->features[FEAT_1_EDX];
    dc->cpuid_ext_features = env->features[FEAT_1_ECX];
    dc->cpuid_ext2_features = env->features[FEAT_8000_0001_EDX];
    dc->cpuid_ext3_features = env->features[FEAT_8000_0001_ECX];
    dc->cpuid_7_0_ebx_features = env->features[FEAT_7_0_EBX];
    dc->cpuid_7_0_ecx_features = env->features[FEAT_7_0_ECX];
    dc->cpuid_7_1_eax_features = env->features[FEAT_7_1_EAX];
    dc->cpuid_xsave_features = env->features[FEAT_XSAVE];
#if defined(XBOX)
    dc->xemu_ret_exit = false;
    dc->xemu_fldcw_exit = false;
    dc->xemu_x87_run = 0;
    dc->xemu_fip_pend = false;
    dc->xemu_fdp_pend = false;
    dc->xemu_fds_seg = -1;
#endif
    dc->jmp_opt = !((cflags & CF_NO_GOTO_TB) ||
                    (flags & (HF_RF_MASK | HF_TF_MASK | HF_INHIBIT_IRQ_MASK)));

    dc->T0 = tcg_temp_new();
    dc->T1 = tcg_temp_new();
    dc->A0 = tcg_temp_new();

    dc->tmp1_i64 = tcg_temp_new_i64();
    dc->tmp2_i32 = tcg_temp_new_i32();
    dc->cc_srcT = tcg_temp_new();
    
    for (int i = 0; i < 8; i++) {
        dc->fpregs[i] = NULL;
    }
    dc->fpstt_delta = 0;
    dc->ft0 = NULL;
    dc->flcr_set = false;
#if defined(XBOX)
    for (int i = 0; i < 8; i++) {
        dc->wt_st[i] = NULL;
    }
    dc->wt_ft0 = NULL;
    dc->wt_pend = 0;
    dc->wt_exact = 0;
    dc->wt_insn_pend = 0;
    dc->wt_epoch = false;
    dc->wt_nodefer = false;
    dc->wt_base = 0;
    dc->pure_call = false;
#endif
}

static void i386_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
}

static void i386_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    target_ulong pc_arg = dc->base.pc_next;

    dc->prev_insn_start = dc->base.insn_start;
    dc->prev_insn_end = tcg_last_op();
    if (tb_cflags(dcbase->tb) & CF_PCREL) {
        pc_arg &= ~TARGET_PAGE_MASK;
    }
    tcg_gen_insn_start(pc_arg, dc->cc_op);
#if defined(XBOX)
    /*
     * Word 2, what a mid-block exit in this insn must land
     * (xemu_x87_restore): how far back the insn whose FIP update is still
     * deferred sits, below 4096 as every insn of a TB starts on its first
     * page, and the x87 stores still deferred, by shadow slot.
     */
    {
        uint64_t w2 = 0;

        if (dc->xemu_fip_pend) {
            w2 = dc->base.pc_next - dc->xemu_fip_pc;
            tcg_debug_assert(w2 <= XEMU_X87_W2_FIP_MASK);
        }
        dc->wt_insn_pend = x87_wt_rel_pend(dc);
        dc->wt_nodefer = false;
        w2 |= (uint64_t)dc->wt_insn_pend << XEMU_X87_W2_PEND_SHIFT;
        if (w2) {
            tcg_set_insn_start_param(tcg_last_op(), 2, w2);
        }
    }
#endif
}

static void i386_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    bool orig_cc_op_dirty = dc->cc_op_dirty;
    CCOp orig_cc_op = dc->cc_op;
    target_ulong orig_pc_save = dc->pc_save;
#if defined(XBOX)
    uint8_t orig_wt_pend = dc->wt_pend;
#endif

#if defined(XBOX)
    xemu_x87_boundary_check(dc);
    x87_wt_boundary_check(dc);
#endif

#ifdef TARGET_VSYSCALL_PAGE
    /*
     * Detect entry into the vsyscall page and invoke the syscall.
     */
    if ((dc->base.pc_next & TARGET_PAGE_MASK) == TARGET_VSYSCALL_PAGE) {
        gen_exception(dc, EXCP_VSYSCALL);
        dc->base.pc_next = dc->pc + 1;
        return;
    }
#endif

    switch (sigsetjmp(dc->jmpbuf, 0)) {
    case 0:
        disas_insn(dc, cpu);
        break;
    case 1:
        gen_exception_gpf(dc);
        break;
    case 2:
        /* Restore state that may affect the next instruction. */
        dc->pc = dc->base.pc_next;
        assert(dc->cc_op_dirty == orig_cc_op_dirty);
        assert(dc->cc_op == orig_cc_op);
        assert(dc->pc_save == orig_pc_save);
#if defined(XBOX)
        /* Decoding ends before any op: no deferred x87 store was emitted. */
        assert(dc->wt_pend == orig_wt_pend);
#endif
        dc->base.num_insns--;
        tcg_remove_ops_after(dc->prev_insn_end);
        dc->base.insn_start = dc->prev_insn_start;
        dc->base.is_jmp = DISAS_TOO_MANY;
        return;
    default:
        g_assert_not_reached();
    }

    /*
     * Instruction decoding completed (possibly with #GP if the
     * 15-byte boundary was exceeded).
     */
    dc->base.pc_next = dc->pc;
    if (dc->base.is_jmp == DISAS_NEXT) {
        if (dc->flags & (HF_TF_MASK | HF_INHIBIT_IRQ_MASK)) {
            /*
             * If single step mode, we generate only one instruction and
             * generate an exception.
             * If irq were inhibited with HF_INHIBIT_IRQ_MASK, we clear
             * the flag and abort the translation to give the irqs a
             * chance to happen.
             */
            dc->base.is_jmp = DISAS_EOB_NEXT;
        } else if (!translator_is_same_page(&dc->base, dc->base.pc_next)) {
            dc->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void i386_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);

#if defined(XBOX)
    /* Past the last insn's exits: its deferred stores need no pend_top reset. */
    dc->wt_insn_pend = 0;
#endif
    gen_flush_fp(dc);

    switch (dc->base.is_jmp) {
    case DISAS_NORETURN:
        /*
         * Most instructions should not use DISAS_NORETURN, as that suppresses
         * the handling of hflags normally done by gen_eob().  We can
         * get here:
         * - for exception and interrupts
         * - for jump optimization (which is disabled by INHIBIT_IRQ/RF/TF)
         * - for VMRUN because RF/TF handling for the host is done after vmexit,
         *   and INHIBIT_IRQ is loaded from the VMCB
         * - for HLT/PAUSE/MWAIT to exit the main loop with specific EXCP_* values;
         *   the helpers handle themselves the tasks normally done by gen_eob().
         */
        break;
    case DISAS_TOO_MANY:
        gen_update_cc_op(dc);
        gen_jmp_rel_csize(dc, 0, 0);
        break;
    case DISAS_EOB_NEXT:
    case DISAS_EOB_INHIBIT_IRQ:
        assert(dc->base.pc_next == dc->pc);
        gen_update_eip_cur(dc);
        /* fall through */
    case DISAS_EOB_ONLY:
    case DISAS_EOB_RECHECK_TF:
    case DISAS_JUMP:
        gen_eob(dc, dc->base.is_jmp);
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps i386_tr_ops = {
    .init_disas_context = i386_tr_init_disas_context,
    .tb_start           = i386_tr_tb_start,
    .insn_start         = i386_tr_insn_start,
    .translate_insn     = i386_tr_translate_insn,
    .tb_stop            = i386_tr_tb_stop,
};

void x86_translate_code(CPUState *cpu, TranslationBlock *tb,
                        int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc;

    tcg_ctx->disas_ctx = &dc;
    translator_loop(cpu, tb, max_insns, pc, host_pc, &i386_tr_ops, &dc.base);
    tcg_ctx->disas_ctx = NULL;
}

void gen_bb_epilogue(void)
{
    DisasContext *s = (DisasContext *)tcg_ctx->disas_ctx;

#if defined(XBOX)
    if (s->pure_call) {
        return;
    }
#endif
    gen_flush_fp(s);
}
