/*
 * Sub-page arm (b): inline NOTDIRTY store-skip — state, init, refuter.
 * See include/exec/xemu-subpage-fast.h for the design summary; the
 * emission half lives in tcg/aarch64/tcg-target.c.inc.
 *
 * Copyright (c) 2026 xemu-macos contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "qemu/osdep.h"

#if defined(XBOX)

#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "exec/ramlist.h"
#include "exec/target_page.h"
#include "exec/cpu-common.h"
#include "system/physmem.h"
#include "exec/xemu-subpage-fast.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "tcg/tcg.h"
#include "tcg/tcg-ldst.h"

uint64_t *xemu_sf_block_map;
uintptr_t xemu_sf_ram_host;
uint64_t xemu_sf_ram_pages;
unsigned long *xemu_sf_dirty0[XEMU_SF_NCLIENTS];

uint64_t xemu_sf_seen;
uint64_t xemu_sf_skips;
uint64_t xemu_sf_demote[XEMU_SF_DEMOTE_NREASONS];
uint64_t xemu_sf_refute_total;
uint64_t xemu_sf_refute_viol;

static const unsigned sf_clients[XEMU_SF_NCLIENTS] = {
    DIRTY_MEMORY_VGA, DIRTY_MEMORY_MIGRATION,
    DIRTY_MEMORY_NV2A, DIRTY_MEMORY_NV2A_TEX,
};

static void xemu_sf_dump(void)
{
    if (!xemu_sf_seen && !xemu_sf_skips && !xemu_sf_refute_total) {
        return;
    }
    fprintf(stderr,
            "xemu: subpage-fast seen=%llu skips=%llu (%.1f%%) demote:"
            " not-pure=%llu code-block=%llu dirty-clean=%llu oob=%llu\n",
            (unsigned long long)xemu_sf_seen,
            (unsigned long long)xemu_sf_skips,
            xemu_sf_seen ? 100.0 * xemu_sf_skips / xemu_sf_seen : 0.0,
            (unsigned long long)xemu_sf_demote[0],
            (unsigned long long)xemu_sf_demote[1],
            (unsigned long long)xemu_sf_demote[2],
            (unsigned long long)xemu_sf_demote[3]);
    if (xemu_sf_refute_total) {
        fprintf(stderr,
                "xemu: subpage-fast refute: total=%llu VIOLATIONS=%llu\n",
                (unsigned long long)xemu_sf_refute_total,
                (unsigned long long)xemu_sf_refute_viol);
    }
}

static bool xemu_sf_init(void)
{
    RAMBlock *rb = qemu_ram_block_by_name("xbox.ram");
    if (!rb) {
        warn_report("subpage-fast: no xbox.ram block; disabled");
        return false;
    }
    xemu_sf_ram_host = (uintptr_t)qemu_ram_get_host_addr(rb);
    xemu_sf_ram_pages = qemu_ram_get_used_length(rb) >> TARGET_PAGE_BITS;
    xemu_sf_block_map = g_malloc0(xemu_sf_ram_pages * sizeof(uint64_t));

    /*
     * Resolve dirty_memory[c]->blocks[0] once: DIRTY_MEMORY_BLOCK_SIZE
     * covers 2 Mi pages, so ALL of xbox.ram's pages live in blocks[0],
     * and the blocks array is never reallocated for fixed-size RAM.
     */
    QEMU_BUILD_BUG_ON(DIRTY_MEMORY_BLOCK_SIZE < (1 << 20));
    for (int i = 0; i < XEMU_SF_NCLIENTS; i++) {
        DirtyMemoryBlocks *b =
            qatomic_rcu_read(&ram_list.dirty_memory[sf_clients[i]]);
        if (!b) {
            warn_report("subpage-fast: dirty client %u unresolved; disabled",
                        sf_clients[i]);
            return false;
        }
        xemu_sf_dirty0[i] = b->blocks[0];
    }
    atexit(xemu_sf_dump);
    return true;
}

int xemu_subpage_fast_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        /*
         * Dark by default everywhere: the refuter validated 6.35M
         * skip decisions with zero violations (2026-07-18), but the
         * predicted +0.3-0.7 fps was never measurable on a quiet
         * machine before the project's closing release — opt in with
         * XEMU_SUBPAGE_FAST=1 and A/B before trusting it as a win.
         */
        int m = 0;
        const char *e = getenv("XEMU_SUBPAGE_FAST");
        if (e) {
            m = (e[0] == '1') ? 1 : 0;
        }
        /*
         * Arm (a) maintains the code-block bitmap this probe consults
         * (xemu_subpage_set_code runs under its gate). With arm (a)
         * disabled the mirror would read all-clear — the wrong-code
         * direction — so force off.
         */
        e = getenv("XEMU_SUBPAGE_DIRTY");
        if (e && e[0] == '0') {
            m = 0;
        }
        e = getenv("XEMU_SUBPAGE_FAST_REFUTE");
        if (m && e && e[0] == '1') {
            m = 2;
        }
        if (m && !xemu_sf_init()) {
            m = 0;
        }
        mode = m;
    }
    return mode;
}

