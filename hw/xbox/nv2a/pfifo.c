/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "nv2a_int.h"
#include "nsprof.h"
#include "qemu/timer.h"
#include "hw/xbox/host_fp.h"
#if defined(__APPLE__)
#include <pthread.h>
#endif

typedef struct RAMHTEntry {
    uint32_t handle;
    hwaddr instance;
    enum FIFOEngine engine;
    unsigned int channel_id : 5;
    bool valid;
} RAMHTEntry;

static void pfifo_run_pusher(NV2AState *d);
static uint32_t ramht_hash(NV2AState *d, uint32_t handle);
static RAMHTEntry ramht_lookup(NV2AState *d, uint32_t handle);

/* PFIFO - MMIO and DMA FIFO submission to PGRAPH and VPE */
uint64_t pfifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    qemu_mutex_lock(&d->pfifo.lock);

    uint64_t r = 0;
    switch (addr) {
    case NV_PFIFO_INTR_0:
        r = d->pfifo.pending_interrupts;
        break;
    case NV_PFIFO_INTR_EN_0:
        r = d->pfifo.enabled_interrupts;
        break;
    case NV_PFIFO_RUNOUT_STATUS:
        r = NV_PFIFO_RUNOUT_STATUS_LOW_MARK; /* low mark empty */
        break;
    case NV_PFIFO_CACHE1_DMA_CTL:
        /* No pushbuffer address translation is cached, so always valid */
        r = d->pfifo.regs[addr] | NV_PFIFO_CACHE1_DMA_CTL_VALID;
        break;
    default:
        r = d->pfifo.regs[addr];
        break;
    }

    qemu_mutex_unlock(&d->pfifo.lock);

    nv2a_reg_log_read(NV_PFIFO, addr, size, r);
    return r;
}

void pfifo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFIFO, addr, size, val);

    qemu_mutex_lock(&d->pfifo.lock);

    switch (addr) {
    case NV_PFIFO_INTR_0:
        d->pfifo.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_INTR_EN_0:
        d->pfifo.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        d->pfifo.regs[addr] = val;
        break;
    }

    pfifo_guest_kick(d);

    qemu_mutex_unlock(&d->pfifo.lock);

    if (icount_enabled()) {
        pfifo_icount_sync(d);
    }
}

void pfifo_kick(NV2AState *d)
{
    /*
     * Callers must hold d->pfifo.lock across the kick-set + broadcast
     * pair. All call sites in the tree satisfy this (24 as of
     * 2026-10-15, six of them through pfifo_guest_kick — re-verify the
     * full set when adding one): pfifo_write,
     * the pgraph/{gl,vk}/{surface,display,renderer,reports}.c paths,
     * nv2a.c:nv2a_unlock_fifo, pgraph.c:pgraph_write /
     * do_wait_for_renderer_switch, user.c:user_write and
     * pfifo_put_doorbell. The
     * PGRAPH-lockless audit (docs/Finished/pgraph-lockless-audit.md)
     * re-verified
     * this set unchanged: pgraph_write's three kicks (NV_PGRAPH_INTR /
     * INCREMENT / FIFO) now run under BQL-free MMIO dispatch but still
     * hold d->pfifo.lock across the kick, so the discipline is intact.
     * With the invariant held we can use an untimed qemu_cond_wait in the
     * pfifo_thread idle path (below) because the writer's broadcast
     * can no longer slip between the reader's kick-check and its
     * atomic release-wait.
     */
    d->pfifo.fifo_kick = true;
    qatomic_set(&d->pfifo.kick_seq, d->pfifo.kick_seq + 1);
    qemu_cond_broadcast(&d->pfifo.fifo_cond);
}

/*
 * A kick for a guest register write. In deterministic null mode the vCPU
 * pushes inline right after the write (pfifo_icount_sync), so waking the
 * thread would only make it contend for the locks.
 */
void pfifo_guest_kick(NV2AState *d)
{
    if (!qatomic_read(&d->pfifo.inline_pusher)) {
        pfifo_kick(d);
    }
}

/*
 * Normal mode: the guest's DMA_PUT doorbell (its hottest nv2a write)
 * without pfifo.lock. The thread holds that lock across report fence waits
 * (REPORTS_SUBMIT), which used to block the vCPU here for ~8% of its time.
 * The release store publishes the pushbuffer words written before it to
 * the pusher's acquire load of PUT. The doorbell flag and the thread's
 * parked flag form a Dekker pair (full barrier on both sides): either this
 * sees the thread parked and kicks it under the lock as before, or the
 * thread sees the doorbell before it parks and runs the pusher again.
 * Only the first doorbell per park pays for the lock and broadcast.
 */
