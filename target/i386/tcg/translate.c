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

static int g_use_hard_fpu;
static int g_use_hard_fpu_inline;

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
#define gen_helper_fdecstp        MAP_GEN_HELPER_SOFT_HARD(fdecstp)
#define gen_helper_fincstp        MAP_GEN_HELPER_SOFT_HARD(fincstp)
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
#define gen_helper_fnstcw         MAP_GEN_HELPER_SOFT_HARD(fnstcw)
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

#if defined(XBOX)
/* Superblock sizing-census tail kinds (mechanism + census live just
 * above gen_jmp_rel; the enum sits here because DisasContext carries the
 * per-TB kind field and early call sites assign it). */
enum {
    XEMU_SB_TAIL_UNCOND = 0,   /* gen_JMP direct jmp */
    XEMU_SB_TAIL_CALL,         /* gen_CALL direct call */
    XEMU_SB_TAIL_JCC_TAKEN,    /* conditional branch, taken edge */
    XEMU_SB_TAIL_JCC_FALL,     /* conditional branch, fallthrough edge */
    XEMU_SB_TAIL_REP,          /* rep-string loop edges */
    XEMU_SB_TAIL_TOOMANY,      /* insn/page budget TB split */
    XEMU_SB_TAIL_OTHER,
    XEMU_SB_TAIL_NKINDS
};
#endif

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
    uint8_t xemu_cc_head; /* census: TB's first flag event (none/kill/use) */
    uint8_t xemu_sb_follows;   /* superblock: seams merged in this TB */
    uint8_t xemu_sb_tail_kind; /* sizing census: kind of the exit being emitted */
    bool xemu_sb_stub;         /* emitting an out-of-line taken-edge exit stub */
    bool xemu_region_cand;     /* census: tail is a forward in-window jcc */
    /* Region former (XEMU_REGION): pending intra-TB taken-edge labels,
     * sorted ascending by target; bound at the join insn start. */
#define XEMU_REGION_MAXPEND 8
    struct {
        target_ulong target;    /* linear pc where the label binds */
        TCGLabel *label;        /* the (region_join) taken label */
        int cc_op;              /* checkpoint at the brcond */
        bool cc_op_dirty;
        target_ulong pc_save;
    } xemu_region_pend[XEMU_REGION_MAXPEND];
    uint8_t xemu_region_npend;
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
#if defined(XBOX)
static bool xemu_superblock_try_merge_fallthrough(DisasContext *s,
        target_long diff, TCGLabel *not_taken, TCGLabel *taken);
static bool xemu_region_try_open(DisasContext *s, target_long diff,
                                 TCGLabel *taken);
#endif
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
#if defined(XBOX)
    /* Census (XEMU_CCOP_CENSUS): every cc_op transition funnels through
     * here, so an op that fully defines flags before any consumption marks
     * the TB head as KILL. Consumption sites mark USE first; first event
     * wins. Unconditional byte write — not worth a latch. */
    if (op != CC_OP_DYNAMIC && s->xemu_cc_head == 0) {
        s->xemu_cc_head = 1; /* XEMU_CCOP_HEAD_KILL */
    }
#endif
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

#if defined(XBOX)
/* Census (XEMU_CCOP_CENSUS): the TB's first flag event is a consumption of
 * inherited state. Called at the top of every flag-reading funnel
 * (gen_prepare_* and gen_compute_eflags*) and at the helper-consuming
 * sites in emit.c.inc (daa family, dynamic shift count). */
static inline void xemu_ccop_mark_use(DisasContext *s)
{
    if (s->xemu_cc_head == 0) {
        s->xemu_cc_head = 2; /* XEMU_CCOP_HEAD_USE */
    }
}
#else
static inline void xemu_ccop_mark_use(DisasContext *s) { }
#endif

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
void xemu_i386_ras_flush(CPUState *cpu);
void xemu_i386_ras_flush(CPUState *cpu)
{
    CPUX86State *env = cpu_env(cpu);
    memset(env->xemu_retc, 0, sizeof(env->xemu_retc));
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
    gen_helper_cc_compute_all(reg, dst, src1, src2, cc_op);
}

/* compute all eflags to cc_src */
static void gen_compute_eflags(DisasContext *s)
{
    xemu_ccop_mark_use(s);
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

    xemu_ccop_mark_use(s);

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
       gen_helper_cc_compute_c(reg, cpu_cc_dst, cpu_cc_src,
                               cpu_cc_src2, cpu_cc_op);
       return (CCPrepare) { .cond = TCG_COND_NE, .reg = reg,
                            .no_setcond = true };
    }
}