void xemu_sf_sync_page(hwaddr page_addr, uint64_t code_blocks)
{
    uint64_t idx = page_addr >> TARGET_PAGE_BITS;
    if (xemu_sf_block_map && idx < xemu_sf_ram_pages) {
        xemu_sf_block_map[idx] = code_blocks;
    }
}

void xemu_sf_clear_all(void)
{
    if (xemu_sf_block_map) {
        memset(xemu_sf_block_map, 0,
               xemu_sf_ram_pages * sizeof(uint64_t));
    }
}

/*
 * Refute mode: the stub routes its would-skip decisions here instead of
 * storing inline. Re-derive every condition from ground truth, count
 * violations loudly, then perform the real store via the normal helper
 * so the guest stays correct through the soak.
 */
static void xemu_sf_refute_check(struct CPUArchState *env, uint64_t addr,
                                 unsigned size, uintptr_t ra)
{
    CPUState *cpu = env_cpu(env);
    hwaddr paddr;
    bool bitmap_says, live_says;

    xemu_sf_refute_total++;

    /*
     * The stub derived ram_addr from the TLB addend; re-derive it
     * independently through the MMU (this also re-checks the mapping
     * the probe assumed).
     */
    void *host = probe_access(env, addr, size, MMU_DATA_STORE,
                              cpu_mmu_index(cpu, false), ra);
    if (!host) {
        xemu_sf_refute_viol++;
        fprintf(stderr, "xemu: subpage-fast REFUTE: probe_access NULL "
                "at vaddr 0x%" PRIx64 "\n", addr);
        return;
    }
    paddr = (uintptr_t)host - xemu_sf_ram_host;
    if ((uint64_t)(paddr >> TARGET_PAGE_BITS) >= xemu_sf_ram_pages) {
        xemu_sf_refute_viol++;
        fprintf(stderr, "xemu: subpage-fast REFUTE: host addr outside "
                "xbox.ram for vaddr 0x%" PRIx64 "\n", addr);
        return;
    }

    /* Condition 2 ground truth: bitmap AND live-TB scan agree non-code. */
    if (!xemu_sf_ground_truth(paddr, size, &bitmap_says, &live_says)) {
        xemu_sf_refute_viol++;
        fprintf(stderr, "xemu: subpage-fast REFUTE VIOLATION: paddr "
                "0x%" PRIx64 " size %u bitmap_noncode=%d live_noncode=%d\n",
                (uint64_t)paddr, size, bitmap_says, live_says);
    }

    /* Condition 3 ground truth: set_dirty_range(NOCODE) is a no-op. */
    if (physical_memory_range_includes_clean(paddr, size,
                                             DIRTY_CLIENTS_NOCODE)) {
        xemu_sf_refute_viol++;
        fprintf(stderr, "xemu: subpage-fast REFUTE VIOLATION: NOCODE "
                "client clean at paddr 0x%" PRIx64 " size %u\n",
                (uint64_t)paddr, size);
    }
}

void xemu_sf_refute_stb(struct CPUArchState *env, uint64_t addr,
                        uint32_t val, MemOpIdx oi, uintptr_t ra)
{
    xemu_sf_refute_check(env, addr, 1, ra);
    helper_stb_mmu(env, addr, val, oi, ra);
}

void xemu_sf_refute_stw(struct CPUArchState *env, uint64_t addr,
                        uint32_t val, MemOpIdx oi, uintptr_t ra)
{
    xemu_sf_refute_check(env, addr, 2, ra);
    helper_stw_mmu(env, addr, val, oi, ra);
}

void xemu_sf_refute_stl(struct CPUArchState *env, uint64_t addr,
                        uint32_t val, MemOpIdx oi, uintptr_t ra)
{
    xemu_sf_refute_check(env, addr, 4, ra);
    helper_stl_mmu(env, addr, val, oi, ra);
}

void xemu_sf_refute_stq(struct CPUArchState *env, uint64_t addr,
                        uint64_t val, MemOpIdx oi, uintptr_t ra)
{
    xemu_sf_refute_check(env, addr, 8, ra);
    helper_stq_mmu(env, addr, val, oi, ra);
}

#endif /* XBOX */
