/*
 * xemu: cross-page direct-chaining (Xbox-relaxed) — Gate-1 refuter helper.
 *
 * helper_xemu_xpage_refute walks the guest page tables the slow way
 * (cpu_get_phys_page_debug) to resolve the CURRENT physical page of a
 * cross-page jump target, and hands it to the accel/tcg shadow, which flags
 * any change since the target was last seen. A change means a real chain to
 * that target would have run stale code. The walk is authoritative regardless
 * of TLB/chain state — it closes the bypass hole a tlb_set_page observer would
 * have (XPAGE-DESIGN §1/§5). #if defined(XBOX), default-inert (emitted only
 * under XEMU_XPAGE_REFUTE=1).
 *
 * The broad-tier (XEMU_XPAGE_CHAIN=2) INVLPG/CR3 backstops live inside the
 * existing helper_flush_page / helper_write_crN (system/misc_helper.c) so they
 * ride the guest's own TLB flush.
 *
 * accel/tcg's xemu-xpage.h is not on the target include path, so the borrowed
 * symbol is declared with the fork's function-local extern idiom.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"

#if defined(XBOX)

/* Borrowed from accel/tcg/xemu-xpage.c (see idiom note above). */
extern void xemu_xpage_refute_note(uint64_t target, uint64_t phys);

void HELPER(xemu_xpage_refute)(CPUX86State *env, uint64_t cs_base)
{
    CPUState *cs = env_cpu(env);
    uint64_t target = cs_base + (uint32_t)env->eip;
    hwaddr phys = cpu_get_phys_page_debug(cs, target & TARGET_PAGE_MASK);

    xemu_xpage_refute_note(target, (uint64_t)phys);
}

#endif /* XBOX */
