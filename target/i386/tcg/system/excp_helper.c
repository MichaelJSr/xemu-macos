/*
 *  x86 exception helpers - system code
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
#include "cpu.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/probe.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "tcg/helper-tcg.h"
#if defined(XBOX)
#include "exec/translation-block.h" /* xemu_tb_page_has_tbs */
#include "qemu/timer.h"
#include "system/memory.h"
#endif

typedef struct TranslateParams {
    target_ulong addr;
    target_ulong cr3;
    int pg_mode;
    int mmu_idx;
    int ptw_idx;
    MMUAccessType access_type;
} TranslateParams;

typedef struct TranslateResult {
    hwaddr paddr;
    int prot;
    int page_size;
#if defined(XBOX)
    /*
     * xemu: by host pointer and as the walk's A/D update left them, the
     * 32-bit walk's PDE of a 4 KB page and every walk's leaf
     * (xemu_tlb_log_fill). Plain stores: no branch in mmu_translate,
     * whose PGO profile is hot.
     */
    uint32_t *pde, *leaf;
    uint32_t pde_val, leaf_val;
#endif
} TranslateResult;

typedef enum TranslateFaultStage2 {
    S2_NONE,
    S2_GPA,
    S2_GPT,
} TranslateFaultStage2;

typedef struct TranslateFault {
    int exception_index;
    int error_code;
    target_ulong cr2;
    TranslateFaultStage2 stage2;
} TranslateFault;

typedef struct PTETranslate {
    CPUX86State *env;
    TranslateFault *err;
    int ptw_idx;
    void *haddr;
    hwaddr gaddr;
} PTETranslate;

static bool ptw_translate(PTETranslate *inout, hwaddr addr)
{
    int flags;

    inout->gaddr = addr;
    flags = probe_access_full_mmu(inout->env, addr, 0, MMU_DATA_STORE,
                                  inout->ptw_idx, &inout->haddr, NULL);

    if (unlikely(flags & TLB_INVALID_MASK)) {
        TranslateFault *err = inout->err;

        assert(inout->ptw_idx == MMU_NESTED_IDX);
        *err = (TranslateFault){
            .error_code = inout->env->error_code,
            .cr2 = addr,
            .stage2 = S2_GPT,
        };
        return false;
    }
    return true;
}

static inline uint32_t ptw_ldl(const PTETranslate *in, uint64_t ra)
{
    if (likely(in->haddr)) {
        return ldl_p(in->haddr);
    }
    return cpu_ldl_mmuidx_ra(in->env, in->gaddr, in->ptw_idx, ra);
}

static inline uint64_t ptw_ldq(const PTETranslate *in, uint64_t ra)
{
    if (likely(in->haddr)) {
        return ldq_p(in->haddr);
    }
    return cpu_ldq_mmuidx_ra(in->env, in->gaddr, in->ptw_idx, ra);
}

/*
 * Note that we can use a 32-bit cmpxchg for all page table entries,
 * even 64-bit ones, because PG_PRESENT_MASK, PG_ACCESSED_MASK and
 * PG_DIRTY_MASK are all in the low 32 bits.
 */
static bool ptw_setl_slow(const PTETranslate *in, uint32_t old, uint32_t new)
{
    uint32_t cmp;

    CPUState *cpu = env_cpu(in->env);
    /* We are in cpu_exec, and start_exclusive can't be called directly.*/
    g_assert(cpu->running);
    cpu_exec_end(cpu);
    /* Does x86 really perform a rmw cycle on mmio for ptw? */
    start_exclusive();
    cmp = cpu_ldl_mmuidx_ra(in->env, in->gaddr, in->ptw_idx, 0);
    if (cmp == old) {
        cpu_stl_mmuidx_ra(in->env, in->gaddr, new, in->ptw_idx, 0);
    }
    end_exclusive();
    cpu_exec_start(cpu);
    return cmp == old;
}

static inline bool ptw_setl(const PTETranslate *in, uint32_t old, uint32_t set)
{
    if (set & ~old) {
        uint32_t new = old | set;
        if (likely(in->haddr)) {
            old = cpu_to_le32(old);
            new = cpu_to_le32(new);
            return qatomic_cmpxchg((uint32_t *)in->haddr, old, new) == old;
        }
        return ptw_setl_slow(in, old, new);
    }
    return true;
}