/* compute eflags.P, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_p(DisasContext *s, TCGv reg)
{
    gen_compute_eflags(s);
    return (CCPrepare) { .cond = TCG_COND_TSTNE, .reg = cpu_cc_src,
                         .imm = CC_P };
}

/* compute eflags.S, trying to store it in reg if not NULL */
static CCPrepare gen_prepare_eflags_s(DisasContext *s, TCGv reg)
{
    xemu_ccop_mark_use(s);
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
    xemu_ccop_mark_use(s);
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
    xemu_ccop_mark_use(s);
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

    xemu_ccop_mark_use(s);
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
     */
    bool can_loop =
        (!(tb_cflags(s->base.tb) & (CF_USE_ICOUNT | CF_SINGLE_STEP))
	 && !(s->flags & (HF_TF_MASK | HF_INHIBIT_IRQ_MASK)));
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
#if defined(XBOX)
    s->xemu_sb_tail_kind = XEMU_SB_TAIL_REP;
#endif
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
#if defined(XBOX)
    s->xemu_sb_tail_kind = XEMU_SB_TAIL_REP;
#endif
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

static TCGv_ptr gen_stn_ptr(int opreg)
{
    TCGv_i32 offset = tcg_temp_new_i32();
    tcg_gen_mov_i32(offset, fpstt);

    if (opreg != 0) {
        tcg_gen_addi_i32(offset, offset, opreg);
        tcg_gen_andi_i32(offset, offset, 7);
    }

    tcg_gen_shli_i32(offset, offset, 4);
    tcg_gen_addi_i32(offset, offset, offsetof(CPUX86State, fpregs[0].d));
    TCGv_ptr ptr = tcg_temp_new_ptr();
    tcg_gen_ext_i32_ptr(ptr, offset);
    tcg_gen_add_ptr(ptr, ptr, tcg_env);

    return ptr;
}

static TCGv_ptr gen_ft0_ptr(void)
{
    TCGv_ptr ft0 = tcg_temp_new_ptr();
    tcg_gen_addi_ptr(ft0, tcg_env, offsetof(CPUX86State, ft0));
    return ft0;
}

static void gen_set_fptag(int offs, int value)
{
    TCGv_ptr p = tcg_temp_new_ptr();
    tcg_gen_ext_i32_ptr(p, fpstt);
    tcg_gen_add_ptr(p, tcg_env, p);
    TCGv_i32 tmp = tcg_constant_i32(value);
    tcg_gen_st8_i32(tmp, p, offsetof(CPUX86State, fptags[0]) + offs);
}

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
extern uint64_t xemu_inv_flcr_emitted;
extern uint64_t xemu_inv_flcr_skip;
extern uint64_t xemu_inv_gototb_emitted;
extern uint64_t xemu_inv_jcprobe_emitted;
extern uint64_t xemu_inv_retmemo_emitted;
/* cc-liveness census (XEMU_CCOP_CENSUS; see accel/tcg/xemu-inv-prof.h) */
extern bool xemu_ccop_census_on(void);
extern uint64_t xemu_ccop_tb_tail[3];
extern uint64_t xemu_ccop_tb_head[3];
/* Cross-page direct-chaining campaign (XEMU_XPAGE_*; docs/xpage-design.md,
 * accel/tcg/xemu-xpage.[ch]). Same function-local extern idiom. */
extern bool xemu_xpage_refute_on(void);
extern bool xemu_xpage_prof_on(void);
extern bool xemu_xpage_allow(uint64_t dest);
extern uint64_t xemu_xpage_taken_kernel;
extern uint64_t xemu_xpage_taken_low;
extern uint64_t xemu_xpage_emitted;
#endif

static void gen_flcr(DisasContext *s)
{
#if defined(XBOX)
    if (unlikely(xemu_inv_prof_on())) {
        if (s->flcr_set) {
            xemu_inv_flcr_skip++;
        } else {
            xemu_inv_flcr_emitted++;
        }
    }
#endif
    if (s->flcr_set) {
        return;
    }

    /*
     * HF_FPU_RC is baked into tb->flags at translation time
     * (cpu_set_fpuc, cpu.h:2841). Every TB currently translating thus
     * pins the guest RC bits to a compile-time-known value; no need
     * to reload env->fpuc and mask at runtime. Materialize rc_bits
     * as a constant so the optimizer can collapse the brcond when
     * cached matches and emit a single MOVI + flcr in the slow path.
     */
    uint32_t rc = (s->flags >> HF_FPU_RC_SHIFT) & 3;
    uint32_t rc_bits_val = rc << 10;
    TCGv_i32 rc_bits = tcg_constant_i32(rc_bits_val);

    TCGv_i32 cached = tcg_temp_new_i32();
    tcg_gen_ld16u_i32(cached, tcg_env, offsetof(CPUX86State, cached_fpuc_rc));

    TCGLabel *skip = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_EQ, cached, (int32_t)rc_bits_val, skip);

    tcg_gen_st16_i32(rc_bits, tcg_env, offsetof(CPUX86State, cached_fpuc_rc));

    /* Slow path: the full MXCSR-like value is now a compile-time literal. */
    TCGv_i32 v = tcg_constant_i32((rc_bits_val << 3) | 0x1f80);
    tcg_gen_flcr(v);

    gen_set_label(skip);
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

static void gen_flush_fp(DisasContext *s)
{
    fp_pc_wrapper(flush_fp_regs)(s);
    s->fpstt_delta = 0;
    s->flcr_set = false;
}

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
    GEN_HELPER_FALLBACK_v_v(fpush);

    tcg_gen_subi_i32(fpstt, fpstt, 1);
    tcg_gen_andi_i32(fpstt, fpstt, 7);
    gen_set_fptag(0, 0); /* validate stack entry */

    s->fpstt_delta -= 1;
}

static void gen_fpop(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fpop);

    gen_set_fptag(0, 1); /* invalidate stack entry */
    tcg_gen_addi_i32(fpstt, fpstt, 1);
    tcg_gen_andi_i32(fpstt, fpstt, 7);

    fp_pc_wrapper(gen_fpop)(s);
    s->fpstt_delta += 1;
}

static void gen_fmov_FT0_STN(DisasContext *s, int st_index)
{
    GEN_HELPER_FALLBACK_v_i(fmov_FT0_STN, st_index);
    fp_pc_wrapper(gen_fmov_FT0_STN)(s, st_index);
}

static void gen_fmov_ST0_STN(DisasContext *s, int st_index)
{
    GEN_HELPER_FALLBACK_v_i(fmov_ST0_STN, st_index);
    fp_pc_wrapper(gen_fmov_ST0_STN)(s, st_index);
}

static void gen_fmov_STN_ST0(DisasContext *s, int st_index)
{
    GEN_HELPER_FALLBACK_v_i(fmov_STN_ST0, st_index);
    fp_pc_wrapper(gen_fmov_STN_ST0)(s, st_index);
}

static void gen_ffree_STN(DisasContext *s, int st_index)
{
    GEN_HELPER_FALLBACK_v_i(ffree_STN, st_index);

    /*
     * Inline: env->fptags[(env->fpstt + st_index) & 7] = 1.
     * Same shape as the fdecstp / fincstp inlines (single byte
     * write into the tag word; no register spill, no env fpus
     * change — ffree only marks the tag slot empty). Saves a
     * helper call frame per freep / ffreep.
     */
    TCGv_i32 idx = tcg_temp_new_i32();
    tcg_gen_addi_i32(idx, fpstt, st_index);
    tcg_gen_andi_i32(idx, idx, 7);

    TCGv_ptr p = tcg_temp_new_ptr();
    tcg_gen_ext_i32_ptr(p, idx);
    tcg_gen_add_ptr(p, tcg_env, p);

    tcg_gen_st8_i32(tcg_constant_i32(1), p,
                    offsetof(CPUX86State, fptags[0]));
}

static void gen_fxchg_ST0_STN(DisasContext *s, int st_index)
{
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

static void gen_flds_FT0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_v_T(flds_FT0, arg);
    fp_pc_wrapper(gen_flds_FT0)(s, arg);
}

