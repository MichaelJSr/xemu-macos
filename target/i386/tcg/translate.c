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
#if defined(XBOX)
/* EBB temps for the x87 census and refuter checks (xemu-x87.h). */
#include "tcg/tcg-temp-internal.h"
#endif
#include "exec/translator.h"
#include "exec/target_page.h"
#include "fpu/softfloat.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "helper-tcg.h"
#include "decode-new.h"

#include "exec/log.h"
#include "exec/icount.h"
#if defined(XBOX) && defined(CONFIG_SYSTEM_ONLY)
#include "xemu-hooks.h"
#endif

static int g_use_hard_fpu;
static int g_use_hard_fpu_inline;
#if defined(XBOX)
static int g_x87_wt; /* write-through inline x87 (x87_wt_*) */
static int g_x87_dbl; /* doubles-primary registers (x87_wt_dbl) */
static int g_x87_quiet; /* m64fp loads quiet SNaNs on the FP side */
static int g_x87_hw; /* hot x87 ops on the host x87 unit (gen_x87_hw_*) */
bool xemu_x87_top_key; /* TB keys carry the x87 TOP (x87_top) */
int xemu_sse_vfp; /* inline packed-single add/sub/mul/div (gen_fp_sse_inline) */
#endif

#if defined(XBOX) && (defined(__x86_64__) || defined(__aarch64__))
#include "ui/xemu-settings.h"
#define MAP_GEN_HELPER_SOFT_HARD(name) \
    (g_use_hard_fpu ? gen_helper_##name##__hard : gen_helper_##name##__soft)
#define gen_helper_flds_FT0       MAP_GEN_HELPER_SOFT_HARD(flds_FT0)
#define gen_helper_fop_ST0_m32    MAP_GEN_HELPER_SOFT_HARD(fop_ST0_m32)
#define gen_helper_fop_ST0_m64    MAP_GEN_HELPER_SOFT_HARD(fop_ST0_m64)
#define gen_helper_fop_ST0_i32    MAP_GEN_HELPER_SOFT_HARD(fop_ST0_i32)
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
    int xemu_fds_pend;          /* deferred FDS: its segment, or -1 */
    bool xemu_strict_cpuid;     /* XEMU_STRICT_CPUID (xemu-strict-cpuid.c.inc) */
    uint8_t xemu_esc;           /* opcode map: 0, 0x0f, 0x38 or 0x3a */
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
     * next flush; the pending set and fpstt_delta at the start of this
     * insn (wt_insn_pend, wt_insn_delta); whether a flush has run inside
     * this insn; and the slots insn_start word 2 lands with their values,
     * set by the insn's first guest access (wt_w2_land, wt_w2_val,
     * x87_wt_spill).
     */
    uint8_t wt_pend;
    uint8_t wt_insn_pend;
    uint8_t wt_w2_land;
    bool wt_insn_flushed;
    int wt_insn_delta;
    TCGv_f64 wt_w2_val[8];
    /*
     * wt_st slots whose env image is, or once stored will be, st80f of the
     * cached double (x87_wt_exact). A slot read with ld80f may hold more
     * than a double (fldt, fild m64, fldpi, a softfloat helper's result).
     */
    uint8_t wt_exact;
    /*
     * Deferred stack bookkeeping (x87_wt_land): push and pop move only
     * fpstt_delta and these tag bytes, by slot relative to wt_top, which
     * holds env->fpstt as the flush interval found it (loaded on first
     * use); env->fpstt and env->fptags catch up at the next flush. The
     * wt_insn_tag_* copies are their values at the start of the insn;
     * wt_w2_fp: word 2 carries them and fpstt_delta (an access before any
     * flush in this insn), and wt_w2_pos is where that access's spill
     * begins (x87_wt_land_early).
     */
    TCGv_i32 wt_top;
    uint8_t wt_tag_mask;
    uint8_t wt_tag_val;
    uint8_t wt_insn_tag_mask;
    uint8_t wt_insn_tag_val;
    bool wt_insn_fp;
    bool wt_w2_fp;
    TCGOp *wt_w2_pos;
    /*
     * TOP-keyed translation (xemu_x87_top_key): the eager x87 TOP, what
     * env->fpstt holds once the deferred stack bookkeeping lands, known
     * at translate time from the TB's key; -1 after a helper moved it by
     * a runtime amount, or when TBs are not keyed (x87_top_move).
     */
    int8_t x87_top;
    /*
     * Doubles-primary registers (x87_wt_dbl): physical registers known
     * not stale here (env->xemu_x87_dstale clear), read with plain ld80f.
     */
    uint8_t wt_nstale;
    /*
     * No register can be stale at runtime here: an MMX entry converted
     * any stale one (helper_enter_mmx_sync, behind a test of env's stale
     * bytes), and no flush in this TB has stored a double since. Unlike wt_nstale this survives flushes:
     * C code never makes a register stale, and gen_enter_mmx never runs
     * inside a branch within an insn, the only kind a TB holds.
     */
    bool mmx_synced;
    /* The call being emitted reads only its arguments (gen_pure_call_*). */
    bool pure_call;
    /* fpstt is 0 and every tag valid since an MMX insn (gen_enter_mmx). */
    bool mmx_entered;
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
            e->tc_ptr = NULL;
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
 * xemu: shortest REP MOVS/STOS (in elements) handed to helper_xemu_rep_bulk
 * when the inline path below can't take it; below it the call costs more
 * host instructions than the loop (heavy-f1: 4, 12 and 32 all save less
 * than 8).
 */
#define REP_BULK_MIN 8

/*
 * xemu: runs of 1 to REP_INLINE_MAX elements (3 dwords on average at the
 * hottest heavy-f1 site, mostly 6-16 at the rest) copy or fill inline as
 * plain host loads and stores once both ranges pass the softmmu fast
 * path's own TLB compare in one piece. The loop pays two TLB lookups and
 * a pass through its label (registers reloaded and spilled) per element,
 * the helper a call and two C probes per run. Longer runs, and runs the
 * probes refuse, take the helper and the loop as before. Little-endian
 * hosts that take unaligned plain loads and stores, 32-bit guests only.
 */
#if !defined(TARGET_X86_64) && !HOST_BIG_ENDIAN && \
    (defined(__aarch64__) || defined(__x86_64__))
#define REP_INLINE_MAX 31
#endif
#endif

#ifdef REP_INLINE_MAX
/*
 * Inline TLB probe of [addr, addr + last] for an access of @type, exactly
 * the qemu_ld/st fast path's compare (also with the last byte, so one
 * page): plain RAM with that access allowed, not MMIO, watched or clean.
 * ORs the mismatch into @miss (0 on a hit) and returns the host address.
 */
static TCGv_ptr gen_rep_probe(TCGv addr, TCGv last, TCGv mask,
                              TCGv_ptr table, MMUAccessType type, TCGv miss)
{
    TCGv_ptr ent = tcg_temp_new_ptr();
    TCGv_ptr host = tcg_temp_new_ptr();
    TCGv t = tcg_temp_new();
    TCGv cmp = tcg_temp_new();

    tcg_gen_shri_tl(t, addr, TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS);
    tcg_gen_and_tl(t, t, mask);
    tcg_gen_add_zext_shl_ptr(ent, table, t, 0);
    /* The low half of the comparator, as the backends compare it. */
    tcg_gen_ld_tl(cmp, ent, type == MMU_DATA_STORE
                            ? offsetof(CPUTLBEntry, addr_write)
                            : offsetof(CPUTLBEntry, addr_read));
    tcg_gen_ld_ptr(host, ent, offsetof(CPUTLBEntry, addend));
    tcg_gen_add_tl(t, addr, last);
    tcg_gen_andi_tl(t, t, TARGET_PAGE_MASK);
    tcg_gen_xor_tl(t, t, cmp);
    tcg_gen_or_tl(miss, miss, t);
    tcg_gen_add_zext_shl_ptr(host, host, addr, 0);
    return host;
}

/*
 * Run a REP MOVS/STOS of ECX <= REP_INLINE_MAX elements with DF=0 inline
 * and branch to @done with ECX/ESI/EDI as the loop leaves them, or branch
 * to @miss with nothing done. Nothing here can fault; the elements go in
 * the loop's order (load, store, ascending, so a destination inside the
 * source reads earlier stores), each host access carries the barrier its
 * guest access would, and the loop would also have run them all in this
 * TB pass, so icount is unchanged. A device thread re-arming TLB_NOTDIRTY
 * after the compare races at most 31 stores, as it races the fast path's
 * one store; its clear-then-read can't fit in that window (the bulk
 * helper's page-long one re-dirties instead).
 */
static void gen_rep_short(DisasContext *s, MemOp ot, bool stos, TCGv dshift,
                          TCGLabel *miss, TCGLabel *done)
{
    int fast_ofs = offsetof(CPUNegativeOffsetState,
                            tlb.f[mmuidx_to_fast_index(s->mem_index)]) -
                   sizeof(CPUNegativeOffsetState);
    TCGv n1 = tcg_temp_new();
    TCGv bytes = tcg_temp_new();
    TCGv last = tcg_temp_new();
    TCGv mask = tcg_temp_new();
    TCGv bad = tcg_temp_new();
    TCGv_ptr table = tcg_temp_new_ptr();
    TCGv_ptr hs = NULL, hd;
    TCGv val = NULL;

    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_regs[R_ECX], 0, done);
    tcg_gen_brcondi_tl(TCG_COND_LT, dshift, 0, miss);

    tcg_gen_subi_tl(n1, cpu_regs[R_ECX], 1);
    tcg_gen_shli_tl(bytes, cpu_regs[R_ECX], ot);
    tcg_gen_subi_tl(last, bytes, 1);
    /* The mask is (entries - 1) << CPU_TLB_ENTRY_BITS: its low half. */
    tcg_gen_ld_tl(mask, tcg_env, fast_ofs + offsetof(CPUTLBDescFast, mask));
    tcg_gen_ld_ptr(table, tcg_env, fast_ofs + offsetof(CPUTLBDescFast, table));
    tcg_gen_movi_tl(bad, 0);
    if (!stos) {
        hs = gen_rep_probe(cpu_regs[R_ESI], last, mask, table,
                           MMU_DATA_LOAD, bad);
    }
    hd = gen_rep_probe(cpu_regs[R_EDI], last, mask, table,
                       MMU_DATA_STORE, bad);
    tcg_gen_brcondi_tl(TCG_COND_NE, bad, 0, miss);

    if (stos) {
        val = cpu_regs[R_EAX];
    } else {
        gen_op_add_reg(s, MO_32, R_ESI, bytes);
    }
    gen_op_add_reg(s, MO_32, R_EDI, bytes);
    tcg_gen_movi_tl(cpu_regs[R_ECX], 0);

    for (int i = 0; i < REP_INLINE_MAX; i++) {
        if (i) {
            tcg_gen_brcondi_tl(TCG_COND_EQ, n1, i - 1, done);
        }
        if (!stos) {
            val = tcg_temp_new();
            tcg_gen_xemu_guest_mo(false);
            switch (ot) {
            case MO_8:
                tcg_gen_ld8u_tl(val, hs, i);
                break;
            case MO_16:
                tcg_gen_ld16u_tl(val, hs, i << 1);
                break;
            default:
                tcg_gen_ld_tl(val, hs, i << 2);
                break;
            }
        }
        tcg_gen_xemu_guest_mo(true);
        switch (ot) {
        case MO_8:
            tcg_gen_st8_tl(val, hd, i);
            break;
        case MO_16:
            tcg_gen_st16_tl(val, hd, i << 1);
            break;
        default:
            tcg_gen_st_tl(val, hd, i << 2);
            break;
        }
    }
    tcg_gen_br(done);
}
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
     * Copy or fill a short run inline, else what the helper can as host
     * memmove/memset first. Both stop where the loop below must take over,
     * with ECX/ESI/EDI as the loop would have left them there, and never
     * past one of the loop's TB exits.
     */
    if (can_loop && (fn == gen_movs || fn == gen_stos) &&
        s->aflag == MO_32 && !ADDSEG(s) && s->override < 0) {
        TCGLabel *loop_only = gen_new_label();
        TCGLabel *bulk = gen_new_label();

#ifdef REP_INLINE_MAX
        if (!s->base.plugin_enabled) {
            TCGLabel *miss = gen_new_label();

            tcg_gen_brcondi_tl(TCG_COND_GTU, cpu_regs[R_ECX], REP_INLINE_MAX,
                               bulk);
            gen_rep_short(s, ot, fn == gen_stos, dshift, miss, done);
            gen_set_label(miss);
        }
#endif
        tcg_gen_brcondi_tl(TCG_COND_LTU, cpu_regs[R_ECX], REP_BULK_MIN,
                           loop_only);
        gen_set_label(bulk);
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

static bool x87_wt(DisasContext *s);
#if defined(XBOX)
static bool x87_defer(DisasContext *s);
static TCGv_i32 x87_wt_top(DisasContext *s);
static bool xemu_x87_refute_on(void);
#endif

/* (top + opreg) & 7; top itself is always 0..7. */
static TCGv_i32 gen_top_index(TCGv_i32 top, int opreg)
{
    TCGv_i32 idx;

    if (opreg == 0) {
        return top;
    }
    idx = tcg_temp_new_i32();
    tcg_gen_addi_i32(idx, top, opreg);
    tcg_gen_andi_i32(idx, idx, 7);
    return idx;
}

#if defined(XBOX)
/*
 * TOP-keyed translation. While the write-through path or the host x87
 * ops are on (xemu_x87_top_key), x86_get_tb_cpu_state puts env->fpstt in
 * the lookup key (cs_base bits 32-34), so a TB starts at a known TOP and
 * x87_top follows it: every push and pop, the helpers that move fpstt by
 * a fixed amount (fld*_ST0 and fbld push; fpatan, fyl2x, fyl2xp1 pop;
 * fxtract pushes; fninit, fnsave and the MMX entry set 0) and nothing
 * else, as env->fpstt moves at runtime. Blocks that defer the stack
 * (x87_defer) then address ST(i) and the tags as fixed env offsets
 * (gen_stn_ptr, x87_wt_gen_land).
 *
 * A TB exit hands its TOP to the next TB's key: goto_tb chaining and the
 * inline jump-cache probe and ret memo use x87_top as a constant
 * (x87_key_cs_base), which is exact because it is what env->fpstt holds
 * there (the flush lands the deferred stack before every exit). After a
 * helper that moves fpstt by a runtime amount (fptan and fsincos push
 * only in range; fldenv, frstor, fxrstor and xrstor, which also end the
 * TB) x87_top is unknown (-1): register addresses come from env->fpstt
 * again (x87_wt_top) and every exit takes helper_lookup_tb_ptr, which
 * reads the key from env (x87_top_exit_ok). A new helper that moves
 * fpstt needs its x87_top_move/x87_top_set at the call. XEMU_X87_REFUTE
 * compares x87_top with env at every insn boundary (x87_top_check).
 */
static void x87_top_move(DisasContext *s, int move)
{
    if (s->x87_top >= 0) {
        s->x87_top = (s->x87_top + move) & 7;
    }
}

/*
 * After a helper that leaves fpstt = top, or -1: by a runtime amount.
 * Those helpers convert every stale register first (xemu_x87_sync), so
 * the rest of the TB, which addresses registers from env->fpstt, runs
 * with images only (x87_wt_dbl).
 */
static void x87_top_set(DisasContext *s, int top)
{
    if (xemu_x87_top_key) {
        s->x87_top = top;
        if (top < 0) {
            s->wt_nstale = 0xff;
        }
    }
}

/* Whether this exit may use translate-time key constants. */
static bool x87_top_exit_ok(DisasContext *s)
{
    return !xemu_x87_top_key || s->x87_top >= 0;
}

/* The next TB's cs_base key at an exit where x87_top_exit_ok holds. */
static uint64_t x87_key_cs_base(DisasContext *s)
{
    uint64_t key = (uint32_t)s->cs_base;

    if (xemu_x87_top_key) {
        tcg_debug_assert(s->x87_top >= 0);
        key |= (uint64_t)s->x87_top << XEMU_TB_KEY_TOP_SHIFT;
    }
    return key;
}

/*
 * Deferring blocks (x87_defer): env->fpstt as the flush interval found it
 * (x87_wt_land), if static, else -1. x87_top and fpstt_delta move
 * together inside an interval (x87_wt_move), so this does not change.
 */
static int x87_wt_base(DisasContext *s)
{
    return s->x87_top >= 0 ? (s->x87_top - s->fpstt_delta) & 7 : -1;
}

/*
 * Doubles-primary registers, in write-through blocks with a static TOP.
 * A flush stores a pending result's double to env->xemu_x87_dbl and sets
 * its env->xemu_x87_dstale byte instead of converting it with st80f, and
 * a TB that reads a register takes the double while the byte is set
 * (ld80d), else ld80f of the image. So a value passed from one TB or
 * basic block to the next is not converted at all, and one a later
 * write replaces never is.
 *
 * Only C code needs the image: whatever reads or writes env->fpregs
 * converts a stale register first (fpu_helper.c x87_reg for the helpers,
 * xemu_x87_sync elsewhere, including helper_enter_mmx_sync before MMX
 * code reads the registers raw: gen_enter_mmx), which clears its byte.
 * TB code that moves an image raw moves the double and the byte with it
 * (x87_wt_copy_dbl), flips the double's sign with the image's
 * (fchs/fabs), and converts through a helper for FT0 (gen_fmov_FT0_STN).
 * A mid-block restore lands images and clears their bytes
 * (xemu_x87_restore_pend). Blocks without the write-through path never
 * see a byte set: precision control single takes cpu_set_fpuc, which
 * converts all first (update_fp_status), and a helper after which the
 * TOP is unknown converts all as well (x87_top_set), so the rest of that
 * TB stores images.
 */
static bool x87_wt_dbl(DisasContext *s)
{
    return g_x87_dbl && s->x87_top >= 0 && x87_wt(s);
}

/* Physical register of ST(i), for a static TOP. */
static int x87_phys(DisasContext *s, int i)
{
    return (s->x87_top + i) & 7;
}

#define X87_IMG_OFS(p)   (offsetof(CPUX86State, fpregs[0].d) + \
                          (p) * sizeof(FPReg))