static bool mmu_translate(CPUX86State *env, const TranslateParams *in,
                          TranslateResult *out, TranslateFault *err,
                          uint64_t ra)
{
    const target_ulong addr = in->addr;
    const int pg_mode = in->pg_mode;
    const bool is_user = is_mmu_index_user(in->mmu_idx);
    const MMUAccessType access_type = in->access_type;
    uint64_t ptep, pte, rsvd_mask;
    PTETranslate pte_trans = {
        .env = env,
        .err = err,
        .ptw_idx = in->ptw_idx,
    };
    hwaddr pte_addr, paddr;
    uint32_t pkr;
    int page_size;
    int error_code;
    int prot;

 restart_all:
    rsvd_mask = ~MAKE_64BIT_MASK(0, env_archcpu(env)->phys_bits);
    rsvd_mask &= PG_ADDRESS_MASK;
    if (!(pg_mode & PG_MODE_NXE)) {
        rsvd_mask |= PG_NX_MASK;
    }

    if (pg_mode & PG_MODE_PAE) {
#ifdef TARGET_X86_64
        if (pg_mode & PG_MODE_LMA) {
            if (pg_mode & PG_MODE_LA57) {
                /*
                 * Page table level 5
                 */
                pte_addr = (in->cr3 & ~0xfff) + (((addr >> 48) & 0x1ff) << 3);
                if (!ptw_translate(&pte_trans, pte_addr)) {
                    return false;
                }
            restart_5:
                pte = ptw_ldq(&pte_trans, ra);
                if (!(pte & PG_PRESENT_MASK)) {
                    goto do_fault;
                }
                if (pte & (rsvd_mask | PG_PSE_MASK)) {
                    goto do_fault_rsvd;
                }
                if (!ptw_setl(&pte_trans, pte, PG_ACCESSED_MASK)) {
                    goto restart_5;
                }
                ptep = pte ^ PG_NX_MASK;
            } else {
                pte = in->cr3;
                ptep = PG_NX_MASK | PG_USER_MASK | PG_RW_MASK;
            }

            /*
             * Page table level 4
             */
            pte_addr = (pte & PG_ADDRESS_MASK) + (((addr >> 39) & 0x1ff) << 3);
            if (!ptw_translate(&pte_trans, pte_addr)) {
                return false;
            }
        restart_4:
            pte = ptw_ldq(&pte_trans, ra);
            if (!(pte & PG_PRESENT_MASK)) {
                goto do_fault;
            }
            if (pte & (rsvd_mask | PG_PSE_MASK)) {
                goto do_fault_rsvd;
            }
            if (!ptw_setl(&pte_trans, pte, PG_ACCESSED_MASK)) {
                goto restart_4;
            }
            ptep &= pte ^ PG_NX_MASK;

            /*
             * Page table level 3
             */
            pte_addr = (pte & PG_ADDRESS_MASK) + (((addr >> 30) & 0x1ff) << 3);
            if (!ptw_translate(&pte_trans, pte_addr)) {
                return false;
            }
        restart_3_lma:
            pte = ptw_ldq(&pte_trans, ra);
            if (!(pte & PG_PRESENT_MASK)) {
                goto do_fault;
            }
            if (pte & rsvd_mask) {
                goto do_fault_rsvd;
            }
            if (!ptw_setl(&pte_trans, pte, PG_ACCESSED_MASK)) {
                goto restart_3_lma;
            }
            ptep &= pte ^ PG_NX_MASK;
            if (pte & PG_PSE_MASK) {
                /* 1 GB page */
                page_size = 1024 * 1024 * 1024;
                goto do_check_protect;
            }
        } else
#endif
        {
            /*
             * Page table level 3
             */
            pte_addr = (in->cr3 & 0xffffffe0ULL) + ((addr >> 27) & 0x18);
            if (!ptw_translate(&pte_trans, pte_addr)) {
                return false;
            }
            rsvd_mask |= PG_HI_USER_MASK;
        restart_3_nolma:
            pte = ptw_ldq(&pte_trans, ra);
            if (!(pte & PG_PRESENT_MASK)) {
                goto do_fault;
            }
            if (pte & (rsvd_mask | PG_NX_MASK)) {
                goto do_fault_rsvd;
            }
            if (!ptw_setl(&pte_trans, pte, PG_ACCESSED_MASK)) {
                goto restart_3_nolma;
            }
            ptep = PG_NX_MASK | PG_USER_MASK | PG_RW_MASK;
        }

        /*
         * Page table level 2
         */
        pte_addr = (pte & PG_ADDRESS_MASK) + (((addr >> 21) & 0x1ff) << 3);
        if (!ptw_translate(&pte_trans, pte_addr)) {
            return false;
        }
    restart_2_pae:
        pte = ptw_ldq(&pte_trans, ra);
        if (!(pte & PG_PRESENT_MASK)) {
            goto do_fault;
        }
        if (pte & rsvd_mask) {
            goto do_fault_rsvd;
        }
        if (pte & PG_PSE_MASK) {
            /* 2 MB page */
            page_size = 2048 * 1024;
            ptep &= pte ^ PG_NX_MASK;
            goto do_check_protect;
        }
        if (!ptw_setl(&pte_trans, pte, PG_ACCESSED_MASK)) {
            goto restart_2_pae;
        }
        ptep &= pte ^ PG_NX_MASK;

        /*
         * Page table level 1
         */
        pte_addr = (pte & PG_ADDRESS_MASK) + (((addr >> 12) & 0x1ff) << 3);
        if (!ptw_translate(&pte_trans, pte_addr)) {
            return false;
        }
        pte = ptw_ldq(&pte_trans, ra);
        if (!(pte & PG_PRESENT_MASK)) {
            goto do_fault;
        }
        if (pte & rsvd_mask) {
            goto do_fault_rsvd;
        }
        /* combine pde and pte nx, user and rw protections */
        ptep &= pte ^ PG_NX_MASK;
        page_size = 4096;
    } else if (pg_mode & PG_MODE_PG) {
        /*
         * Page table level 2
         */
        pte_addr = (in->cr3 & 0xfffff000ULL) + ((addr >> 20) & 0xffc);
        if (!ptw_translate(&pte_trans, pte_addr)) {
            return false;
        }
    restart_2_nopae:
        pte = ptw_ldl(&pte_trans, ra);
        if (!(pte & PG_PRESENT_MASK)) {
            goto do_fault;
        }
        ptep = pte | PG_NX_MASK;

        /* if PSE bit is set, then we use a 4MB page */
        if ((pte & PG_PSE_MASK) && (pg_mode & PG_MODE_PSE)) {
            page_size = 4096 * 1024;
            /*
             * Bits 20-13 provide bits 39-32 of the address, bit 21 is reserved.
             * Leave bits 20-13 in place for setting accessed/dirty bits below.
             */
            pte = (uint32_t)pte | ((pte & 0x1fe000LL) << (32 - 13));
            rsvd_mask = 0x200000;
            goto do_check_protect_pse36;
        }
        if (!ptw_setl(&pte_trans, pte, PG_ACCESSED_MASK)) {
            goto restart_2_nopae;
        }
#if defined(XBOX)
        out->pde = pte_trans.haddr;
        out->pde_val = pte | PG_ACCESSED_MASK;
#endif

        /*
         * Page table level 1
         */
        pte_addr = (pte & ~0xfffu) + ((addr >> 10) & 0xffc);
        if (!ptw_translate(&pte_trans, pte_addr)) {
            return false;
        }
        pte = ptw_ldl(&pte_trans, ra);
        if (!(pte & PG_PRESENT_MASK)) {
            goto do_fault;
        }
        /* combine pde and pte user and rw protections */
        ptep &= pte | PG_NX_MASK;
        page_size = 4096;
        rsvd_mask = 0;
    } else {
        /*
         * No paging (real mode), let's tentatively resolve the address as 1:1
         * here, but conditionally still perform an NPT walk on it later.
         */
        page_size = 0x40000000;
        paddr = in->addr;
        prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        goto stage2;
    }

do_check_protect:
    rsvd_mask |= (page_size - 1) & PG_ADDRESS_MASK & ~PG_PSE_PAT_MASK;
do_check_protect_pse36:
    if (pte & rsvd_mask) {
        goto do_fault_rsvd;
    }
    ptep ^= PG_NX_MASK;

    /* can the page can be put in the TLB?  prot will tell us */
    if (is_user && !(ptep & PG_USER_MASK)) {
        goto do_fault_protect;
    }

    prot = 0;
    if (!is_mmu_index_smap(in->mmu_idx) || !(ptep & PG_USER_MASK)) {
        prot |= PAGE_READ;
        if ((ptep & PG_RW_MASK) || !(is_user || (pg_mode & PG_MODE_WP))) {
            prot |= PAGE_WRITE;
        }
    }
    if (!(ptep & PG_NX_MASK) &&
        (is_user ||
         !((pg_mode & PG_MODE_SMEP) && (ptep & PG_USER_MASK)))) {
        prot |= PAGE_EXEC;
    }

    if (ptep & PG_USER_MASK) {
        pkr = pg_mode & PG_MODE_PKE ? env->pkru : 0;
    } else {
        pkr = pg_mode & PG_MODE_PKS ? env->pkrs : 0;
    }
    if (pkr) {
        uint32_t pk = (pte & PG_PKRU_MASK) >> PG_PKRU_BIT;
        uint32_t pkr_ad = (pkr >> pk * 2) & 1;
        uint32_t pkr_wd = (pkr >> pk * 2) & 2;
        uint32_t pkr_prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

        if (pkr_ad) {
            pkr_prot &= ~(PAGE_READ | PAGE_WRITE);
        } else if (pkr_wd && (is_user || (pg_mode & PG_MODE_WP))) {
            pkr_prot &= ~PAGE_WRITE;
        }
        if ((pkr_prot & (1 << access_type)) == 0) {
            goto do_fault_pk_protect;
        }
        prot &= pkr_prot;
    }

    if ((prot & (1 << access_type)) == 0) {
        goto do_fault_protect;
    }

    /* yes, it can! */
    {
        uint32_t set = PG_ACCESSED_MASK;
        if (access_type == MMU_DATA_STORE) {
            set |= PG_DIRTY_MASK;
        } else if (!(pte & PG_DIRTY_MASK)) {
            /*
             * Only set write access if already dirty...
             * otherwise wait for dirty access.
             */
            prot &= ~PAGE_WRITE;
        }
        if (!ptw_setl(&pte_trans, pte, set)) {
            /*
             * We can arrive here from any of 3 levels and 2 formats.
             * The only safe thing is to restart the entire lookup.
             */
            goto restart_all;
        }
    }

    /* merge offset within page */
    paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));
