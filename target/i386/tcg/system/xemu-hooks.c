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
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "system/runstate.h"
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

typedef struct XemuHookTable {
    struct rcu_head rcu;
    uint32_t nsites;
    XemuHookSite *sites;
    XemuHookEntry *entries;
} XemuHookTable;

static XemuHookTable *hook_table;
unsigned xemu_hooks_nsites;

/* vCPU thread only. */
static uint64_t hook_calls;
static XemuHookStop hook_stop;

/*
 * The arrival a stop interrupted: on resume its instruction runs again as
 * arrival @call (the vCPU's next; under icount nothing comes between),
 * where the hooks up to the stopper's (@prio, @id) in run order already
 * ran.
 */
static struct {
    uint64_t call;
    vaddr pc;
    int prio, id;
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

bool xemu_hooks_site_at_slow(vaddr pc)
{
    XemuHookTable *t = qatomic_rcu_read(&hook_table);
    return t && site_find(t, pc);
}

static void table_free(XemuHookTable *t)
{
    g_free(t->sites);
    g_free(t->entries);
    g_free(t);
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

/*
 * Invalidate, as xemu_gdb_bp_invalidate does for a gdb breakpoint, the
 * TBs that may hold a hooked site's code without its call: those on the
 * page its PC maps to now, unless done since the site was created (this
 * fork invalidates page-wide), or all TBs when it is unmapped, as an
 * earlier mapping may have left some. Run with the vCPU stopped, before
 * the VM starts: a TB translated concurrently from the old table could
 * otherwise land after the invalidation.
 */
static void sites_cover(void)
{
    CPUState *cpu = first_cpu;
    bool flush = false;

    if (!cpu) {
        return;
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
                xemu_tb_invalidate_page_norecycle(ra);
                g_array_append_val(s->pages, ra);
            }
            break;
        }
    }
    if (flush) {
        queue_tb_flush(cpu);
    }
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
        sites_cover();
    } else {
        sites_purge(sites->len > XEMU_HOOK_MAX_SITES);
    }
}

static void hooks_init(void)
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
    hooks_init();
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
    table_publish();
    /* A running VM gets its old TBs dropped at its next start. */
    if (stopped) {
        sites_cover();
    }
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

bool xemu_hooks_stop(XemuHookCtx *ctx)
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
    hook_resume.call = ctx->call + 1;
    hook_resume.pc = ctx->pc;
    hook_resume.prio = ctx->prio;
    hook_resume.id = ctx->id;
    /* cpu_stop_current() set cs->stop; leave before anything else runs. */
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
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

        if (unlikely(resume) &&
            (e->prio < hook_resume.prio ||
             (e->prio == hook_resume.prio && e->id <= hook_resume.id))) {
            continue;
        }
        ctx.prio = e->prio;
        ctx.id = e->id;
        e->fn(&ctx, e->opaque);
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

bool xemu_hook_expr_reads_memory(const XemuHookExpr *e)
{
    for (int i = 0; i < e->nops; i++) {
        if (e->op[i].deref) {
            return true;
        }
    }
    return false;
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
