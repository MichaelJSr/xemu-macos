/*
 * QEMU MCPX Audio Codec Interface implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
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

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/audio/ac97_int.h"
#include "migration/vmstate.h"
#include "exec/icount.h"

typedef struct MCPXACIState {
    PCIDevice dev;

    AC97LinkState ac97;
    bool link_loaded; /* mcpx-aci/ac97 came with a load */

    MemoryRegion io_nam, io_nabm;

    MemoryRegion mmio;
    MemoryRegion nam_mmio, nabm_mmio;
} MCPXACIState;

#define MCPX_ACI_DEVICE(obj) \
    OBJECT_CHECK(MCPXACIState, (obj), "mcpx-aci")

static void mcpx_aci_realize(PCIDevice *dev, Error **errp)
{
    MCPXACIState *d = MCPX_ACI_DEVICE(dev);

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init(&d->mmio, OBJECT(dev), "mcpx-aci-mmio", 0x1000);

    memory_region_init_io(&d->io_nam, OBJECT(dev), &ac97_io_nam_ops, &d->ac97,
                          "mcpx-aci-nam", 0x100);
    memory_region_init_io(&d->io_nabm, OBJECT(dev), &ac97_io_nabm_ops, &d->ac97,
                          "mcpx-aci-nabm", 0x80);

    /*pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &d->io_nam);
    pci_register_bar(&d->dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &d->io_nabm);

    memory_region_init_alias(&d->nam_mmio, NULL, &d->io_nam, 0, 0x100);
    memory_region_add_subregion(&d->mmio, 0x0, &d->nam_mmio);

    memory_region_init_alias(&d->nabm_mmio, NULL, &d->io_nabm, 0, 0x80);
    memory_region_add_subregion(&d->mmio, 0x100, &d->nabm_mmio);*/

    memory_region_add_subregion(&d->mmio, 0x0, &d->io_nam);
    memory_region_add_subregion(&d->mmio, 0x100, &d->io_nabm);

    pci_register_bar(&d->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);
    ac97_common_init(&d->ac97, &d->dev, pci_get_address_space(&d->dev));
}

/*
 * The AC'97 registers are in the savestate only from deterministic mode
 * (icount, mcpx-aci/ac97 below); older saves leave them holding whatever
 * ran before the load. Deterministic mode then starts from the power-on
 * state instead, as a fresh process does. After the PCI state is in, so
 * lowering the line keeps the bus's count.
 */
static int mcpx_aci_post_load(void *opaque, int version_id)
{
    MCPXACIState *d = opaque;

    if (d->link_loaded) {
        ac97_link_post_load(&d->ac97);
    } else if (icount_enabled()) {
        ac97_power_on_reset(&d->ac97);
    }
    d->link_loaded = false;
    return 0;
}

static bool mcpx_aci_link_needed(void *opaque)
{
    return icount_enabled();
}

static int mcpx_aci_link_post_load(void *opaque, int version_id)
{
    MCPXACIState *d = opaque;
    d->link_loaded = true;
    return 0;
}

static const VMStateDescription vmstate_mcpx_aci_link = {
    .name = "mcpx-aci/ac97",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mcpx_aci_link_needed,
    .post_load = mcpx_aci_link_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ac97.glob_cnt, MCPXACIState),
        VMSTATE_UINT32(ac97.glob_sta, MCPXACIState),
        VMSTATE_UINT32(ac97.cas, MCPXACIState),
        VMSTATE_UINT32(ac97.last_samp, MCPXACIState),
        VMSTATE_STRUCT_ARRAY(ac97.bm_regs, MCPXACIState, LAST_INDEX, 1,
                             vmstate_ac97_bm_regs, AC97BusMasterRegs),
        VMSTATE_BUFFER(ac97.mixer_data, MCPXACIState),
        VMSTATE_INT32(ac97.bup_flag, MCPXACIState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mcpx_aci = {
    .name = "mcpx-aci",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mcpx_aci_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, MCPXACIState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_mcpx_aci_link,
        NULL
    },
};

static void mcpx_aci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = PCI_DEVICE_ID_NVIDIA_MCPX_ACI;
    k->revision = 177;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    k->realize = mcpx_aci_realize;

    dc->desc = "MCPX Audio Codec Interface";
    dc->vmsd = &vmstate_mcpx_aci;
}

static const TypeInfo mcpx_aci_info = {
    .name          = "mcpx-aci",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MCPXACIState),
    .class_init    = mcpx_aci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void mcpx_aci_register(void)
{
    type_register_static(&mcpx_aci_info);
}

type_init(mcpx_aci_register);