#define X87_DBL_OFS(p)   (offsetof(CPUX86State, xemu_x87_dbl) + \
                          (p) * sizeof(double))
#define X87_STALE_OFS(p) (offsetof(CPUX86State, xemu_x87_dstale) + (p))

/* Whether physical register p may be stale (its byte set) here. */
static bool x87_maybe_stale(DisasContext *s, int p)
{
    return x87_wt_dbl(s) && !(s->wt_nstale & (1 << p));
}

/* env->xemu_x87_dstale[p] = 0, unless known already. */
static void x87_clear_stale(DisasContext *s, int p)
{
    if (x87_maybe_stale(s, p)) {
        tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, X87_STALE_OFS(p));
        s->wt_nstale |= 1 << p;
    }
}

/*
 * After a raw image copy ST(d) = ST(src), or a swap: give ST(d) the
 * double and the byte of ST(src) as well, so it has the same value
 * whichever is current; with swap, ST(src) gets ST(d)'s.
 */
static void x87_wt_copy_dbl(DisasContext *s, int d, int src, bool swap)
{
    int pd = x87_phys(s, d), ps = x87_phys(s, src);
    bool kd = !x87_maybe_stale(s, pd), ks = !x87_maybe_stale(s, ps);
    TCGv_i64 dd = NULL, ds = NULL;
    TCGv_i32 bd = NULL, bs = NULL;

    if (!x87_wt_dbl(s) || (kd && ks)) {
        return;
    }
    if (!ks) {
        ds = tcg_temp_new_i64();
        bs = tcg_temp_new_i32();
        tcg_gen_ld_i64(ds, tcg_env, X87_DBL_OFS(ps));
        tcg_gen_ld8u_i32(bs, tcg_env, X87_STALE_OFS(ps));
    }
    if (swap && !kd) {
        dd = tcg_temp_new_i64();
        bd = tcg_temp_new_i32();
        tcg_gen_ld_i64(dd, tcg_env, X87_DBL_OFS(pd));
        tcg_gen_ld8u_i32(bd, tcg_env, X87_STALE_OFS(pd));
    }
    if (ks) {
        x87_clear_stale(s, pd);
    } else {
        tcg_gen_st_i64(ds, tcg_env, X87_DBL_OFS(pd));
        tcg_gen_st8_i32(bs, tcg_env, X87_STALE_OFS(pd));
        s->wt_nstale &= ~(1 << pd);
    }
    if (swap) {
        if (kd) {
            if (!ks) {
                tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env,
                                X87_STALE_OFS(ps));
                s->wt_nstale |= 1 << ps;
            }
        } else {
            tcg_gen_st_i64(dd, tcg_env, X87_DBL_OFS(ps));
            tcg_gen_st8_i32(bd, tcg_env, X87_STALE_OFS(ps));
            s->wt_nstale &= ~(1 << ps);
        }
    }
}
#endif

/*
 * (fpstt + opreg) & 7. Deferring blocks defer fpstt (x87_wt_land): there
 * it is the interval's starting fpstt plus fpstt_delta, a constant when
 * the TOP is static (x87_top).
 */
static TCGv_i32 gen_st_index(int opreg)
{
#if defined(XBOX)
    DisasContext *s = tcg_ctx->disas_ctx;

    if (x87_defer(s)) {
        if (s->x87_top >= 0) {
            if (unlikely(xemu_x87_refute_on())) {
                x87_wt_top(s);
            }
            return tcg_constant_i32((s->x87_top + opreg) & 7);
        }
        return gen_top_index(x87_wt_top(s), (s->fpstt_delta + opreg) & 7);
    }
#endif
    return gen_top_index(fpstt, opreg);
}

static X87Ptr gen_stn_ptr(int opreg)
{
    TCGv_ptr base;

#if defined(XBOX)
    DisasContext *s = tcg_ctx->disas_ctx;

    if (x87_defer(s) && s->x87_top >= 0) {
        int r = (s->x87_top + opreg) & 7;

        if (unlikely(xemu_x87_refute_on())) {
            x87_wt_top(s);
        }
        return (X87Ptr){ tcg_env, offsetof(CPUX86State, fpregs[0].d) +
                                  r * sizeof(FPReg) };
    }
#endif
    base = tcg_temp_new_ptr();
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
 * x86_64 hosts (g_x87_hw): the hot x87 ops on the images in env, by the
 * host x87 instructions the __hard helpers run, under the same control
 * word (xemu_sync_host_x87_cw), so bit for bit the helpers. FT0 is not
 * written: no later insn reads it.
 */

/* ST(d) = ST(d) op ST(src); op an x87 /digit, sub and div as ST0_FT0's */
static void gen_x87_hw_arith(int op, int d, int src)
{
    X87Ptr a = gen_stn_ptr(d), b = gen_stn_ptr(src);

    tcg_gen_x87_arith(op, a.base, a.ofs, b.base, b.ofs);
}

/* fpus C3/C2/C0 = cc (helper_fcom_ST0_FT0's fcom_ccval) */
static void gen_x87_hw_fpus_cc(TCGv_i32 cc)
{
    TCGv_i32 fpus = tcg_temp_new_i32();

    tcg_gen_ld16u_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
    tcg_gen_andi_i32(fpus, fpus, ~0x4500);
    tcg_gen_or_i32(fpus, fpus, cc);
    tcg_gen_st16_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
}

/* C3|C2|C0 of ST0 against ST(i), in their fpus places */
static TCGv_i32 gen_x87_hw_cmp(int i)
{
    X87Ptr a = gen_stn_ptr(0), b = gen_stn_ptr(i);
    TCGv_i32 cc = tcg_temp_new_i32();

    tcg_gen_x87_cmp(cc, a.base, a.ofs, b.base, b.ofs);
    return cc;
}

/* ST0 op= m (fop m32fp/m32int/m64fp; op 2 and 3 compare): v32 or v64 */
static void gen_x87_hw_fop(int op, int type, TCGv_i32 v32, TCGv_i64 v64)
{
    X87Ptr p = gen_stn_ptr(0);

    if (op == 2 || op == 3) {
        TCGv_i32 cc = tcg_temp_new_i32();

        if (v64) {
            tcg_gen_x87_cmp_i64(cc, type, p.base, p.ofs, v64);
        } else {
            tcg_gen_x87_cmp_i32(cc, type, p.base, p.ofs, v32);
        }
        gen_x87_hw_fpus_cc(cc);
    } else if (v64) {
        tcg_gen_x87_arith_i64(op, type, p.base, p.ofs, v64);
    } else {
        tcg_gen_x87_arith_i32(op, type, p.base, p.ofs, v32);
    }
}
#endif

#if defined(XBOX)
/*
 * The ops that only move bits in env -- push/pop, fnstsw, register moves
 * and fxch, fchs/fabs, fld1/fldz, ffree -- are emitted as TCG code instead
 * of helper calls (both modes; the inline x87 cache stays off). They write
 * the same env bytes the helpers do, at the same point in the instruction.
 */

/*
 * The 10 bytes of a floatx80, not its padding: an 8-byte load across bytes
 * 8-15 doesn't forward from the 10-byte fstpt the x86_64 hard helpers
 * store, and stalls until it commits.
 */
static void x87_gen_copy(X87Ptr dst, X87Ptr src)
{
    TCGv_i64 lo = tcg_temp_new_i64();
    TCGv_i32 hi = tcg_temp_new_i32();
    tcg_gen_ld_i64(lo, src.base, src.ofs);
    tcg_gen_ld16u_i32(hi, src.base, src.ofs + 8);
    tcg_gen_st_i64(lo, dst.base, dst.ofs);
    tcg_gen_st16_i32(hi, dst.base, dst.ofs + 8);
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
#include "xemu-strict-cpuid.h"

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
    /* Code after a BB_END op may be reached by a branch as well. */
    s->wt_nstale = 0;
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
 *    shadow double the exiting access's slow path stored (x87_wt_spill,
 *    st_f64_slow);
 *  - the conversions are the backend's ld80f/st80f, which match the C
 *    conversions bit for bit;
 *  - each register's double, as the next helper would read it back, is
 *    kept in a temp until the next flush (gen_flush_fp), so a chain of
 *    x87 instructions does not reconvert, and past the flush in env
 *    (x87_wt_dbl). st80f is exact, so
 *    that is the computed result itself; only a double loaded from memory
 *    can be a signalling NaN, which reads back quieted (x87_wt_canon, or
 *    FMAX on the FP side for m64fp loads, x87_wt_ld_f64);
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
 * Blocks that defer the x87 stack bookkeeping (x87_wt_land) and, under
 * icount, FDP and FDS (xemu_x87_defer_fdp): the write-through ones, and
 * on x86_64 hosts with the hard FPU every block, at any precision, since
 * the host x87 ops (g_x87_hw) write their results to env at once and only
 * push, pop and the tags are left to defer. The soft FPU keeps the eager
 * path.
 */
static bool x87_defer(DisasContext *s)
{
    return x87_wt(s) || g_x87_hw;
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
        *slot = tcg_temp_new_f64();
        if (x87_maybe_stale(s, x87_phys(s, i))) {
            /* The double while stale (x87_wt_dbl), else ld80f. */
            int ph = x87_phys(s, i);
            tcg_gen_ld80d_f64(*slot, tcg_env, X87_IMG_OFS(ph),
                              X87_STALE_OFS(ph), X87_DBL_OFS(ph));
            if (unlikely(xemu_x87_census_on())) {
                TCGv_i64 b = tcg_temp_new_i64();
                tcg_gen_ld8u_i64(b, tcg_env, X87_STALE_OFS(ph));
                xemu_x87_gen_acc_i64(&xemu_x87_dbl_hit, b);
                xemu_gen_counter_inc(&xemu_x87_dbl_reads);
            }
        } else {
            X87Ptr p = gen_stn_ptr(i);
            tcg_gen_ld80f_f64(*slot, p.base, p.ofs);
        }
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
 * Deferred stores. A result's store waits for the next flush (gen_flush_fp:
 * every helper call except the argument-only pure calls (gen_pure_call_begin),
 * every BB_END op and the TB end), so a slot written again
 * before then, which most x87 chains do (fld, fmul, fadd... on one ST0),
 * stores once: its double, the image left stale, with a static TOP
 * (x87_wt_dbl), else st80f. Only an overwrite cancels a store: a popped
 * slot is still stored, since FSAVE/FXSAVE expose empty registers.
 *
 * A mid-block exit (a guest fault, cpu_io_recompile) must still leave env
 * as the eager stores would. Only a guest access (qemu_ld/st) or a helper
 * call can leave mid-block, and a call flushes first; an access leaves
 * only through its slow path. So before each guest access, x87_wt_spill
 * (gen_ldst_prologue, from tcg-op-ldst.c) has the access's slow path store
 * the doubles of the slots pending at the start of the insn to
 * env->xemu_x87_shadow (st_f64_slow: no code on the fast path), and the
 * insn's first access writes into insn_start word 2 those slots and where
 * fpstt stands. Shadow slot r is wt_st slot r: physical register
 * (fpstt + r) & 7 for the fpstt at the flush interval's start. A restore
 * (x86_restore_state_to_opc -> xemu_x87_restore) converts them with
 * double_to_floatx80_fast, st80f's C twin.
 *
 * A flush inside the insn freezes word 2: if no access came first, its
 * pending part stays 0, so an exit later in the insn (the helper's fault,
 * a later access) lands nothing over what the flush and the helper wrote.
 * If one came first (`div [mem]`, an SSE helper with a memory operand),
 * the flush also stores each landing slot's shadow, in line, as it stores
 * the slot, so a later exit lands the same bytes: no helper reached after
 * an access in the same insn writes x87 registers (XEMU_X87_REFUTE checks
 * it at every restore).
 *
 * Invariants: a pending slot always has its double in wt_st; and env's
 * image of a pending slot is stale, so TCG code that reads or writes the
 * raw image (x87_gen_copy, fxch, the sign ops, the constants) goes through
 * the cached double or x87_wt_sync first (and, for a register a flush left
 * stale, x87_wt_dbl). Helpers need nothing: their call flushes, and the
 * pure calls that skip the flush read only their arguments. An insn's own
 * push, pop and tag changes may not reach env before its last guest
 * access, since a restore does not roll them back; x87_wt_land_early
 * lands only the stack state deferred from earlier insns, before the
 * first access.
 */
static bool x87_wt_pending(DisasContext *s, int slot)
{
    return s->wt_pend & (1 << slot);
}

/* Store wt_st slot `slot`'s image now, as the eager path did. */
static void x87_wt_store_img(DisasContext *s, int slot)
{
    X87Ptr p = gen_stn_ptr((slot - s->fpstt_delta) & 7);

    tcg_gen_st80f_f64(s->wt_st[slot], p.base, p.ofs);
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_st80f);
    }
}

