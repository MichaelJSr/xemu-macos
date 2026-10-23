/*
 * xemu QMP events: XEMU_READY, XEMU_STATE_LOADED, XEMU_DEVICE_LOST, and
 * query-xemu-display's flip counters
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

/*
 * A failed checkpoint restore starts over: its next qemu_loadvm_state takes
 * its source again (the restore resets it if the retry fails first).
 * load_snapshot sets its source before every attempt instead, so a retry
 * that fails early leaves nothing pending.
 */
void xemu_events_load_retry(void);

/* End of qemu_loadvm_state: emit XEMU_STATE_LOADED if @ok. */
void xemu_events_load_done(bool ok);

/*
 * The guest wrote the CRTC start address (a flip; query-xemu-display).
 * vCPU thread, BQL held.
 */
void xemu_events_flip(void);

/*
 * The GPU wrote the colour surface at VRAM [start, start + size): a draw,
 * clear or 2D blit, from whichever thread runs the renderer (it holds
 * pgraph.lock). query-xemu-display's picture-loads and picture-frame
 * date the frame on screen by the last such write into it.
 */
void xemu_events_gpu_write(uint64_t start, uint64_t size);

/*
 * The guest ended a frame (NV097_FLIP_STALL), on the renderer's thread.
 * GPU writes count toward a picture's age only from the first frame end
 * after a load: until then they finish the frame the load caught under
 * way, drawn partly before the save, or resolve one drawn wholly before
 * it.
 */
void xemu_events_frame_end(void);

/*
 * The state being loaded was saved after its frame ended (nv2a's
 * post_load: PGRAPH waits for the flip), so the next frame starts after
 * the load. Applies if the load succeeds.
 */
void xemu_events_load_frame_ended(void);

/*
 * The Vulkan renderer lost its device; xemu ends next. Any thread; only
 * the first call emits. @where may be NULL.
 */
void xemu_events_device_lost(XemuDeviceLostReason reason, const char *where,
                             int vk_result);

#endif
