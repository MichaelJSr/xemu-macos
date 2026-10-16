/*
 * xemu guest PC hooks: the engine (see tcg/xemu-hooks.h).
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "gdbstub/xemu.h"
#include "tcg/xemu-hooks.h"

/* Main thread (BQL): every hook, and every site. */
typedef struct HookRec {
    int id;
    int prio;
    vaddr pc;
    XemuHookFn fn;
    void *opaque;
} HookRec;

/*
 * A TB holds a site's call when it was translated after the site was
 * created, so a site needs only the TBs from before invalidated, page by
 * page as its PC maps to them: @pages lists the pages done (the PC may
 * map elsewhere in another loaded state), @all says a full flush did
 * them all.
 */
typedef struct SiteRec {
    vaddr pc;
    bool all;
    GArray *pages;              /* ram_addr_t */
} SiteRec;

static GArray *hook_recs;       /* HookRec */
static GArray *sites;           /* SiteRec, ascending pc */
static int hook_next_id = 1;

/* Published to the vCPU (RCU). */
typedef struct XemuHookEntry {
    XemuHookFn fn;
    void *opaque;
    int prio, id;
} XemuHookEntry;

typedef struct XemuHookSite {
    vaddr pc;
    uint32_t n;
    uint32_t idle;              /* arrivals with no hook (vCPU writes) */
    XemuHookEntry *e;
} XemuHookSite;

/*
 * @phys: the RAM addresses the sites' PCs have mapped to (SiteRec pages),
 * ascending. i386 TBs are CF_PCREL, so a TB translated at another mapping
 * of a site's page also runs at the site's PC: the translator cuts and
 * calls at those addresses too, and the call reads the running PC.
 */
typedef struct XemuHookTable {
    struct rcu_head rcu;
    uint32_t nsites;
    XemuHookSite *sites;
    XemuHookEntry *entries;
    uint32_t nphys;
    ram_addr_t *phys;
} XemuHookTable;

static XemuHookTable *hook_table;
unsigned xemu_hooks_nsites;

/* vCPU thread only. */
static uint64_t hook_calls;
static XemuHookStop hook_stop;

/*
 * The arrival a stop (or a re-execution) interrupted: on resume its
 * instruction runs again as arrival @call (the vCPU's next; under icount
 * nothing comes between), where the hooks up to the stopper's (@prio,
 * @id) in run order already ran; with @rerun the stopper runs once more.
 * Arrival numbers start at 1, so @call 0 resumes nothing.
 */
static struct {
    uint64_t call;
    vaddr pc;
    int prio, id;
    bool rerun;
} hook_resume;

/* The Xbox RAM block, for hook reads (looked up on the main thread). */
static const uint8_t *hook_ram;
static uint64_t hook_ram_size;