/*
 * The flush's store of a pending slot: its double, the image left stale
 * (x87_wt_dbl), or st80f.
 */
static void x87_wt_store(DisasContext *s, int slot)
{
    int ph;

    if (!x87_wt_dbl(s)) {
        x87_wt_store_img(s, slot);
        return;
    }
    ph = (x87_wt_base(s) + slot) & 7;
    tcg_gen_st_f64(s->wt_st[slot], tcg_env, X87_DBL_OFS(ph));
    tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, X87_STALE_OFS(ph));
    s->wt_nstale &= ~(1 << ph);
    s->mmx_synced = false;
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_dbl_stores);
    }
}

/* Make env's image of a slot current before TCG code reads it raw. */
static void x87_wt_sync(DisasContext *s, int slot)
{
    if (x87_wt_pending(s, slot)) {
        x87_wt_store_img(s, slot);
        if (x87_wt_dbl(s)) {
            x87_clear_stale(s, (x87_wt_base(s) + slot) & 7);
        }
        s->wt_pend &= ~(1 << slot);
    }
}

/*
 * Refuter: (top + opreg) & 7, as gen_top_index computes it, in an EBB
 * temp the caller frees (xemu-x87.h, xemu_x87_temp_ebb_tl).
 */
static TCGv_i32 x87_chk_index(TCGv_i32 top, int opreg)
{
    TCGv_i32 idx = tcg_temp_ebb_new_i32();

    tcg_gen_addi_i32(idx, top, opreg);
    tcg_gen_andi_i32(idx, idx, 7);
    return idx;
}

/*
 * Refuter: gen_stn_ptr in a write-through block, where a TOP that is not
 * static (x87_top < 0) takes an EBB temp the caller frees
 * (x87_chk_ptr_free).
 */
static X87Ptr x87_chk_stn_ptr(DisasContext *s, int opreg)
{
    TCGv_ptr base;
    TCGv_i32 idx;

    if (s->x87_top >= 0) {
        return gen_stn_ptr(opreg);
    }
    idx = x87_chk_index(x87_wt_top(s), (s->fpstt_delta + opreg) & 7);
    base = tcg_temp_ebb_new_ptr();
    tcg_gen_add_zext_shl_ptr(base, tcg_env, idx, 4);
    tcg_temp_free_i32(idx);
    return (X87Ptr){ base, offsetof(CPUX86State, fpregs[0].d) };
}

static void x87_chk_ptr_free(X87Ptr p)
{
    if (p.base != tcg_env) {
        tcg_temp_free_ptr(p.base);
    }
}

/* Defer slot `slot`'s store of r. */
static void x87_wt_pend(DisasContext *s, int slot, TCGv_f64 r)
{
    s->wt_st[slot] = r;
    s->wt_pend |= 1 << slot;
    s->wt_exact |= 1 << slot;
    if (unlikely(xemu_x87_refute_on())) {
        X87Ptr p = x87_chk_stn_ptr(s, (slot - s->fpstt_delta) & 7);

        xemu_x87_gen_poison(p);
        x87_chk_ptr_free(p);
    }
}

/*
 * Deferred stack bookkeeping (x87_defer: write-through blocks, and every
 * block with the host x87 ops). Push and pop only move fpstt_delta and
 * note the tag byte (x87_wt_set_tag); env->fpstt and env->fptags catch up
 * at the next flush (x87_wt_land), from wt_top, env->fpstt as the flush
 * interval found it. Nothing else in such a block writes env->fpstt
 * (the fpstt global is unused there), so it holds wt_top at
 * every exit, and an insn's first guest access before any flush puts the
 * insn's starting delta and tags in word 2 (x87_wt_spill), which a
 * restore lands (xemu_x87_restore_stack).
 *
 * A flush after that access in the same insn lands them as well, so an
 * exit later in the insn must not apply them again: fpstt would move
 * twice. That flush moves a landing of the insn's starting state to just
 * before the access and clears the stack fields from word 2, whose
 * shadow base then sits -delta from the fpstt landed there
 * (x87_wt_land_early). wt_top is loaded before that point, so the moved
 * code and the flush both build on the interval's fpstt.
 *
 * XEMU_X87_REFUTE keeps the eager fpstt and tags in env's *_chk copies,
 * synced from env where wt_top is loaded, and compares the deferred view
 * with them at every insn boundary, flush and restore.
 */
static void x87_wt_chk_sync(TCGv_i32 top)
{
    TCGv_i64 t = tcg_temp_ebb_new_i64();

    tcg_gen_st_i32(top, tcg_env, offsetof(CPUX86State, xemu_x87_fpstt_chk));
    tcg_gen_ld_i64(t, tcg_env, offsetof(CPUX86State, fptags));
    tcg_gen_st_i64(t, tcg_env, offsetof(CPUX86State, xemu_x87_fptags_chk));
    tcg_temp_free_i64(t);
}

static TCGv_i32 x87_wt_top(DisasContext *s)
{
    if (!s->wt_top) {
        int base = x87_wt_base(s);

        if (base >= 0) {
            /* TOP-keyed (x87_top): a constant; the refuter syncs from env. */
            s->wt_top = tcg_constant_i32(base);
            if (unlikely(xemu_x87_refute_on())) {
                TCGv_i32 t = tcg_temp_ebb_new_i32();

                tcg_gen_ld_i32(t, tcg_env, offsetof(CPUX86State, fpstt));
                x87_wt_chk_sync(t);
                xemu_x87_gen_ne_count_i32(&xemu_x87_key_viol, t, s->wt_top);
                xemu_gen_counter_inc(&xemu_x87_key_checks);
                tcg_temp_free_i32(t);
            }
            return s->wt_top;
        }
        s->wt_top = tcg_temp_new_i32();
        tcg_gen_ld_i32(s->wt_top, tcg_env, offsetof(CPUX86State, fpstt));
        if (unlikely(xemu_x87_refute_on())) {
            x87_wt_chk_sync(s->wt_top);
        }
    }
    return s->wt_top;
}

/* Refuter: the eager copy of "fpstt += move; tag(ST(opreg)) = value". */
static void x87_wt_chk_step(DisasContext *s, int move, int opreg, int value)
{
    TCGv_i32 chk;

    x87_wt_top(s);
    chk = tcg_temp_ebb_new_i32();
    tcg_gen_ld_i32(chk, tcg_env, offsetof(CPUX86State, xemu_x87_fpstt_chk));
    if (move) {
        tcg_gen_addi_i32(chk, chk, move);
        tcg_gen_andi_i32(chk, chk, 7);
        tcg_gen_st_i32(chk, tcg_env,
                       offsetof(CPUX86State, xemu_x87_fpstt_chk));
    }
    if (value >= 0) {
        TCGv_ptr p = tcg_temp_ebb_new_ptr();
        TCGv_i32 idx = x87_chk_index(chk, opreg);

        tcg_gen_add_zext_shl_ptr(p, tcg_env, idx, 0);
        tcg_gen_st8_i32(tcg_constant_i32(value), p,
                        offsetof(CPUX86State, xemu_x87_fptags_chk[0]));
        tcg_temp_free_ptr(p);
        tcg_temp_free_i32(idx);
    }
    tcg_temp_free_i32(chk);
}

/* fpstt += move (push -1, pop +1, fdecstp, fincstp). */
static void x87_wt_move(DisasContext *s, int move)
{
    if (unlikely(xemu_x87_refute_on())) {
        x87_wt_chk_step(s, move, 0, -1);
    }
    s->fpstt_delta += move;
    x87_top_move(s, move);
}

/* The tag of ST(opreg) = value (0 valid, 1 empty), as gen_set_fptag_stn. */
static void x87_wt_set_tag(DisasContext *s, int opreg, int value)
{
    int r = (s->fpstt_delta + opreg) & 7;

    if (unlikely(xemu_x87_refute_on())) {
        x87_wt_chk_step(s, 0, opreg, value);
    }
    s->wt_tag_mask |= 1 << r;
    s->wt_tag_val = (s->wt_tag_val & ~(1 << r)) | (value << r);
}

/*
 * Emit the tag bytes `mask`/`val` by slot from top, and fpstt = top + move;
 * top is the constant base when that is static (base >= 0, x87_wt_base).
 */
static void x87_wt_gen_land(TCGv_i32 top, int base, uint8_t mask,
                            uint8_t val, int move)
{
    for (int r = 0; r < 8; r++) {
        if (!(mask & (1 << r))) {
            continue;
        }
        if (base >= 0) {
            tcg_gen_st8_i32(tcg_constant_i32((val >> r) & 1), tcg_env,
                            offsetof(CPUX86State, fptags[0]) +
                            ((base + r) & 7));
        } else {
            TCGv_ptr p = tcg_temp_new_ptr();
            tcg_gen_add_zext_shl_ptr(p, tcg_env, gen_top_index(top, r), 0);
            tcg_gen_st8_i32(tcg_constant_i32((val >> r) & 1), p,
                            offsetof(CPUX86State, fptags[0]));
        }
    }
    if (move & 7) {
        tcg_gen_st_i32(base >= 0 ? tcg_constant_i32((base + move) & 7)
                                 : gen_top_index(top, move & 7),
                       tcg_env, offsetof(CPUX86State, fpstt));
    }
}

/* Refuter: count env's fpstt and tags differing from the eager copy. */
static void x87_wt_chk_env(void)
{
    TCGv_i32 a = tcg_temp_ebb_new_i32(), b = tcg_temp_ebb_new_i32();
    TCGv_i64 x = tcg_temp_ebb_new_i64(), y = tcg_temp_ebb_new_i64();

    tcg_gen_ld_i32(a, tcg_env, offsetof(CPUX86State, fpstt));
    tcg_gen_ld_i32(b, tcg_env, offsetof(CPUX86State, xemu_x87_fpstt_chk));
    xemu_x87_gen_ne_count_i32(&xemu_x87_wt_top_viol, a, b);
    tcg_gen_ld_i64(x, tcg_env, offsetof(CPUX86State, fptags));
    tcg_gen_ld_i64(y, tcg_env, offsetof(CPUX86State, xemu_x87_fptags_chk));
    tcg_gen_setcond_i64(TCG_COND_NE, x, x, y);
    xemu_x87_gen_acc_i64(&xemu_x87_wt_top_viol, x);
    xemu_gen_counter_inc(&xemu_x87_wt_top_checks);
    tcg_temp_free_i32(a);
    tcg_temp_free_i32(b);
    tcg_temp_free_i64(x);
    tcg_temp_free_i64(y);
}

/*
 * Refuter, at the top of every insn: the deferred view (wt_top +
 * fpstt_delta, env's tags under the noted ones) is the eager copy.
 */
static void x87_wt_chk_view(DisasContext *s)
{
    TCGv_i32 a, b;

    if (!s->wt_top) {
        return;
    }
    a = x87_chk_index(s->wt_top, s->fpstt_delta & 7);
    b = tcg_temp_ebb_new_i32();
    tcg_gen_ld_i32(b, tcg_env, offsetof(CPUX86State, xemu_x87_fpstt_chk));
    xemu_x87_gen_ne_count_i32(&xemu_x87_wt_top_viol, a, b);
    tcg_temp_free_i32(a);
    tcg_temp_free_i32(b);
    for (int r = 0; r < 8; r++) {
        TCGv_ptr p = tcg_temp_ebb_new_ptr();
        TCGv_i32 idx = x87_chk_index(s->wt_top, r);
        TCGv_i32 want = tcg_temp_ebb_new_i32();
        TCGv_i32 have = tcg_temp_ebb_new_i32();

        tcg_gen_add_zext_shl_ptr(p, tcg_env, idx, 0);
        if (s->wt_tag_mask & (1 << r)) {
            tcg_gen_movi_i32(want, (s->wt_tag_val >> r) & 1);
        } else {
            tcg_gen_ld8u_i32(want, p, offsetof(CPUX86State, fptags[0]));
        }
        tcg_gen_ld8u_i32(have, p,
                         offsetof(CPUX86State, xemu_x87_fptags_chk[0]));
        xemu_x87_gen_ne_count_i32(&xemu_x87_wt_top_viol, want, have);
        tcg_temp_free_ptr(p);
        tcg_temp_free_i32(idx);
        tcg_temp_free_i32(want);
        tcg_temp_free_i32(have);
    }
    xemu_gen_counter_inc(&xemu_x87_wt_top_checks);
}

/* The flush's half: land the tags and fpstt, then start a new interval. */
static void x87_wt_land(DisasContext *s)
{
    if (!x87_defer(s)) {
        return;
    }
    if (s->wt_tag_mask || (s->fpstt_delta & 7)) {
        x87_wt_gen_land(x87_wt_top(s), x87_wt_base(s), s->wt_tag_mask,
                        s->wt_tag_val, s->fpstt_delta);
    }
    if (unlikely(xemu_x87_refute_on()) && s->wt_top) {
        x87_wt_chk_env();
    }
    s->wt_top = NULL;
    s->wt_tag_mask = 0;
    s->wt_tag_val = 0;
}

