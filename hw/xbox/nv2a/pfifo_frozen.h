/*
 * QEMU Geforce NV2A: pushbuffer words frozen at a ring restart
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_NV2A_PFIFO_FROZEN_H
#define HW_XBOX_NV2A_PFIFO_FROZEN_H

#include "qemu/osdep.h"
#include "qemu/atomic.h"

/*
 * The hardware's DMA pusher fetches pushbuffer words into CACHE1 (128
 * method/data entries, so at most 256 words with a header per entry)
 * ahead of the puller, also while the puller waits for a software-method
 * interrupt or a flip. xemu's threaded pusher reads each word from guest
 * RAM only when it executes it, so it can be far behind the guest: a
 * PGRAPH finish waits for the host GPU, and a NOP notify waits for a
 * guest DPC round trip.
 *
 * pbkit's pb_reset() writes a jump at PUT, moves PUT to the ring head and
 * then waits only 100 ticks for DMA_GET to arrive there before it writes
 * the next frame over the ring anyway. On hardware the words it overwrites
 * are long fetched; here they may not be read yet, and the pusher then
 * runs data words as commands (an "invalid GPU command" abort, a bogus
 * jump, or a wedged FIFO). So at a DMA_PUT write that moves PUT back
 * behind DMA_GET (a ring restart), the doorbell copies the words the
 * pusher hasn't read, up to and including the word at the old PUT (the
 * jump back), as CACHE1 would hold them, and the pusher reads them from
 * the copy.
 *
 * Normal mode only: under icount the pusher runs at every guest PFIFO
 * write (pfifo_icount_sync), so it is never behind.
 */
#define PFIFO_FROZEN_WORDS 256

typedef struct PFIFOFrozen {
    uint32_t words[PFIFO_FROZEN_WORDS];
    /* DMA offsets of words[0] and past the last word */
    uint32_t start;
    uint32_t end;
    /*
     * Set with release by the doorbell; cleared by the pusher once it
     * leaves the copy, and by guest DMA_GET/DMA_PUT register writes,
     * reset and loads.
     */
    bool valid;
} PFIFOFrozen;

/*
 * The doorbell, at a write of new_put over old_put: freeze [get, old_put]
 * of dma (dma_limit is the DMA object's last byte), capped at
 * PFIFO_FROZEN_WORDS. Call before publishing new_put. Only a restart
 * behind the pusher (new_put < get <= old_put: it must run up to the jump
 * at old_put to get back there) freezes anything; a PUT moved back to or
 * past GET leaves the pusher nothing it would read in [get, old_put], and
 * the guest may write new words there (a channel reset that writes GET
 * first). One segment at a time: a restart while one is pending keeps the
 * old one.
 */
static inline void pfifo_frozen_capture(PFIFOFrozen *f, const uint8_t *dma,
                                        uint64_t dma_limit, uint32_t get,
                                        uint32_t old_put, uint32_t new_put)
{
    if (new_put >= get || get > old_put || (get & 3) || (old_put & 3) ||
        old_put > dma_limit || qatomic_read(&f->valid)) {
        return;
    }
    uint64_t end = MIN((uint64_t)old_put + 4,
                       (uint64_t)get + 4 * PFIFO_FROZEN_WORDS);
    end = MIN(end, (dma_limit + 1) & ~3ull);
    if (end <= get) {
        return;
    }
    memcpy(f->words, dma + get, end - get);
    f->start = get;
    f->end = end;
    qatomic_store_release(&f->valid, true);
}

/*
 * The pusher, right after it read the word at get from RAM: the frozen
 * copy of that word, or NULL to keep the RAM one. *avail is capped so a
 * command's data words don't run past the copy. A segment the pusher has
 * left (a jump, or past its end) is dropped.
 */
static inline uint32_t *pfifo_frozen_lookup(PFIFOFrozen *f, uint32_t get,
                                            size_t *avail)
{
    /*
     * Order the caller's RAM read before this test: if no freeze is seen,
     * the word was read before the doorbell copied it, so before the
     * guest could change it.
     */
    smp_rmb();
    if (likely(!qatomic_load_acquire(&f->valid))) {
        return NULL;
    }
    if (get < f->start || get >= f->end) {
        qatomic_set(&f->valid, false);
        return NULL;
    }
    *avail = MIN(*avail, (size_t)(f->end - get) / 4);
    return &f->words[(get - f->start) / 4];
}

static inline void pfifo_frozen_drop(PFIFOFrozen *f)
{
    qatomic_set(&f->valid, false);
}

#endif
