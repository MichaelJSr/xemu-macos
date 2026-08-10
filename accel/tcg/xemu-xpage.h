/*
 * xemu: cross-page direct-chaining (Xbox-relaxed) — knobs, Gate-0 taken
 * counters, Gate-1 refuter shadow, and the R2 remap observer.
 *
 * See docs/xpage-design.md (worktree root) for the full soundness argument.
 *
 * Default off, zero cost when unset. The xbox machine runs exactly one vCPU
 * thread, which is the only writer of every counter/shadow here; the exit
 * dump runs from atexit() after that thread has stopped. Counters are plain
 * unsynchronized uint64 (the fork's XEMU_INV_PROF model). Declarations are
 * unconditional (harmless on any target); the definitions and all touching
 * sites are #if defined(XBOX).
 *
 * accel/tcg consumers (translator.c, cputlb.c, xemu-xpage.c) include this
 * header directly. target/i386 consumers (translate.c, xemu_xpage_helper.c)
 * cannot — this dir is not on the target include path — so they re-declare
 * the handful of symbols they use with the fork's function-local extern idiom
 * (mirrors the XEMU_INV_PROF externs in translate.c).
 */
#ifndef XEMU_XPAGE_H
#define XEMU_XPAGE_H

/*
 * Kernel-identity window base: a linear address >= this is the fixed identity
 * alias of physical RAM the Xbox kernel maps at boot and never remaps for a
 * running title (XPAGE-DESIGN §2). Cross-page chains to such targets are
 * sound with no backstop.
 */
#define XEMU_XPAGE_KERNEL_BASE  0x80000000ULL

/* Latched knobs (getenv on first call). */
int  xemu_xpage_chain_level(void);   /* XEMU_XPAGE_CHAIN: 0 off, 1 kernel, 2 broad */
bool xemu_xpage_refute_on(void);     /* XEMU_XPAGE_REFUTE=1 */
bool xemu_xpage_prof_on(void);       /* XEMU_XPAGE_PROF=1 or XEMU_INV_PROF=1 */

/*
 * Translate-time permission for a cross-page direct chain to @dest (a linear
 * address). Callers invoke this only for a dest already known to be
 * cross-page. Returns false whenever refute mode is on, so refute always
 * routes the exit through the observing helper on the safe lookup path.
 */
bool xemu_xpage_allow(uint64_t dest);

/*
 * Full-flush backstop (called from tlb_flush_by_mmuidx and the CR3 helper):
 * sever every registered cross-page chain so full-TLB-flush classes the
 * INVLPG helper doesn't see (CR3 reloads, CR4.PGE toggles, mode switches,
 * loadvm) can't leave a chain pointing at a retired virtual->phys binding.
 * Default mechanism is the tb-maint.c link-registry unlink (keeps every
 * translation; chains relink lazily); XEMU_XPAGE_UNLINK=0 restores the
 * v0.11 wholesale queued tb_flush, and registry overflow falls back to it
 * automatically. No-op when cross-page chaining is off.
 */
void xemu_xpage_note_full_flush(CPUState *cpu);
bool xemu_xpage_unlink_on(void);     /* XEMU_XPAGE_UNLINK, default 1 */
extern uint64_t xemu_xpage_full_flush_backstops;

/* tb-maint.c link registry (see the block comment there). phys_only marks
 * a registration caused by the phys-page comparison with no cross-emitter
 * flag on the source (conservative over-approximation class, counted). */
struct TranslationBlock;
void xemu_xpage_link_note_cross(struct TranslationBlock *dest, bool phys_only);
bool xemu_xpage_unlink_registered(CPUState *cs);
extern uint64_t xemu_xpage_unlink_events;
extern uint64_t xemu_xpage_unlink_dests;
extern uint64_t xemu_xpage_reg_overflows;
extern uint64_t xemu_xpage_reg_phys_only;
extern uint64_t xemu_xpage_unlink_total_us;
extern uint64_t xemu_xpage_unlink_max_us;

/* Gate-0 taken counters, inline-incremented from generated code in prof mode,
 * split by target region so the R1(kernel)-vs-broad opportunity is visible. */
extern uint64_t xemu_xpage_taken_kernel;
extern uint64_t xemu_xpage_taken_low;
extern uint64_t xemu_xpage_emitted;   /* translate-time cross-page-direct sites */

/*
 * Gate-1 refuter: called by the i386 helper on every cross-page-direct exit
 * with the target linear address and the page-table-walked CURRENT physical
 * page (the slow, authoritative resolution — closes the TLB-bypass hole).
 */
void xemu_xpage_refute_note(uint64_t target, uint64_t phys);

/*
 * R2 observer: called for exec-fetch TLB installs (secondary cross-check that
 * logs remaps seen at refill; has the documented bypass hole, so it only
 * corroborates the per-exit refuter, never replaces it).
 */
void xemu_xpage_observe_mapping(uint64_t vpage, uint64_t ppage);

#endif /* XEMU_XPAGE_H */
