/*
 * xemu QMP events: XEMU_READY, XEMU_STATE_LOADED, XEMU_DEVICE_LOST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_EVENTS_H
#define XEMU_EVENTS_H

#include "qapi/qapi-types-xemu.h"

/* The machine and pads exist (ui/xemu.c, with "xemu: ready"). */
void xemu_events_ready(void);

/*
 * The next machine state load is @source, named @name (may be NULL). A
 * load that sets nothing is an incoming migration. Set right before
 * qemu_loadvm_state, which takes it (and so resets it) on entry.
 */
void xemu_events_load_source(XemuLoadSource source, const char *name);

/* Start of qemu_loadvm_state: take the pending source for this load. */
void xemu_events_load_begin(void);

/* End of qemu_loadvm_state: emit XEMU_STATE_LOADED if @ok. */
void xemu_events_load_done(bool ok);

/*
 * The Vulkan renderer lost its device; xemu ends next. Any thread; only
 * the first call emits. @where may be NULL.
 */
void xemu_events_device_lost(XemuDeviceLostReason reason, const char *where,
                             int vk_result);

#endif
