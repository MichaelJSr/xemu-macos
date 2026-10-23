/*
 * Softmmu TLB memo: the translation-time plan (xemu)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.
 *
 * See the COPYING file in the top-level directory for details.
 */

/*
 * In straight-line TB code, a guest access on the page that an earlier
 * access of the same kind and mmu_idx looked up reuses that lookup
 * instead of walking the TLB again. A backend that defines
 * TCG_TARGET_TLB_MEMO (aarch64 and x86_64 hosts) keeps two slots in
 * reserved registers, one for loads and one for stores: ntag = -page
 * (64 bits) and the addend, or the TLB entry's address for stores that
 * re-read addr_write (normal mode); tlbm_st_addend's stores (icount)
 * keep the addend too. A sharing access checks zext(addr) + ntag <=
 * PAGE_SIZE - size (both ends on the page), after rotating the offset
 * right by its alignment bits if it has any, so a misaligned address
 * fails the bound; on a miss it walks the TLB out of line, refilling the
 * slot, and goes back to the access. A slot is used only between its
 * walk and the next label, helper call or TB exit (tcg_tlbm_forget at
 * translation time), and every slow path sets the ntags to a value no
 * zero-extended address matches after its helper call. The backend's
 * comment above its slot registers has the rest.
 *
 * tcg_tlbm_plan decides at translation time, tracking each address as
 * (root value, constant offset) through mov/add/sub: an access shares
 * when it has the root of its slot's fill and lies within half a page
 * of it (on the same page, for constant addresses); otherwise it walks
 * the TLB, and fills the slot only if a later access shares the fill and
 * needs it sooner than the slot's current page. The backend takes one
 * hint per counted access op, in op order (tlbm_take_hint), so every op
 * that looks up the TLB with the memo on is counted here: qemu_ld/st,
 * qemu_ld2/st2, qemu_ld_v128, qemu_ld_fp/qemu_st_fp, and on x86_64 the
 * host x87 ops on the guest page (x87_*_g); their emitters in
 * tcg-op-ldst.c count them in nb_guest_access too.
 */
#define TLBM_SLOTS 2

/* The slots' registers, reserved from the allocator, when the memo is in use. */
static TCGRegSet tlbm_call_regs;

/* Stores keep the addend (icount, latched in tcg_target_init). */
static bool tlbm_st_addend;

enum { TLBM_WALK, TLBM_FILL, TLBM_SHARE };

static void tcg_tlbm_forget(TCGContext *s)
{
    s->tlbm_mmu[0] = s->tlbm_mmu[1] = -1;
}

typedef struct {
    int root;           /* the address's value: -1 for a constant */
    uint32_t off;       /* constant offset from it */
    int seg;            /* straight-line run of ops */
    int8_t slot;        /* 0: loads, 1: stores */
    int8_t mmu;
    uint8_t size;
} TLBMAccess;

/* Temps are indexed from @s, not through the thread-local tcg_ctx. */
static void tlbm_val(TCGContext *s, int *vroot, uint32_t *voff, int *next,
                     TCGArg a, int *r, uint32_t *o)
{
    TCGTemp *ts = arg_temp(a);
    size_t i = ts - s->temps;

    if (ts->kind == TEMP_CONST) {
        *r = -1;
        *o = ts->val;
        return;
    }
    if (vroot[i] == 0) {
        vroot[i] = (*next)++;
        voff[i] = 0;
    }
    *r = vroot[i];
    *o = voff[i];
}

static void tlbm_def(TCGContext *s, int *vroot, uint32_t *voff, TCGArg a,
                     int r, uint32_t o)
{
    size_t i = arg_temp(a) - s->temps;

    vroot[i] = r;
    voff[i] = o;
}

/* Is access @x expected on the page that access @f filled the slot with? */
static bool tlbm_hits(const TLBMAccess *f, const TLBMAccess *x)
{
    if (f->seg != x->seg || f->slot != x->slot || f->mmu != x->mmu
        || f->root != x->root) {
        return false;
    }
    if (x->root == -1) {
        return (x->off & TARGET_PAGE_MASK) == (f->off & TARGET_PAGE_MASK)
            && (x->off & ~TARGET_PAGE_MASK) + x->size <= TARGET_PAGE_SIZE;
    }
    return (uint32_t)(x->off - f->off + TARGET_PAGE_SIZE / 2)
           < TARGET_PAGE_SIZE;
}