static void gen_flds_ST0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_v_T(flds_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_flds_ST0)(s, arg);
}

static void gen_fldl_FT0(DisasContext *s, TCGv_i64 arg)
{
    GEN_HELPER_FALLBACK_v_T(fldl_FT0, arg);
    fp_pc_wrapper(gen_fldl_FT0)(s, arg);
}

static void gen_fldl_ST0(DisasContext *s, TCGv_i64 arg)
{
    GEN_HELPER_FALLBACK_v_T(fldl_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_fldl_ST0)(s, arg);
}

static void gen_fildl_FT0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_v_T(fildl_FT0, arg);
    fp_pc_wrapper(gen_fildl_FT0)(s, arg);
}

static void gen_fildl_ST0(DisasContext *s, TCGv_i32 arg)
{
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
    GEN_HELPER_FALLBACK_T_v(fsts_ST0, arg);
    fp_pc_wrapper(gen_fsts_ST0)(s, arg);
}

static void gen_fstl_ST0(DisasContext *s, TCGv_i64 arg)
{
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

static void gen_fist_ST0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_T_v(fist_ST0, arg);
    fp_pc_wrapper(gen_fist_ST0)(s, arg);
}

/*
 * FISTTP must always truncate toward zero regardless of FPCR RC.
 * The inline path in ops_fpu.h emits tcg_gen_cvt_f{32,64}_i{32,64},
 * which lowers to FCVTZS (hard truncate) on AArch64 but to
 * CVTSS2SI / CVTSD2SI on x86_64 — the x86 op respects MXCSR, which
 * gen_flcr has programmed to the *guest's* general RC (not
 * necessarily truncate). On non-AArch64 hosts that divergence
 * would round FISTTP instead of truncating it, so fall back to the
 * floatx80 helper. AArch64 hosts keep the inline convert.
 */
static void gen_fistt_ST0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_T_v(fistt_ST0, arg);
#if !defined(__aarch64__)
    gen_helper_fistt_ST0(arg, tcg_env);
    return;
#endif
    fp_pc_wrapper(gen_fistt_ST0)(s, arg);
}

static void gen_fisttl_ST0(DisasContext *s, TCGv_i32 arg)
{
    GEN_HELPER_FALLBACK_T_v(fisttl_ST0, arg);
#if !defined(__aarch64__)
    gen_helper_fisttl_ST0(arg, tcg_env);
    return;
#endif
    fp_pc_wrapper(gen_fisttl_ST0)(s, arg);
}

static void gen_fisttll_ST0(DisasContext *s, TCGv_i64 arg)
{
    GEN_HELPER_FALLBACK_T_v(fisttll_ST0, arg);
#if !defined(__aarch64__)
    gen_helper_fisttll_ST0(arg, tcg_env);
    return;
#endif
    fp_pc_wrapper(gen_fisttll_ST0)(s, arg);
}

static void gen_fchs_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fchs_ST0);
    fp_pc_wrapper(gen_fchs_ST0)(s);
}

static void gen_fabs_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fabs_ST0);
    fp_pc_wrapper(gen_fabs_ST0)(s);
}

static void gen_fsqrt(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fsqrt);
    fp_pc_wrapper(gen_fsqrt)(s);
}

static void gen_frndint(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(frndint);
    fp_pc_wrapper(gen_frndint)(s);
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
#if defined(__aarch64__)
    gen_flush_fp(s);
    gen_helper_fsin(tcg_env);
    gen_clear_fpus_c2(s);
    return;
#endif
    fp_pc_wrapper(gen_fsin)(s);
    gen_clear_fpus_c2(s); /* FIXME: Does not check range correctly */
}

static void gen_fcos(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fcos);
#if defined(__aarch64__)
    gen_flush_fp(s);
    gen_helper_fcos(tcg_env);
    gen_clear_fpus_c2(s);
    return;
#endif
    fp_pc_wrapper(gen_fcos)(s);
    gen_clear_fpus_c2(s); /* FIXME: Does not check range correctly */
}

static void gen_helper_fp_arith_ST0_FT0(DisasContext *s, int op)
{
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

static void gen_fucomi_ST0_FT0_inline(DisasContext *s)
{
    gen_compute_eflags(s);

    TCGv_i64 cmp_result = tcg_temp_new_i64();
    fp_pc_wrapper(gen_fucomi_ST0_FT0)(s, cmp_result);

    TCGv_i64 cc = tcg_temp_new_i64();
    tcg_gen_extu_tl_i64(cc, cpu_cc_src);
    tcg_gen_andi_i64(cc, cc, ~(uint64_t)0x45);
    tcg_gen_or_i64(cc, cc, cmp_result);
    tcg_gen_trunc_i64_tl(cpu_cc_src, cc);
}

static void gen_fcomi_ST0_FT0_inline(DisasContext *s)
{
    gen_compute_eflags(s);

    TCGv_i64 cmp_result = tcg_temp_new_i64();
    fp_pc_wrapper(gen_fcomi_ST0_FT0)(s, cmp_result);

    TCGv_i64 cc = tcg_temp_new_i64();
    tcg_gen_extu_tl_i64(cc, cpu_cc_src);
    tcg_gen_andi_i64(cc, cc, ~(uint64_t)0x45);
    tcg_gen_or_i64(cc, cc, cmp_result);
    tcg_gen_trunc_i64_tl(cpu_cc_src, cc);
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
    GEN_HELPER_FALLBACK_v_v(fld1_ST0);
    fp_pc_wrapper(gen_fld1_ST0)(s);
}

static void gen_fldz_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldz_ST0);
    fp_pc_wrapper(gen_fldz_ST0)(s);
    /* FIXME: Set tag word */
}

static void gen_fldz_FT0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldz_FT0);
    fp_pc_wrapper(gen_fldz_FT0)(s);
}

static void gen_fldl2t_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldl2t_ST0);
    fp_pc_wrapper(gen_fldl2t_ST0)(s);
}

static void gen_fldl2e_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldl2e_ST0);
    fp_pc_wrapper(gen_fldl2e_ST0)(s);
}

static void gen_fldpi_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldpi_ST0);
    fp_pc_wrapper(gen_fldpi_ST0)(s);
}

static void gen_fldlg2_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldlg2_ST0);
    fp_pc_wrapper(gen_fldlg2_ST0)(s);
}

