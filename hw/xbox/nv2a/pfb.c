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

/*
 * NV_PFB_CSTATUS reports the RAM size. Above 128 MiB (an experimental -m)
 * it reports 128 MiB, the most the hardware's NV2A reaches and a 128 MiB
 * console has,
 * unless XEMU_CSTATUS_FULL_RAM=1 asks for the full size. At 64 and 128
 * MiB both are the RAM size.
 */
void pfb_init(NV2AState *d)
{
    const char *full = getenv("XEMU_CSTATUS_FULL_RAM");
    bool want_full = full && !strcmp(full, "1");
    uint64_t ram = memory_region_size(d->vram);

    d->pfb.cstatus = want_full ? ram : nv2a_aperture_size(d);
    if (ram > NV2A_APERTURE_MAX) {
        fprintf(stderr, "xemu: guest RAM %" PRIu64 " MiB (experimental above "
                "128 MiB): NV2A BAR1 and VGA cover the first 128 MiB, "
                "NV_PFB_CSTATUS reports %u MiB%s\n", ram / MiB,
                (unsigned)(d->pfb.cstatus / MiB),
                want_full ? " (XEMU_CSTATUS_FULL_RAM=1)" : "");
    }
}

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = d->pfb.cstatus;
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending. */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }
}