static XemuHookSite *site_find(XemuHookTable *t, vaddr pc)
{
    uint32_t lo = 0, hi = t->nsites;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (t->sites[mid].pc < pc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < t->nsites && t->sites[lo].pc == pc ? &t->sites[lo] : NULL;
}

static bool phys_find(XemuHookTable *t, ram_addr_t ra)
{
    uint32_t lo = 0, hi = t->nphys;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (t->phys[mid] < ra) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < t->nphys && t->phys[lo] == ra;
}

bool xemu_hooks_site_at_slow(vaddr pc, uint64_t phys)
{
    XemuHookTable *t = qatomic_rcu_read(&hook_table);
    return t && (site_find(t, pc) ||
                 (phys != (uint64_t)-1 && phys_find(t, phys)));
}

static void table_free(XemuHookTable *t)
{
    g_free(t->sites);
    g_free(t->entries);
    g_free(t->phys);
    g_free(t);
}

static gint ram_addr_cmp(gconstpointer a, gconstpointer b)
{
    ram_addr_t x = *(const ram_addr_t *)a, y = *(const ram_addr_t *)b;

    return x < y ? -1 : x > y;
}

static gint rec_cmp(gconstpointer a, gconstpointer b)
{
    const HookRec *x = a, *y = b;

    if (x->pc != y->pc) {
        return x->pc < y->pc ? -1 : 1;
    }
    if (x->prio != y->prio) {
        return x->prio < y->prio ? -1 : 1;
    }
    return x->id < y->id ? -1 : x->id > y->id;
}

static void table_publish(void)
{
    XemuHookTable *old = hook_table;
    XemuHookTable *t = g_new0(XemuHookTable, 1);
    uint32_t r = 0;

    g_array_sort(hook_recs, rec_cmp);
    t->nsites = sites->len;
    t->sites = g_new0(XemuHookSite, MAX(t->nsites, 1));
    t->entries = g_new0(XemuHookEntry, MAX(hook_recs->len, 1));
    for (uint32_t i = 0; i < t->nsites; i++) {
        XemuHookSite *s = &t->sites[i];
        XemuHookSite *was;

        s->pc = g_array_index(sites, SiteRec, i).pc;
        s->e = &t->entries[r];
        was = old ? site_find(old, s->pc) : NULL;
        s->idle = was ? qatomic_read(&was->idle) : 0;
        while (r < hook_recs->len &&
               g_array_index(hook_recs, HookRec, r).pc == s->pc) {
            HookRec *h = &g_array_index(hook_recs, HookRec, r);
            t->entries[r].fn = h->fn;
            t->entries[r].opaque = h->opaque;
            t->entries[r].prio = h->prio;
            t->entries[r].id = h->id;
            s->n++;
            r++;
        }
        if (s->n) {
            s->idle = 0;
        }
    }
    assert(r == hook_recs->len);

    GArray *phys = g_array_new(false, false, sizeof(ram_addr_t));
    for (guint i = 0; i < sites->len; i++) {
        SiteRec *sr = &g_array_index(sites, SiteRec, i);
        for (guint j = 0; j < sr->pages->len; j++) {
            ram_addr_t ra = g_array_index(sr->pages, ram_addr_t, j) |
                            (sr->pc & ~TARGET_PAGE_MASK);
            g_array_append_val(phys, ra);
        }
    }
    g_array_sort(phys, ram_addr_cmp);
    t->nphys = phys->len;
    t->phys = (ram_addr_t *)g_array_free(phys, false);

    if (t->nsites) {
        qatomic_rcu_set(&hook_table, t);
        qatomic_set(&xemu_hooks_nsites, t->nsites);
    } else {
        qatomic_set(&xemu_hooks_nsites, 0);
        qatomic_rcu_set(&hook_table, NULL);
        table_free(t);
    }
    if (old) {
        call_rcu(old, table_free, rcu);
    }
}

/*
 * The RAM page @pc maps to now: 1 and *ra, 0 when it maps to no RAM (MMIO
 * code runs from one-shot TBs: nothing cached), -1 when unmapped.
 */
static int pc_page(CPUState *cpu, vaddr pc, ram_addr_t *ra)
{
    MemTxAttrs attrs;
    hwaddr page = cpu_get_phys_page_attrs_debug(cpu, pc & TARGET_PAGE_MASK,
                                                &attrs);
    int ret = 0;

    if (page == -1) {
        return -1;
    }
    WITH_RCU_READ_LOCK_GUARD() {
        AddressSpace *as =
            cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu, attrs));
        hwaddr xlat, len = 1;
        MemoryRegion *mr = address_space_translate(as, page, &xlat, &len,
                                                   false, attrs);
        if (memory_region_is_ram(mr) || memory_region_is_romd(mr)) {
            *ra = (memory_region_get_ram_addr(mr) + xlat) & TARGET_PAGE_MASK;
            ret = 1;
        }
    }
    return ret;
}

static bool site_has_hooks(vaddr pc)
{
    for (guint i = 0; i < hook_recs->len; i++) {
        if (g_array_index(hook_recs, HookRec, i).pc == pc) {
            return true;
        }
    }
    return false;
}

static void cover_page_work(CPUState *cpu, run_on_cpu_data data)
{
    ram_addr_t *ra = data.host_ptr;

    xemu_tb_invalidate_page_norecycle(*ra);
    g_free(ra);
}

/*
 * Invalidate the TBs that may hold a hooked site's code without its call:
 * those on the page its PC maps to now, unless done since the site was
 * created (this fork invalidates page-wide), or all TBs when it is
 * unmapped, as an earlier mapping may have left some. Run with the vCPU
 * stopped, before the VM starts: a TB translated concurrently from the
 * old table could otherwise land after the invalidation. With @running
 * (a gdb breakpoint inserted while the VM runs) the invalidation waits
 * for the vCPU as safe work instead, which no translation straddles.
 * Returns true when it recorded a page, which the table's @phys lists
 * once published again; publish before the vCPU runs.
 */