#if defined(XBOX)
    out->leaf = pte_trans.haddr;
    out->leaf_val = pte;                /* before the A/D update */
#endif
 stage2:

    /*
     * Note that NPT is walked (for both paging structures and final guest
     * addresses) using the address with the A20 bit set.
     */
    if (in->ptw_idx == MMU_NESTED_IDX) {
        CPUTLBEntryFull *full;
        int flags, nested_page_size;

        flags = probe_access_full_mmu(env, paddr, 0, access_type,
                                      MMU_NESTED_IDX, &pte_trans.haddr, &full);
        if (unlikely(flags & TLB_INVALID_MASK)) {
            *err = (TranslateFault){
                .error_code = env->error_code,
                .cr2 = paddr,
                .stage2 = S2_GPA,
            };
            return false;
        }

        /* Merge stage1 & stage2 protection bits. */
        prot &= full->prot;

        /* Re-verify resulting protection. */
        if ((prot & (1 << access_type)) == 0) {
            goto do_fault_protect;
        }

        /* Merge stage1 & stage2 addresses to final physical address. */
        nested_page_size = 1 << full->lg_page_size;
        paddr = (full->phys_addr & ~(nested_page_size - 1))
              | (paddr & (nested_page_size - 1));

        /*
         * Use the larger of stage1 & stage2 page sizes, so that
         * invalidation works.
         */
        if (nested_page_size > page_size) {
            page_size = nested_page_size;
        }
    }

    out->paddr = paddr & x86_get_a20_mask(env);
    out->prot = prot;
    out->page_size = page_size;
    return true;

 do_fault_rsvd:
    error_code = PG_ERROR_RSVD_MASK;
    goto do_fault_cont;
 do_fault_protect:
    error_code = PG_ERROR_P_MASK;
    goto do_fault_cont;
 do_fault_pk_protect:
    assert(access_type != MMU_INST_FETCH);
    error_code = PG_ERROR_PK_MASK | PG_ERROR_P_MASK;
    goto do_fault_cont;
 do_fault:
    error_code = 0;
 do_fault_cont:
    if (is_user) {
        error_code |= PG_ERROR_U_MASK;
    }
    switch (access_type) {
    case MMU_DATA_LOAD:
        break;
    case MMU_DATA_STORE:
        error_code |= PG_ERROR_W_MASK;
        break;
    case MMU_INST_FETCH:
        if (pg_mode & (PG_MODE_NXE | PG_MODE_SMEP)) {
            error_code |= PG_ERROR_I_D_MASK;
        }
        break;
    }
    *err = (TranslateFault){
        .exception_index = EXCP0E_PAGE,
        .error_code = error_code,
        .cr2 = addr,
    };
    return false;
}