/*
 * A flush after the insn's first access, which put the insn's starting
 * delta and tags in word 2: land those just before that access instead,
 * so that no exit in the insn applies them on top of this flush.
 */
static void x87_wt_land_early(DisasContext *s)
{
    TCGOp *last = tcg_last_op();
    TCGOp *pos = s->wt_w2_pos;
    TCGOp *op = s->base.insn_start;
    uint64_t w2 = tcg_get_insn_start_param(op, 2);

    w2 &= ~(XEMU_X87_W2_STACK_MASK | (7ull << XEMU_X87_W2_TOP_SHIFT));
    w2 |= (uint64_t)(-s->wt_insn_delta & 7) << XEMU_X87_W2_TOP_SHIFT;
    tcg_set_insn_start_param(op, 2, w2);

    x87_wt_gen_land(s->wt_top, x87_wt_base(s), s->wt_insn_tag_mask,
                    s->wt_insn_tag_val, s->wt_insn_delta);
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_stack_early);
    }
    while ((op = QTAILQ_NEXT(last, link)) != NULL) {
        QTAILQ_REMOVE(&tcg_ctx->ops, op, link);
        QTAILQ_INSERT_AFTER(&tcg_ctx->ops, pos, op, link);
        pos = op;
    }
    s->wt_w2_fp = false;
}

/*
 * Refuter, at every guest access that can land stores: exactly the
 * landing slots hold the poison. Their doubles also go to
 * env->xemu_x87_shadow_chk here, in line, which the restore compares with
 * what the slow path stored (xemu_x87_restore_pend).
 */
static void x87_wt_exit_check(DisasContext *s, uint8_t land)
{
    for (int r = 0; r < 8; r++) {
        if (land & (1 << r)) {
            X87Ptr p = x87_chk_stn_ptr(s, (r - s->fpstt_delta) & 7);

            xemu_x87_gen_poison_check(&xemu_x87_wt_exit_viol, p, true);
            x87_chk_ptr_free(p);
            tcg_gen_st_f64(s->wt_st[r], tcg_env,
                           offsetof(CPUX86State, xemu_x87_shadow_chk[r]));
        }
    }
    /* Translate-time: word 2's stack fields hold at this access. */
    xemu_x87_wt_exit_viol += s->fpstt_delta != s->wt_insn_delta ||
                             s->wt_tag_mask != s->wt_insn_tag_mask ||
                             s->wt_tag_val != s->wt_insn_tag_val;
    xemu_gen_counter_inc(&xemu_x87_wt_exit_checks);
}

/*
 * Before a guest access, which may leave the TB here: a restore at this
 * insn lands word 2's slots, the stores pending at the start of the insn,
 * from their shadows, which the access's slow path stores (st_f64_slow:
 * no code on the fast path). Nothing to do once a flush ran in this insn
 * (word 2 is frozen, see above).
 */
static void x87_wt_spill(DisasContext *s)
{
    uint8_t land = s->wt_insn_pend & s->wt_pend;

    if ((!land && !s->wt_insn_fp) || s->wt_insn_flushed) {
        return;
    }
    if (!s->wt_w2_land && !s->wt_w2_fp) {
        TCGOp *op = s->base.insn_start;
        uint64_t w2 = tcg_get_insn_start_param(op, 2);

        /*
         * env->fpstt is the flush interval's (x87_wt_land), the base of
         * shadow slot 0: no shadow offset.
         */
        w2 |= (uint64_t)land << XEMU_X87_W2_PEND_SHIFT;
        if (s->wt_insn_fp) {
            /* A flush later in this insn moves the landing here. */
            x87_wt_top(s);
            s->wt_w2_pos = tcg_last_op();
            s->wt_w2_fp = true;
            w2 |= (uint64_t)(s->wt_insn_delta & 7) << XEMU_X87_W2_MOVE_SHIFT;
            w2 |= (uint64_t)s->wt_insn_tag_mask << XEMU_X87_W2_TAGM_SHIFT;
            w2 |= (uint64_t)s->wt_insn_tag_val << XEMU_X87_W2_TAGV_SHIFT;
        }
        tcg_set_insn_start_param(op, 2, w2);
        s->wt_w2_land = land;
        for (int r = 0; r < 8; r++) {
            s->wt_w2_val[r] = s->wt_st[r];
        }
    }
    tcg_debug_assert(s->fpstt_delta == s->wt_insn_delta);
    tcg_debug_assert(s->wt_tag_mask == s->wt_insn_tag_mask &&
                     s->wt_tag_val == s->wt_insn_tag_val);
    if (unlikely(xemu_x87_refute_on())) {
        x87_wt_exit_check(s, land);
    }
    for (int r = 0; r < 8; r++) {
        if (land & (1 << r)) {
            tcg_gen_st_f64_slow(s->wt_st[r],
                                offsetof(CPUX86State, xemu_x87_shadow[r]));
        }
    }
}

/*
 * The flush's half: store every pending slot, then the stack bookkeeping
 * (x87_wt_land). Called from gen_flush_fp before it drops wt_st and
 * fpstt_delta.
 */
static void x87_wt_flush(DisasContext *s)
{
    if (s->wt_w2_fp && !s->wt_insn_flushed) {
        x87_wt_land_early(s);
    }
    if (s->wt_w2_land && !s->wt_insn_flushed) {
        /*
         * An exit later in this insn lands word 2's slots again: give
         * their shadows what this flush stores (they are unchanged since
         * the access, see above).
         */
        for (int r = 0; r < 8; r++) {
            if (s->wt_w2_land & (1 << r)) {
                tcg_gen_st_f64(s->wt_w2_val[r], tcg_env,
                               offsetof(CPUX86State, xemu_x87_shadow[r]));
                if (unlikely(xemu_x87_refute_on())) {
                    tcg_gen_st_f64(s->wt_w2_val[r], tcg_env,
                                   offsetof(CPUX86State,
                                            xemu_x87_shadow_chk[r]));
                }
            }
        }
    }
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
    x87_wt_land(s);
    s->wt_insn_flushed = true;
}

/*
 * Refuter, at the top of every insn: exactly the pending slots hold the
 * poison (their shadows are checked where an exit can happen,
 * x87_wt_exit_check).
 */
static void x87_wt_boundary_check(DisasContext *s)
{
    if (!xemu_x87_refute_on() || !x87_defer(s)) {
        return;
    }
    if (!x87_wt(s)) {
        /* Host x87 ops: their results are in env already. */
        x87_wt_chk_view(s);
        return;
    }
    for (int r = 0; r < 8; r++) {
        if (x87_wt_pending(s, r) || !x87_wt_dbl(s)) {
            X87Ptr p = x87_chk_stn_ptr(s, (r - s->fpstt_delta) & 7);

            xemu_x87_gen_poison_check(&xemu_x87_wt_bnd_viol, p,
                                      x87_wt_pending(s, r));
            x87_chk_ptr_free(p);
        } else {
            /* Poison exactly while stale (x87_wt_dbl). */
            xemu_x87_gen_poison_stale(&xemu_x87_wt_bnd_viol,
                                      (x87_wt_base(s) + r) & 7);
        }
    }
    xemu_x87_gen_poison_check(&xemu_x87_wt_bnd_viol, gen_ft0_ptr(), false);
    xemu_gen_counter_inc(&xemu_x87_wt_bnd_checks);
    x87_wt_chk_view(s);
}

/*
 * Refuter, at the top of every insn of a TOP-keyed TB: x87_top is the TOP
 * env holds. Deferring blocks keep env->fpstt at the interval's base
 * (x87_wt_land); other blocks move the fpstt global eagerly. At a TB's
 * first insn this checks the exit that chose it (goto_tb, the probes).
 */
static void x87_top_check(DisasContext *s)
{
    TCGv_i32 t;

    if (!xemu_x87_refute_on() || s->x87_top < 0) {
        return;
    }
    t = tcg_temp_ebb_new_i32();
    if (x87_defer(s)) {
        tcg_gen_ld_i32(t, tcg_env, offsetof(CPUX86State, fpstt));
        xemu_x87_gen_ne_count_i32(&xemu_x87_key_viol, t,
                                  tcg_constant_i32(x87_wt_base(s)));
    } else {
        tcg_gen_andi_i32(t, fpstt, 7);
        xemu_x87_gen_ne_count_i32(&xemu_x87_key_viol, t,
                                  tcg_constant_i32(s->x87_top));
    }
    xemu_gen_counter_inc(&xemu_x87_key_checks);
    tcg_temp_free_i32(t);
}

/*
 * ST(i) = r, as a helper writes double_to_floatx80_fast(result). r reads
 * back unchanged: it is computed by the host (fadd, fcvt, scvtf, fmax...),
 * which never returns a signalling NaN.
 */
static void x87_wt_set_st(DisasContext *s, int i, TCGv_f64 r)
{
    int slot = (s->fpstt_delta + i) & 7;

    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_writes);
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

/*
 * fpus C3/C2/C0 from a compare's ZF|PF|CF in their fpus places
 * (tcg_gen_com_sh_f64 by 8: ZF, PF, CF at 6, 2, 0 become C3, C2, C0 at
 * 14, 10, 8); C1 and the rest stay.
 */
static void x87_wt_fpus_cc(TCGv_i64 res)
{
    TCGv_i64 fpus = tcg_temp_new_i64();

    tcg_gen_ld16u_i64(fpus, tcg_env, offsetof(CPUX86State, fpus));
    tcg_gen_andi_i64(fpus, fpus, ~0x4500);
    tcg_gen_or_i64(fpus, fpus, res);
    tcg_gen_st16_i64(fpus, tcg_env, offsetof(CPUX86State, fpus));
}

/* helper_fcom_ST0_FT0: fpus C3/C2/C0 from a double compare. */
static void x87_wt_fcom(TCGv_f64 a, TCGv_f64 b)
{
    TCGv_i64 res = tcg_temp_new_i64();

    tcg_gen_com_sh_f64(res, a, b, 8);
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
static TCGv_i64 x87_wt_com_st(DisasContext *s, int i, bool quiet,
                              unsigned sh)
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
    tcg_gen_com_sh_f64(res, x87_wt_get_st(s, 0), x87_wt_get_st(s, i), sh);
    return res;
}

/* fucom/fucomp/fucompp: false if the helper must run. */
static bool gen_fucom_wt(DisasContext *s, int i)
{
    TCGv_i64 res = x87_wt_com_st(s, i, true, 8);

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
    TCGv_i64 res = x87_wt_com_st(s, i, quiet, 0);
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

/* A load helper that pushes (fld*_ST0): the static TOP follows (x87_top). */
#if defined(XBOX)
#define GEN_HELPER_FALLBACK_PUSH_v_T(func, arg) do { \
        if (!g_use_hard_fpu_inline) { \
            gen_helper_ ## func(tcg_env, arg); \
            x87_top_move(s, -1); \
            return; \
        }} while(0)
#else
#define GEN_HELPER_FALLBACK_PUSH_v_T GEN_HELPER_FALLBACK_v_T
#endif

static void gen_fpush(DisasContext *s)
{
#if !defined(XBOX)
    GEN_HELPER_FALLBACK_v_v(fpush);
#endif

#if defined(XBOX)
    if (x87_defer(s)) {
        x87_wt_move(s, -1);
        x87_wt_set_tag(s, 0, 0); /* validate stack entry */
        return;
    }
#endif
    tcg_gen_subi_i32(fpstt, fpstt, 1);
    tcg_gen_andi_i32(fpstt, fpstt, 7);
    gen_set_fptag_stn(0, 0); /* validate stack entry */

    s->fpstt_delta -= 1;
#if defined(XBOX)
    x87_top_move(s, -1);
#endif
}

static void gen_fpop(DisasContext *s)
{
#if !defined(XBOX)
    GEN_HELPER_FALLBACK_v_v(fpop);
#endif

#if defined(XBOX)
    if (x87_defer(s)) {
        x87_wt_set_tag(s, 0, 1); /* invalidate stack entry */
        fp_pc_wrapper(gen_fpop)(s);
        x87_wt_move(s, 1);
        return;
    }
#endif
    gen_set_fptag_stn(0, 1); /* invalidate stack entry */
    tcg_gen_addi_i32(fpstt, fpstt, 1);
    tcg_gen_andi_i32(fpstt, fpstt, 7);

    fp_pc_wrapper(gen_fpop)(s);
    s->fpstt_delta += 1;
#if defined(XBOX)
    x87_top_move(s, 1);
#endif
}

/* FT0 = ST(i) for a helper (fucom*, fcomi*), which reads env->ft0. */
static void gen_fmov_FT0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    {
        int src = (s->fpstt_delta + st_index) & 7;
        if (x87_wt(s)) {
            if (!x87_wt_pending(s, src) &&
                x87_maybe_stale(s, x87_phys(s, st_index))) {
                /* Its image may be stale: the helper converts it first. */
                gen_helper_fmov_FT0_STN(tcg_env, tcg_constant_i32(st_index));
                return;
            }
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
        x87_wt_copy_dbl(s, d, src_i, false);
        s->wt_st[dst] = s->wt_st[src];
        s->wt_pend &= ~(1 << dst);
        x87_wt_set_exact(s, dst, s->wt_exact & (1 << src));
    }
}
#endif

static void gen_fmov_ST0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    x87_lean_move(s, 0, st_index);
    return;
#endif
    GEN_HELPER_FALLBACK_v_i(fmov_ST0_STN, st_index);
    fp_pc_wrapper(gen_fmov_ST0_STN)(s, st_index);
}

static void gen_fmov_STN_ST0(DisasContext *s, int st_index)
{
#if defined(XBOX)
    x87_lean_move(s, st_index, 0);
    return;
#endif
    GEN_HELPER_FALLBACK_v_i(fmov_STN_ST0, st_index);
    fp_pc_wrapper(gen_fmov_STN_ST0)(s, st_index);
}

static void gen_ffree_STN(DisasContext *s, int st_index)
{
#if !defined(XBOX)
    GEN_HELPER_FALLBACK_v_i(ffree_STN, st_index);
#endif

    /*
     * Inline: a single byte write into the tag word; no register spill,
     * no env fpus change (ffree only marks the tag slot empty).
     */
#if defined(XBOX)
    if (x87_defer(s)) {
        x87_wt_set_tag(s, st_index, 1);
        return;
    }
#endif
    gen_set_fptag_stn(st_index, 1);
}

