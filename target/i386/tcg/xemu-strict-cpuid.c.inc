/*
 * xemu strict CPUID decode (XEMU_STRICT_CPUID=1), translator side.
 * Included by decode-new.c.inc; rationale in xemu_strict_cpuid.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define XS_M  XEMU_STRICT_MMX
#define XS_1  XEMU_STRICT_SSE
#define XS_2  XEMU_STRICT_SSE2
#define XS_3  XEMU_STRICT_SSE3

/*
 * The feature each legacy (non-VEX) 0F-map SSE/MMX opcode needs on real
 * hardware (Intel SDM opcode map), by SSE prefix in decode_by_prefix's
 * order: none, 66, F3, F2. Without a prefix the integer ops use MMX
 * registers: plain MMX, except SSE's MMX extensions and SSE2's PADDQ,
 * PMULUDQ and PSUBQ. Used only for entries without a cpuid() of their own
 * (the 0F38 and 0F3A maps all have one); invalid prefixes have already
 * raised #UD.
 */
static const uint8_t xemu_strict_0f[256][4] = {
    [0x10] = { XS_1, XS_2, XS_1, XS_2 },   /* movups movupd movss movsd */
    [0x11] = { XS_1, XS_2, XS_1, XS_2 },
    [0x12] = { XS_1, XS_2, XS_3, XS_3 },   /* movlps movlpd movsldup movddup */
    [0x13] = { XS_1, XS_2 },
    [0x14] = { XS_1, XS_2 },               /* unpcklps/pd */
    [0x15] = { XS_1, XS_2 },
    [0x16] = { XS_1, XS_2, XS_3 },         /* movhps movhpd movshdup */
    [0x17] = { XS_1, XS_2 },
    [0x28] = { XS_1, XS_2 },               /* movaps/pd */
    [0x29] = { XS_1, XS_2 },
    [0x2a] = { XS_1, XS_2, XS_1, XS_2 },   /* cvtpi2ps cvtpi2pd cvtsi2ss cvtsi2sd */
    [0x2b] = { XS_1, XS_2 },               /* movntps/pd */
    [0x2c] = { XS_1, XS_2, XS_1, XS_2 },   /* cvtt{ps2pi,pd2pi,ss2si,sd2si} */
    [0x2d] = { XS_1, XS_2, XS_1, XS_2 },
    [0x2e] = { XS_1, XS_2 },               /* ucomiss/sd */
    [0x2f] = { XS_1, XS_2 },
    [0x50] = { XS_1, XS_2 },               /* movmskps/pd */
    [0x51] = { XS_1, XS_2, XS_1, XS_2 },   /* sqrt */
    [0x52] = { XS_1, 0,    XS_1 },         /* rsqrtps/ss */
    [0x53] = { XS_1, 0,    XS_1 },         /* rcpps/ss */
    [0x54] = { XS_1, XS_2 },               /* and, andn, or, xor */
    [0x55] = { XS_1, XS_2 },
    [0x56] = { XS_1, XS_2 },
    [0x57] = { XS_1, XS_2 },
    [0x58] = { XS_1, XS_2, XS_1, XS_2 },   /* add */
    [0x59] = { XS_1, XS_2, XS_1, XS_2 },   /* mul */
    [0x5a] = { XS_2, XS_2, XS_2, XS_2 },   /* cvtps2pd cvtpd2ps cvtss2sd cvtsd2ss */
    [0x5b] = { XS_2, XS_2, XS_2 },         /* cvtdq2ps cvtps2dq cvttps2dq */
    [0x5c] = { XS_1, XS_2, XS_1, XS_2 },   /* sub, min, div, max */
    [0x5d] = { XS_1, XS_2, XS_1, XS_2 },
    [0x5e] = { XS_1, XS_2, XS_1, XS_2 },
    [0x5f] = { XS_1, XS_2, XS_1, XS_2 },
    [0x60] = { XS_M, XS_2 }, [0x61] = { XS_M, XS_2 },   /* punpckl*, pack*, pcmpgt* */
    [0x62] = { XS_M, XS_2 }, [0x63] = { XS_M, XS_2 },
    [0x64] = { XS_M, XS_2 }, [0x65] = { XS_M, XS_2 },
    [0x66] = { XS_M, XS_2 }, [0x67] = { XS_M, XS_2 },
    [0x68] = { XS_M, XS_2 }, [0x69] = { XS_M, XS_2 },   /* punpckh*, packssdw */
    [0x6a] = { XS_M, XS_2 }, [0x6b] = { XS_M, XS_2 },
    [0x6c] = { 0,    XS_2 },               /* punpcklqdq */
    [0x6d] = { 0,    XS_2 },               /* punpckhqdq */
    [0x6e] = { XS_M, XS_2 },               /* movd */
    [0x6f] = { XS_M, XS_2, XS_2 },         /* movq movdqa movdqu */
    [0x70] = { XS_1, XS_2, XS_2, XS_2 },   /* pshufw pshufd pshufhw pshuflw */
    [0x71] = { XS_M, XS_2 },               /* shift groups 12-14 */
    [0x72] = { XS_M, XS_2 },
    [0x73] = { XS_M, XS_2 },
    [0x74] = { XS_M, XS_2 }, [0x75] = { XS_M, XS_2 },   /* pcmpeq* */
    [0x76] = { XS_M, XS_2 },
    [0x77] = { XS_M },                     /* emms */
    [0x7e] = { XS_M, XS_2, XS_2 },         /* movd movd movq */
    [0x7f] = { XS_M, XS_2, XS_2 },         /* movq movdqa movdqu */
    [0xae] = { XS_1 },                     /* ldmxcsr stmxcsr (rest of group 15 tabled) */
    [0xc2] = { XS_1, XS_2, XS_1, XS_2 },   /* cmp */
    [0xc4] = { XS_1, XS_2 },               /* pinsrw */
    [0xc5] = { XS_1, XS_2 },               /* pextrw */
    [0xc6] = { XS_1, XS_2 },               /* shufps/pd */
    [0xd1] = { XS_M, XS_2 }, [0xd2] = { XS_M, XS_2 },   /* psrl* */
    [0xd3] = { XS_M, XS_2 },
    [0xd4] = { XS_2, XS_2 },               /* paddq */
    [0xd5] = { XS_M, XS_2 },               /* pmullw */
    [0xd6] = { 0,    XS_2, XS_2, XS_2 },   /* movq movq2dq movdq2q */
    [0xd7] = { XS_1, XS_2 },               /* pmovmskb */
    [0xd8] = { XS_M, XS_2 }, [0xd9] = { XS_M, XS_2 },   /* psubus* */
    [0xda] = { XS_1, XS_2 },               /* pminub */
    [0xdb] = { XS_M, XS_2 }, [0xdc] = { XS_M, XS_2 },   /* pand, paddus* */
    [0xdd] = { XS_M, XS_2 },
    [0xde] = { XS_1, XS_2 },               /* pmaxub */
    [0xdf] = { XS_M, XS_2 },               /* pandn */
    [0xe0] = { XS_1, XS_2 },               /* pavgb */
    [0xe1] = { XS_M, XS_2 }, [0xe2] = { XS_M, XS_2 },   /* psra* */
    [0xe3] = { XS_1, XS_2 },               /* pavgw */
    [0xe4] = { XS_1, XS_2 },               /* pmulhuw */
    [0xe5] = { XS_M, XS_2 },               /* pmulhw */
    [0xe6] = { 0,    XS_2, XS_2, XS_2 },   /* cvttpd2dq cvtdq2pd cvtpd2dq */
    [0xe7] = { XS_1, XS_2 },               /* movntq movntdq */
    [0xe8] = { XS_M, XS_2 }, [0xe9] = { XS_M, XS_2 },   /* psubs* */
    [0xea] = { XS_1, XS_2 },               /* pminsw */
    [0xeb] = { XS_M, XS_2 }, [0xec] = { XS_M, XS_2 },   /* por, padds* */
    [0xed] = { XS_M, XS_2 },
    [0xee] = { XS_1, XS_2 },               /* pmaxsw */
    [0xef] = { XS_M, XS_2 },               /* pxor */
    [0xf1] = { XS_M, XS_2 }, [0xf2] = { XS_M, XS_2 },   /* psll* */
    [0xf3] = { XS_M, XS_2 },
    [0xf4] = { XS_2, XS_2 },               /* pmuludq */
    [0xf5] = { XS_M, XS_2 },               /* pmaddwd */
    [0xf6] = { XS_1, XS_2 },               /* psadbw */
    [0xf7] = { XS_1, XS_2 },               /* maskmovq maskmovdqu */
    [0xf8] = { XS_M, XS_2 }, [0xf9] = { XS_M, XS_2 },   /* psub* */
    [0xfa] = { XS_M, XS_2 },
    [0xfb] = { XS_2, XS_2 },               /* psubq */
    [0xfc] = { XS_M, XS_2 }, [0xfd] = { XS_M, XS_2 },   /* padd* */
    [0xfe] = { XS_M, XS_2 },
};