static bool sites_cover(bool running)
{
    CPUState *cpu = first_cpu;
    bool flush = false, added = false;

    if (!cpu) {
        return false;
    }
    for (guint i = 0; i < sites->len; i++) {
        SiteRec *s = &g_array_index(sites, SiteRec, i);
        ram_addr_t ra;
        guint j;

        if (s->all || !site_has_hooks(s->pc)) {
            continue;
        }
        switch (pc_page(cpu, s->pc, &ra)) {
        case -1:
            s->all = flush = true;
            break;
        case 1:
            for (j = 0; j < s->pages->len; j++) {
                if (g_array_index(s->pages, ram_addr_t, j) == ra) {
                    break;
                }
            }
            if (j == s->pages->len) {
                if (running) {
                    async_safe_run_on_cpu(cpu, cover_page_work,
                                          RUN_ON_CPU_HOST_PTR(
                                              g_memdup2(&ra, sizeof(ra))));
                } else {
                    xemu_tb_invalidate_page_norecycle(ra);
                }
                g_array_append_val(s->pages, ra);
                added = true;
            }
            break;
        }
    }
    if (flush) {
        queue_tb_flush(cpu);
    }
    return added;
}

/* Drop the site at index @i; its TBs are invalidated (only for speed). */
static void site_drop(guint i)
{
    SiteRec *s = &g_array_index(sites, SiteRec, i);
    ram_addr_t ra;

    for (guint j = 0; j < s->pages->len; j++) {
        xemu_tb_invalidate_page_norecycle(g_array_index(s->pages, ram_addr_t,
                                                        j));
    }
    if (s->all && first_cpu && pc_page(first_cpu, s->pc, &ra) == 1) {
        xemu_tb_invalidate_page_norecycle(ra);
    }
    g_array_free(s->pages, true);
    g_array_remove_index(sites, i);
}

/*
 * Drop hookless sites that still run often (with the VM stopped), or all
 * hookless ones when there are too many.
 */
static void sites_purge(bool all)
{
    XemuHookTable *t = hook_table;
    bool any = false;

    for (guint i = 0; i < sites->len;) {
        vaddr pc = g_array_index(sites, SiteRec, i).pc;
        XemuHookSite *s = t ? site_find(t, pc) : NULL;

        if (!site_has_hooks(pc) &&
            (all || (s && qatomic_read(&s->idle) >= XEMU_HOOK_IDLE_PURGE))) {
            site_drop(i);
            any = true;
        } else {
            i++;
        }
    }
    if (any) {
        table_publish();
    }
}

static void hooks_vm_state_change(void *opaque, bool running, RunState state)
{
    if (!sites->len) {
        return;
    }
    if (running) {
        /* The vCPU hasn't resumed yet; a load may have moved the code. */
        if (sites_cover(false)) {
            table_publish();
        }
    } else {
        sites_purge(sites->len > XEMU_HOOK_MAX_SITES);
    }
}

void xemu_hooks_init(void)
{
    if (hook_recs) {
        return;
    }
    hook_recs = g_array_new(false, false, sizeof(HookRec));
    sites = g_array_new(false, false, sizeof(SiteRec));
    qemu_add_vm_change_state_handler(hooks_vm_state_change, NULL);

    RAMBlock *rb = qemu_ram_block_by_name("xbox.ram");
    if (rb) {
        hook_ram = qemu_ram_get_host_addr(rb);
        hook_ram_size = qemu_ram_get_used_length(rb);
    }
}

int xemu_hooks_add(vaddr pc, int prio, XemuHookFn fn, void *opaque)
{
    HookRec h = { .id = hook_next_id++, .prio = prio, .pc = pc,
                  .fn = fn, .opaque = opaque };
    SiteRec s = { .pc = pc };
    guint i;

    assert(bql_locked());
    xemu_hooks_init();
    g_array_append_val(hook_recs, h);
    for (i = 0; i < sites->len; i++) {
        vaddr at = g_array_index(sites, SiteRec, i).pc;
        if (at == pc) {
            return h.id;
        }
        if (at > pc) {
            break;
        }
    }
    s.pages = g_array_new(false, false, sizeof(ram_addr_t));
    g_array_insert_val(sites, i, s);
    return h.id;
}

