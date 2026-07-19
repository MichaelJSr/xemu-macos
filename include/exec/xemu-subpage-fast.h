/*
 * Sub-page arm (b): inline NOTDIRTY store-skip ("subpage-fast").
 *
 * The aarch64 store slow-path stub gains a pre-filter that completes a
 * guest store inline — skipping the ~300 ns notdirty_write round trip —
 * when the slow path is a PROVABLE NO-OP: the TLB mismatch is exactly
 * TLB_NOTDIRTY (no MMIO/watchpoint/bswap/discard/invalid, tag matches,
 * aligned), the store's 64 B sub-block carries no translated code (the
 * arm-(a) bitmap, mirrored flat per RAM page for generated code), and
 * every NOCODE dirty client (VGA/MIGRATION/NV2A/NV2A_TEX) is already
 * dirty for the page so set_dirty_range would set nothing. Any failed
 * condition demotes to the normal helper — correctness by construction.
 *
 * Design memo of record: the 2026-07-18 arm-(b) session notes
 * (docs/roadmap.md item 3 carries the receipt trail).
 *
 * Copyright (c) 2026 xemu-macos contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef XEMU_SUBPAGE_FAST_H
#define XEMU_SUBPAGE_FAST_H

#if defined(XBOX)

#include "exec/hwaddr.h"
#include "exec/memopidx.h"

/*
 * Mode: 0 = off, 1 = inline skip, 2 = refute (decisions cross-checked
 * in C against ground truth; store always via the real helper).
 * Defaults: __APPLE__ 1, elsewhere 0; XEMU_SUBPAGE_FAST=0/1 overrides
 * both ways; XEMU_SUBPAGE_FAST_REFUTE=1 selects mode 2.
 * First >0 call performs lazy init; a missing "xbox.ram" block or
 * allocation failure forces mode 0 (warned once).
 */
int xemu_subpage_fast_mode(void);

/*
 * State read by generated code (addresses baked as literals at
 * translate time; single-vCPU — the translating thread is the storing
 * thread, so plain loads are ordered by program order).
 */
extern uint64_t *xemu_sf_block_map;     /* per-RAM-page code-block words */
extern uintptr_t xemu_sf_ram_host;      /* host base of xbox.ram */
extern uint64_t xemu_sf_ram_pages;      /* xbox.ram size in target pages */
/* Resolved dirty_memory[c]->blocks[0] for the four NOCODE clients. */
#define XEMU_SF_NCLIENTS 4
extern unsigned long *xemu_sf_dirty0[XEMU_SF_NCLIENTS];

/* Counters (bumped from generated code / the refute helper). */
extern uint64_t xemu_sf_seen;           /* pure-NOTDIRTY stub entries */
extern uint64_t xemu_sf_skips;          /* stores completed inline */
#define XEMU_SF_DEMOTE_NREASONS 4
/* 0 = not-pure-NOTDIRTY, 1 = code sub-block set, 2 = a NOCODE client
 * clean (dirty-set needed), 3 = ram_addr outside xbox.ram */
extern uint64_t xemu_sf_demote[XEMU_SF_DEMOTE_NREASONS];
extern uint64_t xemu_sf_refute_total;
extern uint64_t xemu_sf_refute_viol;

/* Mirror maintenance (tb-maint.c call sites). */
void xemu_sf_sync_page(hwaddr page_addr, uint64_t code_blocks);
void xemu_sf_clear_all(void);

/* Ground truth for the refuter, implemented in tb-maint.c (needs
 * PageDesc access): returns true if [paddr, paddr+len) overlaps NO
 * translated-code sub-block per BOTH the bitmap and a live TB scan;
 * *bitmap_says / *live_says report the two verdicts separately. */
bool xemu_sf_ground_truth(hwaddr paddr, unsigned len,
                          bool *bitmap_says, bool *live_says);

/* Refute-mode store helpers (same ABI as helper_st*_mmu; validate the
 * inline decision, count violations, then perform the real store). */
struct CPUArchState;
void xemu_sf_refute_stb(struct CPUArchState *env, uint64_t addr,
                        uint32_t val, MemOpIdx oi, uintptr_t ra);
void xemu_sf_refute_stw(struct CPUArchState *env, uint64_t addr,
                        uint32_t val, MemOpIdx oi, uintptr_t ra);
void xemu_sf_refute_stl(struct CPUArchState *env, uint64_t addr,
                        uint32_t val, MemOpIdx oi, uintptr_t ra);
void xemu_sf_refute_stq(struct CPUArchState *env, uint64_t addr,
                        uint64_t val, MemOpIdx oi, uintptr_t ra);

#endif /* XBOX */
#endif /* XEMU_SUBPAGE_FAST_H */