void pfifo_put_doorbell(NV2AState *d, uint32_t put)
{
    qatomic_store_release(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT], put);
    qatomic_store_release(&d->pfifo.doorbell, true);
    smp_mb();
    /*
     * Inside a flip stall a new PUT gives the thread nothing to do: it
     * would only take pgraph.lock, find the flip incomplete and park
     * again (three of four wakes on heavy-f1). Only the vCPU ends the
     * stall, by moving READ_3D: NV_PGRAPH_INCREMENT kicks, and a direct
     * NV_PGRAPH_SURFACE write (no kick) is the vCPU's own store, which
     * this test reads. The acquire pairs with FLIP_STALL's release, so
     * WRITE_3D is the one the stall compares against. After the barrier
     * above, a stale true can't meet a thread that parked without seeing
     * the doorbell, which stays set: a thread that hasn't parked runs the
     * pusher once more. A PUT equal to GET (a re-rung doorbell on an idle
     * FIFO, as a guest polling a report value might write) still wakes:
     * the FIFO-idle report delivery (process_pending_reports) runs only
     * then. XEMU_PFIFO_STALL_DOORBELL=0 always wakes.
     */
    if (d->pfifo.stall_doorbell &&
        qatomic_load_acquire(&d->pgraph.waiting_for_flip) &&
        put != qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET])) {
        uint32_t s = qatomic_read(&d->pgraph.regs_[NV_PGRAPH_SURFACE]);
        if (GET_MASK(s, NV_PGRAPH_SURFACE_READ_3D) ==
            GET_MASK(s, NV_PGRAPH_SURFACE_WRITE_3D)) {
            return;
        }
    }
    if (qatomic_read(&d->pfifo.parked) &&
        qatomic_xchg(&d->pfifo.parked, false)) {
        qemu_mutex_lock(&d->pfifo.lock);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
    }
}

/*
 * Deterministic mode (icount): guest state that PFIFO changes (DMA_GET,
 * semaphores, notifiers, reports, PGRAPH interrupts) must change at a fixed
 * guest instruction count, not whenever the thread gets to it. So after any
 * guest PFIFO, USER or PGRAPH register write, and before the vCPU runs again
 * after a VM start (the icount sync hook), the vCPU brings PFIFO to rest:
 * out of pushbuffer, or stalled on something only the guest can clear.
 *
 * With the null renderer, which keeps no per-thread state, the vCPU runs the
 * pusher itself (inline_pusher, set by the thread once the renderer is up)
 * and no thread handoff is needed. Otherwise it waits until the thread has
 * acted on every kick so far and parked. Call without d->pfifo.lock.
 */
void pfifo_icount_sync(NV2AState *d)
{
    if (qatomic_read(&d->pfifo.inline_pusher)) {
        /*
         * Nothing to push: the pusher would only read registers. Only the
         * vCPU writes DMA_GET/DMA_PUT in this mode (reset and loads run
         * with it parked).
         */
        if (d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] ==
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]) {
            return;
        }
        /* As on the PFIFO thread, not under the guest's x87 rounding
         * mode, which TCG leaves in the host FPCR (see mcpx_apu_det_tick). */
        XboxHostFp guest_fp;
        xbox_host_fp_enter(&guest_fp);
        /*
         * Hold both locks for the whole run instead of swapping them around
         * every method (pfifo_run_puller): same order as pgraph_write. The
         * method paths that take the BQL drop pfifo.lock first unless the
         * BQL is already held (pgraph_inline_bql_lock).
         */
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (!qatomic_read(&d->pfifo.halt)) {
            d->pfifo.inline_locked = true;
            pfifo_run_pusher(d);
            d->pfifo.inline_locked = false;
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_unlock(&d->pfifo.lock);
        xbox_host_fp_leave(&guest_fp);
        return;
    }

    if (qatomic_load_acquire(&d->pfifo.idle_seq) ==
        qatomic_read(&d->pfifo.kick_seq)) {
        return;
    }

    /* The thread takes the BQL to raise interrupts. */
    bool had_bql = bql_locked();
    if (had_bql) {
        bql_unlock();
    }
    qemu_mutex_lock(&d->pfifo.lock);
    while (d->pfifo.idle_seq != d->pfifo.kick_seq &&
           !qatomic_read(&d->exiting)) {
        qemu_cond_wait(&d->pfifo.fifo_idle_cond, &d->pfifo.lock);
    }
    qemu_mutex_unlock(&d->pfifo.lock);
    if (had_bql) {
        bql_lock();
    }
}

