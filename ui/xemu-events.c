/*
 * xemu QMP events: XEMU_READY, XEMU_STATE_LOADED, XEMU_DEVICE_LOST, and
 * query-xemu-display's flip counters
 *
 * Harnesses used to poll the run log for the stderr lines "xemu: ready",
 * "xemu: state loaded" and "xemu: Vulkan device lost; exiting". The lines
 * stay; these events carry the same news over QMP, and query-xemu-status
 * covers a client that negotiates after XEMU_READY went out. Nothing here
 * touches guest state, so deterministic runs are unaffected.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "qapi/qapi-events-xemu.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "xemu-events.h"

int64_t xemu_frame_count(void);

/* Main thread under the BQL, except device_lost_sent. */
static bool ready;
static int64_t loads;
/* Set for the next load (pending_*); taken by the load in progress (load_*). */
static XemuLoadSource pending_source = XEMU_LOAD_SOURCE_INCOMING;
static char *pending_name;
static XemuLoadSource load_source = XEMU_LOAD_SOURCE_INCOMING;
static char *load_name;
static bool device_lost_sent;
/* Guest flips: NV_PCRTC_START writes, on the vCPU thread under the BQL. */
static int64_t flips, flip_frame, flip_loads;

void xemu_events_ready(void)
{
    if (!ready) {
        ready = true;
        qapi_event_send_xemu_ready();
    }
}

void xemu_events_load_source(XemuLoadSource source, const char *name)
{
    pending_source = source;
    g_free(pending_name);
    pending_name = g_strdup(name);
}

void xemu_events_load_begin(void)
{
    /*
     * Single use: every qemu_loadvm_state takes the pending source here,
     * whichever way it returns, so a load that fails early can't lend its
     * name to the next one (an incoming migration sets nothing).
     */
    load_source = pending_source;
    g_free(load_name);
    load_name = pending_name;
    pending_source = XEMU_LOAD_SOURCE_INCOMING;
    pending_name = NULL;
}

void xemu_events_load_retry(void)
{
    /* The same load starts over (XEMU_LOADVM_ADD_PADS): it keeps its source. */
    pending_source = load_source;
    g_free(pending_name);
    pending_name = g_steal_pointer(&load_name);
    load_source = XEMU_LOAD_SOURCE_INCOMING;
}

void xemu_events_load_done(bool ok)
{
    if (ok) {
        loads++;
        qapi_event_send_xemu_state_loaded(load_source, load_name,
                                          xemu_frame_count(), loads);
    }
}

void xemu_events_device_lost(XemuDeviceLostReason reason, const char *where,
                             int vk_result)
{
    /*
     * Usually the PFIFO thread, without the BQL: event emission takes only
     * the monitor locks and writes the socket from this thread, so the
     * event is on its way before the caller exits or aborts.
     */
    if (qatomic_xchg(&device_lost_sent, true)) {
        return;
    }
    qapi_event_send_xemu_device_lost(reason, where, vk_result);
}

XemuStatus *qmp_query_xemu_status(Error **errp)
{
    XemuStatus *s = g_new0(XemuStatus, 1);

    s->ready = ready;
    s->loads = loads;
    return s;
}

void xemu_events_flip(void)
{
    flips++;
    flip_frame = xemu_frame_count();
    flip_loads = loads;
}

XemuDisplayInfo *qmp_query_xemu_display(Error **errp)
{
    XemuDisplayInfo *info = g_new0(XemuDisplayInfo, 1);

    info->frame = xemu_frame_count();
    info->loads = loads;
    info->flips = flips;
    if (flips) {
        info->has_flip_frame = true;
        info->flip_frame = flip_frame;
        info->has_flip_loads = true;
        info->flip_loads = flip_loads;
    }
    info->scanout = nv2a_get_crtc_start();
    return info;
}
