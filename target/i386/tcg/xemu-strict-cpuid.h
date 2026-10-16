/*
 * xemu strict CPUID decode (XEMU_STRICT_CPUID=1): see xemu_strict_cpuid.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_STRICT_CPUID_H
#define XEMU_STRICT_CPUID_H

/* The feature an instruction needs, beyond what its decoder entry names. */
enum {
    XEMU_STRICT_NONE,
    XEMU_STRICT_MMX,
    XEMU_STRICT_SSE,
    XEMU_STRICT_SSE2,
    XEMU_STRICT_SSE3,
};

/* XEMU_STRICT_CPUID is set (read once; latched per translation). */
bool xemu_strict_cpuid_enabled(void);

#endif