static bool can_fifo_access(NV2AState *d) {
    return qatomic_read(&d->pgraph.regs_[NV_PGRAPH_FIFO]) &
           NV_PGRAPH_FIFO_ACCESS;
}

/* If NV097_FLIP_STALL was executed, check if the flip has completed.
 * This will usually happen in the VSYNC interrupt handler.
 */
static bool is_flip_stall_complete(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    uint32_t s = pgraph_reg_r(pg, NV_PGRAPH_SURFACE);

    NV2A_DPRINTF("flip stall read: %d, write: %d, modulo: %d\n",
        GET_MASK(s, NV_PGRAPH_SURFACE_READ_3D),
        GET_MASK(s, NV_PGRAPH_SURFACE_WRITE_3D),
        GET_MASK(s, NV_PGRAPH_SURFACE_MODULO_3D));

    if (GET_MASK(s, NV_PGRAPH_SURFACE_READ_3D)
        != GET_MASK(s, NV_PGRAPH_SURFACE_WRITE_3D)) {
        return true;
    }

    return false;
}

static bool pfifo_stall_for_flip(NV2AState *d)
{
    bool should_stall = false;

    if (qatomic_read(&d->pgraph.waiting_for_flip)) {
        bool locked = d->pfifo.inline_locked;
        if (!locked) {
            qemu_mutex_lock(&d->pgraph.lock);
        }
        if (!is_flip_stall_complete(d)) {
            should_stall = true;
        } else {
            qatomic_set(&d->pgraph.waiting_for_flip, false);
            nsprof_flip_wait_end();
        }
        if (!locked) {
            qemu_mutex_unlock(&d->pgraph.lock);
        }
    }

    return should_stall;
}

static bool pfifo_puller_should_stall(NV2AState *d)
{
    return pfifo_stall_for_flip(d) ||
           qatomic_read(&d->pgraph.waiting_for_nop) ||
           qatomic_read(&d->pgraph.waiting_for_context_switch) ||
           !can_fifo_access(d);
}

/* inl: the deterministic inline run (pfifo.inline_locked), see the pusher. */
static inline QEMU_ALWAYS_INLINE ssize_t
pfifo_run_puller(NV2AState *d, uint32_t method_entry, uint32_t parameter,
                 uint32_t *parameters, size_t num_words_available,
                 size_t max_lookahead_words, const bool inl)
{
    /*
     * Inline, the pusher has just checked FIFO access and waiting_for_nop
     * and cleared LOW_MARK, and no other thread can change them.
     */
    if (inl ? pfifo_stall_for_flip(d) ||
                  qatomic_read(&d->pgraph.waiting_for_context_switch)
            : pfifo_puller_should_stall(d)) {
        return -1;
    }

    uint32_t *pull0 = &d->pfifo.regs[NV_PFIFO_CACHE1_PULL0];
    uint32_t *pull1 = &d->pfifo.regs[NV_PFIFO_CACHE1_PULL1];
    uint32_t *engine_reg = &d->pfifo.regs[NV_PFIFO_CACHE1_ENGINE];
    uint32_t *status = &d->pfifo.regs[NV_PFIFO_CACHE1_STATUS];
    ssize_t num_proc = -1;
    /* Inline run on the vCPU: pgraph.lock is held throughout. */
    const bool locked = inl;

    // TODO think more about locking

    if (!GET_MASK(*pull0, NV_PFIFO_CACHE1_PULL0_ACCESS) ||
        (!inl && (*status & NV_PFIFO_CACHE1_STATUS_LOW_MARK))) {
        return -1;
    }

    uint32_t method = method_entry & 0x1FFC;
    uint32_t subchannel =
        GET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_SUBCHANNEL);
    bool inc = !GET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_TYPE);

    if (method == 0) {
        RAMHTEntry entry = ramht_lookup(d, parameter);
        assert(entry.valid);
        // assert(entry.channel_id == state->channel_id);
        assert(entry.engine == ENGINE_GRAPHICS);

        /* the engine is bound to the subchannel */
        assert(subchannel < 8);
        SET_MASK(*engine_reg, 3 << (4*subchannel), entry.engine);
        SET_MASK(*pull1, NV_PFIFO_CACHE1_PULL1_ENGINE, entry.engine);

        if (!locked) {
            qemu_mutex_unlock(&d->pfifo.lock);
            qemu_mutex_lock(&d->pgraph.lock);
        }

        if (inl || can_fifo_access(d)) {
            pgraph_context_switch(d, entry.channel_id);
            if (!qatomic_read(&d->pgraph.waiting_for_context_switch)) {
                num_proc =
                    pgraph_method(d, subchannel, 0, entry.instance, parameters,
                                  num_words_available, max_lookahead_words, inc);
            }
        }

        if (!locked) {
            qemu_mutex_unlock(&d->pgraph.lock);
            qemu_mutex_lock(&d->pfifo.lock);
        }

    } else if (method >= 0x100) {
        // method passed to engine

        /* methods that take objects.
         * TODO: Check this range is correct for the nv2a */
        if (method >= 0x180 && method < 0x200) {
            /*
             * ramht_lookup reads pfifo.regs[NV_PFIFO_RAMHT] and pfifo-
             * adjacent channel state; all of this is already guarded
             * by d->pfifo.lock (held here). BQL is not required and
             * was intentionally removed when the BQL-event-batching
             * experiment was reverted (see docs/optimizations.md
             * "Failed / reverted experiments": BQL event batching).
             */
            RAMHTEntry entry = ramht_lookup(d, parameter);
            assert(entry.valid);
            // assert(entry.channel_id == state->channel_id);
            parameter = entry.instance;
        }

        enum FIFOEngine engine = GET_MASK(*engine_reg, 3 << (4*subchannel));
        assert(engine == ENGINE_GRAPHICS);
        SET_MASK(*pull1, NV_PFIFO_CACHE1_PULL1_ENGINE, engine);

        if (!locked) {
            qemu_mutex_unlock(&d->pfifo.lock);
            qemu_mutex_lock(&d->pgraph.lock);
        }

        if (inl || can_fifo_access(d)) {
            num_proc =
                pgraph_method(d, subchannel, method, parameter, parameters,
                              num_words_available, max_lookahead_words, inc);
        }

        if (!locked) {
            qemu_mutex_unlock(&d->pgraph.lock);
            qemu_mutex_lock(&d->pfifo.lock);
        }
    } else {
        assert(!"Unrecognized pfifo puller method");
    }

    if (num_proc > 0) {
        *status |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    }

    return num_proc;
}