/* The first access after @i that a fill by access @f would serve. */
static int tlbm_next_use(const TLBMAccess *acc, int n, int i, int f)
{
    for (int j = i + 1; j < n && acc[j].seg == acc[i].seg; j++) {
        if (tlbm_hits(&acc[f], &acc[j])) {
            return j;
        }
    }
    return n;
}

#ifdef CONFIG_DEBUG_TCG
/* The access ops left in the TB, the ones the planner's walk counts. */
static int tlbm_count_accesses(TCGContext *s)
{
    TCGOp *op;
    int n = 0;

    QTAILQ_FOREACH(op, &s->ops, link) {
        switch (op->opc) {
        case INDEX_op_qemu_ld:
        case INDEX_op_qemu_st:
        case INDEX_op_qemu_ld2:
        case INDEX_op_qemu_st2:
        case INDEX_op_qemu_ld_v128:
        case INDEX_op_qemu_ld_fp:
        case INDEX_op_qemu_st_fp:
#if TCG_TARGET_HAS_x87
        case INDEX_op_x87_ld_g:
        case INDEX_op_x87_arith_g:
        case INDEX_op_x87_cmp_g:
        case INDEX_op_x87_st_g:
#endif
            n++;
            break;
        default:
            break;
        }
    }
    return n;
}
#endif