static G_NORETURN void raise_stage2(CPUX86State *env, TranslateFault *err,
                                    uintptr_t retaddr)
{
    uint64_t exit_info_1 = err->error_code;

    switch (err->stage2) {
    case S2_GPT:
        exit_info_1 |= SVM_NPTEXIT_GPT;
        break;
    case S2_GPA:
        exit_info_1 |= SVM_NPTEXIT_GPA;
        break;
    default:
        g_assert_not_reached();
    }

    x86_stq_phys(env_cpu(env),
                 env->vm_vmcb + offsetof(struct vmcb, control.exit_info_2),
                 err->cr2);
    cpu_vmexit(env, SVM_EXIT_NPF, exit_info_1, retaddr);
}

static bool get_physical_address(CPUX86State *env, vaddr addr,
                                 MMUAccessType access_type, int mmu_idx,
                                 TranslateResult *out, TranslateFault *err,
                                 uint64_t ra)
{
    TranslateParams in;
    bool use_stage2 = env->hflags2 & HF2_NPT_MASK;

    in.addr = addr;
    in.access_type = access_type;
#if defined(XBOX)
    out->pde = out->leaf = NULL;
#endif

    switch (mmu_idx) {
    case MMU_PHYS_IDX:
        break;

    case MMU_NESTED_IDX:
        if (likely(use_stage2)) {
            in.cr3 = env->nested_cr3;
            in.pg_mode = env->nested_pg_mode;
            in.mmu_idx =
                env->nested_pg_mode & PG_MODE_LMA ? MMU_USER64_IDX : MMU_USER32_IDX;
            in.ptw_idx = MMU_PHYS_IDX;

            if (!mmu_translate(env, &in, out, err, ra)) {
                err->stage2 = S2_GPA;
                return false;
            }
            return true;
        }
        break;

    default:
        if (is_mmu_index_32(mmu_idx)) {
            addr = (uint32_t)addr;
        }

        if (likely(env->cr[0] & CR0_PG_MASK || use_stage2)) {
            in.cr3 = env->cr[3];
            in.mmu_idx = mmu_idx;
            in.ptw_idx = use_stage2 ? MMU_NESTED_IDX : MMU_PHYS_IDX;
            in.pg_mode = get_pg_mode(env);

            if (in.pg_mode & PG_MODE_LMA) {
                /* test virtual address sign extension */
                int shift = in.pg_mode & PG_MODE_LA57 ? 56 : 47;
                int64_t sext = (int64_t)addr >> shift;
                if (sext != 0 && sext != -1) {
                    *err = (TranslateFault){
                        .exception_index = EXCP0D_GPF,
                        /* non-canonical #GP doesn't change CR2 */
                        .cr2 = env->cr[2],
                    };
                    return false;
                }
            }
            return mmu_translate(env, &in, out, err, ra);
        }
        break;
    }

    /* No translation needed. */
    out->paddr = addr & x86_get_a20_mask(env);
    out->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    out->page_size = TARGET_PAGE_SIZE;
    return true;
}

