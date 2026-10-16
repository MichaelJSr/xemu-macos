/*
 * xemu: cross-page direct-chaining (Xbox-relaxed) — knobs, Gate-0 taken
 * counters, Gate-1 refuter shadow, and the R2 remap observer.
 *
 * See docs/Finished/xpage-design.md for the full soundness argument.
 *
 * Chaining is always on (broad, since 2026-07-12); the counters and shadow cost
 * nothing unless their env vars are set. The xbox machine runs exactly one vCPU
 * thread, which is the only writer of every counter/shadow here; the exit dump
 * runs from atexit() after that thread has stopped. Counters are plain
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
 * Latched knobs (getenv on first call). Every cross-page direct chain is
 * allowed, whatever its target, unless the refuter is on: then
 * translator_use_goto_tb() and gen_jmp_rel() decline them all, so each exit
 * goes through the observing helper on the safe lookup path.
 */
bool xemu_xpage_refute_on(void);     /* XEMU_XPAGE_REFUTE=1 */
bool xemu_xpage_prof_on(void);       /* XEMU_XPAGE_PROF=1 or XEMU_INV_PROF=1 */

/*
 * Full-flush backstop (called from tlb_flush_by_mmuidx and the CR3 helper):
 * sever every registered cross-page chain so full-TLB-flush classes the
 * INVLPG helper doesn't see (CR3 reloads, CR4.PGE toggles, mode switches,
 * loadvm) can't leave a chain pointing at a retired virtual->phys binding.
 * The mechanism is the tb-maint.c link-registry unlink (keeps every
 * translation; chains relink lazily); registry overflow falls back to the
 * v0.11 wholesale queued tb_flush automatically.
 */
void xemu_xpage_note_full_flush(CPUState *cpu);
extern uint64_t xemu_xpage_full_flush_backstops;

/* tb-maint.c link registry (see the block comment there). phys_only marks
 * a registration caused by the phys-page comparison with no cross-emitter
 * flag on the source (a canary class, expected ~0, counted). */
struct TranslationBlock;
void xemu_xpage_link_note_cross(struct TranslationBlock *dest, bool phys_only);
bool xemu_xpage_unlink_registered(CPUState *cs);
extern uint64_t xemu_xpage_unlink_events;
extern uint64_t xemu_xpage_unlink_dests;
extern uint64_t xemu_xpage_reg_overflows;
extern uint64_t xemu_xpage_reg_phys_only;
extern uint64_t xemu_xpage_unlink_total_us;
extern uint64_t xemu_xpage_unlink_max_us;
/*
 * Jumps into the dests the backstop walked (prof mode): same-page by
 * tb_add_jump's predicate (xemu_xpage_jump_cross), from an emitter onto
 * its own physical page, and onto another page; and every link patch.
 */
extern uint64_t xemu_xpage_walk_same;
extern uint64_t xemu_xpage_walk_emitter_same;
extern uint64_t xemu_xpage_walk_cross;
extern uint64_t xemu_xpage_links;
/*
 * Link flushes wait in a queue (flush_idcache_link): TB exits through a
 * slot already linked (a stale fetch) and the patches they flushed, and
 * the patches flushed when cpu_exec returned; the rest went in full
 * queues.
 */
extern uint64_t xemu_xpage_link_stale;
extern uint64_t xemu_xpage_link_stale_flushed;
extern uint64_t xemu_xpage_link_exit_flushed;

/* Gate-0 taken counters, inline-incremented from generated code in prof mode,
 * split by target region: the kernel's identity window (linear >= 0x80000000,
 * never remapped for a running title) vs everything below it. */
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

/* Forget the refuter and observer bindings, as a tb_flush does (loads). */
void xemu_xpage_shadow_reset(void);

#endif /* XEMU_XPAGE_H */
