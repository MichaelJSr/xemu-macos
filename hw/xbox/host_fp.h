/*
 * Device work on the vCPU thread under the default host FP control state
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TCG leaves the guest's x87 rounding mode in the host FPCR and
 * re-programs it lazily, so device code that does float math on the vCPU
 * thread (deterministic mode's inline PFIFO pusher and APU ticks) runs
 * between xbox_host_fp_enter() and xbox_host_fp_leave(): the rounding the
 * device threads have, and TCG's FPCR back afterwards.
 *
 * On aarch64 that is FPCR alone, and it is written only when it differs
 * from the value wanted (on entry the default, 0, which is FE_DFL_ENV's):
 * TCG's FPCR almost always is the default, and a write costs tens of ns
 * where a read costs a few. FPSR, the sticky exception flags, is left
 * alone, as nothing on the vCPU thread reads it.
 */

#ifndef HW_XBOX_HOST_FP_H
#define HW_XBOX_HOST_FP_H

#if defined(__aarch64__)

typedef struct XboxHostFp {
    uint64_t fpcr;
} XboxHostFp;

static inline void xbox_host_fp_enter(XboxHostFp *s)
{
    uint64_t fpcr;

    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    s->fpcr = fpcr;
    if (unlikely(fpcr)) {
        __asm__ volatile("msr fpcr, %0" : : "r"((uint64_t)0) : "memory");
    }
}

static inline void xbox_host_fp_leave(const XboxHostFp *s)
{
    uint64_t fpcr;

    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (unlikely(fpcr != s->fpcr)) {
        __asm__ volatile("msr fpcr, %0" : : "r"(s->fpcr) : "memory");
    }
}

#elif defined(__x86_64__)

/*
 * x86_64: the guest's precision and rounding control stay in the x87
 * control word (target/i386 xemu_sync_host_x87_cw), so device code gets
 * FE_DFL_ENV's control state, x87 CW 0x37f and MXCSR's 0x1f80 control
 * bits, compare-skipped as on aarch64; mingw's fegetenv/fesetenv cost ~1%
 * of the det vCPU. The exit puts both registers back exactly.
 */
#include <xmmintrin.h>

typedef struct XboxHostFp {
    uint32_t mxcsr;
    uint16_t fpcw;
} XboxHostFp;

static inline void xbox_host_fp_enter(XboxHostFp *s)
{
    uint16_t cw, dflt = 0x037f;
    uint32_t csr = _mm_getcsr();

    __asm__ volatile("fnstcw %0" : "=m"(cw));
    s->fpcw = cw;
    s->mxcsr = csr;
    if (cw != dflt) {
        __asm__ volatile("fldcw %0" : : "m"(dflt));
    }
    if (unlikely((csr & ~0x3fu) != 0x1f80)) {
        _mm_setcsr(0x1f80 | (csr & 0x3f));
    }
}

static inline void xbox_host_fp_leave(const XboxHostFp *s)
{
    uint16_t cw;
    uint32_t csr = _mm_getcsr();

    __asm__ volatile("fnstcw %0" : "=m"(cw));
    if (cw != s->fpcw) {
        __asm__ volatile("fldcw %0" : : "m"(s->fpcw));
    }
    if (csr != s->mxcsr) {
        _mm_setcsr(s->mxcsr);
    }
}

#else

#include <fenv.h>

typedef struct XboxHostFp {
    fenv_t env;
} XboxHostFp;

static inline void xbox_host_fp_enter(XboxHostFp *s)
{
    fegetenv(&s->env);
    fesetenv(FE_DFL_ENV);
}

static inline void xbox_host_fp_leave(const XboxHostFp *s)
{
    fesetenv(&s->env);
}

#endif

#endif