/*
 * The deterministic inline run's fused command loop: the data words of one
 * kelvin command from byte offset get on, until the command or the
 * pushbuffer ends or a handler sets a stall flag. The checks and state the
 * per-word path (pfifo_run_puller, pgraph_method) repeats for every word
 * run once per command; per word only the kelvin handler call is left, then
 * one test of the flags a handler can set (waiting_for_nop: NO_OPERATION;
 * waiting_for_flip: FLIP_STALL; waiting_for_context_switch). FIFO access,
 * PULL0, the engine binding, CTX_SWITCH and the DMA error change only by
 * guest MMIO, loads, reset, method 0 and the pusher's header decode, none
 * of which lands mid-command, so the subchannel whose PULL0, engine and
 * class were checked stays checked (*fused_subch) until the per-word path
 * runs a method again or the run ends. DMA_STATE, DCOUNT, DATA_SHADOW and
 * LOW_MARK are written back at the end as the per-word path would have
 * left them. Returns the new GET; get itself, having changed nothing that
 * path would not, when the command needs it: method 0 or an object method
 * (RAMHT lookups), a method range that would wrap, a non-kelvin class, or a
 * flip or context-switch stall pending.
 */
static inline QEMU_ALWAYS_INLINE uint32_t
pfifo_inline_command(NV2AState *d, uint8_t *dma, hwaddr dma_len,
                     uint32_t put, uint32_t get, uint32_t method, bool inc,
                     unsigned int subchannel, uint32_t method_count,
                     int *fused_subch)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t *dma_state = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE];
    /* The methods the command can reach: [method, end) */
    uint32_t end = inc ? method + 4 * method_count : method + 4;
    /* As in the per-word path, this wraps when GET is past PUT. */
    uint32_t avail = (put - get) / 4;
    uint32_t words = MIN(method_count, avail);

    if (method < 0x100 || end > 0x2000 || (method < 0x200 && end > 0x180) ||
        (hwaddr)get + 4 * (hwaddr)words > dma_len ||
        (pg->waiting_for_flip | pg->waiting_for_context_switch)) {
        return get;
    }
    if (unlikely((int)subchannel != *fused_subch)) {
        if (!GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PULL0],
                      NV_PFIFO_CACHE1_PULL0_ACCESS) ||
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_ENGINE],
                     3 << (4 * subchannel)) != ENGINE_GRAPHICS) {
            return get;
        }
        SET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PULL1],
                 NV_PFIFO_CACHE1_PULL1_ENGINE, ENGINE_GRAPHICS);
        if (pgraph_inline_command_class(d, subchannel, method) !=
            NV_KELVIN_PRIMITIVE) {
            return get;
        }
        *fused_subch = subchannel;
    }

    uint32_t *const start = (uint32_t *)(dma + get);
    uint32_t *const limit = start + words;
    uint32_t *p = start;
    uint32_t word;
    do {
        word = ldl_le_p(p);
        size_t n = pgraph_inline_kelvin_method(d, subchannel, method, word, p,
                                               limit - p,
                                               avail - (uint32_t)(p - start),
                                               inc);
        p += n;
        if (inc) {
            method += 4 * n;
        }
    } while (p < limit &&
             !(pg->waiting_for_nop | pg->waiting_for_flip |
               pg->waiting_for_context_switch));

    /* Past the command only by the DRAW_ARRAYS squash, and never past PUT. */
    uint32_t done = p - start;
    d->pfifo.regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW] = word;
    if (inc) {
        SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD, method >> 2);
    }
    SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
             done < method_count ? method_count - done : 0);
    d->pfifo.regs[NV_PFIFO_CACHE1_DMA_DCOUNT] += done;
    d->pfifo.regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    return get + 4 * done;
}