static void gen_fldln2_ST0(DisasContext *s)
{
    GEN_HELPER_FALLBACK_v_v(fldln2_ST0);
    fp_pc_wrapper(gen_fldln2_ST0)(s);
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
#if defined(XBOX)
    /* Region-former candidate census (bit packed at tb_stop; window
     * must match XEMU_CCOP_REGION_WINDOW in accel/tcg/xemu-inv-prof.h,
     * which this file cannot include — function-local extern idiom). */
    s->xemu_region_cand = (diff > 0 && diff <= 64);
    if (xemu_region_try_open(s, diff, taken)) {
        /* Both edges stay inline: the fallthrough continues here and
         * the taken label binds at the join. */
        if (not_taken) {
            gen_set_label(not_taken);
        }
        return;
    }
    if (xemu_superblock_try_merge_fallthrough(s, diff, not_taken, taken)) {
        return;
    }
#endif
    if (not_taken) {
        gen_set_label(not_taken);
    }
#if defined(XBOX)
    s->xemu_sb_tail_kind = XEMU_SB_TAIL_JCC_FALL;
#endif
    gen_jmp_rel_csize(s, 0, 1);

    gen_set_label(taken);
#if defined(XBOX)
    s->xemu_sb_tail_kind = XEMU_SB_TAIL_JCC_TAKEN;
#endif
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
 * live tb->cflags load catches CF_INVALID (invalidation); flush
 * clears entries wholesale, and empty entries fail the tb != NULL
 * check. Misses fall through to the helper, which also owns every
 * special case (breakpoints, single-step, logging, epilogue return).
 */
static bool xemu_ras_inline_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_RAS");
        on = !(e && e[0] == '0');
    }
    return on;
}

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
    tcg_gen_shri_i32(idx, idx, xemu_retc_shift);
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
        if (s->xemu_ret_exit) {
            s->xemu_ret_exit = false;
            if (xemu_ras_inline_on() &&
                !(tb_cflags(s->base.tb) & CF_NO_GOTO_PTR)) {
                xemu_gen_inline_ret_memo(s);
            }
            tcg_gen_xemu_lookup_ret_and_goto_ptr();
        } else {
            if (xemu_ras_inline_on() &&
                !(tb_cflags(s->base.tb) & CF_NO_GOTO_PTR)) {
                TCGv_i32 lin = tcg_temp_new_i32();
                tcg_gen_addi_i32(lin, cpu_eip, (int32_t)s->cs_base);
                tcg_gen_xemu_jc_probe_and_goto_ptr(
                    lin,
                    offsetof(X86CPU, parent_obj.tb_jmp_cache) -
                        offsetof(X86CPU, env),
                    (uint64_t)s->cs_base,
                    (uint32_t)s->base.tb->flags,
                    tb_cflags(s->base.tb));
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
#if defined(XBOX)
/*
 * Superblock M1: follow unconditional same-page FORWARD direct jumps at
 * translate time instead of ending the TB, so TCG liveness and register
 * allocation span the seam — the dead cc_op/operand materialization the
 * XEMU_CCOP_CENSUS sized at ~39-44% of block boundaries simply never
 * gets emitted for the merged edges, and the goto_tb dispatch + successor
 * prologue re-check disappear with it.
 *
 * Safety rests on two gates (docs of record: the 2026-07-18 design memo
 * in the landing commit):
 *  - same page as pc_first (P0): translator_ld can only fault in the
 *    virtually-CONTIGUOUS next page, and CF_PCREL exception unwind takes
 *    the page from env->eip — both are only sound while every merged
 *    instruction stays on P0 (advance_pc's rollback enforces the body,
 *    this gate enforces the target).
 *  - forward only (new_pc > current insn start >= pc_first): tb->size
 *    models the TB as one contiguous [pc_first, pc_next) interval; a
 *    target below pc_first would leave executed code outside the
 *    SMC-registered range (wrong-code-hang class).
 * Everything else (TF/RF/INHIBIT_IRQ, NO_GOTO_TB, breakpoint pages) is
 * already folded into jmp_opt or re-checked per-insn by the loop.
 *
 * XEMU_SUPERBLOCK=N caps follows per TB (0 = off — the default while
 * dark). XEMU_SUPERBLOCK_SIZE=1 arms the runtime taken-exit census
 * below (works under normal chaining; the bump precedes goto_tb).
 * Tail-kind enum: above DisasContext (early call sites assign it).
 */
static uint64_t xemu_sb_tail_taken[XEMU_SB_TAIL_NKINDS];
static uint64_t xemu_sb_m1_capturable;
static uint64_t xemu_sb_merged_seams;
static uint64_t xemu_sb_merged_fallthroughs;
static uint64_t xemu_region_internalized;
static uint64_t xemu_region_demoted_misalign;
static uint64_t xemu_region_demoted_drain;

static void xemu_sb_dump(void)
{
    static const char *const nm[XEMU_SB_TAIL_NKINDS] = {
        "uncond", "call", "jcc_taken", "jcc_fall", "rep", "toomany", "other"
    };
    uint64_t tot = 0;
    for (int i = 0; i < XEMU_SB_TAIL_NKINDS; i++) {
        tot += xemu_sb_tail_taken[i];
    }
    if (!tot && !xemu_sb_merged_seams && !xemu_region_internalized &&
        !xemu_region_demoted_misalign && !xemu_region_demoted_drain) {
        return;
    }
    fprintf(stderr, "xemu: superblock taken goto_tb exits=%llu:",
            (unsigned long long)tot);
    for (int i = 0; i < XEMU_SB_TAIL_NKINDS; i++) {
        fprintf(stderr, " %s=%llu (%.1f%%)", nm[i],
                (unsigned long long)xemu_sb_tail_taken[i],
                tot ? 100.0 * xemu_sb_tail_taken[i] / tot : 0.0);
    }
    fprintf(stderr, "\nxemu: superblock m1_capturable=%llu (%.1f%% of "
            "taken exits)  merged jmp-seams=%llu fallthroughs=%llu "
            "(translate-time)\n",
            (unsigned long long)xemu_sb_m1_capturable,
            tot ? 100.0 * xemu_sb_m1_capturable / tot : 0.0,
            (unsigned long long)xemu_sb_merged_seams,
            (unsigned long long)xemu_sb_merged_fallthroughs);
    fprintf(stderr, "xemu: region internalized=%llu demoted: "
            "misalign=%llu drain=%llu (translate-time)\n",
            (unsigned long long)xemu_region_internalized,
            (unsigned long long)xemu_region_demoted_misalign,
            (unsigned long long)xemu_region_demoted_drain);
}

static void xemu_sb_arm_dump(void)
{
    static bool armed;
    if (!armed) {
        atexit(xemu_sb_dump);
        armed = true;
    }
}

/* Emit "*counter += 1" as TCG ops (translate-time census bump) — the
 * load/add/store the xpage and superblock census sites share. */
static void xemu_gen_counter_inc(uint64_t *counter)
{
    TCGv_ptr p = tcg_constant_ptr(counter);
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, p, 0);
    tcg_gen_addi_i64(t, t, 1);
    tcg_gen_st_i64(t, p, 0);
}

static int xemu_superblock_max(void)
{
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("XEMU_SUPERBLOCK");
        cap = e ? atoi(e) : 0;
        cap = MAX(0, MIN(cap, 64));
        if (cap > 0) {
            xemu_sb_arm_dump();
        }
    }
    return cap;
}