void xemu_hooks_remove(int id)
{
    assert(bql_locked());
    for (guint i = 0; hook_recs && i < hook_recs->len; i++) {
        if (g_array_index(hook_recs, HookRec, i).id == id) {
            g_array_remove_index(hook_recs, i);
            return;
        }
    }
}

void xemu_hooks_commit(void)
{
    bool stopped = !runstate_is_running();

    assert(bql_locked());
    if (!hook_recs) {
        return;
    }
    if (stopped && sites->len > XEMU_HOOK_MAX_SITES) {
        sites_purge(true);
    }
    /* A running VM gets its old TBs dropped at its next start. */
    if (stopped) {
        sites_cover(false);
    }
    table_publish();
}

uint64_t xemu_hooks_calls(void)
{
    return hook_calls;
}

const XemuHookStop *xemu_hooks_last_stop(void)
{
    return &hook_stop;
}

uint32_t xemu_hook_reg(XemuHookCtx *ctx, int reg)
{
    CPUX86State *env = ctx->env;

    if (reg >= 0 && reg < CPU_NB_REGS32) {
        return env->regs[reg];
    }
    if (reg == XEMU_HREG_EIP) {
        return ctx->pc - env->segs[R_CS].base;
    }
    /* At the top of a TB the lazy flags are in env (cc_op dynamic). */
    return cpu_compute_eflags(env);
}

bool xemu_hook_read(CPUX86State *env, vaddr va, void *buf, unsigned len)
{
    CPUState *cs = env_cpu(env);
    uint8_t *out = buf;

    va = (uint32_t)va;
    while (len) {
        vaddr in_page = va & ~TARGET_PAGE_MASK;
        unsigned chunk = MIN(len, TARGET_PAGE_SIZE - in_page);
        hwaddr pa = cpu_get_phys_page_debug(cs, va - in_page);

        if (pa == -1 || !hook_ram || pa + in_page + chunk > hook_ram_size) {
            return false;
        }
        memcpy(out, hook_ram + pa + in_page, chunk);
        out += chunk;
        va = (uint32_t)(va + chunk);
        len -= chunk;
    }
    return true;
}

static void resume_set(XemuHookCtx *ctx, bool rerun)
{
    hook_resume.call = ctx->call + 1;
    hook_resume.pc = ctx->pc;
    hook_resume.prio = ctx->prio;
    hook_resume.id = ctx->id;
    hook_resume.rerun = rerun;
}