static void gen_fxchg_ST0_STN(DisasContext *s, int st_index)
{
#if defined(XBOX)
    {
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
                x87_wt_copy_dbl(s, 0, st_index, false);
                s->wt_pend &= ~(1 << a);
                s->wt_st[a] = vb;
                x87_wt_set_exact(s, a, eb);
            }
            if (!pa) {
                x87_gen_copy(gen_stn_ptr(st_index), gen_stn_ptr(0));
                x87_wt_copy_dbl(s, st_index, 0, false);
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
        TCGv_i64 lo0 = tcg_temp_new_i64(), lon = tcg_temp_new_i64();
        TCGv_i32 hi0 = tcg_temp_new_i32(), hin = tcg_temp_new_i32();
        /* 10 bytes each, as x87_gen_copy */
        tcg_gen_ld_i64(lo0, p0.base, p0.ofs);
        tcg_gen_ld16u_i32(hi0, p0.base, p0.ofs + 8);
        tcg_gen_ld_i64(lon, pn.base, pn.ofs);
        tcg_gen_ld16u_i32(hin, pn.base, pn.ofs + 8);
        tcg_gen_st_i64(lo0, pn.base, pn.ofs);
        tcg_gen_st16_i32(hi0, pn.base, pn.ofs + 8);
        tcg_gen_st_i64(lon, p0.base, p0.ofs);
        tcg_gen_st16_i32(hin, p0.base, p0.ofs + 8);
        x87_wt_copy_dbl(s, 0, st_index, true);
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

#if defined(XBOX)
/*
 * Refuter, at every MMX entry (XEMU_X87_REFUTE): MMX code moves the x87
 * images raw, so no register may be stale (x87_wt_dbl) once it runs.
 */
static void gen_mmx_stale_check(void)
{
    TCGv_i64 t;

    if (!g_x87_dbl || likely(!xemu_x87_refute_on())) {
        return;
    }
    t = tcg_temp_ebb_new_i64();
    tcg_gen_ld_i64(t, tcg_env, offsetof(CPUX86State, xemu_x87_dstale));
    tcg_gen_setcondi_i64(TCG_COND_NE, t, t, 0);
    xemu_x87_gen_acc_i64(&xemu_x87_mmx_viol, t);
    xemu_gen_counter_inc(&xemu_x87_mmx_checks);
    tcg_temp_free_i64(t);
}
#endif

static void gen_enter_mmx(DisasContext *s)
{
#if defined(XBOX)
    /*
     * helper_enter_mmx inline, once per TB: an MMX insn sets fpstt to 0
     * and every tag to valid, so the next one changes nothing until an insn
     * that moves fpstt or the tags runs (x87 escapes, EMMS and FEMMS,
     * FXRSTOR, XRSTOR), which clears mmx_entered; until then x87_top stays
     * 0 too. The flush lands the write-through x87 results and FIP first,
     * as the helper call's gen_bb_epilogue did; nothing becomes pending
     * again before such an insn, so no double is stored before the next
     * entry either. Deferring blocks keep fpstt in env and leave the
     * fpstt global unused (x87_wt_land), so they store env->fpstt; the
     * flush started a new interval, whose wt_top reads it back.
     */
    if (s->mmx_entered) {
        gen_mmx_stale_check();
        return;
    }
    s->mmx_entered = true;
    gen_flush_fp(s);
    if (g_x87_dbl && !s->mmx_synced) {
        /*
         * MMX code moves the images raw, so a register left as a double
         * (x87_wt_dbl) is converted first. Decided after the flush, which
         * may itself store one (x87_wt_store clears mmx_synced). At
         * runtime one is stale only if this TB or an earlier one stored a
         * double since the last conversion: one load of the eight stale
         * bytes skips the call otherwise. Both ways no register is stale
         * past the label, which mmx_synced records for the TB.
         */
        TCGLabel *synced = gen_new_label();
        TCGv_i64 t = tcg_temp_new_i64();

        tcg_gen_ld_i64(t, tcg_env, offsetof(CPUX86State, xemu_x87_dstale));
        tcg_gen_brcondi_i64(TCG_COND_EQ, t, 0, synced);
        gen_helper_enter_mmx_sync(tcg_env);
        gen_set_label(synced);
        s->mmx_synced = true;
    }
    gen_mmx_stale_check();
    if (x87_defer(s)) {
        tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                       offsetof(CPUX86State, fpstt));
    } else {
        tcg_gen_movi_i32(fpstt, 0);
    }
    if (QEMU_IS_ALIGNED(offsetof(CPUX86State, fptags), 8)) {
        tcg_gen_st_i64(tcg_constant_i64(0), tcg_env,
                       offsetof(CPUX86State, fptags));
    } else {
        tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                       offsetof(CPUX86State, fptags));
        tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                       offsetof(CPUX86State, fptags) + 4);
    }
    /* After the flush, which lands the old TOP: fpstt = 0. */
    x87_top_set(s, 0);
#else
    GEN_HELPER_FALLBACK_v_v(enter_mmx);

    gen_flush_fp((DisasContext *)tcg_ctx->disas_ctx);

    tcg_gen_movi_i32(fpstt, 0);

    TCGv_i32 v = tcg_constant_i32(0);
    for (int i = 0; i < 8; i++) {
        tcg_gen_st8_i32(v, tcg_env, offsetof(CPUX86State, fptags[0]) + i);
    }
#endif
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

/*
 * The m32fp operand at A0 as a double, as x87_wt_f32_to_f64. The guest
 * load goes straight into an FP register (qemu_ld_fp: LDR St), not
 * through a GPR and an FMOV.
 */
static TCGv_f64 x87_wt_ld_f32(DisasContext *s)
{
    TCGv_f32 t = tcg_temp_new_f32();
    TCGv_f64 d = tcg_temp_new_f64();

    tcg_gen_qemu_ld_f32(t, s->A0, s->mem_index, MO_LEUL);
    tcg_gen_cvt32f_f64(d, t);
    return d;
}

/*
 * The m64fp operand at A0 as x87_wt_canon gives it: loaded straight into
 * an FP register (LDR Dt), a signalling NaN quieted there (quiet_f64:
 * FMAX Dd, Dn, Dn), instead of LDR X, the integer select and an FMOV.
 */
static TCGv_f64 x87_wt_ld_f64(DisasContext *s)
{
    TCGv_f64 t = tcg_temp_new_f64();
    TCGv_f64 d = tcg_temp_new_f64();

    tcg_gen_qemu_ld_f64(t, s->A0, s->mem_index, MO_LEUQ);
    tcg_gen_quiet_f64(d, t);
    return d;
}

/* fst/fstp m32fp: gen_fsts_ST0's float, stored from its FP register. */
static void x87_wt_st_f32(DisasContext *s)
{
    TCGv_f32 t = tcg_temp_new_f32();

    tcg_gen_cvt64f_f32(t, x87_wt_get_st(s, 0));
    tcg_gen_qemu_st_f32(t, s->A0, s->mem_index, MO_LEUL);
}
#endif

#if defined(XBOX)
/* g_x87_hw: push ST0 = the m32fp/m64fp/m32int/m64int in v32 or v64 */
static void gen_x87_hw_push_ld(DisasContext *s, int type, TCGv_i32 v32,
                               TCGv_i64 v64)
{
    X87Ptr p;

    gen_fpush(s);
    p = gen_stn_ptr(0);
    if (v64) {
        tcg_gen_x87_ld_i64(type, p.base, p.ofs, v64);
    } else {
        tcg_gen_x87_ld_i32(type, p.base, p.ofs, v32);
    }
}

/* g_x87_hw: r32 or r64 = ST0 as a TCG_X87_M* type (fst, fist) */
static void gen_x87_hw_st(int type, TCGv_i32 r32, TCGv_i64 r64)
{
    X87Ptr p = gen_stn_ptr(0);

    if (r64) {
        tcg_gen_x87_st_i64(r64, type, p.base, p.ofs);
    } else {
        tcg_gen_x87_st_i32(r32, type, p.base, p.ofs);
    }
}

/*
 * g_x87_hw with a static TOP, ST(i) a fixed env offset: the x87 op on the
 * m32fp/m64fp/m32int/m64int at A0 fused with its guest access (x87_*_g),
 * the host x87 insn on the guest page. False where it can't be: then the
 * access and the (%rsp) form.
 */
static bool gen_x87_hw_g(DisasContext *s, TCGOpcode opc, TCGv_i32 ret,
                         int kind, int i)
{
    X87Ptr p;

    if (!g_x87_hw || s->x87_top < 0) {
        return false;
    }
    p = gen_stn_ptr(i);
    tcg_debug_assert(p.base == tcg_env);
    return tcg_gen_qemu_x87(opc, ret, kind, p.ofs, s->A0, s->mem_index);
}

/*
 * Push ST0 = the operand at A0. The access writes the image the push
 * makes ST0, ST(7), before the push: none may come first (x87_wt_spill).
 */
static bool gen_x87_hw_push_ld_g(DisasContext *s, int type)
{
    if (!gen_x87_hw_g(s, INDEX_op_x87_ld_g, NULL, type, 7)) {
        return false;
    }
    gen_fpush(s);
    return true;
}

/* ST0 op= the operand at A0, as gen_x87_hw_fop */
static bool gen_x87_hw_fop_g(DisasContext *s, int op, int type)
{
    TCGv_i32 cc;

    if (op != 2 && op != 3) {
        return gen_x87_hw_g(s, INDEX_op_x87_arith_g, NULL, op | type << 3, 0);
    }
    cc = tcg_temp_new_i32();
    if (!gen_x87_hw_g(s, INDEX_op_x87_cmp_g, cc, type, 0)) {
        return false;
    }
    gen_x87_hw_fpus_cc(cc);
    return true;
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
    if (g_x87_hw) {
        gen_x87_hw_push_ld(s, TCG_X87_M32F, arg, NULL);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_PUSH_v_T(flds_ST0, arg);
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
    if (g_x87_hw) {
        gen_x87_hw_push_ld(s, TCG_X87_M64F, NULL, arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_PUSH_v_T(fldl_ST0, arg);
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
    if (g_x87_hw) {
        gen_x87_hw_push_ld(s, TCG_X87_M32I, arg, NULL);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_PUSH_v_T(fildl_ST0, arg);
    gen_fpush(s);
    fp_pc_wrapper(gen_fildl_ST0)(s, arg);
}

static void gen_fildll_ST0(DisasContext *s, TCGv_i64 arg)
{
#if defined(XBOX)
    if (g_x87_hw) {
        gen_x87_hw_push_ld(s, TCG_X87_M64I, NULL, arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_PUSH_v_T(fildll_ST0, arg);
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
    if (g_x87_hw) {
        gen_x87_hw_st(TCG_X87_M32F, arg, NULL);
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
    if (g_x87_hw) {
        gen_x87_hw_st(TCG_X87_M64F, NULL, arg);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_T_v(fstl_ST0, arg);
    fp_pc_wrapper(gen_fstl_ST0)(s, arg);
}

#if defined(XBOX)
/*
 * ST0 as an integer, as the __hard fist helpers compute it:
 * llrint(floatx80_to_double_fast(ST0)), which is FRINTX under the live
 * FPCR (the x87 RC) and a saturating FCVTZS to 64 bits: NaN gives 0.
 * fistl keeps the low 32 bits of that, not the integer indefinite. No
 * flags: merge_exception_flags is empty under USE_HARD_FPU, and the
 * invalid flag helper_fist_ST0 leaves in fp_status is never read (every
 * reader clears the flags first).
 */
static TCGv_i64 x87_wt_fist(DisasContext *s)
{
    TCGv_f64 r = tcg_temp_new_f64();
    TCGv_i64 v = tcg_temp_new_i64();

    tcg_gen_rint_f64(r, x87_wt_get_st(s, 0));
    tcg_gen_cvt64f_i64(v, r);
    return v;
}
#endif

/* fist/fistp m16: helper_fist_ST0 gives -32768 outside int16. */
static void gen_fist_ST0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        TCGv_i32 t = tcg_temp_new_i32();

        tcg_gen_extrl_i64_i32(arg, x87_wt_fist(s));
        tcg_gen_ext16s_i32(t, arg);
        tcg_gen_movcond_i32(TCG_COND_NE, arg, t, arg,
                            tcg_constant_i32(-32768), arg);
        return;
    }
    if (g_x87_hw) {
        /* the __hard fist helpers are fistp, integer indefinite and all */
        gen_x87_hw_st(TCG_X87_M16I, arg, NULL);
        return;
    }
#endif
    gen_helper_fist_ST0(arg, tcg_env);
}

static void gen_fistl_ST0(DisasContext *s, TCGv_i32 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        tcg_gen_extrl_i64_i32(arg, x87_wt_fist(s));
        return;
    }
    if (g_x87_hw) {
        gen_x87_hw_st(TCG_X87_M32I, arg, NULL);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_T_v(fistl_ST0, arg);
    fp_pc_wrapper(gen_fistl_ST0)(s, arg);
}

static void gen_fistll_ST0(DisasContext *s, TCGv_i64 arg)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        tcg_gen_mov_i64(arg, x87_wt_fist(s));
        return;
    }
    if (g_x87_hw) {
        gen_x87_hw_st(TCG_X87_M64I, NULL, arg);
        return;
    }
#endif
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

/*
 * After the sign op on ST0's image, the same on its double, which is the
 * value while the image is stale (x87_wt_dbl): st80f copies the sign
 * through on every path.
 */
static void x87_wt_sign_dbl(DisasContext *s, bool chs)
{
    int ph;
    TCGv_i64 t;

    if (!x87_wt(s) || !x87_maybe_stale(s, x87_phys(s, 0))) {
        return;
    }
    ph = x87_phys(s, 0);
    t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, tcg_env, X87_DBL_OFS(ph));
    if (chs) {
        tcg_gen_xori_i64(t, t, INT64_MIN);
    } else {
        tcg_gen_andi_i64(t, t, INT64_MAX);
    }
    tcg_gen_st_i64(t, tcg_env, X87_DBL_OFS(ph));
}
#endif

static void gen_fchs_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_wt_sign_pending(s, true)) {
        return;
    }
    x87_gen_sign(gen_stn_ptr(0), true);
    x87_wt_sign_dbl(s, true);
    /*
     * Reload rather than negate the cached double: the conversion does
     * not commute with the sign for an 80-bit value beyond double range
     * under RP/RM (inf on one side, DBL_MAX on the other), nor for the
     * fixed-sign default NaN of an unnormal or pseudo-NaN.
     */
    s->wt_st[s->fpstt_delta & 7] = NULL;
    return;
#endif
    GEN_HELPER_FALLBACK_v_v(fchs_ST0);
    fp_pc_wrapper(gen_fchs_ST0)(s);
}

static void gen_fabs_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_wt_sign_pending(s, false)) {
        return;
    }
    x87_gen_sign(gen_stn_ptr(0), false);
    x87_wt_sign_dbl(s, false);
    s->wt_st[s->fpstt_delta & 7] = NULL;   /* as in gen_fchs_ST0 */
    return;
#endif
    GEN_HELPER_FALLBACK_v_v(fabs_ST0);
    fp_pc_wrapper(gen_fabs_ST0)(s);
}