#if defined(XBOX)
/*
 * xemu: same-value CR3 reloads. The Xbox kernel never sets CR4.PGE, so its
 * only TLB flush is a reload of the CR3 it always holds, which runs several
 * times a second while a level streams, almost always over unchanged page
 * tables, and cost every TLB fill, jump-cache, step-cache and ret-memo
 * entry and cross-page chain it dropped. Each of those was made after the
 * last full flush through a TLB fill of its page (lookups fill code pages),
 * so the log covers every page filled since then: a fill of the 32-bit
 * two-level walk records its PDE in a copy of the page directory and its
 * PTE in a copy of the page table, by virtual page (directory and table
 * slot), as the walk's A/D update left them, with the mmu_idx it filled. A
 * reload compares the copies with the guest's tables through the walk's
 * host pointers and flushes only the logged pages whose PTE changed or was
 * refilled from other bits (tlb_flush_page_by_mmuidx drops their jump-cache,
 * step-cache and ret-memo entries too); the caller severs the cross-page
 * chains only if one of them held TBs. A changed PDE, a fill the log can't
 * record (another paging mode, a table outside RAM), more tables than it
 * holds or too many changed pages leave a full flush; every full flush
 * starts the log over (xemu_i386_tlb_log_reset). XEMU_TLB_REVAL_REFUTE=1
 * checks what a reload keeps against a fresh walk.
 */
#define XEMU_PT_WORDS 1024
#define XEMU_TLB_LOG_TABLES 64