static void tcg_tlbm_plan(TCGContext *s)
{
    TCGOp *op;
    int n, next = 1, k = 0, seg = 0, cur[TLBM_SLOTS] = { -1, -1 };
    int *vroot;
    uint32_t *voff;
    TLBMAccess *acc;
    bool *used;

    s->tlbm_ihint = s->tlbm_nhint = 0;
    s->tlbm_filled = 0;
    if (!tlbm_call_regs || s->addr_type != TCG_TYPE_I32
        || TARGET_PAGE_BITS > 12) {
        return;
    }
    /*
     * No hints means every access walks (tlbm_take_hint), which is all a
     * TB with fewer than two accesses gets: a lone fill has no sharer
     * (the last pass below). nb_guest_access is an upper bound on the
     * accesses, and nb_ops an exact bound that sizes the arrays, so no
     * walk has to count them first. An undercount only loses sharing,
     * which nothing else would show, so a debug build counts them here,
     * ahead of the return.
     */
#ifdef CONFIG_DEBUG_TCG
    tcg_debug_assert(tlbm_count_accesses(s) <= s->nb_guest_access);
#endif
    if (s->nb_guest_access < 2) {
        return;
    }
    s->tlbm_hint = tcg_malloc(s->nb_ops);
    acc = tcg_malloc(sizeof(*acc) * s->nb_ops);
    vroot = tcg_malloc(sizeof(int) * s->nb_temps);
    voff = tcg_malloc(sizeof(uint32_t) * s->nb_temps);
    memset(vroot, 0, sizeof(int) * s->nb_temps);

    /* Every access's address as (root, offset), and its segment. */
    QTAILQ_FOREACH(op, &s->ops, link) {
        TCGOpcode opc = op->opc;
        int r, r2;
        uint32_t o, o2;

        switch (opc) {
        case INDEX_op_insn_start:
            break;
        case INDEX_op_set_label:
        case INDEX_op_br:
        case INDEX_op_exit_tb:
        case INDEX_op_goto_tb:
        case INDEX_op_goto_ptr:
            seg++;
            break;
        case INDEX_op_call:
            /* The helper may write any global. */
            seg++;
            memset(vroot, 0, sizeof(int) * s->nb_globals);
            for (int i = 0; i < TCGOP_CALLO(op); i++) {
                tlbm_def(s, vroot, voff, op->args[i], next++, 0);
            }
            break;
        case INDEX_op_mov:
            tlbm_val(s, vroot, voff, &next, op->args[1], &r, &o);
            tlbm_def(s, vroot, voff, op->args[0], r, o);
            break;
        case INDEX_op_add:
        case INDEX_op_sub:
            tlbm_val(s, vroot, voff, &next, op->args[1], &r, &o);
            tlbm_val(s, vroot, voff, &next, op->args[2], &r2, &o2);
            if (r2 == -1) {
                o = opc == INDEX_op_add ? o + o2 : o - o2;
            } else if (r == -1 && opc == INDEX_op_add) {
                r = r2;
                o += o2;
            } else {
                r = next++;
                o = 0;
            }
            tlbm_def(s, vroot, voff, op->args[0], r, o);
            break;
        case INDEX_op_qemu_ld:
        case INDEX_op_qemu_st:
        case INDEX_op_qemu_ld2:
        case INDEX_op_qemu_st2:
        case INDEX_op_qemu_ld_v128:
        case INDEX_op_qemu_ld_fp:
        case INDEX_op_qemu_st_fp: {
            /* One data register, or two (the i128 accesses) */
            int nd = opc == INDEX_op_qemu_ld2 || opc == INDEX_op_qemu_st2
                     ? 2 : 1;
            bool is_st = opc == INDEX_op_qemu_st || opc == INDEX_op_qemu_st2 ||
                         opc == INDEX_op_qemu_st_fp;
            MemOpIdx oi = op->args[nd + 1];
            MemOp mop = get_memop(oi);
            TLBMAccess *x = &acc[k++];

            tlbm_val(s, vroot, voff, &next, op->args[nd], &x->root, &x->off);
            x->seg = seg;
            x->slot = is_st;
            x->mmu = get_mmuidx(oi);
            x->size = 1u << (mop & MO_SIZE);
            if (!is_st) {
                for (int i = 0; i < nd; i++) {
                    tlbm_def(s, vroot, voff, op->args[i], next++, 0);
                }
            }
            break;
        }
#if TCG_TARGET_HAS_x87
        case INDEX_op_x87_ld_g:
        case INDEX_op_x87_arith_g:
        case INDEX_op_x87_cmp_g:
        case INDEX_op_x87_st_g: {
            /* [x87_cmp_g's output,] the address, ofs, kind, oi */
            int nd = opc == INDEX_op_x87_cmp_g;
            MemOpIdx oi = op->args[nd + 3];
            TLBMAccess *x = &acc[k++];

            tlbm_val(s, vroot, voff, &next, op->args[nd], &x->root, &x->off);
            x->seg = seg;
            x->slot = opc == INDEX_op_x87_st_g;
            x->mmu = get_mmuidx(oi);
            x->size = 1u << (get_memop(oi) & MO_SIZE);
            if (nd) {
                tlbm_def(s, vroot, voff, op->args[0], next++, 0);
            }
            break;
        }
#endif
        default:
            for (int i = 0; i < tcg_op_defs[opc].nb_oargs; i++) {
                tlbm_def(s, vroot, voff, op->args[i], next++, 0);
            }
            break;
        }
    }
    tcg_debug_assert(k <= s->nb_guest_access);
    n = k;
    s->tlbm_nhint = n;
    used = tcg_malloc(n);
    memset(used, 0, n);

    /* Share, fill or plain walk; a fill replaces the page needed later. */
    for (int i = 0; i < n; i++) {
        const TLBMAccess *x = &acc[i];
        int c = cur[x->slot];

        s->tlbm_hint[i] = TLBM_WALK;
        if (c >= 0 && tlbm_hits(&acc[c], x)) {
            s->tlbm_hint[i] = TLBM_SHARE;
            used[c] = true;
        } else if (c < 0 || acc[c].seg != x->seg
                   || tlbm_next_use(acc, n, i, i)
                      <= tlbm_next_use(acc, n, i, c)) {
            s->tlbm_hint[i] = TLBM_FILL;
            cur[x->slot] = i;
        }
    }
    /* A fill that nothing shares is a plain walk. */
    for (int i = 0; i < n; i++) {
        if (s->tlbm_hint[i] == TLBM_FILL && !used[i]) {
            s->tlbm_hint[i] = TLBM_WALK;
        }
    }
}

static int tlbm_take_hint(TCGContext *s)
{
    return s->tlbm_ihint < s->tlbm_nhint ? s->tlbm_hint[s->tlbm_ihint++]
                                         : TLBM_WALK;
}