#if defined(XBOX)
/*
 * floatx80_is_neg(ST(i)) as a helper would see it, 0 or 1. An exact slot's
 * image is st80f of its double, which copies the sign through. Otherwise
 * env holds the register: its double while stale (x87_wt_dbl), else its
 * image, whose sign the double may not have (ld80f gives the fixed-sign
 * default NaN for an unnormal or pseudo-NaN).
 */
static TCGv_i32 x87_wt_sign(DisasContext *s, int i)
{
    TCGv_f64 d = x87_wt_get_st(s, i);       /* first: it may clear exact */
    bool exact = x87_wt_exact(s, i);
    bool stale = !exact && x87_maybe_stale(s, x87_phys(s, i));
    TCGv_i32 neg, img = NULL;
    TCGv_i64 b;

    if (!exact) {
        X87Ptr p = gen_stn_ptr(i);

        img = tcg_temp_new_i32();
        tcg_gen_ld16u_i32(img, p.base, p.ofs + 8);
        tcg_gen_extract_i32(img, img, 15, 1);
        if (!stale) {
            return img;
        }
    }
    neg = tcg_temp_new_i32();
    b = tcg_temp_new_i64();
    tcg_gen_mov64f_i64(b, d);
    tcg_gen_extract_i64(b, b, 63, 1);
    tcg_gen_extrl_i64_i32(neg, b);
    if (stale) {
        TCGv_i32 st = tcg_temp_new_i32();

        tcg_gen_ld8u_i32(st, tcg_env, X87_STALE_OFS(x87_phys(s, i)));
        tcg_gen_movcond_i32(TCG_COND_NE, neg, st, tcg_constant_i32(0),
                            neg, img);
    }
    return neg;
}
#endif

/*
 * helper_fsqrt: a negative ST0 (-0 included) sets C2 and clears C3/C1/C0
 * first; the result is double_to_floatx80_fast(sqrt(the double)), which
 * FSQRT gives under the live FPCR (__hard ignores precision control).
 */
static void gen_fsqrt(DisasContext *s)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        TCGv_i32 neg = x87_wt_sign(s, 0);
        TCGv_i32 fpus = tcg_temp_new_i32();
        TCGv_i32 t = tcg_temp_new_i32();
        TCGv_f64 r = tcg_temp_new_f64();

        tcg_gen_ld16u_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
        tcg_gen_andi_i32(t, fpus, ~0x4700);
        tcg_gen_ori_i32(t, t, 0x400);
        tcg_gen_movcond_i32(TCG_COND_NE, fpus, neg, tcg_constant_i32(0),
                            t, fpus);
        tcg_gen_st16_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
        tcg_gen_sqrt_f64(r, x87_wt_get_st(s, 0));
        x87_wt_set_st(s, 0, r);
        return;
    }
    if (g_x87_hw) {
        X87Ptr p = gen_stn_ptr(0);
        TCGv_i32 neg = tcg_temp_new_i32();
        TCGv_i32 fpus = tcg_temp_new_i32();
        TCGv_i32 t = tcg_temp_new_i32();

        tcg_gen_ld16u_i32(neg, p.base, p.ofs + 8);
        tcg_gen_andi_i32(neg, neg, 0x8000);
        tcg_gen_ld16u_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
        tcg_gen_andi_i32(t, fpus, ~0x4700);
        tcg_gen_ori_i32(t, t, 0x400);
        tcg_gen_movcond_i32(TCG_COND_NE, fpus, neg, tcg_constant_i32(0),
                            t, fpus);
        tcg_gen_st16_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
        tcg_gen_x87_unary(TCG_X87_FSQRT, p.base, p.ofs);
        return;
    }
#endif
    GEN_HELPER_FALLBACK_v_v(fsqrt);
    fp_pc_wrapper(gen_fsqrt)(s);
}