static bool xemu_sb_size_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_SUPERBLOCK_SIZE");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on) {
            xemu_sb_arm_dump();
        }
    }
    return on;
}

/* The M1 gate set minus flag/budget — shared by the merge and the
 * capturable-census bump so the census measures exactly what the merge
 * would take. */
static bool xemu_sb_m1_eligible(DisasContext *s, MemOp ot,
                                target_ulong new_pc)
{
    return s->jmp_opt &&
           !(tb_cflags(s->base.tb) & CF_USE_ICOUNT) &&
           !(!CODE64(s) && ot == MO_16) &&
           translator_is_same_page(&s->base, new_pc) &&
           new_pc > s->base.pc_next;
}

static bool xemu_superblock_try_merge(DisasContext *s, MemOp ot, int diff)
{
    int cap = xemu_superblock_max();
    target_ulong new_pc = s->pc + diff;

    if (likely(cap == 0) || s->xemu_sb_follows >= cap) {
        return false;
    }
    if (!xemu_sb_m1_eligible(s, ot, new_pc)) {
        return false;
    }
    /*
     * Merge: repoint the decode cursor and emit nothing. cc_op /
     * cc_op_dirty / pc_save / is_jmp are deliberately untouched — the
     * lazy flag state flows across the seam exactly as it does between
     * sequential instructions, and CF_PCREL's EIP(X) = EIP(pc_save) +
     * (X - pc_save) invariant holds for any same-segment X.
     */
    s->pc = new_pc;
    s->xemu_sb_follows++;
    xemu_sb_merged_seams++;
    return true;
}

/*
 * Superblock M2: follow the FALLTHROUGH of a conditional branch. The
 * taken edge becomes an out-of-line exit stub reached only by the
 * condition's branch; the runtime fallthrough hops over the stub and
 * translation continues inline at the sequential pc. Sequential
 * continuation is always page-safe (advance_pc rolls the next insn back
 * into a clean DISAS_TOO_MANY end if it leaves the page) and always
 * forward (tb->size stays contiguous), so M2 needs none of M1's
 * page/direction gates. The stub's exit is forced onto the lookup path
 * (this fork's inline jump-cache probe) so it never consumes a goto_tb
 * slot the TB's real final exit may need — and, being unchained, stub
 * edges never enter the xpage backstop registry either. cc state needs
 * no special handling: gen_jmp_rel's !cc_op_dirty invariant means flags
 * were already synced before the condition, on both runtime paths.
 */
/*
 * Region/diamond former (XEMU_REGION=N, default 0 = off): for a FORWARD
 * in-window conditional, do not exit on either edge — leave the already-
 * emitted brcond targeting `taken` as a pending intra-TB label, keep
 * translating the fallthrough arm, and bind the label when linear decode
 * reaches the target (the join). Neither edge exits, so the recorded-
 * label liveness relaxation (tcg.c, TCGLabel.region_join) can elide the
 * dead flag materializations the CCOP census sized. gen_jcc already
 * materialized cc_op before the brcond, so both edges carry the
 * checkpoint state; at the join, a changed arm reconciles via the
 * do_gen_rep label-merge idiom (materialize + CC_OP_DYNAMIC), and a
 * pc_save mismatch uses the translator's -1 resync sentinel. Arms that
 * end the block (is_jmp), overrun the budget, or misalign with the
 * decode stream DEMOTE: the pending label becomes an out-of-line exit
 * stub (the M2 shape) with the checkpointed state restored around it.
 */
static int xemu_region_max(void)
{
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("XEMU_REGION");
        cap = e ? atoi(e) : 0;
        cap = MAX(0, MIN(cap, XEMU_REGION_MAXPEND));
        if (cap > 0) {
            xemu_sb_arm_dump();
        }
    }
    return cap;
}

static bool xemu_region_try_open(DisasContext *s, target_long diff,
                                 TCGLabel *taken)
{
    target_ulong new_pc = s->pc + diff;

    if (likely(xemu_region_max() == 0) ||
        s->xemu_region_npend >= xemu_region_max()) {
        return false;
    }
    if (!s->jmp_opt || (tb_cflags(s->base.tb) & CF_USE_ICOUNT)) {
        return false;
    }
    if (!CODE64(s) && s->dflag == MO_16) {
        return false;
    }
    /* Forward, in-window, same page as pc_first (translator_ld and
     * CF_PCREL unwind both require staying on P0). Window 16 bytes:
     * the first A/B at 64 B measured 72% of opens drain-demoting into
     * the known-negative stub shape (arms long enough to contain
     * block-enders) at −1.34 fps 3/3; a 2-5 insn arm rarely does.
     * Pre-registered: this is the single mechanism-driven iteration —
     * if Gate A still fails, the campaign stops. */
    if (diff <= 0 || diff > 16 ||
        !translator_is_same_page(&s->base, new_pc)) {
        return false;
    }

    taken->region_join = true;
    {
        int i = s->xemu_region_npend++;
        while (i > 0 && s->xemu_region_pend[i - 1].target > new_pc) {
            s->xemu_region_pend[i] = s->xemu_region_pend[i - 1];
            i--;
        }
        s->xemu_region_pend[i] = (typeof(s->xemu_region_pend[0])){
            .target = new_pc, .label = taken,
            .cc_op = s->cc_op, .cc_op_dirty = s->cc_op_dirty,
            .pc_save = s->pc_save,
        };
    }
    /* Translation continues at the fallthrough; is_jmp stays NEXT. */
    return true;
}

