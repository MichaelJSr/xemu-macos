/*
 * TranslationBlock internal declarations (target specific)
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_TB_INTERNAL_TARGET_H
#define ACCEL_TCG_TB_INTERNAL_TARGET_H

#include "exec/translation-block.h"
#include "exec/target_page.h"

/*
 * The true return address will often point to a host insn that is part of
 * the next translated guest insn.  Adjust the address backward to point to
 * the middle of the call insn.  Subtracting one would do the job except for
 * several compressed mode architectures (arm, mips) which set the low bit
 * to indicate the compressed mode; subtracting two works around that.  It
 * is also the case that there are no host isas that contain a call insn
 * smaller than 4 bytes, so we don't worry about special-casing this.
 */
#define GETPC_ADJ   2

void tb_lock_page0(tb_page_addr_t);

#ifdef CONFIG_USER_ONLY
/*
 * For user-only, page_protect sets the page read-only.
 * Since most execution is already on read-only pages, and we'd need to
 * account for other TBs on the same page, defer undoing any page protection
 * until we receive the write fault.
 */
static inline void tb_lock_page1(tb_page_addr_t p0, tb_page_addr_t p1)
{
    tb_lock_page0(p1);
}

static inline void tb_unlock_page1(tb_page_addr_t p0, tb_page_addr_t p1) { }
static inline void tb_unlock_pages(TranslationBlock *tb) { }
#else
void tb_lock_page1(tb_page_addr_t, tb_page_addr_t);
void tb_unlock_page1(tb_page_addr_t, tb_page_addr_t);
void tb_unlock_pages(TranslationBlock *);
#endif

#ifdef CONFIG_SOFTMMU
void tb_invalidate_phys_range_fast(CPUState *cpu, ram_addr_t ram_addr,
                                   unsigned size, uintptr_t retaddr);
#endif /* CONFIG_SOFTMMU */

bool tb_invalidate_phys_page_unwind(CPUState *cpu, tb_page_addr_t addr,
                                    uintptr_t pc);

#ifdef XBOX
/* Forget a pending icount step (cpu-exec.c); a load keeps the TBs. */
void xemu_istep_disarm(void);
void xemu_flush_jmp_cache(CPUState *cpu, bool steps);
/* TBs keep cpu->neg.can_do_io only under replay (translator.c). */
bool xemu_elide_can_do_io(void);
/* Under icount: may the access returning to @retaddr do I/O now? */
bool xemu_io_ra_in_last_insn(uintptr_t retaddr);
/* Either mode: false when io_prepare() will cpu_io_recompile() it. */
bool xemu_io_ok(CPUState *cpu, uintptr_t retaddr);

/*
 * Is @src's direct jump into @dest a cross-page chain, which tb_add_jump
 * registers for the xpage backstop? Yes when @src has a cross-page goto_tb
 * slot (an emitter registers every link) or @dest is off the physical page
 * of @src's first byte (page_addr[0] holds the full physical pc).
 */
static inline bool xemu_xpage_jump_cross(const TranslationBlock *src,
                                         const TranslationBlock *dest)
{
    return (src->xemu_xpage_reg & XEMU_XPAGE_TB_CROSS_EMITTER) ||
           ((tb_page_addr0(src) ^ tb_page_addr0(dest)) & TARGET_PAGE_MASK);
}
#endif

#endif