/* helper_frndint: double_to_floatx80_fast(rint(the double)). */
static void gen_frndint(DisasContext *s)
{
#if defined(XBOX)
    if (x87_wt(s)) {
        TCGv_f64 r = tcg_temp_new_f64();

        tcg_gen_rint_f64(r, x87_wt_get_st(s, 0));
        x87_wt_set_st(s, 0, r);
        return;
    }
    if (g_x87_hw) {
        X87Ptr p = gen_stn_ptr(0);

        tcg_gen_x87_unary(TCG_X87_FRNDINT, p.base, p.ofs);
        return;
    }
#endif
    gen_helper_frndint(tcg_env);
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

/* fcom/fucom ST0, ST(i): fpus C3/C2/C0 (the helpers' compares agree) */
static void gen_fcom_ST0_STN(DisasContext *s, int i, bool quiet)
{
#if defined(XBOX)
    if (g_x87_hw) {
        gen_x87_hw_fpus_cc(gen_x87_hw_cmp(i));
        return;
    }
#endif
    if (!quiet) {
        gen_fload_FT0_STN(s, i);
        gen_fcom_ST0_FT0(s);
    } else if (!gen_fucom_wt(s, i)) {
        gen_fmov_FT0_STN(s, i);
        gen_helper_fucom_ST0_FT0(tcg_env);
    }
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
    CCPrepare cc;
    TCGv c2;
    TCGLabel *l1;

    if (!x87_wt(s)) {
        return false;
    }
    /*
     * First, so that a flushing call in it cannot outdate the check. The
     * condition is used as prepared (after fucomi: TSTNE on CC_SRC),
     * without a setcond; no_setcond values are 0 or 1, so comparing them
     * with 0 under their condition is what gen_setcc computes.
     */
    cc = gen_prepare_cc(s, b, NULL);
    c2 = cc.use_reg2 ? cc.reg2 : tcg_constant_tl(cc.imm);
    if (x87_wt_exact(s, 0) && x87_wt_exact(s, opreg)) {
        TCGv_f64 r = tcg_temp_new_f64();

        if (unlikely(xemu_x87_census_on())) {
            xemu_gen_counter_inc(&xemu_x87_wt_fcmov_inline);
        }
        tcg_gen_movcond_tl_f64(cc.cond, r, cc.reg, c2,
                               x87_wt_get_st(s, 0), x87_wt_get_st(s, opreg));
        x87_wt_set_st(s, 0, r);
        return true;
    }
    if (unlikely(xemu_x87_census_on())) {
        xemu_gen_counter_inc(&xemu_x87_wt_fcmov_branch);
    }
    l1 = gen_new_label();
    if (cc.cond == TCG_COND_ALWAYS) {
        tcg_gen_br(l1);
    } else if (cc.cond != TCG_COND_NEVER) {
        tcg_gen_brcond_tl(cc.cond, cc.reg, c2, l1);
    }
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

#if defined(XBOX)
/*
 * fcmov on x86_64 hosts (g_x87_hw): ST0 = jcc(b) ? ST0 : ST(i) as a select
 * of the 10 image bytes, not a branch around the move: a branch ends the
 * TCG basic block, so every live global was synced around each fcmov of
 * the fld/fld/fucomi/fcmov min/max idiom. False off that path.
 */
static bool gen_fcmov_hw(DisasContext *s, int b, int opreg)
{
    CCPrepare cc;
    TCGv t;
    TCGv_i64 t64, lo0, loi;
    TCGv_i32 t32, hi0, hii;
    X87Ptr p0, pi;

    if (!g_x87_hw) {
        return false;
    }
    cc = gen_prepare_cc(s, b, NULL);
    t = tcg_temp_new();
    tcg_gen_setcond_tl(cc.cond, t, cc.reg,
                       cc.use_reg2 ? cc.reg2 : tcg_constant_tl(cc.imm));
    t64 = tcg_temp_new_i64();
    t32 = tcg_temp_new_i32();
    tcg_gen_extu_tl_i64(t64, t);
    tcg_gen_trunc_tl_i32(t32, t);
    p0 = gen_stn_ptr(0);
    pi = gen_stn_ptr(opreg);
    lo0 = tcg_temp_new_i64();
    loi = tcg_temp_new_i64();
    hi0 = tcg_temp_new_i32();
    hii = tcg_temp_new_i32();
    tcg_gen_ld_i64(lo0, p0.base, p0.ofs);
    tcg_gen_ld16u_i32(hi0, p0.base, p0.ofs + 8);
    tcg_gen_ld_i64(loi, pi.base, pi.ofs);
    tcg_gen_ld16u_i32(hii, pi.base, pi.ofs + 8);
    tcg_gen_movcond_i64(TCG_COND_NE, lo0, t64, tcg_constant_i64(0), lo0, loi);
    tcg_gen_movcond_i32(TCG_COND_NE, hi0, t32, tcg_constant_i32(0), hi0, hii);
    tcg_gen_st_i64(lo0, p0.base, p0.ofs);
    tcg_gen_st16_i32(hi0, p0.base, p0.ofs + 8);
    return true;
}
#else
static bool gen_fcmov_hw(DisasContext *s, int b, int opreg)
{
    return false;
}
#endif

/* fcomi/fucomi ST0, ST(i): EFLAGS ZF/PF/CF, CC_OP_EFLAGS. */
static void gen_fcomi_ST0_STN(DisasContext *s, int opreg, bool quiet)
{
#if defined(XBOX)
    if (g_x87_hw) {
        /* C3, C2, C0 at 14, 10, 8 become ZF, PF, CF at 6, 2, 0 */
        TCGv t = tcg_temp_new();

        gen_compute_eflags(s);
        tcg_gen_extu_i32_tl(t, gen_x87_hw_cmp(opreg));
        tcg_gen_shri_tl(t, t, 8);
        tcg_gen_andi_tl(cpu_cc_src, cpu_cc_src, ~(CC_Z | CC_P | CC_C));
        tcg_gen_or_tl(cpu_cc_src, cpu_cc_src, t);
        return;
    }
#endif
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

#if defined(XBOX)
static void gen_fnstsw_inline(DisasContext *s, TCGv_i32 result)
{
    TCGv_i32 fpus = tcg_temp_new_i32();
    tcg_gen_ld16u_i32(fpus, tcg_env, offsetof(CPUX86State, fpus));
    tcg_gen_andi_i32(fpus, fpus, ~0x3800);
    TCGv_i32 top = tcg_temp_new_i32();
    if (x87_defer(s)) {
        /* fpstt is deferred (x87_wt_land) */
        tcg_gen_shli_i32(top, gen_st_index(0), 11);
    } else {
        tcg_gen_andi_i32(top, fpstt, 0x7);
        tcg_gen_shli_i32(top, top, 11);
    }
    tcg_gen_or_i32(result, fpus, top);
}
#endif

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
    if (x87_wt_dbl(s)) {
        /* A pending double, as any result (x87_wt_dbl). */
        x87_wt_set_st(s, 0, x87_wt_from_bits(
                          tcg_constant_i64(0x3FF0000000000000ull)));
        return;
    }
    x87_gen_const(gen_stn_ptr(0), 0x3fff, 0x8000000000000000ULL);
    if (x87_wt(s)) {
        s->wt_st[s->fpstt_delta & 7] =
            x87_wt_from_bits(tcg_constant_i64(0x3FF0000000000000ull));
        s->wt_pend &= ~(1 << (s->fpstt_delta & 7));
        s->wt_exact |= 1 << (s->fpstt_delta & 7);
    }
    return;
#endif
    GEN_HELPER_FALLBACK_v_v(fld1_ST0);
    fp_pc_wrapper(gen_fld1_ST0)(s);
}

static void gen_fldz_ST0(DisasContext *s)
{
#if defined(XBOX)
    if (x87_wt_dbl(s)) {
        x87_wt_set_st(s, 0, x87_wt_from_bits(tcg_constant_i64(0)));
        return;
    }
    x87_gen_const(gen_stn_ptr(0), 0, 0);
    if (x87_wt(s)) {
        s->wt_st[s->fpstt_delta & 7] =
            x87_wt_from_bits(tcg_constant_i64(0));
        s->wt_pend &= ~(1 << (s->fpstt_delta & 7));
        s->wt_exact |= 1 << (s->fpstt_delta & 7);
    }
    return;
#endif
    GEN_HELPER_FALLBACK_v_v(fldz_ST0);
    fp_pc_wrapper(gen_fldz_ST0)(s);
    /* FIXME: Set tag word */
}

static void gen_fldz_FT0(DisasContext *s)
{
#if defined(XBOX)
    x87_gen_const(gen_ft0_ptr(), 0, 0);
    if (x87_wt(s)) {
        s->wt_ft0 = x87_wt_from_bits(tcg_constant_i64(0));
    }
    return;
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
 * of the helper fallback, only in TBs with the base cflags
 * (cpu->tcg_cflags). Validation against the ret site's translate-time
 * context constants is exact: nothing between TB entry and its ret exit
 * changes cs_base/flags/cflags, so the runtime context at this exit
 * equals this TB's own translation context. A hit jumps to the entry's
 * tc_ptr without reading the TB: the fill stores it only for a TB with
 * the base cflags, and every invalidation clears the memo (see
 * XemuRetcEntry), so CF_INVALID needs no check here; CONFIG_DEBUG_TCG
 * builds still check the TB's cflags behind every hit. Empty entries
 * hold tc_ptr == NULL. Misses fall through to the helper, which also
 * owns every special case (breakpoints, single-step, logging, epilogue
 * return).
 */
static void xemu_gen_inline_ret_memo(DisasContext *s)
{
    TCGLabel *slow = gen_new_label();
    TCGv_i32 eip = tcg_temp_new_i32();
    TCGv_i32 idx = tcg_temp_new_i32();
    TCGv_i64 t64 = tcg_temp_new_i64();
    TCGv_ptr ep = tcg_temp_new_ptr();
    TCGv_ptr tcp = tcg_temp_new_ptr();
    const intptr_t retc = offsetof(CPUX86State, xemu_retc);

    QEMU_BUILD_BUG_ON(sizeof(struct XemuRetcEntry) != 32);
    tcg_gen_mov_i32(eip, cpu_eip);
    /* xemu_retc_index(eip) * 32, the entry stride */
    tcg_gen_shri_i32(idx, eip, XEMU_RETC_HASH_SHIFT);
    tcg_gen_xor_i32(idx, idx, eip);
    tcg_gen_deposit_z_i32(idx, idx, 1, XEMU_RETC_BITS);
    tcg_gen_add_zext_shl_ptr(ep, tcg_env, idx, 4);

#if HOST_BIG_ENDIAN
    {
        TCGv_i32 t32 = tcg_temp_new_i32();

        tcg_gen_ld_i32(t32, ep, retc + offsetof(struct XemuRetcEntry, eip));
        tcg_gen_brcond_i32(TCG_COND_NE, t32, eip, slow);
        tcg_gen_ld_i32(t32, ep, retc + offsetof(struct XemuRetcEntry, flags));
        tcg_gen_brcondi_i32(TCG_COND_NE, t32, (int32_t)s->base.tb->flags,
                            slow);
    }
#else
    {
        /* eip and flags as one little-endian word: flags << 32 | eip */
        TCGv_i64 key = tcg_temp_new_i64();

        QEMU_BUILD_BUG_ON(offsetof(struct XemuRetcEntry, flags) !=
                          offsetof(struct XemuRetcEntry, eip) + 4);
        tcg_gen_add_zext_i64(key, tcg_constant_i64(
                                 (uint64_t)s->base.tb->flags << 32), eip);
        tcg_gen_ld_i64(t64, ep, retc + offsetof(struct XemuRetcEntry, eip));
        tcg_gen_brcond_i64(TCG_COND_NE, t64, key, slow);
    }
#endif
    tcg_gen_ld_i64(t64, ep, retc + offsetof(struct XemuRetcEntry, cs_base));
    tcg_gen_brcondi_i64(TCG_COND_NE, t64, (int64_t)x87_key_cs_base(s), slow);
    tcg_gen_ld_ptr(tcp, ep, retc + offsetof(struct XemuRetcEntry, tc_ptr));
    tcg_gen_brcondi_ptr(TCG_COND_EQ, tcp, 0, slow);
#ifdef CONFIG_DEBUG_TCG
    tcg_gen_addi_ptr(ep, ep, retc);
    gen_helper_xemu_retc_check(tcg_env, ep, tcp);
#endif
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
              (CF_NO_GOTO_TB | CF_NO_GOTO_PTR | CF_NOIRQ | CF_COUNT_MASK)) &&
            x87_top_exit_ok(s);
        if (s->xemu_ret_exit) {
            s->xemu_ret_exit = false;
            /* The memo holds only TBs with the base cflags (XemuRetcEntry). */
            if (xemu_probe_ok && tb_cflags(s->base.tb) ==
                                 env_cpu(s->xemu_env)->tcg_cflags) {
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
                    x87_key_cs_base(s), flags, tb_cflags(s->base.tb));
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

#if defined(XBOX)
    /* The chained TB's key takes this exit's TOP (x87_top). */
    if (!x87_top_exit_ok(s)) {
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

#if defined(XBOX)
/*
 * fldcw in a write-through block, after the load of the new word. One that
 * keeps the bits in the TB flags, RC (HF_FPU_RC) and precision control's
 * bit 9 (HF_FPU_PC), leaves cpu_set_fpuc nothing to do but store fpuc and
 * the softfloat precision, double or extended (bit 9 is set here): the
 * hflags, the softfloat rounding mode and the host FPCR stay. So it skips
 * the helper and chains to the next insn (goto_tb) with the TB's own
 * flags. Azurik's CRT switches 0x23f to 0x133f or 0x27f and back around
 * its math calls, 98% of its fldcw. Any other word takes the helper and
 * the jump-cache probe, which reads the new RC/PC from env (gen_eob).
 * Only while jmp_opt holds, so neither exit resets RF or the IRQ inhibit.
 */
static void gen_fldcw_wt(DisasContext *s, TCGv_i32 val)
{
    TCGLabel *slow = gen_new_label();
    TCGv_i32 t = tcg_temp_new_i32();

    QEMU_BUILD_BUG_ON(floatx80_precision_x != 0 ||
                      floatx80_precision_d != 1);
    gen_update_cc_op(s);
    tcg_gen_ld16u_i32(t, tcg_env, offsetof(CPUX86State, fpuc));
    tcg_gen_xor_i32(t, t, val);
    tcg_gen_andi_i32(t, t, 0xe00);      /* RC, PC bit 9 */
    tcg_gen_brcondi_i32(TCG_COND_NE, t, 0, slow);

    tcg_gen_st16_i32(val, tcg_env, offsetof(CPUX86State, fpuc));
    tcg_gen_extract_i32(t, val, 8, 1);  /* PC 3: extended, 2: double */
    tcg_gen_xori_i32(t, t, 1);
    tcg_gen_st8_i32(t, tcg_env,
                    offsetof(CPUX86State,
                             fp_status.floatx80_rounding_precision));
    gen_jmp_rel_csize(s, 0, 0);

    gen_set_label(slow);
    gen_helper_fldcw(tcg_env, val);
    gen_update_eip_next(s);
    s->xemu_fldcw_exit = true;
    gen_eob(s, DISAS_JUMP);
}
#endif

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

    /* One 16-byte store where the backend can, so a reload forwards. */
    tcg_gen_qemu_ld_env_i128(offset, s->A0, s->mem_index, mop);
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
 * The Xbox kernel's idle thread never executes HLT. KiIdleLoop spins on
 * "sti; nop; nop; cli", checks the DPC list and the next-thread pointer,
 * and goes round again; every STI ends a TB, so idle time goes round the
 * TCG dispatch loop as fast as the host can. Both exit conditions only
 * change in interrupt handlers, so while they are false and nothing is
 * pending, halting until the next interrupt (helper_xemu_ki_idle) is the
 * same as spinning until it.
 *
 * Deterministic mode (icount) halts at once: the spin made idle time, not
 * game work, bound fast-forward, and icount warps virtual time to the next
 * timer deadline. Normal mode (MTTCG) halts after a short spin, which gives
 * the host core back instead of burning it until the next interrupt;
 * XEMU_KI_IDLE_HALT=0 keeps the spin there.
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

static bool xemu_ki_idle_halt_normal(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("XEMU_KI_IDLE_HALT");
        on = !(e && !strcmp(e, "0"));
    }
    return on;
}

static int xemu_idle_loop_kind(DisasContext *s)
{
    static const uint8_t head[] = {
        0x90, 0x90, 0xfa, 0x3b, 0x6d, 0x00, 0x74, 0xff,
    };
    static const uint8_t tail[] = { 0x83, 0x7b, 0x2c, 0x00, 0x74, 0xff };
    vaddr sti = s->pc - 1;
    vaddr page = sti & TARGET_PAGE_MASK;

    if ((!icount_enabled() && !xemu_ki_idle_halt_normal()) ||
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
    TCGv a0_shared = s->A0;
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
    /* Any x87 insn may move fpstt or change the tags (gen_enter_mmx). */
    s->mmx_entered = false;
    if (s->xemu_x87_run < UINT8_MAX) {
        s->xemu_x87_run++;
    }
#endif
    mod = (modrm >> 6) & 3;
    rm = modrm & 7;
    op = ((b & 7) << 3) | ((modrm >> 3) & 7);
    if (mod != 3) {
        /* memory op */
        TCGv ea, last_addr;
        bool update_fdp = true;
#if defined(XBOX) && !defined(TARGET_X86_64)
        /*
         * With 32-bit addressing and no segment base to add, the linear
         * address is the offset FDP takes (gen_lea_v_seg only moves it).
         * Compute it into a temp of this insn's own, which no later insn
         * writes, and let that temp hold the FDP until the flush too: a
         * copy of the shared A0 cost a MOV per x87 memory operand with a
         * displacement or an index. So nothing below may write s->A0
         * after the lea (an access at an offset from it takes a temp of
         * its own), or the held FDP moves with it.
         */
        bool own_a0 = s->aflag == MO_32 && s->override < 0 && !ADDSEG(s);

        if (own_a0) {
            s->A0 = tcg_temp_new();
        }
#else
        const bool own_a0 = false;
#endif

        ea = gen_lea_modrm_1(s, decode->mem, false);
        if (own_a0) {
            gen_lea_v_seg(s, ea, decode->mem.def_seg, s->override);
            last_addr = s->A0;
        } else {
            last_addr = tcg_temp_new();
            tcg_gen_mov_tl(last_addr, ea);
            gen_lea_v_seg(s, ea, decode->mem.def_seg, s->override);
        }

        switch (op) {
        case 0x00 ... 0x07: /* fxxxs */
        case 0x10 ... 0x17: /* fixxxl */
        case 0x20 ... 0x27: /* fxxxl */
        case 0x30 ... 0x37: /* fixxx */
            {
                int op1;
                op1 = op & 7;

#if defined(XBOX)
                if (!x87_wt(s) && !g_use_hard_fpu_inline) {
                    /* The load into FT0 and the op in one helper call. */
                    TCGv_i32 opc = tcg_constant_i32(op1);

                    switch (op >> 4) {
                    case 0:
                        if (gen_x87_hw_fop_g(s, op1, TCG_X87_M32F)) {
                            break;
                        }
                        tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                            s->mem_index, MO_LEUL);
                        if (g_x87_hw) {
                            gen_x87_hw_fop(op1, TCG_X87_M32F, s->tmp2_i32,
                                           NULL);
                        } else {
                            gen_helper_fop_ST0_m32(tcg_env, s->tmp2_i32, opc);
                        }
                        break;
                    case 2:
                        if (gen_x87_hw_fop_g(s, op1, TCG_X87_M64F)) {
                            break;
                        }
                        tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                            s->mem_index, MO_LEUQ);
                        if (g_x87_hw) {
                            gen_x87_hw_fop(op1, TCG_X87_M64F, NULL,
                                           s->tmp1_i64);
                        } else {
                            gen_helper_fop_ST0_m64(tcg_env, s->tmp1_i64, opc);
                        }
                        break;
                    default:
                        if (op >> 4 == 1 &&
                            gen_x87_hw_fop_g(s, op1, TCG_X87_M32I)) {
                            break;
                        }
                        /* m32int, or m16int sign-extended to it */
                        tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                            s->mem_index,
                                            op >> 4 == 1 ? MO_LEUL : MO_LESW);
                        if (g_x87_hw) {
                            gen_x87_hw_fop(op1, TCG_X87_M32I, s->tmp2_i32,
                                           NULL);
                        } else {
                            gen_helper_fop_ST0_i32(tcg_env, s->tmp2_i32, opc);
                        }
                        break;
                    }
                    if (op1 == 3) {
                        /* fcomp needs pop */
                        gen_fpop(s);
                    }
                    break;
                }
#endif
                switch (op >> 4) {
                case 0:
#if defined(XBOX)
                    if (x87_wt(s)) {
                        s->wt_ft0 = x87_wt_ld_f32(s);
                        break;
                    }
#endif
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
#if defined(XBOX)
                    if (x87_wt(s) && g_x87_quiet) {
                        s->wt_ft0 = x87_wt_ld_f64(s);
                        break;
                    }
#endif
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
#if defined(XBOX)
                    if (x87_wt(s)) {
                        TCGv_f64 d = x87_wt_ld_f32(s);

                        gen_fpush(s);
                        x87_wt_set_st(s, 0, d);
                        break;
                    }
                    if (gen_x87_hw_push_ld_g(s, TCG_X87_M32F)) {
                        break;
                    }
#endif
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    gen_flds_ST0(s, s->tmp2_i32);
                    break;
                case 1:
#if defined(XBOX)
                    if (gen_x87_hw_push_ld_g(s, TCG_X87_M32I)) {
                        break;
                    }
#endif
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    gen_fildl_ST0(s, s->tmp2_i32);
                    break;
                case 2:
#if defined(XBOX)
                    if (x87_wt(s) && g_x87_quiet) {
                        TCGv_f64 d = x87_wt_ld_f64(s);

                        gen_fpush(s);
                        x87_wt_set_st(s, 0, d);
                        break;
                    }
                    if (gen_x87_hw_push_ld_g(s, TCG_X87_M64F)) {
                        break;
                    }
#endif
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
#if defined(XBOX)
                    if (x87_wt(s)) {
                        x87_wt_st_f32(s);
                        break;
                    }
                    if (gen_x87_hw_g(s, INDEX_op_x87_st_g, NULL,
                                     TCG_X87_M32F, 0)) {
                        break;
                    }
#endif
                    gen_fsts_ST0(s, s->tmp2_i32);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    break;
                case 1:
#if defined(XBOX)
                    if (gen_x87_hw_g(s, INDEX_op_x87_st_g, NULL,
                                     TCG_X87_M32I, 0)) {
                        break;
                    }
#endif
                    gen_fistl_ST0(s, s->tmp2_i32);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUL);
                    break;
                case 2:
#if defined(XBOX)
                    if (x87_wt(s)) {
                        /* The double's bits, from its FP register */
                        tcg_gen_qemu_st_f64(x87_wt_get_st(s, 0), s->A0,
                                            s->mem_index, MO_LEUQ);
                        break;
                    }
                    if (gen_x87_hw_g(s, INDEX_op_x87_st_g, NULL,
                                     TCG_X87_M64F, 0)) {
                        break;
                    }
#endif
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
#if defined(XBOX)
            x87_top_set(s, -1);
#endif
            update_fip = update_fdp = false;
            gen_update_eip_next(s);
            gen_eob(s, DISAS_EOB_ONLY);
            break;
        case 0x0d: /* fldcw mem */
            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                s->mem_index, MO_LEUW);
            update_fip = update_fdp = false;
#if defined(XBOX)
            if (x87_wt(s) && s->jmp_opt) {
                gen_fldcw_wt(s, s->tmp2_i32);
                break;
            }
#endif
            gen_helper_fldcw(tcg_env, s->tmp2_i32);
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
#if defined(XBOX)
            x87_top_move(s, -1);
#endif
            break;
        case 0x1f: /* fstpt mem */
            gen_helper_fstt_ST0(tcg_env, s->A0);
            gen_fpop(s);
            break;
        case 0x2c: /* frstor mem */
            gen_helper_frstor(tcg_env, s->A0,
                              tcg_constant_i32(s->dflag - 1));
#if defined(XBOX)
            x87_top_set(s, -1);
#endif
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
#if defined(XBOX)
            x87_top_set(s, 0); /* do_fninit */
#endif
            update_fip = update_fdp = false;
            break;
        case 0x2f: /* fnstsw mem */
#if defined(XBOX)
            gen_fnstsw_inline(s, s->tmp2_i32);
#else
            gen_helper_fnstsw(s->tmp2_i32, tcg_env);
#endif
            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                s->mem_index, MO_LEUW);
            update_fip = update_fdp = false;
            break;
        case 0x3c: /* fbld */
            gen_helper_fbld_ST0(tcg_env, s->A0);
#if defined(XBOX)
            x87_top_move(s, -1);
#endif
            break;
        case 0x3e: /* fbstp */
            gen_helper_fbst_ST0(tcg_env, s->A0);
            gen_fpop(s);
            break;
        case 0x3d: /* fildll */
#if defined(XBOX)
            if (gen_x87_hw_push_ld_g(s, TCG_X87_M64I)) {
                break;
            }
#endif
            tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                s->mem_index, MO_LEUQ);
            gen_fildll_ST0(s, s->tmp1_i64);
            break;
        case 0x3f: /* fistpll */
#if defined(XBOX)
            if (!gen_x87_hw_g(s, INDEX_op_x87_st_g, NULL, TCG_X87_M64I, 0))
#endif
            {
                gen_fistll_ST0(s, s->tmp1_i64);
                tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                    s->mem_index, MO_LEUQ);
            }
            gen_fpop(s);
            break;
        default:
            goto illegal_op;
        }

        if (update_fdp) {
            int last_seg = s->override >= 0 ? s->override : decode->mem.def_seg;
            bool emit_now = true;
            bool emit_fds = true;
            bool emit_fdp = true;

#if defined(XBOX)
            if (unlikely(xemu_x87_census_on())) {
                xemu_gen_counter_inc(&xemu_x87_fdp_legacy);
            }
            if (xemu_x87_fdp_eager()) {
                emit_fdp = !xemu_x87_defer_fdp(s, last_addr);
                emit_fds = emit_fdp ? xemu_x87_fds_needed(s, last_seg)
                                    : xemu_x87_defer_fds(s, last_seg);
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
                if (emit_fdp) {
                    tcg_gen_st_tl(last_addr, tcg_env,
                                  offsetof(CPUX86State, fpdp));
                }
            }
        }
        s->A0 = a0_shared;
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
#if defined(XBOX)
                x87_top_move(s, 1);
#endif
                break;
            case 2: /* fptan */
                gen_helper_fptan(tcg_env);
#if defined(XBOX)
                x87_top_set(s, -1); /* pushes only in range */
#endif
                break;
            case 3: /* fpatan */
                gen_helper_fpatan(tcg_env);
#if defined(XBOX)
                x87_top_move(s, 1);
#endif
                break;
            case 4: /* fxtract */
                gen_helper_fxtract(tcg_env);
#if defined(XBOX)
                x87_top_move(s, -1);
#endif
                break;
            case 5: /* fprem1 */
                gen_helper_fprem1(tcg_env);
                break;
            case 6: /* fdecstp */
            {
#if defined(XBOX)
                if (x87_defer(s)) {
                    x87_wt_move(s, -1);
                } else
#endif
                {
                    tcg_gen_subi_i32(fpstt, fpstt, 1);
                    tcg_gen_andi_i32(fpstt, fpstt, 7);
                    s->fpstt_delta -= 1;
#if defined(XBOX)
                    x87_top_move(s, -1);
#endif
                }
                TCGv_i32 fpus_tmp = tcg_temp_new_i32();
                tcg_gen_ld16u_i32(fpus_tmp, tcg_env, offsetof(CPUX86State, fpus));
                tcg_gen_andi_i32(fpus_tmp, fpus_tmp, ~0x4700);
                tcg_gen_st16_i32(fpus_tmp, tcg_env, offsetof(CPUX86State, fpus));
                break;
            }
            default:
            case 7: /* fincstp */
            {
#if defined(XBOX)
                if (x87_defer(s)) {
                    x87_wt_move(s, 1);
                } else
#endif
                {
                    tcg_gen_addi_i32(fpstt, fpstt, 1);
                    tcg_gen_andi_i32(fpstt, fpstt, 7);
                    s->fpstt_delta += 1;
#if defined(XBOX)
                    x87_top_move(s, 1);
#endif
                }
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
#if defined(XBOX)
                x87_top_move(s, 1);
#endif
                break;
            case 2: /* fsqrt */
                gen_fsqrt(s);
                break;
            case 3: /* fsincos */
                gen_helper_fsincos(tcg_env);
#if defined(XBOX)
                x87_top_set(s, -1); /* pushes only in range */
#endif
                break;
            case 4: /* frndint */
                gen_frndint(s);
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
#if defined(XBOX)
                    if (g_x87_hw) {
                        /* fsubr_STN_ST0 is ST0 - STi: the ST0_FT0 order's reverse */
                        gen_x87_hw_arith(op1 >= 4 ? op1 ^ 1 : op1, opreg, 0);
                    } else
#endif
                    gen_helper_fp_arith_STN_ST0(s, op1, opreg);
                    if (op >= 0x30) {
                        gen_fpop(s);
                    }
                } else {
#if defined(XBOX)
                    if (g_x87_hw) {
                        gen_x87_hw_arith(op1, 0, opreg);
                        break;
                    }
#endif
                    gen_fload_FT0_STN(s, opreg);
                    gen_helper_fp_arith_ST0_FT0(s, op1);
                }
            }
            break;
        case 0x02: /* fcom */
        case 0x22: /* fcom2, undocumented op */
            gen_fcom_ST0_STN(s, opreg, false);
            break;
        case 0x03: /* fcomp */
        case 0x23: /* fcomp3, undocumented op */
        case 0x32: /* fcomp5, undocumented op */
            gen_fcom_ST0_STN(s, opreg, false);
            gen_fpop(s);
            break;
        case 0x15: /* da/5 */
            switch (rm) {
            case 1: /* fucompp */
                gen_fcom_ST0_STN(s, 1, true);
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
#if defined(XBOX)
                x87_top_set(s, 0);
#endif
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
            gen_fcom_ST0_STN(s, opreg, true);
            if (op == 0x2d) {
                gen_fpop(s);
            }
            break;
        case 0x33: /* de/3 */
            switch (rm) {
            case 1: /* fcompp */
                gen_fcom_ST0_STN(s, 1, false);
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
#if defined(XBOX)
                gen_fnstsw_inline(s, s->tmp2_i32);
#else
                gen_helper_fnstsw(s->tmp2_i32, tcg_env);
#endif
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
                if (gen_fcmov_wt(s, op1, opreg) ||
                    gen_fcmov_hw(s, op1, opreg)) {
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
    s->A0 = a0_shared;
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
#if defined(__x86_64__)
    /*
     * The hot x87 ops as the host x87 instructions their __hard helpers
     * run (x87_* TCG ops), without the call: bit for bit the helpers.
     */
    g_x87_hw = g_use_hard_fpu &&
               tcg_op_supported(INDEX_op_x87_arith, TCG_TYPE_I64, 0);
#if !defined(TARGET_X86_64)
    /* ... on registers at fixed env offsets: TB keys carry TOP (x87_top). */
    xemu_x87_top_key = g_x87_hw;
#endif
#endif
#if defined(__aarch64__)
    /*
     * The write-through path (x87_wt_*) instead matches the aarch64
     * double-based hard helpers bit for bit. Not on x86_64 hosts, whose
     * hard helpers compute in 80-bit long double. The backend must have
     * FP ops (Windows ARM64 does not).
     */
    g_x87_wt = g_use_hard_fpu &&
               tcg_op_supported(INDEX_op_add_f64, TCG_TYPE_I64, 0) &&
               tcg_op_supported(INDEX_op_rint_f64, TCG_TYPE_I64, 0);
    /* m64fp loads into an FP register, quieted there (x87_wt_ld_f64). */
    g_x87_quiet = g_x87_wt &&
                  tcg_op_supported(INDEX_op_quiet_f64, TCG_TYPE_I64, 0);
#if !defined(TARGET_X86_64)
    /* Write-through blocks address ST(i) from the key's TOP (x87_top). */
    xemu_x87_top_key = g_x87_wt;
    /* ... and keep doubles across flushes (x87_wt_dbl). */
    g_x87_dbl = xemu_x87_top_key &&
                tcg_op_supported(INDEX_op_ld80d_f64, TCG_TYPE_I64, 0);
#endif
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

#if defined(XBOX) && !defined(TARGET_X86_64)
    /* Bits 32-34 of the key are the x87 TOP (x87_top). */
    dc->cs_base = (uint32_t)dc->base.tb->cs_base;
    dc->x87_top = xemu_x87_top_key
        ? (dc->base.tb->cs_base >> XEMU_TB_KEY_TOP_SHIFT) & 7 : -1;
#else
    dc->cs_base = dc->base.tb->cs_base;
#if defined(XBOX)
    dc->x87_top = -1;
#endif
#endif
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
    dc->xemu_fds_pend = -1;
    dc->xemu_strict_cpuid = xemu_strict_cpuid_enabled();
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
    dc->wt_w2_land = 0;
    dc->wt_insn_flushed = false;
    dc->wt_insn_delta = 0;
    dc->wt_top = NULL;
    dc->wt_tag_mask = 0;
    dc->wt_tag_val = 0;
    dc->wt_insn_tag_mask = 0;
    dc->wt_insn_tag_val = 0;
    dc->wt_insn_fp = false;
    dc->wt_w2_fp = false;
    dc->wt_w2_pos = NULL;
    dc->wt_nstale = 0;
    dc->mmx_synced = false;
    dc->pure_call = false;
    dc->mmx_entered = false;
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
     * page, and the x87 stores still deferred, by shadow slot. The first
     * guest access of the insn adds those (x87_wt_spill).
     */
    if (dc->xemu_fip_pend) {
        uint64_t w2 = dc->base.pc_next - dc->xemu_fip_pc;

        tcg_debug_assert(w2 <= XEMU_X87_W2_FIP_MASK);
        tcg_set_insn_start_param(tcg_last_op(), 2, w2);
    }
    if (dc->xemu_fdp_pend && !dc->xemu_fdp_sel && xemu_x87_refute_on()) {
        TCGOp *op = tcg_last_op();

        tcg_set_insn_start_param(op, 2, tcg_get_insn_start_param(op, 2) |
                                        XEMU_X87_W2_FDP);
    }
    if (dc->xemu_fds_pend >= 0) {
        TCGOp *op = tcg_last_op();

        tcg_set_insn_start_param(op, 2, tcg_get_insn_start_param(op, 2) |
                                 (uint64_t)(dc->xemu_fds_pend + 1)
                                 << XEMU_X87_W2_FDS_SHIFT);
    }
    dc->wt_insn_pend = dc->wt_pend;
    dc->wt_insn_delta = dc->fpstt_delta;
    dc->wt_insn_tag_mask = dc->wt_tag_mask;
    dc->wt_insn_tag_val = dc->wt_tag_val;
    dc->wt_insn_fp = x87_defer(dc) &&
                     ((dc->fpstt_delta & 7) || dc->wt_tag_mask);
    dc->wt_insn_flushed = false;
    dc->wt_w2_land = 0;
    dc->wt_w2_fp = false;
#endif
}

#if defined(XBOX) && defined(CONFIG_SYSTEM_ONLY)
/* The RAM address of @pc on the TB's first page, or -1 (MMIO code). */
static uint64_t xemu_hook_phys(DisasContext *dc, vaddr pc)
{
    tb_page_addr_t p0 = tb_page_addr0(dc->base.tb);

    assert(translator_is_same_page(&dc->base, pc));
    return p0 == -1 ? (uint64_t)-1
                    : (p0 & TARGET_PAGE_MASK) | (pc & ~TARGET_PAGE_MASK);
}
#endif

static void i386_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    bool orig_cc_op_dirty = dc->cc_op_dirty;
    CCOp orig_cc_op = dc->cc_op;
    target_ulong orig_pc_save = dc->pc_save;
#if defined(XBOX)
    uint8_t orig_wt_pend = dc->wt_pend;
    bool orig_mmx_entered = dc->mmx_entered;
    int8_t orig_x87_top = dc->x87_top;
#endif

#if defined(XBOX)
    xemu_x87_boundary_check(dc);
    x87_wt_boundary_check(dc);
    x87_top_check(dc);
#endif
#if defined(XBOX) && defined(CONFIG_SYSTEM_ONLY)
    /*
     * A guest PC hook site always starts its TB (see the cut below), so
     * nothing is pending here and no page-crossing retry can drop this.
     * A CF_NOIRQ TB re-executes an instruction that has already started
     * (cpu_io_recompile, precise SMC, a watchpoint), whose hooks already
     * ran at the top of the TB it started in: no second call. (Record and
     * replay's one-instruction TB is CF_NOIRQ too; xemu doesn't run it.)
     */
    if (dc->base.num_insns == 1 &&
        !(tb_cflags(dc->base.tb) & CF_NOIRQ) &&
        xemu_hooks_site_at(dc->base.pc_next,
                           xemu_hook_phys(dc, dc->base.pc_next))) {
        gen_helper_xemu_hook(tcg_env, tcg_constant_tl(dc->base.pc_next),
                             tcg_constant_i32(tb_cflags(dc->base.tb) &
                                              CF_PCREL ? 1 : 0));
    }
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
        /* Decoding ends before any op: no deferred x87 store or spill. */
        assert(dc->wt_pend == orig_wt_pend && !dc->wt_w2_land);
        dc->mmx_entered = orig_mmx_entered;
        assert(dc->x87_top == orig_x87_top);
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
#if defined(XBOX) && defined(CONFIG_SYSTEM_ONLY)
        } else if (xemu_hooks_site_at(dc->base.pc_next,
                                      xemu_hook_phys(dc, dc->base.pc_next))) {
            /* Start the next TB at the hook site. */
            dc->base.is_jmp = DISAS_TOO_MANY;
#endif
        }
    }
}

static void i386_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);

#if defined(XBOX)
    /* Past the last insn's exits: no shadow need match this flush. */
    dc->wt_insn_flushed = true;
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

#if defined(XBOX)
/* Before every guest load or store (tcg-op-ldst.c gen_ldst1/2). */
void gen_ldst_prologue(void)
{
    DisasContext *s = (DisasContext *)tcg_ctx->disas_ctx;

    if (s && (s->wt_insn_pend || s->wt_insn_fp)) {
        x87_wt_spill(s);
    }
    /* Last: nothing may come between these and the access. */
    if (s && s->xemu_fdp_pend && !s->xemu_fdp_sel) {
#if TARGET_LONG_BITS == 32
        tcg_gen_st_i32_slow(s->xemu_fdp_val, offsetof(CPUX86State, fpdp));
#else
        tcg_gen_st_i64_slow(s->xemu_fdp_val, offsetof(CPUX86State, fpdp));
#endif
    }
}
#endif