static void xemu_region_pop_front(DisasContext *s)
{
    for (int i = 1; i < s->xemu_region_npend; i++) {
        s->xemu_region_pend[i - 1] = s->xemu_region_pend[i];
    }
    s->xemu_region_npend--;
}

/* Demote the front pending label to an out-of-line exit stub (the M2
 * shape) with the checkpointed translator state restored around it. */
static void xemu_region_demote_front(DisasContext *s, bool misalign)
{
    typeof(s->xemu_region_pend[0]) *p = &s->xemu_region_pend[0];
    TCGLabel *cont = gen_new_label();
    int save_op = s->cc_op;
    bool save_dirty = s->cc_op_dirty;
    target_ulong save_ps = s->pc_save;
    DisasJumpType save_jmp = s->base.is_jmp;

    tcg_gen_br(cont);
    gen_set_label(p->label);
    s->cc_op = p->cc_op;
    s->cc_op_dirty = p->cc_op_dirty;
    s->pc_save = p->pc_save;
    s->xemu_sb_stub = true;
    s->xemu_sb_tail_kind = XEMU_SB_TAIL_JCC_TAKEN;
    gen_update_cc_op(s);
    gen_jmp_rel(s, s->dflag, p->target - s->pc, 0);
    s->xemu_sb_stub = false;
    s->cc_op = save_op;
    s->cc_op_dirty = save_dirty;
    s->pc_save = save_ps;
    s->base.is_jmp = save_jmp;
    gen_set_label(cont);

    if (misalign) {
        xemu_region_demoted_misalign++;
    } else {
        xemu_region_demoted_drain++;
    }
    xemu_region_pop_front(s);
}

/* Bind every pending label whose target is the insn about to decode;
 * demote any stepped-over target. Called from i386_tr_insn_start. */
static void xemu_region_insn_start(DisasContext *s)
{
    while (s->xemu_region_npend &&
           s->xemu_region_pend[0].target < s->base.pc_next) {
        xemu_region_demote_front(s, true);
    }
    while (s->xemu_region_npend &&
           s->xemu_region_pend[0].target == s->base.pc_next) {
        typeof(s->xemu_region_pend[0]) *p = &s->xemu_region_pend[0];
        bool cc_differs = (s->cc_op != p->cc_op ||
                           s->cc_op_dirty != p->cc_op_dirty);

        if (cc_differs) {
            /* Fall-edge materialization (pruned by liveness when the
             * join proves it dead); taken edge carries the checkpoint
             * gen_jcc already materialized. */
            gen_update_cc_op(s);
        }
        gen_set_label(p->label);
        if (cc_differs) {
            /* Label-merge idiom (do_gen_rep): flags now live in env. */
            set_cc_op(s, CC_OP_DYNAMIC);
        }
        if (s->pc_save != p->pc_save) {
            s->pc_save = -1;
        }
        xemu_region_internalized++;
        xemu_region_pop_front(s);
    }
}

/* Drain (demote) every still-pending label; called before any
 * TB-terminating emission. */
static void xemu_region_drain(DisasContext *s)
{
    while (s->xemu_region_npend) {
        xemu_region_demote_front(s, false);
    }
}

static bool xemu_superblock_try_merge_fallthrough(DisasContext *s,
        target_long diff, TCGLabel *not_taken, TCGLabel *taken)
{
    int cap = xemu_superblock_max();

    if (likely(cap == 0) || s->xemu_sb_follows >= cap) {
        return false;
    }
    if (!s->jmp_opt || (tb_cflags(s->base.tb) & CF_USE_ICOUNT)) {
        return false;
    }
    /*
     * FORWARD-target conditionals only. Backward conditionals are loop
     * back-edges whose TAKEN side is the hot path — merging them demotes
     * that hot edge from a direct goto_tb chain to the lookup stub,
     * which the first all-conditionals A/B measured as a unanimous
     * regression (−1.6% draw throughput, feedback-amplified to −8.4%
     * fps on F8). Forward conditionals fall through hot; their rare
     * taken edge can afford the stub.
     */
    if (diff <= 0) {
        return false;
    }

    TCGLabel *cont = gen_new_label();
    tcg_gen_br(cont);

    gen_set_label(taken);
    s->xemu_sb_tail_kind = XEMU_SB_TAIL_JCC_TAKEN;
    s->xemu_sb_stub = true;
    gen_jmp_rel(s, s->dflag, diff, 0);
    s->xemu_sb_stub = false;

    gen_set_label(cont);
    if (not_taken) {
        gen_set_label(not_taken);
    }
    /* Undo the stub's terminal state: this TB keeps translating. */
    s->base.is_jmp = DISAS_NEXT;
    s->xemu_sb_follows++;
    xemu_sb_merged_fallthroughs++;
    return true;
}
#endif

