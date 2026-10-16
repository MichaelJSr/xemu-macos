/*
 * xemu trace recording (XEMU_TRACE_RECORD): guest memory at every vblank.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_TRACE_RECORD_H
#define XEMU_TRACE_RECORD_H

#include "qapi/qapi-types-xemu.h"

void xemu_trace_record_init(void);
/* At a vblank while the VM runs, after its input landed. BQL held. */
void xemu_trace_record_frame(uint64_t frame);
/* After loadvm or an incoming migration. BQL held. */
void xemu_trace_record_load(uint64_t frame);

/* Also behind xemu-frame-advance traces (ui/xemu.c). */
bool xemu_trace_regions_check(XemuTraceRegionList *list, Error **errp);
bool xemu_trace_region_read(uint8_t *buf, const uint8_t *ram,
                            uint64_t ram_size, const XemuTraceRegion *r);

#endif
