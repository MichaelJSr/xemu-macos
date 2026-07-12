/*
 * xemu: cross-page direct-chaining (Xbox-relaxed) — knobs, Gate-0 taken
 * counters, Gate-1 refuter shadow, R2 remap observer, atexit dump.
 *
 * See docs/xpage-design.md. Default off, zero cost when unset. Single vCPU writer;
 * plain unsynchronized uint64 counters; dump from atexit after the thread has
 * stopped (the fork's XEMU_INV_PROF model). Whole body #if defined(XBOX);
 * declarations in xemu-xpage.h are unconditional (harmless), and every caller
 * is XBOX-gated, so non-XBOX builds compile this to nothing (mirrors
 * xemu-inline-jc.c).
 */
#include "qemu/osdep.h"
#include "xemu-xpage.h"

#if defined(XBOX)
#include "qemu/atomic.h"
#include "exec/target_page.h"
#include "exec/tb-flush.h"
#include "tb-context.h"

/* ---------------- Gate-0 counters (extern, inline-incremented) ------------ */
uint64_t xemu_xpage_taken_kernel;
uint64_t xemu_xpage_taken_low;
uint64_t xemu_xpage_emitted;

/* ---------------- refuter + observer stats -------------------------------- */
static uint64_t xpage_checks;          /* per-exit re-observations of a known target */
static uint64_t xpage_violations;      /* target phys changed since last seen (HAZARD) */
static uint64_t xpage_first_sight;     /* first observation of a target (recorded) */
static uint64_t xpage_unmapped;        /* target had no phys mapping (skipped) */
static uint64_t xpage_obs_installs;    /* observer: first exec-fetch binding for a vpage */
static uint64_t xpage_obs_remaps;      /* observer: exec-fetch binding changed */

/* ---------------- dump arming + wall clock -------------------------------- */
static int64_t xpage_arm_us;
static void xemu_xpage_dump(void);

static void xpage_arm(void)
{
    static bool done;
    if (!done) {
        done = true;
        xpage_arm_us = g_get_monotonic_time();
        atexit(xemu_xpage_dump);
    }
}

/* ---------------- latched knobs ------------------------------------------- */
int xemu_xpage_chain_level(void)
{
    static int lvl = -1;
    if (lvl < 0) {
        /*
         * Default BROAD (2) since 2026-07-12: the generic tlb_flush
         * backstop (xemu_xpage_note_full_flush) closed the last
         * soundness residual (CR4.PGE / whole-TLB flush classes), so
         * broad is sound by construction, and the quiet A/B measured
         * +1.30±0.29 fps on F8 (7/7 pairs). XEMU_XPAGE_CHAIN=0
         * restores upstream same-page-only chaining; =1 keeps the
         * kernel-identity-window tier. The atexit dump is only armed
         * when the env is set explicitly (default path stays silent).
         */
        const char *e = getenv("XEMU_XPAGE_CHAIN");
        if (e) {
            lvl = (e[0] >= '1' && e[0] <= '9') ? (e[0] - '0') : 0;
            xpage_arm();
        } else {
            lvl = 2;
        }
    }
    return lvl;
}

/*
 * Full-flush backstop: any full TLB flush class beyond the INVLPG helper
 * (CR3 reloads, CR4.PGE toggles, mode switches, loadvm restore) may retire
 * a virtual->phys binding some cross-page chain baked in.
 *
 * v0.11 shipped this as a wholesale queued tb_flush — correct, but a
 * retranslation storm: level streaming flushes the TLB continuously, and
 * the movement probe measured 139 tb_flushes in 20 s of first-visit
 * walking (F8, 2026-07-12) with an fps trough to 14.6 vs a 38.6 settled
 * baseline — the reported "new-area lag". The default is now the
 * tb-maint.c link-registry unlink: sever exactly the registered
 * cross-page chains, synchronously (which also closes the old
 * queue-to-safe-point window), keeping every translation. Chains relink
 * lazily on the next execution. XEMU_XPAGE_UNLINK=0 restores the v0.11
 * queued-flush behavior wholesale; registry overflow falls back to it
 * automatically, so correctness never depends on registry capacity.
 * Armed at every chain level that can create cross-page links (>=1; the
 * unlink is a no-op when nothing is registered).
 */
