/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
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
#include "qemu/log.h"

/* USER - PFIFO MMIO and DMA submission area */
uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    unsigned int channel_id = addr >> 16;
    if (channel_id >= NV2A_NUM_CHANNELS) {
        /* The window is 128 channels wide; the model has 32. Read as 0. */
        qemu_log_mask(LOG_UNIMP, "nv2a: NV_USER read past the last "
                      "channel (%u)\n", channel_id);
        nv2a_reg_log_read(NV_USER, addr, size, 0);
        return 0;
    }

    /*
     * With the lockless doorbell, reads skip pfifo.lock too, since the
     * guest's DMA_GET polls would otherwise wait out the thread's report
     * fence waits. GET is one word, stored with release after the words
     * before it are consumed.
     */
    bool locked = !d->pfifo.lockless_put;
    if (locked) {
        qemu_mutex_lock(&d->pfifo.lock);
    }

    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];

    uint64_t r = 0;
    if (channel_modes & (1 << channel_id)) {
        /* DMA Mode */

        unsigned int cur_channel_id =
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID);

        if (channel_id == cur_channel_id) {
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                r = qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]);
                break;
            case NV_USER_DMA_GET:
                /* Pairs with the pusher's release store. */
                r = qatomic_load_acquire(
                    &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]);
                break;
            case NV_USER_REF:
                r = qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_REF]);
                break;
            default:
                break;
            }
        } else {
            /*
             * ramfc: unsupported, read as 0. A gdbstub memory read that
             * strays onto this MMIO lands here too; it must not abort.
             */
            qemu_log_mask(LOG_UNIMP, "nv2a: NV_USER read of inactive "
                          "channel %u\n", channel_id);
        }
    } else {
        /* PIO Mode: unsupported, read as 0 (see above). */
        qemu_log_mask(LOG_UNIMP, "nv2a: NV_USER read in PIO mode, "
                      "channel %u\n", channel_id);
    }

    if (locked) {
        qemu_mutex_unlock(&d->pfifo.lock);
    }

    nv2a_reg_log_read(NV_USER, addr, size, r);
    return r;
}

void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_USER, addr, size, val);

    /*
     * Writes the model doesn't handle (past the last channel, an offset
     * other than PUT/GET/REF, a channel that isn't current, PIO mode) are
     * ignored and logged, as user_read reads them as 0. Guests don't make
     * them; a debugger or a stray pointer might.
     */
    unsigned int channel_id = addr >> 16;
    if (channel_id >= NV2A_NUM_CHANNELS) {
        qemu_log_mask(LOG_UNIMP, "nv2a: NV_USER write past the last "
                      "channel (%u) ignored\n", channel_id);
        return;
    }

    /*
     * The pushbuffer doorbell skips pfifo.lock in normal mode. The channel
     * registers it checks change only by guest writes on this thread, or
     * with the vCPU parked (reset, loads).
     */
    if ((addr & 0xFFFF) == NV_USER_DMA_PUT && d->pfifo.lockless_put &&
        (d->pfifo.regs[NV_PFIFO_MODE] & (1 << channel_id)) &&
        channel_id == GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                               NV_PFIFO_CACHE1_PUSH1_CHID)) {
        pfifo_put_doorbell(d, val);
        return;
    }

    qemu_mutex_lock(&d->pfifo.lock);

    bool handled = false;
    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];
    if (channel_modes & (1 << channel_id)) {
        /* DMA Mode */
        unsigned int cur_channel_id =
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID);

        if (channel_id == cur_channel_id) {
            handled = true;
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = val;
                break;
            case NV_USER_DMA_GET:
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = val;
                break;
            case NV_USER_REF:
                d->pfifo.regs[NV_PFIFO_CACHE1_REF] = val;
                break;
            default:
                handled = false;
                qemu_log_mask(LOG_UNIMP, "nv2a: unsupported NV_USER write "
                              "ignored: channel %u offset 0x%04x\n",
                              channel_id, (unsigned)(addr & 0xFFFF));
                break;
            }

            if (handled) {
                pfifo_guest_kick(d);
            }
        } else {
            /* ramfc: unsupported */
            qemu_log_mask(LOG_UNIMP, "nv2a: NV_USER write of inactive "
                          "channel %u ignored\n", channel_id);
        }
    } else {
        /* PIO Mode: unsupported */
        qemu_log_mask(LOG_UNIMP, "nv2a: NV_USER write in PIO mode ignored, "
                      "channel %u\n", channel_id);
    }

    qemu_mutex_unlock(&d->pfifo.lock);

    if (handled && icount_enabled()) {
        pfifo_icount_sync(d);
    }
}