static bool hooks_stop(XemuHookCtx *ctx, bool rerun)
{
    CPUX86State *env = ctx->env;
    CPUState *cs = env_cpu(env);
    bool ok;

    {
        BQL_LOCK_GUARD();
        ok = xemu_frame_advance_hook_stop();
    }
    if (!ok) {
        return false;
    }
    /*
     * EIP, cc_op, the deferred x87 state and (under icount) the
     * instruction count all go back to the hooked instruction, the first
     * of its TB: none of the TB has run.
     */
    cpu_restore_state(cs, ctx->ra);
    hook_stop.pc = ctx->pc;
    for (int i = 0; i < CPU_NB_REGS32; i++) {
        hook_stop.regs[i] = env->regs[i];
    }
    hook_stop.regs[XEMU_HREG_EIP] = env->eip;
    hook_stop.regs[XEMU_HREG_EFLAGS] = cpu_compute_eflags(env);
    resume_set(ctx, rerun);
    /* cpu_stop_current() set cs->stop; leave before anything else runs. */
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

bool xemu_hooks_stop(XemuHookCtx *ctx)
{
    return hooks_stop(ctx, false);
}

bool xemu_hooks_stop_rerun(XemuHookCtx *ctx)
{
    return hooks_stop(ctx, true);
}

/*
 * gdb breakpoints (BP_GDB, gdbstub Z0/Z1) are hook sites: the site's TB
 * calls the hook however it is entered (a lookup, a chain, the jump-cache
 * probe, the return memo), so the rest of the page runs as usual instead
 * of one instruction per TB (CF_BP_PAGE), and only a PC's first
 * breakpoint costs an invalidation. Each PC's hook runs first
 * (XEMU_HOOK_PRIO_GDB) and stops the VM as check_for_breakpoints_slow
 * does at a lookup at that PC (which still fires, before the TB): EIP at
 * the site, none of the TB run, EXCP_DEBUG, so the arrival's other hooks
 * run only when the instruction does. gdb clients insert a breakpoint
 * once per resume, so a PC keeps its hook while any is armed there.
 */
typedef struct GdbBp {
    vaddr pc;
    int id;
    int refs;
} GdbBp;

static GArray *gdb_bps;         /* GdbBp, main thread (BQL) */

static void gdb_bp_hook(XemuHookCtx *ctx, void *opaque)
{
    CPUState *cs = env_cpu(ctx->env);

    /*
     * Single-step overrides breakpoints, as at a lookup: 's', and the
     * Qxemu.stepover step over the breakpoint under the PC, which runs
     * single-stepped. The breakpoint list says whether a z0 since the
     * table was published removed it.
     */
    if (cs->singlestep_enabled ||
        !cpu_breakpoint_test(cs, ctx->pc, BP_GDB)) {
        return;
    }
    /* EIP, cc_op, deferred x87 state and icount back to the site. */
    cpu_restore_state(cs, ctx->ra);
    cs->exception_index = EXCP_DEBUG;
    cpu_loop_exit(cs);
}

static GdbBp *gdb_bp_find(vaddr pc, guint *index)
{
    for (guint i = 0; gdb_bps && i < gdb_bps->len; i++) {
        GdbBp *b = &g_array_index(gdb_bps, GdbBp, i);
        if (b->pc == pc) {
            *index = i;
            return b;
        }
    }
    return NULL;
}

void xemu_gdb_bp_site_insert(vaddr pc)
{
    GdbBp b = { .pc = pc, .refs = 1 };
    guint i;
    GdbBp *old = gdb_bp_find(pc, &i);

    assert(bql_locked());
    if (old) {
        old->refs++;
        return;
    }
    xemu_hooks_init();
    if (!gdb_bps) {
        gdb_bps = g_array_new(false, false, sizeof(GdbBp));
    }
    b.id = xemu_hooks_add(pc, XEMU_HOOK_PRIO_GDB, gdb_bp_hook, NULL);
    g_array_append_val(gdb_bps, b);
    if (runstate_is_running()) {
        /* The safe work runs after this publish: it needs the BQL. */
        sites_cover(true);
        table_publish();
    } else {
        xemu_hooks_commit();
    }
}

static void gdb_bp_drop(guint i)
{
    xemu_hooks_remove(g_array_index(gdb_bps, GdbBp, i).id);
    g_array_remove_index_fast(gdb_bps, i);
}

/* The site stays, hookless, so a later breakpoint there costs nothing. */
void xemu_gdb_bp_site_remove(vaddr pc)
{
    guint i;
    GdbBp *b = gdb_bp_find(pc, &i);

    assert(bql_locked());
    if (!b || --b->refs) {
        return;
    }
    gdb_bp_drop(i);
    xemu_hooks_commit();
}

void xemu_gdb_bp_site_remove_all(void)
{
    assert(bql_locked());
    if (!gdb_bps || !gdb_bps->len) {
        return;
    }
    while (gdb_bps->len) {
        gdb_bp_drop(0);
    }
    xemu_hooks_commit();
}

void xemu_hooks_state_loaded(void)
{
    hook_resume.call = 0;
}

/*
 * A hook wrote on a code page of the TB it runs in, which the write
 * invalidated (its rest, or old code its jumps lead to, would run stale):
 * leave the TB, as a stop does but without stopping, and look the
 * instruction up again, translated afresh or revived with reset jumps.
 * The hooks up to this one already ran for the arrival.
 */
static G_NORETURN void hooks_reexec(XemuHookCtx *ctx)
{
    CPUState *cs = env_cpu(ctx->env);

    cpu_restore_state(cs, ctx->ra);
    resume_set(ctx, false);
    cpu_loop_exit_noexc(cs);
}

/*
 * The RAM a hook may write at @va: @n bytes on one page that the memory
 * API writes directly (guest RAM, not ROM or a device), by CPU 0's page
 * tables now. Fills the access for address_space_write and the RAM
 * address.
 */
static bool hook_ram_span(CPUState *cs, vaddr va, unsigned n,
                          AddressSpace **as, hwaddr *pa, MemTxAttrs *attrs,
                          ram_addr_t *ra)
{
    hwaddr page = cpu_get_phys_page_attrs_debug(cs, va & TARGET_PAGE_MASK,
                                                attrs);
    bool ok = false;

    if (page == -1) {
        return false;
    }
    *pa = page + (va & ~TARGET_PAGE_MASK);
    *as = cpu_get_address_space(cs, cpu_asidx_from_attrs(cs, *attrs));
    WITH_RCU_READ_LOCK_GUARD() {
        hwaddr xlat, l = n;
        MemoryRegion *mr = address_space_translate(*as, *pa, &xlat, &l, true,
                                                   *attrs);
        if (l >= n && memory_region_is_ram(mr) &&
            memory_access_is_direct(mr, true, *attrs)) {
            *ra = memory_region_get_ram_addr(mr) + xlat;
            ok = true;
        }
    }
    return ok;
}

static bool hook_write(XemuHookCtx *ctx, vaddr va, const void *buf,
                       unsigned len, bool write)
{
    CPUState *cs = env_cpu(ctx->env);
    const uint8_t *in = buf;
    TranslationBlock *tb = NULL;

    /* Check every page first: all or nothing. */
    for (int pass = 0; pass < (write ? 2 : 1); pass++) {
        vaddr at = (uint32_t)va;
        unsigned left = len, off = 0;

        while (left) {
            unsigned chunk = MIN(left, TARGET_PAGE_SIZE -
                                       (at & ~TARGET_PAGE_MASK));
            AddressSpace *as = NULL;
            MemTxAttrs attrs = {};
            hwaddr pa = 0;
            ram_addr_t ra = 0;

            if (!hook_ram_span(cs, at, chunk, &as, &pa, &attrs, &ra)) {
                return false;
            }
            if (pass) {
                /*
                 * Is it on a page of the TB this hook runs in? Then the
                 * write drops that TB with the rest of the page (this fork
                 * invalidates page-wide), whatever bytes it covers, and
                 * the TB must not run on: one invalidated before the
                 * target of its direct jump leaves the target's jump list
                 * with the jump in place, so the target's own invalidation
                 * no longer resets it.
                 */
                if (!tb) {
                    tb = tcg_tb_lookup(ctx->ra);
                }
                if (tb && tb_page_addr0(tb) != -1) {
                    ram_addr_t page = ra & TARGET_PAGE_MASK;
                    tb_page_addr_t p1 = tb_page_addr1(tb);

                    if ((tb_page_addr0(tb) & TARGET_PAGE_MASK) == page ||
                        (p1 != -1 && (p1 & TARGET_PAGE_MASK) == page)) {
                        ctx->own_code = true;
                    }
                }
                address_space_write(as, pa, attrs, in + off, chunk);
            }
            at = (uint32_t)(at + chunk);
            off += chunk;
            left -= chunk;
        }
    }
    return true;
}

bool xemu_hook_write(XemuHookCtx *ctx, vaddr va, const void *buf,
                     unsigned len)
{
    return hook_write(ctx, va, buf, len, true);
}

bool xemu_hook_writable(XemuHookCtx *ctx, vaddr va, unsigned len)
{
    return hook_write(ctx, va, NULL, len, false);
}

/*
 * The call the translator puts at the top of a site's TB. @pc_arg is the
 * linear PC the TB was translated for; a CF_PCREL TB can run at another
 * virtual address of the same code, so it reads the PC from env (always
 * current at the top of a PCREL TB) instead.
 */
void helper_xemu_hook(CPUX86State *env, target_ulong pc_arg, uint32_t pcrel)
{
    XemuHookTable *t = qatomic_rcu_read(&hook_table);
    vaddr pc = pc_arg;
    XemuHookSite *s;

    hook_calls++;
    if (pcrel) {
        pc = (env->hflags & HF_CS64_MASK) ? env->eip
             : (uint32_t)(env->segs[R_CS].base + env->eip);
    }
    s = t ? site_find(t, pc) : NULL;
    if (!s) {
        return;
    }
    if (!s->n) {
        qatomic_set(&s->idle, s->idle + 1);
        return;
    }

    XemuHookCtx ctx = { .env = env, .pc = pc, .ra = GETPC(),
                        .call = hook_calls };
    bool resume = hook_resume.call == hook_calls && hook_resume.pc == pc;
    for (uint32_t i = 0; i < s->n; i++) {
        XemuHookEntry *e = &s->e[i];

        ctx.resumed = false;
        if (unlikely(resume)) {
            int c = e->prio != hook_resume.prio
                    ? (e->prio < hook_resume.prio ? -1 : 1)
                    : (e->id < hook_resume.id ? -1 : e->id > hook_resume.id);

            if (c < 0 || (c == 0 && !hook_resume.rerun)) {
                continue;
            }
            ctx.resumed = c == 0;
        }
        ctx.prio = e->prio;
        ctx.id = e->id;
        e->fn(&ctx, e->opaque);
        if (unlikely(ctx.own_code)) {
            hooks_reexec(&ctx);
        }
    }
}

/* Address expressions. */

static const char *const expr_regs[] = {
    [R_EAX] = "eax", [R_ECX] = "ecx", [R_EDX] = "edx", [R_EBX] = "ebx",
    [R_ESP] = "esp", [R_EBP] = "ebp", [R_ESI] = "esi", [R_EDI] = "edi",
    [XEMU_HREG_EIP] = "eip",
};

static const char *expr_skip(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static bool expr_number(const char **pp, uint32_t *out, Error **errp)
{
    const char *end;
    uint64_t v;

    if (qemu_strtou64(*pp, &end, 0, &v) < 0 || end == *pp ||
        v > UINT32_MAX) {
        error_setg(errp, "expected a 32-bit number at '%s'", *pp);
        return false;
    }
    *out = v;
    *pp = end;
    return true;
}

static bool expr_op(XemuHookExpr *e, bool deref, uint32_t add, Error **errp)
{
    if (e->nops >= XEMU_HOOK_EXPR_OPS) {
        error_setg(errp, "expression too long");
        return false;
    }
    e->op[e->nops].deref = deref;
    e->op[e->nops].add = add;
    e->nops++;
    return true;
}

static bool expr_parse(const char **pp, XemuHookExpr *e, int depth,
                       Error **errp)
{
    const char *p = expr_skip(*pp);

    if (*p == '[') {
        if (depth >= XEMU_HOOK_EXPR_DEREFS) {
            error_setg(errp, "at most %d dereferences",
                       XEMU_HOOK_EXPR_DEREFS);
            return false;
        }
        p++;
        if (!expr_parse(&p, e, depth + 1, errp)) {
            return false;
        }
        p = expr_skip(p);
        if (*p != ']') {
            error_setg(errp, "expected ']' at '%s'", p);
            return false;
        }
        p++;
        if (!expr_op(e, true, 0, errp)) {
            return false;
        }
    } else if (g_ascii_isalpha(*p)) {
        size_t n = 0;
        int reg = -1;

        while (g_ascii_isalpha(p[n])) {
            n++;
        }
        for (int i = 0; i < ARRAY_SIZE(expr_regs); i++) {
            if (expr_regs[i] && n == strlen(expr_regs[i]) &&
                !g_ascii_strncasecmp(p, expr_regs[i], n)) {
                reg = i;
            }
        }
        if (reg < 0) {
            error_setg(errp, "unknown register '%.*s'", (int)n, p);
            return false;
        }
        e->base_reg = reg;
        p += n;
    } else if (!expr_number(&p, &e->base, errp)) {
        return false;
    }
    for (;;) {
        uint32_t k;
        char sign;

        p = expr_skip(p);
        if (*p != '+' && *p != '-') {
            break;
        }
        sign = *p;
        p = expr_skip(p + 1);
        if (!expr_number(&p, &k, errp) ||
            !expr_op(e, false, sign == '-' ? -k : k, errp)) {
            return false;
        }
    }
    *pp = p;
    return true;
}

bool xemu_hook_expr_parse(const char *s, XemuHookExpr *e, Error **errp)
{
    const char *p = s;

    memset(e, 0, sizeof(*e));
    e->base_reg = -1;
    if (!expr_parse(&p, e, 0, errp)) {
        return false;
    }
    p = expr_skip(p);
    if (*p) {
        error_setg(errp, "unexpected '%s' in '%s'", p, s);
        return false;
    }
    return true;
}

bool xemu_hook_expr_eval(XemuHookCtx *ctx, const XemuHookExpr *e,
                         uint32_t *out)
{
    uint32_t v = e->base_reg >= 0 ? xemu_hook_reg(ctx, e->base_reg)
                                  : e->base;

    for (int i = 0; i < e->nops; i++) {
        if (e->op[i].deref) {
            uint8_t b[4];

            if (!xemu_hook_read(ctx->env, v, b, 4)) {
                return false;
            }
            v = ldl_le_p(b);
        } else {
            v += e->op[i].add;
        }
    }
    *out = v;
    return true;
}