uint64_t xemu_xpage_full_flush_backstops;

bool xemu_xpage_unlink_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_XPAGE_UNLINK");
        on = (e && e[0] == '0') ? 0 : 1;
        if (e) {
            xpage_arm();
        }
    }
    return on;
}

void xemu_xpage_note_full_flush(CPUState *cpu)
{
    if (xemu_xpage_chain_level() >= 1) {
        xemu_xpage_full_flush_backstops++;
        if (!xemu_xpage_unlink_on() || !xemu_xpage_unlink_registered(cpu)) {
            queue_tb_flush(cpu);
        }
    }
}

bool xemu_xpage_refute_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_XPAGE_REFUTE");
        on = (e && e[0] == '1') ? 1 : 0;
        if (on) {
            xpage_arm();
        }
    }
    return on;
}

bool xemu_xpage_prof_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *a = getenv("XEMU_XPAGE_PROF");
        const char *b = getenv("XEMU_INV_PROF");
        on = ((a && a[0] == '1') || (b && b[0] == '1')) ? 1 : 0;
        if (on) {
            xpage_arm();
        }
    }
    return on;
}

bool xemu_xpage_allow(uint64_t dest)
{
    int lvl;

    /* Refute mode observes on the safe (non-chaining) lookup path. */
    if (xemu_xpage_refute_on()) {
        return false;
    }
    lvl = xemu_xpage_chain_level();
    if (lvl <= 0) {
        return false;
    }
    if (lvl >= 2) {
        return true;                          /* broad: any cross-page target */
    }
    /*
     * R1-kernel: identity window only. Mask to the 32-bit linear target (the
     * Xbox guest is always 32-bit) so a jump that wraps the 4 GiB boundary —
     * huge unmasked dest, real target low/remappable — is NOT misclassified
     * as the invariant kernel window. Wrapping jumps never occur on Xbox code
     * layouts, but R1 must be sound by construction, not by likelihood.
     */
    return (dest & 0xffffffffULL) >= XEMU_XPAGE_KERNEL_BASE;
}

/* ---------------- shadow structures (reset on tb_flush) ------------------- */
#define XPAGE_TAB_BITS  16
#define XPAGE_TAB_SIZE  (1u << XPAGE_TAB_BITS)

struct xpage_ent {
    uint64_t vpage;
    uint64_t ppage;
    bool valid;
};

static struct xpage_ent xpage_shadow[XPAGE_TAB_SIZE];   /* refuter: target vpage->phys */
static struct xpage_ent xpage_map[XPAGE_TAB_SIZE];      /* observer: exec vpage->phys */
static uint32_t xpage_last_flush = 0xffffffff;

static inline uint32_t xpage_hash(uint64_t page)
{
    return (uint32_t)((page >> TARGET_PAGE_BITS) * 2654435761u) &
           (XPAGE_TAB_SIZE - 1);
}

static void xpage_maybe_reset(void)
{
    uint32_t f = qatomic_read(&tb_ctx.tb_flush_count);
    if (f != xpage_last_flush) {
        xpage_last_flush = f;
        memset(xpage_shadow, 0, sizeof(xpage_shadow));
        memset(xpage_map, 0, sizeof(xpage_map));
    }
}

void xemu_xpage_refute_note(uint64_t target, uint64_t phys)
{
    uint64_t vp, pp;
    struct xpage_ent *e;

    xpage_maybe_reset();

    if (phys == (uint64_t)-1) {
        xpage_unmapped++;
        return;
    }
    vp = target & TARGET_PAGE_MASK;
    pp = phys & TARGET_PAGE_MASK;
    e = &xpage_shadow[xpage_hash(vp)];

    if (e->valid && e->vpage == vp) {
        xpage_checks++;
        if (e->ppage != pp) {
            xpage_violations++;
            fprintf(stderr,
                    "xemu: XPAGE REFUTE VIOLATION target=0x%016llx "
                    "old_phys=0x%016llx new_phys=0x%016llx "
                    "(a cross-page chain to this target would run STALE code)\n",
                    (unsigned long long)target,
                    (unsigned long long)e->ppage,
                    (unsigned long long)pp);
            e->ppage = pp;
        }
    } else {
        xpage_first_sight++;
        e->valid = true;
        e->vpage = vp;
        e->ppage = pp;
    }
}