typedef struct XemuPtCopy {
    uint32_t *host;                     /* the guest's table */
    uint32_t words[XEMU_PT_WORDS];      /* as in guest RAM (little-endian) */
    uint64_t logged[XEMU_PT_WORDS / 64];
    uint64_t stale[XEMU_PT_WORDS / 64]; /* refilled from other bits */
    uint8_t idx[XEMU_PT_WORDS];         /* mmu_idx bits filled through it */
} XemuPtCopy;

typedef struct XemuTlbLog {
    bool lost;                          /* a fill it couldn't record */
    unsigned n;                         /* tables in use */
    XemuPtCopy dir;                     /* in use while dir.host is set */
    uint8_t table_of[XEMU_PT_WORDS];    /* directory slot: 1 + table, or 0 */
    uint16_t slot[XEMU_TLB_LOG_TABLES];
    XemuPtCopy *tables[XEMU_TLB_LOG_TABLES];    /* kept across resets */
} XemuTlbLog;

static struct {
    uint64_t reloads, full, lost, changed, pages, sever, refute_checks, ns;
    unsigned tables_max;
} xemu_reval;

static bool xemu_reval_prof_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_TB_PROF");
        on = e && e[0] == '1';
    }
    return on;
}

static bool xemu_reval_refute_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_TLB_REVAL_REFUTE");
        on = e && e[0] == '1';
    }
    return on;
}

static void xemu_reval_dump(void)
{
    fprintf(stderr, "xemu: tlb-reval reloads=%" PRIu64 " kept=%" PRIu64
            " changed=%" PRIu64 " pages=%" PRIu64 " sever=%" PRIu64
            " full=%" PRIu64 " (lost %" PRIu64 ") tables_max=%u mean=%.1fus",
            xemu_reval.reloads, xemu_reval.reloads - xemu_reval.full,
            xemu_reval.changed, xemu_reval.pages, xemu_reval.sever,
            xemu_reval.full, xemu_reval.lost, xemu_reval.tables_max,
            xemu_reval.reloads ? xemu_reval.ns / 1e3 / xemu_reval.reloads : 0);
    if (xemu_reval_refute_on()) {
        fprintf(stderr, " refute checks=%" PRIu64, xemu_reval.refute_checks);
    }
    fprintf(stderr, "\n");
}

static void xemu_pt_start(XemuPtCopy *t, uint32_t *host)
{
    t->host = host;
    memcpy(t->words, host, sizeof(t->words));
    memset(t->logged, 0, sizeof(t->logged));
    memset(t->stale, 0, sizeof(t->stale));
    memset(t->idx, 0, sizeof(t->idx));
}

/*
 * Log word @w of @t as a fill left it. False when the log held other bits
 * for it than the walk's A/D additions: an entry it covers may then come
 * from the old word.
 */
static bool xemu_pt_note(XemuPtCopy *t, unsigned w, uint32_t val, int mmu_idx)
{
    uint32_t old = le32_to_cpu(t->words[w]);
    uint64_t bit = 1ull << (w % 64);
    bool was = t->logged[w / 64] & bit;

    t->words[w] = cpu_to_le32(val);
    t->logged[w / 64] |= bit;
    t->idx[w] |= 1 << mmu_idx;
    return !was ||
           !((old ^ val) & ~(val & (PG_ACCESSED_MASK | PG_DIRTY_MASK)));
}

/* Out of line, so x86_cpu_tlb_fill keeps its PGO profile. */
static void __attribute__((noinline))
xemu_tlb_log_fill(CPUX86State *env, vaddr addr, int mmu_idx,
                  MMUAccessType access_type, const TranslateResult *out)
{
    XemuTlbLog *log = env->xemu_tlb_log;
    unsigned s = (uint32_t)addr >> 22, w = (uint32_t)addr >> 12 & 1023;
    bool large = out->page_size != 4096;
    uint32_t *pde = large ? out->leaf : out->pde;
    uint32_t *pte = large ? NULL : out->leaf;
    /* The walk's A/D update, as mmu_translate made it. */
    uint32_t leaf = out->leaf_val | PG_ACCESSED_MASK |
                    (access_type == MMU_DATA_STORE ? PG_DIRTY_MASK : 0);
    XemuPtCopy *t;

    if (!(env->hflags2 & HF2_NPT_MASK) &&
        (mmu_idx >= MMU_PHYS_IDX || !(env->cr[0] & CR0_PG_MASK))) {
        return;                         /* no page tables */
    }
    if (!log) {
        log = env->xemu_tlb_log = g_new0(XemuTlbLog, 1);
        if (xemu_reval_prof_on() || xemu_reval_refute_on()) {
            atexit(xemu_reval_dump);
        }
    }
    if (log->lost) {
        return;
    }
    if ((env->cr[4] & CR4_PAE_MASK) || (env->hflags2 & HF2_NPT_MASK) ||
        !pde || (!large && !pte)) {
        goto lost;
    }
    if (!log->dir.host) {
        xemu_pt_start(&log->dir, pde - s);
    } else if (log->dir.host != pde - s) {
        goto lost;
    }
    if (!xemu_pt_note(&log->dir, s, large ? leaf : out->pde_val, mmu_idx)) {
        goto lost;
    }
    if (large) {
        return;
    }
    if (log->table_of[s]) {
        t = log->tables[log->table_of[s] - 1];
        if (t->host != pte - w) {
            goto lost;
        }
    } else {
        if (log->n == XEMU_TLB_LOG_TABLES) {
            goto lost;
        }
        if (!log->tables[log->n]) {
            log->tables[log->n] = g_new(XemuPtCopy, 1);
        }
        t = log->tables[log->n];
        xemu_pt_start(t, pte - w);
        log->slot[log->n++] = s;
        log->table_of[s] = log->n;
        xemu_reval.tables_max = MAX(xemu_reval.tables_max, log->n);
    }
    if (!xemu_pt_note(t, w, leaf, mmu_idx)) {
        t->stale[w / 64] |= 1ull << (w % 64);
    }
    return;

lost:
    log->lost = true;
}