static bool pfifo_pusher_should_stall(NV2AState *d)
{
    return !can_fifo_access(d) ||
           qatomic_read(&d->pgraph.waiting_for_nop);
}

/*
 * inl: the deterministic inline run (pfifo_icount_sync, pfifo.inline_locked).
 * The vCPU is then the only thread that touches PFIFO and PGRAPH state for
 * the whole run: it holds both locks (it drops them only to take the BQL,
 * and no other thread writes this state then either), and no guest access
 * comes in until it returns. So nothing else moves DMA_PUT or reads DMA_GET
 * mid-run, and the release/acquire pair the threaded pusher needs (the
 * lockless doorbell and USER reads) guards nothing. pfifo_run_pusher builds
 * one copy of this loop per mode.
 */
static inline QEMU_ALWAYS_INLINE void pfifo_run_pusher_impl(NV2AState *d,
                                                            const bool inl)
{
    uint32_t *push0 = &d->pfifo.regs[NV_PFIFO_CACHE1_PUSH0];
    uint32_t *push1 = &d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1];
    uint32_t *dma_subroutine = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_SUBROUTINE];
    uint32_t *dma_state = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE];
    uint32_t *dma_push = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUSH];
    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
    uint32_t *dma_dcount = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_DCOUNT];
    uint32_t *status = &d->pfifo.regs[NV_PFIFO_CACHE1_STATUS];

    if (!GET_MASK(*push0, NV_PFIFO_CACHE1_PUSH0_ACCESS) ||
        !GET_MASK(*dma_push, NV_PFIFO_CACHE1_DMA_PUSH_ACCESS) ||
        GET_MASK(*dma_push, NV_PFIFO_CACHE1_DMA_PUSH_STATUS)) {
        return;
    }

    // TODO: should we become busy here??
    // NV_PFIFO_CACHE1_DMA_PUSH_STATE _BUSY

    unsigned int channel_id = GET_MASK(*push1,
                                       NV_PFIFO_CACHE1_PUSH1_CHID);


    /* Channel running DMA mode */
    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];
    assert(channel_modes & (1 << channel_id));

    assert(GET_MASK(*push1, NV_PFIFO_CACHE1_PUSH1_MODE)
            == NV_PFIFO_CACHE1_PUSH1_MODE_DMA);

    /* We're running so there should be no pending errors... */
    assert(GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR)
            == NV_PFIFO_CACHE1_DMA_STATE_ERROR_NONE);

    hwaddr dma_instance =
        GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_INSTANCE],
                 NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS) << 4;

    hwaddr dma_len;
    uint8_t *dma = nv_dma_map(d, dma_instance, &dma_len);

    /* Inline, GET lives in a register and nothing moves PUT. */
    uint32_t get = inl ? *dma_get : 0;
    const uint32_t put = inl ? *dma_put : 0;
    /* Inline: see pfifo_inline_command. */
    int fused_subch = -1;
    if (inl) {
        /* Guest writes and loads may have changed CTX_SWITCH since. */
        d->pgraph.ctx_switch_subch = -1;
    }

    while (!pfifo_pusher_should_stall(d)) {
        uint32_t dma_get_v = inl ? get : *dma_get;
        /* Pairs with the release in pfifo_put_doorbell. */
        uint32_t dma_put_v = inl ? put : qatomic_load_acquire(dma_put);
        if (dma_get_v == dma_put_v) break;
        if (dma_get_v >= dma_len) {
            assert(!"Dma value is out of range in PFIFO pusher");
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                     NV_PFIFO_CACHE1_DMA_STATE_ERROR_PROTECTION);
            break;
        }

        size_t num_words_available = dma_put_v - dma_get_v;
        assert(num_words_available % 4 == 0);
        num_words_available /= 4;

        uint32_t *word_ptr = (uint32_t*)(dma + dma_get_v);
        uint32_t word = ldl_le_p(word_ptr);
        dma_get_v += 4;

        uint32_t method_type =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE);
        uint32_t method_subchannel =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL);
        uint32_t method =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD) << 2;
        uint32_t method_count =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);

        uint32_t subroutine_state =
            GET_MASK(*dma_subroutine, NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE);

        uint32_t fused_get;
        if (method_count && inl &&
            (fused_get = pfifo_inline_command(
                 d, dma, dma_len, put, dma_get_v - 4, method,
                 method_type == NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC,
                 method_subchannel, method_count, &fused_subch)) !=
                dma_get_v - 4) {
            /* The rest of the command, or up to a stall or PUT. */
            dma_get_v = fused_get;
        } else if (method_count) {
            if (inl) {
                /* It may run method 0, which binds a new context. */
                fused_subch = -1;
            }

            /* data word of methods command */
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW] = word;

            assert((method & 3) == 0);
            uint32_t method_entry = 0;
            SET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_ADDRESS, method >> 2);
            SET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_TYPE, method_type);
            SET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_SUBCHANNEL,
                     method_subchannel);

            *status &= ~NV_PFIFO_CACHE1_STATUS_LOW_MARK;

            ssize_t num_words_processed =
                pfifo_run_puller(d, method_entry, word, word_ptr,
                                 MIN(method_count, num_words_available),
                                 num_words_available, inl);
            if (num_words_processed < 0) {
                break;
            }

            dma_get_v += (num_words_processed-1)*4;

            if (method_type == NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC) {
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (method + 4*num_words_processed) >> 2);
            }
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                     method_count - MIN(method_count, num_words_processed));

            (*dma_dcount) += num_words_processed;
        } else {
            /* no command active - this is the first word of a new one */
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_RSVD_SHADOW] = word;

            /* match all forms */
            if ((word & 0xe0000003) == 0x20000000) {
                /* old jump */
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] =
                    dma_get_v;
                dma_get_v = word & 0x1fffffff;
                NV2A_DPRINTF("pb OLD_JMP 0x%x\n", dma_get_v);
            } else if ((word & 3) == 1) {
                /* jump */
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] =
                    dma_get_v;
                dma_get_v = word & 0xfffffffc;
                NV2A_DPRINTF("pb JMP 0x%x\n", dma_get_v);
            } else if ((word & 3) == 2) {
                /* call */
                if (subroutine_state) {
                    SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                             NV_PFIFO_CACHE1_DMA_STATE_ERROR_CALL);
                    break;
                } else {
                    *dma_subroutine = dma_get_v;
                    SET_MASK(*dma_subroutine,
                             NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, 1);
                    dma_get_v = word & 0xfffffffc;
                    NV2A_DPRINTF("pb CALL 0x%x\n", dma_get_v);
                }
            } else if (word == 0x00020000) {
                /* return */
                if (!subroutine_state) {
                    SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                             NV_PFIFO_CACHE1_DMA_STATE_ERROR_RETURN);
                    // break;
                } else {
                    dma_get_v = *dma_subroutine & 0xfffffffc;
                    SET_MASK(*dma_subroutine,
                             NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, 0);
                    NV2A_DPRINTF("pb RET 0x%x\n", dma_get_v);
                }
            } else if ((word & 0xe0030003) == 0) {
                /* increasing methods */
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (word & 0x1fff) >> 2 );
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,
                         (word >> 13) & 7);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                         (word >> 18) & 0x7ff);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,
                         NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC);
                *dma_dcount = 0;
                if (inl && (word >> 18) & 0x7ff && dma_get_v != put) {
                    /* Straight into the data words. */
                    dma_get_v = pfifo_inline_command(
                        d, dma, dma_len, put, dma_get_v, word & 0x1ffc, true,
                        (word >> 13) & 7, (word >> 18) & 0x7ff, &fused_subch);
                }
            } else if ((word & 0xe0030003) == 0x40000000) {
                /* non-increasing methods */
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (word & 0x1fff) >> 2 );
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,
                         (word >> 13) & 7);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                         (word >> 18) & 0x7ff);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,
                         NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_NON_INC);
                *dma_dcount = 0;
                if (inl && (word >> 18) & 0x7ff && dma_get_v != put) {
                    dma_get_v = pfifo_inline_command(
                        d, dma, dma_len, put, dma_get_v, word & 0x1ffc, false,
                        (word >> 13) & 7, (word >> 18) & 0x7ff, &fused_subch);
                }
            } else {
                NV2A_DPRINTF("pb reserved cmd 0x%x - 0x%x\n",
                             dma_get_v, word);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                         NV_PFIFO_CACHE1_DMA_STATE_ERROR_RESERVED_CMD);
                // break;
                assert(!"Reserved pb command - invalid GPU command. Check logs for more info");
            }
        }

        if (inl) {
            *dma_get = get = dma_get_v;
        } else {
            /* The words before GET are consumed (lockless user_read). */
            qatomic_store_release(dma_get, dma_get_v);
        }

        if (GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR)) {
            break;
        }
    }

    // NV2A_DPRINTF("DMA pusher done: max 0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx " - 0x%" HWADDR_PRIx "\n",
    //      dma_len, control->dma_get, control->dma_put);

    uint32_t error = GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR);
    if (error) {
        NV2A_DPRINTF("pb error: %d\n", error);
        assert(!"Error reported in PFIFO dma state - check logs for more info");

        SET_MASK(*dma_push, NV_PFIFO_CACHE1_DMA_PUSH_STATUS, 1); /* suspended */

        // d->pfifo.pending_interrupts |= NV_PFIFO_INTR_0_DMA_PUSHER;
        // nv2a_update_irq(d);
    }
}

