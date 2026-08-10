/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Define target-specific opcode support
 * Copyright (c) 2013 Huawei Technologies Duesseldorf GmbH
 */

#ifndef TCG_TARGET_HAS_H
#define TCG_TARGET_HAS_H

#include "host/cpuinfo.h"

#define have_lse    (cpuinfo & CPUINFO_LSE)
#define have_lse2   (cpuinfo & CPUINFO_LSE2)

/* optional instructions */
#define TCG_TARGET_HAS_extr_i64_i32     0

/*
 * Without FEAT_LSE2, we must use LDXP+STXP to implement atomic 128-bit load,
 * which requires writable pages.  We must defer to the helper for user-only,
 * but in system mode all ram is writable for the host.
 */
#ifdef CONFIG_USER_ONLY
#define TCG_TARGET_HAS_qemu_ldst_i128   have_lse2
#else
#define TCG_TARGET_HAS_qemu_ldst_i128   1
#endif

#define TCG_TARGET_HAS_tst              1

#define TCG_TARGET_HAS_v64              1
#define TCG_TARGET_HAS_v128             1
#define TCG_TARGET_HAS_v256             0

#define TCG_TARGET_HAS_andc_vec         1
#define TCG_TARGET_HAS_orc_vec          1
#define TCG_TARGET_HAS_nand_vec         0
#define TCG_TARGET_HAS_nor_vec          0
#define TCG_TARGET_HAS_eqv_vec          0
#define TCG_TARGET_HAS_not_vec          1
#define TCG_TARGET_HAS_neg_vec          1
#define TCG_TARGET_HAS_abs_vec          1
#define TCG_TARGET_HAS_roti_vec         0
#define TCG_TARGET_HAS_rots_vec         0
#define TCG_TARGET_HAS_rotv_vec         0
#define TCG_TARGET_HAS_shi_vec          1
#define TCG_TARGET_HAS_shs_vec          0
#define TCG_TARGET_HAS_shv_vec          1
#define TCG_TARGET_HAS_mul_vec          1
#define TCG_TARGET_HAS_sat_vec          1
#define TCG_TARGET_HAS_minmax_vec       1
#define TCG_TARGET_HAS_bitsel_vec       1
#define TCG_TARGET_HAS_cmpsel_vec       0
#define TCG_TARGET_HAS_tst_vec          1

/*
 * Inline x87 FPU (FP TCG ops). Disabled for Windows/ARM64: with
 * TCG_TARGET_HAS_fpu=1 the generic tcg.c/optimize.c FP paths
 * become reachable and llvm-mingw fails to constant-fold every
 * qemu_build_not_reached() branch out of them, leaving undefined
 * references at link (upstream links there precisely because it
 * has no aarch64 FP TCG backend). Windows/ARM64 falls back to the
 * bit-equivalent helper-based hard FPU, like x86_64 hosts.
 */
#ifdef _WIN32
#define TCG_TARGET_HAS_fpu              0
#define TCG_TARGET_HAS_rint_cvt         0
#else
#define TCG_TARGET_HAS_fpu              1
#define TCG_TARGET_HAS_rint_cvt         1
#endif

#define TCG_TARGET_extract_valid(type, ofs, len)   1
#define TCG_TARGET_sextract_valid(type, ofs, len)  1
#define TCG_TARGET_deposit_valid(type, ofs, len)   1

#endif