void xemu_i386_tlb_log_reset(CPUState *cs)
{
    XemuTlbLog *log = cpu_env(cs)->xemu_tlb_log;

    if (log) {
        for (unsigned i = 0; i < log->n; i++) {
            log->table_of[log->slot[i]] = 0;
        }
        log->n = 0;
        log->dir.host = NULL;
        log->lost = false;
    }
}

/*
 * Whether the guest-physical page @pa holds a TB, which a cross-page chain
 * may have reached through a page that maps it no longer. TBs off RAM are
 * never chained to.
 */
static bool xemu_frame_has_tbs(CPUState *cs, hwaddr pa)
{
    hwaddr xlat, len = TARGET_PAGE_SIZE;
    MemoryRegion *mr;
    ram_addr_t ra;

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(cs->as, pa, &xlat, &len, false,
                                 MEMTXATTRS_UNSPECIFIED);
    if (!memory_region_is_ram(mr) && !memory_region_is_romd(mr)) {
        return false;
    }
    ra = memory_region_get_ram_addr(mr) + xlat;
    if (xemu_tb_page_has_tbs(ra)) {
        return true;
    }
    if (xemu_reval_refute_on()) {
        xemu_reval.refute_checks += xemu_xpage_refute_frame(ra);
    }
    return false;
}

/*
 * Bring @t's copy up to the guest's table. A logged word that changed or
 * is stale leaves the log, and its page (@base + word) is flushed for the
 * mmu_idx it filled while *@budget lasts; past it the scan returns -1.
 * Returns the pages flushed; sets *@sever if one mapped (or may have
 * mapped, when stale) a page with TBs.
 */
static int xemu_pt_reval(CPUState *cs, XemuPtCopy *t, uint32_t base,
                         int *budget, bool *sever)
{
    int n = 0;

    for (unsigned c = 0; c < XEMU_PT_WORDS / 64; c++) {
        uint64_t drop = 0;

        if (!t->stale[c] &&
            !memcmp(&t->words[c * 64], &t->host[c * 64], 64 * 4)) {
            continue;
        }
        for (unsigned j = 0; j < 64; j++) {
            unsigned w = c * 64 + j;
            uint32_t now = qatomic_read(&t->host[w]);
            uint32_t old = le32_to_cpu(t->words[w]);

            if (now == t->words[w] && !(t->stale[c] >> j & 1)) {
                continue;
            }
            t->words[w] = now;
            if (t->logged[c] >> j & 1) {
                if (--*budget < 0) {
                    return -1;
                }
                tlb_flush_page_by_mmuidx(cs, base | w << 12, t->idx[w]);
                *sever = *sever || (t->stale[c] >> j & 1) ||
                         xemu_frame_has_tbs(cs, old & TARGET_PAGE_MASK);
                t->idx[w] = 0;
                drop |= 1ull << j;
                n++;
            }
        }
        t->logged[c] &= ~drop;
        t->stale[c] = 0;
    }
    return n;
}