static void pfifo_run_pusher(NV2AState *d)
{
    if (d->pfifo.inline_locked) {
        pfifo_run_pusher_impl(d, true);
    } else {
        pfifo_run_pusher_impl(d, false);
    }
}

void *pfifo_thread(void *arg)
{
    NV2AState *d = (NV2AState *)arg;

#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    pgraph_init_thread(d);

    rcu_register_thread();

    qemu_mutex_lock(&d->pfifo.lock);
    if (icount_enabled() &&
        d->pgraph.renderer->type == CONFIG_DISPLAY_RENDERER_NULL) {
        qatomic_set(&d->pfifo.inline_pusher, true);
    }
    qemu_mutex_unlock(&d->pfifo.lock);

    /*
     * Diagnostic: rate-limited heartbeat + pending-flag snapshot.
     * Only active when XEMU_PFIFO_HEARTBEAT=1. Prints every ~2 s,
     * shows per-flag state so a freeze report can be categorized as
     * "pfifo thread is alive but game is waiting on X" vs "pfifo
     * thread is stuck" (if the heartbeat stops).
     */
    bool pfifo_heartbeat = false;
    {
        const char *s = getenv("XEMU_PFIFO_HEARTBEAT");
        pfifo_heartbeat = (s && s[0] == '1');
    }
    int64_t pfifo_last_heartbeat_ns = 0;
    uint64_t pfifo_loop_iters = 0;

    qemu_mutex_lock(&d->pfifo.lock);
    while (true) {
        d->pfifo.fifo_kick = false;
        /* Before the pusher reads PUT (pfifo_put_doorbell). */
        qatomic_xchg(&d->pfifo.doorbell, false);

        pgraph_process_pending(d);

        /*
         * All writers (nv2a.c, {vk,gl}/surface.c) use qatomic_set on halt;
         * the heartbeat reader below uses qatomic_read. Match the contract
         * on this hot-path read even though d->pfifo.lock is held here,
         * so TSAN is quiet and future refactors that drop the lock don't
         * silently tear.
         */
        if (!qatomic_read(&d->pfifo.halt) &&
            !qatomic_read(&d->pfifo.inline_pusher)) {
            pfifo_run_pusher(d);
        }

        pgraph_process_pending_reports(d);

        pfifo_loop_iters++;
        if (pfifo_heartbeat) {
            /* Monotonic; QEMU_CLOCK_HOST is wall clock (NTP-jumpable) */
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (pfifo_last_heartbeat_ns == 0 ||
                now - pfifo_last_heartbeat_ns >=
                    2LL * 1000 * 1000 * 1000) {
                pfifo_last_heartbeat_ns = now;
                /*
                 * Renderer-specific pending flags (downloads_pending,
                 * download_dirty_surfaces_pending) live inside
                 * PGRAPHVkState / PGRAPHGLState which are only
                 * forward-declared in pgraph.h, so this log sticks
                 * to the pgraph.h-visible flags. Those are enough to
                 * categorize the freeze: if flush_pending / sync_pending
                 * stays stuck at 1 the renderer's vkWaitForFences /
                 * vkQueueSubmit is hung; if waiting_for_flip stays stuck
                 * the game is waiting on NV_PGRAPH_INCREMENT READ_3D.
                 */
                fprintf(stderr,
                        "xemu: pfifo heartbeat "
                        "iters=%" PRIu64 " halt=%d "
                        "flush=%d sync=%d "
                        "waiting_flip=%d waiting_nop=%d "
                        "waiting_ctxsw=%d\n",
                        pfifo_loop_iters,
                        qatomic_read(&d->pfifo.halt),
                        qatomic_read(&d->pgraph.flush_pending),
                        qatomic_read(&d->pgraph.sync_pending),
                        qatomic_read(&d->pgraph.waiting_for_flip),
                        qatomic_read(&d->pgraph.waiting_for_nop),
                        qatomic_read(&d->pgraph.waiting_for_context_switch));
#ifdef _WIN32
                /*
                 * ui/xemu.c reopens stderr onto CONOUT$ / xemu.log at startup;
                 * freopen gives the stream the CRT default buffering for its
                 * new target, which for the log file is full buffering. This
                 * diagnostic exists to be read after a freeze is force-killed,
                 * i.e. exactly when a buffered tail is lost, so flush it. One
                 * flush per ~2 s, only when XEMU_PFIFO_HEARTBEAT=1.
                 */
                fflush(stderr);
#endif
            }
        }

        if (!d->pfifo.fifo_kick) {
            /* The other half of the pfifo_put_doorbell Dekker pair. */
            qatomic_set(&d->pfifo.parked, true);
            smp_mb();
            if (!qatomic_read(&d->pfifo.doorbell)) {
                qatomic_store_release(&d->pfifo.idle_seq,
                                      d->pfifo.kick_seq);
                qemu_cond_broadcast(&d->pfifo.fifo_idle_cond);
                /*
                 * Untimed wait. Invariant: every pfifo_kick call site
                 * holds d->pfifo.lock across the fifo_kick-set +
                 * cond_broadcast pair (see comment on pfifo_kick above),
                 * so the reader's kick-check + qemu_cond_wait atomic
                 * release is race-free. Eliminates the earlier ~1 kHz
                 * safety-net wake-up.
                 */
                qemu_cond_wait(&d->pfifo.fifo_cond, &d->pfifo.lock);
            }
            qatomic_set(&d->pfifo.parked, false);
        }

        if (qatomic_read(&d->exiting)) {
            break;
        }
    }
    /* Release any pfifo_icount_sync waiter. */
    qemu_cond_broadcast(&d->pfifo.fifo_idle_cond);
    qemu_mutex_unlock(&d->pfifo.lock);

    rcu_unregister_thread();

    return NULL;
}