#undef XS_M
#undef XS_1
#undef XS_2
#undef XS_3

/*
 * After the decoder's own CPUID check: if the instruction needs a feature
 * the CPU model lacks, emit #UD (through helper_xemu_strict_ud, which
 * reports it) and return true. Only when s->xemu_strict_cpuid.
 */
static bool xemu_strict_cpuid_fault(DisasContext *s, X86DecodedInsn *decode)
{
    static const uint8_t prefix_byte[4] = { 0, 0x66, 0xf3, 0xf2 };
    int mod = s->modrm >> 6, reg = (s->modrm >> 3) & 7;
    int need = XEMU_STRICT_NONE;
    int col;
    bool has;

    if (s->prefix & PREFIX_VEX) {
        return false;   /* gated by CR4.OSXSAVE/XCR0, which need AVX/XSAVE */
    }
    col = s->prefix & PREFIX_REPNZ ? 3 : s->prefix & PREFIX_REPZ ? 2 :
          s->prefix & PREFIX_DATA ? 1 : 0;
    if (s->xemu_esc == 0x0f) {
        if (decode->b == 0xae && s->has_modrm && mod == 3 && reg == 5) {
            need = XEMU_STRICT_SSE2;    /* LFENCE: tabled as cpuid(SSE) */
        } else if (decode->e.cpuid == X86_FEAT_None) {
            need = xemu_strict_0f[decode->b][col];
        }
    } else if (s->xemu_esc == 0 && decode->e.gen == gen_x87 &&
               s->has_modrm && mod != 3 && reg == 1 &&
               (decode->b == 0xdb || decode->b == 0xdd || decode->b == 0xdf)) {
        need = XEMU_STRICT_SSE3;        /* FISTTP m32/m64/m16 */
    }

    switch (need) {
    case XEMU_STRICT_NONE:
        return false;
    case XEMU_STRICT_MMX:
        has = s->cpuid_features & CPUID_MMX;
        break;
    case XEMU_STRICT_SSE:
        has = s->cpuid_features & CPUID_SSE;
        break;
    case XEMU_STRICT_SSE2:
        has = s->cpuid_features & CPUID_SSE2;
        break;
    default:
        has = s->cpuid_ext_features & CPUID_EXT_SSE3;
        break;
    }
    if (has) {
        return false;
    }

    gen_update_cc_op(s);
    gen_update_eip_cur(s);
    gen_helper_xemu_strict_ud(tcg_env,
        tcg_constant_i32(need | (s->has_modrm ? 0x100 : 0)),
        tcg_constant_i32((uint32_t)prefix_byte[col] << 24 | s->xemu_esc << 16 |
                         decode->b << 8 | (s->has_modrm ? s->modrm : 0)));
    s->base.is_jmp = DISAS_NORETURN;
    return true;
}