void xemu_xpage_observe_mapping(uint64_t vpage, uint64_t ppage, bool exec_fetch)
{
    uint64_t vp, pp;
    struct xpage_ent *e;

    if (!exec_fetch) {
        return;
    }
    xpage_maybe_reset();
    vp = vpage & TARGET_PAGE_MASK;
    pp = ppage & TARGET_PAGE_MASK;
    e = &xpage_map[xpage_hash(vp)];

    if (e->valid && e->vpage == vp) {
        if (e->ppage != pp) {
            xpage_obs_remaps++;
            fprintf(stderr,
                    "xemu: XPAGE observe: exec-page remap vaddr=0x%016llx "
                    "0x%016llx -> 0x%016llx\n",
                    (unsigned long long)vp,
                    (unsigned long long)e->ppage,
                    (unsigned long long)pp);
            e->ppage = pp;
        }
    } else {
        xpage_obs_installs++;
        e->valid = true;
        e->vpage = vp;
        e->ppage = pp;
    }
}

static void xemu_xpage_dump(void)
{
    int64_t now = g_get_monotonic_time();
    double secs = (now - xpage_arm_us) / 1.0e6;
    uint64_t taken = xemu_xpage_taken_kernel + xemu_xpage_taken_low;
    double per5 = secs > 0 ? (double)taken / secs * 5.0 : 0.0;
    double per5k = secs > 0 ? (double)xemu_xpage_taken_kernel / secs * 5.0 : 0.0;

    fprintf(stderr,
            "xemu: XPAGE summary (elapsed=%.1fs chain_level=%d refute=%d "
            "tb_flush=%u)\n",
            secs, xemu_xpage_chain_level(), xemu_xpage_refute_on() ? 1 : 0,
            qatomic_read(&tb_ctx.tb_flush_count));
    fprintf(stderr,
            "xemu:  (0) taken cross-page-direct exits=%llu "
            "(kernel=%llu low=%llu) sites-emitted=%llu "
            "full-flush-backstops=%llu\n",
            (unsigned long long)taken,
            (unsigned long long)xemu_xpage_taken_kernel,
            (unsigned long long)xemu_xpage_taken_low,
            (unsigned long long)xemu_xpage_emitted,
            (unsigned long long)xemu_xpage_full_flush_backstops);
    fprintf(stderr,
            "xemu:      backstop unlink=%d: events=%llu dests-severed=%llu "
            "reg-overflows=%llu reg-phys-only=%llu (overflow/unlink-off "
            "fall back to queued tb_flush)\n",
            xemu_xpage_unlink_on() ? 1 : 0,
            (unsigned long long)xemu_xpage_unlink_events,
            (unsigned long long)xemu_xpage_unlink_dests,
            (unsigned long long)xemu_xpage_reg_overflows,
            (unsigned long long)xemu_xpage_reg_phys_only);
    fprintf(stderr,
            "xemu:      unlink walk cost: total=%lluus max=%lluus "
            "mean=%.0fus (vCPU-thread stall per backstop event)\n",
            (unsigned long long)xemu_xpage_unlink_total_us,
            (unsigned long long)xemu_xpage_unlink_max_us,
            xemu_xpage_unlink_events ?
                (double)xemu_xpage_unlink_total_us /
                (double)xemu_xpage_unlink_events : 0.0);
    fprintf(stderr,
            "xemu:      approx per-5s: total=%.0f kernel=%.0f "
            "(kill threshold X=10,000,000/5s; contended runs undercount)\n",
            per5, per5k);
    fprintf(stderr,
            "xemu:  (1) refuter checks=%llu VIOLATIONS=%llu "
            "first-sight=%llu unmapped-skipped=%llu\n",
            (unsigned long long)xpage_checks,
            (unsigned long long)xpage_violations,
            (unsigned long long)xpage_first_sight,
            (unsigned long long)xpage_unmapped);
    fprintf(stderr,
            "xemu:  (2) observer exec-installs=%llu exec-remaps=%llu\n",
            (unsigned long long)xpage_obs_installs,
            (unsigned long long)xpage_obs_remaps);
}

#endif /* XBOX */