/* XEMU_TLB_REVAL_REFUTE: is @page logged for @mmu_idx (-1: any)? */
static bool xemu_tlb_log_covers(CPUState *cs, vaddr page, int mmu_idx)
{
    XemuTlbLog *log = cpu_env(cs)->xemu_tlb_log;
    unsigned s = (uint32_t)page >> 22, w = (uint32_t)page >> 12 & 1023;
    uint8_t bits = mmu_idx < 0 ? 0xff : 1 << mmu_idx;
    XemuPtCopy *t;

    if (!log->dir.host || page >> 32 ||
        !(log->dir.logged[s / 64] >> (s % 64) & 1) ||
        !(log->dir.idx[s] & bits)) {
        return false;
    }
    if (!log->table_of[s]) {
        return true;                    /* a 4 MB page */
    }
    t = log->tables[log->table_of[s] - 1];
    return (t->logged[w / 64] >> (w % 64) & 1) && (t->idx[w] & bits);
}

int xemu_tlb_reval(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);
    XemuTlbLog *log = env->xemu_tlb_log;
    int64_t t0 = xemu_reval_prof_on() ? get_clock() : 0;
    bool sever = false;
    int budget = 0, n = 0;

    if (!log) {
        return 0;                       /* nothing walked yet */
    }
    xemu_reval.reloads++;
    if (log->lost || (log->dir.host &&
                      xemu_pt_reval(cs, &log->dir, 0, &budget, &sever) < 0)) {
        goto full;
    }
    /* Past this many pages (~0.4 us each) a full flush and its refills
     * cost less: a streaming reload rarely changes more than 16, a boot
     * frees up to ~1600 at once. */
    budget = 1024;
    for (unsigned i = 0; i < log->n; i++) {
        int r = xemu_pt_reval(cs, log->tables[i],
                              (uint32_t)log->slot[i] << 22, &budget, &sever);
        if (r < 0) {
            goto full;
        }
        n += r;
    }
    xemu_reval.changed += n > 0;
    xemu_reval.pages += n;
    xemu_reval.sever += sever;
    if (xemu_reval_refute_on()) {
        const MMUIdxMap paging = MAKE_64BIT_MASK(0, MMU_PHYS_IDX);

        xemu_reval.refute_checks +=
            tlb_xemu_refute_kept(cs, paging, xemu_tlb_log_covers);
        for (unsigned i = 0; i < XEMU_RETC_SIZE; i++) {
            struct XemuRetcEntry *e = &env->xemu_retc[i];

            xemu_reval.refute_checks += e->tb &&
                tlb_xemu_refute_tb(cs, (uint32_t)(e->cs_base + e->eip),
                                   e->tb, xemu_tlb_log_covers);
        }
    }
    if (t0) {
        xemu_reval.ns += get_clock() - t0;
    }
    return sever;

full:
    xemu_reval.full++;
    xemu_reval.lost += log->lost;
    return -1;
}
#endif

bool x86_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool probe, uintptr_t retaddr)
{
    CPUX86State *env = cpu_env(cs);
    TranslateResult out;
    TranslateFault err;

    if (get_physical_address(env, addr, access_type, mmu_idx, &out, &err,
                             retaddr)) {
        /*
         * Even if 4MB pages, we map only one 4KB page in the cache to
         * avoid filling it too fast.
         */
        assert(out.prot & (1 << access_type));
        tlb_set_page_with_attrs(cs, addr & TARGET_PAGE_MASK,
                                out.paddr & TARGET_PAGE_MASK,
                                cpu_get_mem_attrs(env),
                                out.prot, mmu_idx, out.page_size);
#if defined(XBOX)
        xemu_tlb_log_fill(env, addr, mmu_idx, access_type, &out);
#endif
        return true;
    }

    if (probe) {
        /* This will be used if recursing for stage2 translation. */
        env->error_code = err.error_code;
        return false;
    }

    if (err.stage2 != S2_NONE) {
        raise_stage2(env, &err, retaddr);
    }

    if (env->intercept_exceptions & (1 << err.exception_index)) {
        /* cr2 is not modified in case of exceptions */
        x86_stq_phys(cs, env->vm_vmcb +
                     offsetof(struct vmcb, control.exit_info_2),
                     err.cr2);
    } else {
        env->cr[2] = err.cr2;
    }
    raise_exception_err_ra(env, err.exception_index, err.error_code, retaddr);
}

G_NORETURN void x86_cpu_do_unaligned_access(CPUState *cs, vaddr vaddr,
                                            MMUAccessType access_type,
                                            int mmu_idx, uintptr_t retaddr)
{
    X86CPU *cpu = X86_CPU(cs);
    handle_unaligned_access(&cpu->env, vaddr, access_type, retaddr);
}
