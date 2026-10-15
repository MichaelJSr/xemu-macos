/*
 * xemu: REP MOVS/STOS as host memmove/memset, one page run at a time.
 *
 * Xbox titles copy and clear memory with REP MOVSD/STOSD all the time,
 * mostly 8 to 128 dwords at once. The translated loop pays two softmmu TLB
 * lookups (and outside icount two barriers) per element; here each run of
 * elements that stays on one source and one destination page costs two
 * probes and a memmove. The helper only ever takes the elements the loop
 * would have run without a break, and stops at anything the loop must see
 * element by element (a page that isn't plain RAM or is watched, a fault,
 * an element straddling a page, DF=1), leaving the rest to it. Guest
 * memory, registers, dirty tracking and TB invalidation come out as from
 * the loop, and the loop's TB exits are untouched, so icount is exact.
 *
 * Copyright (c) 2026 xemu-macos contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/target_page.h"
#include "exec/icount.h"
#include "accel/tcg/probe.h"
#include "qemu/bswap.h"

#if defined(XBOX)

/* Forward REP MOVS with dst inside the source: elements read earlier stores. */
static void rep_movs_overlap(uint8_t *d, const uint8_t *s, uint32_t n, int sz)
{
    uint8_t t[4];

    for (uint32_t i = 0; i < n; i++, d += sz, s += sz) {
        memcpy(t, s, sz);
        memcpy(d, t, sz);
    }
}

static void rep_stos_fill(uint8_t *d, uint32_t val, uint32_t n, MemOp ot)
{
    switch (ot) {
    case MO_8:
        memset(d, val, n);
        break;
    case MO_16:
        if ((uint8_t)val == (uint8_t)(val >> 8)) {
            memset(d, val, n << 1);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                stw_le_p(d + (i << 1), val);
            }
        }
        break;
    default:
        if (val == (uint8_t)val * 0x01010101u) {
            memset(d, val, n << 2);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                stl_le_p(d + (i << 2), val);
            }
        }
        break;
    }
}

/*
 * desc = ot | stos << 2 | mmu_idx << 3. Called at the top of a REP with a
 * 32-bit address size, flat DS/ES and no segment override, before the
 * ECX == 0 check.
 */
void HELPER(xemu_rep_bulk)(CPUX86State *env, uint32_t desc)
{
    MemOp ot = desc & 3;
    bool stos = desc & 4;
    int mmu_idx = desc >> 3;
    uintptr_t ra = GETPC();
    uint32_t ecx = env->regs[R_ECX];
    uint32_t n;

    if (env->df != 1) {
        return;
    }
    /*
     * The loop leaves the TB to the main loop whenever the ECX it is left
     * with is 1 modulo 65536 (REP_MAX), short of the last element: never
     * step onto or past such a count, so the TB exits stay the same.
     */
    n = ecx <= 65536 ? ecx : (ecx - 2) & 0xffff;
    if (!icount_enabled()) {
        /* Order the bulk stores after earlier guest accesses (TSO). */
        smp_mb();
    }

    while (n) {
        uint32_t dst = env->regs[R_EDI];
        uint32_t run = MIN(n, (TARGET_PAGE_SIZE - (dst & ~TARGET_PAGE_MASK))
                              >> ot);
        uint32_t bytes;
        uint8_t *d;

        if (stos) {
            bytes = run << ot;
            if (!run) {
                break;
            }
            d = xemu_probe_ram(env, dst, bytes, MMU_DATA_STORE, mmu_idx, ra);
            if (!d) {
                break;
            }
            rep_stos_fill(d, env->regs[R_EAX], run, ot);
        } else {
            uint32_t src = env->regs[R_ESI];
            uint8_t *s;

            run = MIN(run, (TARGET_PAGE_SIZE - (src & ~TARGET_PAGE_MASK)) >> ot);
            bytes = run << ot;
            if (!run) {
                break;
            }
            /* In the loop's order: load, then store. */
            s = xemu_probe_ram(env, src, bytes, MMU_DATA_LOAD, mmu_idx, ra);
            if (!s) {
                break;
            }
            d = xemu_probe_ram(env, dst, bytes, MMU_DATA_STORE, mmu_idx, ra);
            if (!d) {
                break;
            }
            if (d > s && d < s + bytes) {
                rep_movs_overlap(d, s, run, 1 << ot);
            } else {
                memmove(d, s, bytes);
            }
            env->regs[R_ESI] = src + bytes;
        }
        env->regs[R_EDI] = dst + bytes;
        env->regs[R_ECX] -= run;
        n -= run;
        if (!icount_enabled()) {
            xemu_probe_ram_stored(env, dst, bytes, mmu_idx, ra);
        }
    }
}

#endif /* XBOX */