static uint32_t ramht_hash(NV2AState *d, uint32_t handle)
{
    unsigned int ramht_size =
        1 << (GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT], NV_PFIFO_RAMHT_SIZE)+12);

    /* XXX: Think this is different to what nouveau calculates... */
    unsigned int bits = ctz32(ramht_size)-1;

    uint32_t hash = 0;
    while (handle) {
        hash ^= (handle & ((1 << bits) - 1));
        handle >>= bits;
    }

    unsigned int channel_id = GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                                       NV_PFIFO_CACHE1_PUSH1_CHID);
    hash ^= channel_id << (bits - 4);

    return hash;
}


static RAMHTEntry ramht_lookup(NV2AState *d, uint32_t handle)
{
    hwaddr ramht_size =
        1 << (GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT], NV_PFIFO_RAMHT_SIZE)+12);

    uint32_t hash = ramht_hash(d, handle);
    assert(hash * 8 < ramht_size);

    hwaddr ramht_address =
        GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT],
                 NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;

    assert(ramht_address + hash * 8 < memory_region_size(&d->ramin));

    uint8_t *entry_ptr = d->ramin_ptr + ramht_address + hash * 8;

    uint32_t entry_handle = ldl_le_p((uint32_t*)entry_ptr);
    uint32_t entry_context = ldl_le_p((uint32_t*)(entry_ptr + 4));

    return (RAMHTEntry){
        .handle = entry_handle,
        .instance = (entry_context & NV_RAMHT_INSTANCE) << 4,
        .engine = (entry_context & NV_RAMHT_ENGINE) >> 16,
        .channel_id = (entry_context & NV_RAMHT_CHID) >> 24,
        .valid = entry_context & NV_RAMHT_STATUS,
    };
}