static void gen_jmp_rel(DisasContext *s, MemOp ot, int diff, int tb_num)
{
#if defined(XBOX)
    /* Out-of-line stub edges must not consume a goto_tb slot (two per
     * TB, owed to the real final exit) — force the lookup path. */
    bool use_goto_tb = s->jmp_opt && !s->xemu_sb_stub;
#else
    bool use_goto_tb = s->jmp_opt;
#endif
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
             * Cross-page relaxation: the andi above keeps cpu_eip
             * wrap-correct regardless, so only forbid the direct chain when
             * the fork rule disallows it (translator_use_goto_tb agrees
             * below). Default off; see docs/xpage-design.md.
             */
            if (!use_goto_tb || !xemu_xpage_allow(new_pc)) {
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
            /* Masked 32-bit linear target, matching xemu_xpage_allow()'s
             * kernel-window test (wrap-safe classification). */
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

    if (use_goto_tb && translator_use_goto_tb(&s->base, new_pc)) {
        /* jump to same page: we can use a direct jump */
#if defined(XBOX)
        if (xpage_cpd) {
            /*
             * Relaxed cross-page slot emitted: mark the source so
             * tb_add_jump registers every dest it links in the backstop
             * registry (tb-maint.c), regardless of the phys-page
             * comparison — a virtual-alias target can share the source's
             * phys page and would evade a phys-only test.
             */
            s->base.tb->xemu_xpage_reg |= XEMU_XPAGE_TB_CROSS_EMITTER;
        }
        if (unlikely(xemu_inv_prof_on())) {
            xemu_inv_gototb_emitted++;
        }
        if (unlikely(xemu_sb_size_on())) {
            /*
             * Superblock sizing census: runtime-weighted taken-exit
             * counts by tail kind (xpage_taken load/add/store pattern —
             * executes before goto_tb even when the exit is chained).
             * The capturable bucket applies the exact M1 merge gates.
             */
            xemu_gen_counter_inc(&xemu_sb_tail_taken[s->xemu_sb_tail_kind]);
            if (s->xemu_sb_tail_kind == XEMU_SB_TAIL_UNCOND &&
                xemu_sb_m1_eligible(s, ot, new_pc)) {
                xemu_gen_counter_inc(&xemu_sb_m1_capturable);
            }
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
                    gen_fisttl_ST0(s, s->tmp2_i32);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    break;
                case 2:
                    gen_fisttll_ST0(s, s->tmp1_i64);
                    tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    break;
                case 3:
                default:
                    gen_fistt_ST0(s, s->tmp2_i32);
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
                    gen_fist_ST0(s, s->tmp2_i32);
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
            gen_eob(s, DISAS_EOB_ONLY);
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
            /*
             * Inline FPU path caches D-regs in DisasContext::fpregs[]. The
             * helper writes env->fpregs[] directly; flush dirty cache first
             * so the post-write view is coherent, and end the TB so the
             * next TB reloads from memory.
             */
            if (g_use_hard_fpu_inline) {
                gen_flush_fp(s);
            }
            gen_helper_fldt_ST0(tcg_env, s->A0);
            if (g_use_hard_fpu_inline) {
                gen_update_eip_next(s);
                gen_eob(s, DISAS_EOB_ONLY);
            }
            break;
        case 0x1f: /* fstpt mem */
            if (g_use_hard_fpu_inline) {
                gen_flush_fp(s);
            }
            gen_helper_fstt_ST0(tcg_env, s->A0);
            gen_fpop(s);
            break;
        case 0x2c: /* frstor mem */
            if (g_use_hard_fpu_inline) {
                gen_flush_fp(s);
            }
            gen_helper_frstor(tcg_env, s->A0,
                              tcg_constant_i32(s->dflag - 1));
            update_fip = update_fdp = false;
            if (g_use_hard_fpu_inline) {
                gen_update_eip_next(s);
                gen_eob(s, DISAS_EOB_ONLY);
            }
            break;
        case 0x2e: /* fnsave mem */
            if (g_use_hard_fpu_inline) {
                gen_flush_fp(s);
            }
            gen_helper_fsave(tcg_env, s->A0,
                             tcg_constant_i32(s->dflag - 1));
            update_fip = update_fdp = false;
            if (g_use_hard_fpu_inline) {
                gen_update_eip_next(s);
                gen_eob(s, DISAS_EOB_ONLY);
            }
            break;
        case 0x2f: /* fnstsw mem */
            if (g_use_hard_fpu_inline) {
                gen_fnstsw_inline(s, s->tmp2_i32);
            } else {
                gen_helper_fnstsw(s->tmp2_i32, tcg_env);
            }
            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                s->mem_index, MO_LEUW);
            update_fip = update_fdp = false;
            break;
        case 0x3c: /* fbld */
            if (g_use_hard_fpu_inline) {
                gen_flush_fp(s);
            }
            gen_helper_fbld_ST0(tcg_env, s->A0);
            if (g_use_hard_fpu_inline) {
                gen_update_eip_next(s);
                gen_eob(s, DISAS_EOB_ONLY);
            }
            break;
        case 0x3e: /* fbstp */
            if (g_use_hard_fpu_inline) {
                gen_flush_fp(s);
            }
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

            tcg_gen_ld_i32(s->tmp2_i32, tcg_env,
                           offsetof(CPUX86State,
                                    segs[last_seg].selector));
            tcg_gen_st16_i32(s->tmp2_i32, tcg_env,
                             offsetof(CPUX86State, fpds));
            tcg_gen_st_tl(last_addr, tcg_env,
                          offsetof(CPUX86State, fpdp));
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
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
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
                    gen_fldl2t_ST0(s);
                    break;
                case 2:
                    gen_fpush(s);
                    gen_fldl2e_ST0(s);
                    break;
                case 3:
                    gen_fpush(s);
                    gen_fldpi_ST0(s);
                    break;
                case 4:
                    gen_fpush(s);
                    gen_fldlg2_ST0(s);
                    break;
                case 5:
                    gen_fpush(s);
                    gen_fldln2_ST0(s);
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
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_f2xm1(tcg_env);
                break;
            case 1: /* fyl2x */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fyl2x(tcg_env);
                break;
            case 2: /* fptan */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fptan(tcg_env);
                break;
            case 3: /* fpatan */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fpatan(tcg_env);
                break;
            case 4: /* fxtract */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fxtract(tcg_env);
                break;
            case 5: /* fprem1 */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
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
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fprem(tcg_env);
                break;
            case 1: /* fyl2xp1 */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fyl2xp1(tcg_env);
                break;
            case 2: /* fsqrt */
                gen_fsqrt(s);
                break;
            case 3: /* fsincos */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fsincos(tcg_env);
                break;
            case 4: /* frndint */
                gen_frndint(s);
                break;
            case 5: /* fscale */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
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
                    gen_fmov_FT0_STN(s, opreg);
                    gen_helper_fp_arith_ST0_FT0(s, op1);
                }
            }
            break;
        case 0x02: /* fcom */
        case 0x22: /* fcom2, undocumented op */
            gen_fmov_FT0_STN(s, opreg);
            gen_fcom_ST0_FT0(s);
            break;
        case 0x03: /* fcomp */
        case 0x23: /* fcomp3, undocumented op */
        case 0x32: /* fcomp5, undocumented op */
            gen_fmov_FT0_STN(s, opreg);
            gen_fcom_ST0_FT0(s);
            gen_fpop(s);
            break;
        case 0x15: /* da/5 */
            switch (rm) {
            case 1: /* fucompp */
                gen_fmov_FT0_STN(s, 1);
                if (g_use_hard_fpu_inline) {
                    gen_fcom_ST0_FT0(s);
                } else {
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
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
                gen_helper_fclex(tcg_env);
                update_fip = false;
                break;
            case 3: /* fninit */
                if (g_use_hard_fpu_inline) { gen_flush_fp(s); }
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
            if (!(s->cpuid_features & CPUID_CMOV)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_fmov_FT0_STN(s, opreg);
            if (g_use_hard_fpu_inline) {
                gen_fucomi_ST0_FT0_inline(s);
            } else {
                gen_helper_fucomi_ST0_FT0(tcg_env);
            }
            assume_cc_op(s, CC_OP_EFLAGS);
            break;
        case 0x1e: /* fcomi */
            if (!(s->cpuid_features & CPUID_CMOV)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_fmov_FT0_STN(s, opreg);
            if (g_use_hard_fpu_inline) {
                gen_fcomi_ST0_FT0_inline(s);
            } else {
                gen_helper_fcomi_ST0_FT0(tcg_env);
            }
            assume_cc_op(s, CC_OP_EFLAGS);
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
            gen_fmov_FT0_STN(s, opreg);
            if (g_use_hard_fpu_inline) {
                gen_fcom_ST0_FT0(s);
            } else {
                gen_helper_fucom_ST0_FT0(tcg_env);
            }
            break;
        case 0x2d: /* fucomp st(i) */
            gen_fmov_FT0_STN(s, opreg);
            if (g_use_hard_fpu_inline) {
                gen_fcom_ST0_FT0(s);
            } else {
                gen_helper_fucom_ST0_FT0(tcg_env);
            }
            gen_fpop(s);
            break;
        case 0x33: /* de/3 */
            switch (rm) {
            case 1: /* fcompp */
                gen_fmov_FT0_STN(s, 1);
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
                if (g_use_hard_fpu_inline) {
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
            if (!(s->cpuid_features & CPUID_CMOV)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_fmov_FT0_STN(s, opreg);
            if (g_use_hard_fpu_inline) {
                gen_fucomi_ST0_FT0_inline(s);
            } else {
                gen_helper_fucomi_ST0_FT0(tcg_env);
            }
            gen_fpop(s);
            assume_cc_op(s, CC_OP_EFLAGS);
            break;
        case 0x3e: /* fcomip */
            if (!(s->cpuid_features & CPUID_CMOV)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_fmov_FT0_STN(s, opreg);
            if (g_use_hard_fpu_inline) {
                gen_fcomi_ST0_FT0_inline(s);
            } else {
                gen_helper_fcomi_ST0_FT0(tcg_env);
            }
            gen_fpop(s);
            assume_cc_op(s, CC_OP_EFLAGS);
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
                if (g_use_hard_fpu_inline) {
                    /*
                     * Ensure both operands are loaded before the
                     * conditional branch. If a prior flush (from
                     * FUCOMI helper, fclex, transcendentals, etc.)
                     * cleared the inline FP temps, get_st0/get_stn
                     * inside the conditional block would emit ld80f
                     * loads that only execute on the taken path,
                     * leaving TCG temps undefined on the not-taken
                     * path and corrupting subsequent FP operations.
                     */
                    if (fpu_using_double_precision(s)) {
                        (void)get_st0_f64(s);
                        (void)get_stn_f64(s, opreg);
                    } else {
                        (void)get_st0_f32(s);
                        (void)get_stn_f32(s, opreg);
                    }
                }
                op1 = fcmov_cc[op & 3] | (((op >> 3) & 1) ^ 1);
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
        tcg_gen_ld_i32(s->tmp2_i32, tcg_env,
                       offsetof(CPUX86State, segs[R_CS].selector));
        tcg_gen_st16_i32(s->tmp2_i32, tcg_env,
                         offsetof(CPUX86State, fpcs));
        tcg_gen_st_tl(eip_cur_tl(s),
                      tcg_env, offsetof(CPUX86State, fpip));
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
    g_use_hard_fpu_inline = g_use_hard_fpu;
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
    dc->xemu_cc_head = 0;
    dc->xemu_sb_follows = 0;
    dc->xemu_sb_tail_kind = XEMU_SB_TAIL_OTHER;
    dc->xemu_sb_stub = false;
    dc->xemu_region_cand = false;
    dc->xemu_region_npend = 0;
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
#if defined(XBOX)
    /* Bind/demote pending region joins BEFORE this insn's insn_start
     * marker (label + reconcile code belongs to the seam, not the
     * joined instruction). */
    if (unlikely(dc->xemu_region_npend)) {
        xemu_region_insn_start(dc);
    }
#endif
    if (tb_cflags(dcbase->tb) & CF_PCREL) {
        pc_arg &= ~TARGET_PAGE_MASK;
    }
    tcg_gen_insn_start(pc_arg, dc->cc_op);
}

static void i386_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    bool orig_cc_op_dirty = dc->cc_op_dirty;
    CCOp orig_cc_op = dc->cc_op;
    target_ulong orig_pc_save = dc->pc_save;

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
    /* Any region join the decode stream never reached (arm ended the
     * block, budget, page end) demotes to an exit stub before the TB's
     * terminal emission. */
    if (unlikely(dc->xemu_region_npend)) {
        xemu_region_drain(dc);
    }
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
#if defined(XBOX)
        dc->xemu_sb_tail_kind = XEMU_SB_TAIL_TOOMANY;
#endif
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

#if defined(XBOX)
    /* Census: record this TB's flag-liveness classes. gen_update_cc_op
     * only spills; dc->cc_op still names the semantic tail state here. */
    {
        /* Literals, not the XEMU_CCOP_TAIL / HEAD_SHIFT macros: those
         * live in accel/tcg/xemu-inv-prof.h, which is private to the
         * accel/tcg source dir (this file uses the function-local
         * extern idiom instead of including it). Encoding documented
         * there: tail 0=DYN 1=EFLAGS 2=LAZY, head at shift 2. */
        uint8_t tail = (dc->cc_op == CC_OP_DYNAMIC) ? 0
                     : (dc->cc_op == CC_OP_EFLAGS)  ? 1 : 2;
        dc->base.tb->xemu_ccop = tail | (dc->xemu_cc_head << 2)
                               | (dc->xemu_region_cand ? 0x10 : 0);
        if (unlikely(xemu_ccop_census_on())) {
            xemu_ccop_tb_tail[tail]++;
            xemu_ccop_tb_head[dc->xemu_cc_head]++;
        }
    }
#endif
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
    gen_flush_fp((DisasContext *)tcg_ctx->disas_ctx);
}
