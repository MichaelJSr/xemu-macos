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
#include "qemu/seqlock.h"
#include "qemu/thread.h"
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

/*
 * Frame ends (NV097_FLIP_STALL) since the last load, capped at 2, and the
 * load they count for (renderer thread; see xemu_events_gpu_write). A
 * state saved after its frame ended counts as one end.
 */
static int64_t frame_end_loads;
static int frame_ends;
static bool load_frame_ended;   /* set by the load in progress */

void xemu_events_frame_end(void)
{
    int64_t l = qatomic_read(&loads);

    if (qatomic_read(&frame_end_loads) != l) {
        qatomic_set(&frame_ends, 0);
        qatomic_set(&frame_end_loads, l);
    }
    if (qatomic_read(&frame_ends) < 2) {
        qatomic_set(&frame_ends, qatomic_read(&frame_ends) + 1);
    }
}

void xemu_events_load_frame_ended(void)
{
    load_frame_ended = true;
}

void xemu_events_load_begin(void)
{
    load_frame_ended = false;
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
        qatomic_set(&loads, loads + 1);   /* renderer threads read it */
        qatomic_set(&frame_ends, load_frame_ended ? 1 : 0);
        qatomic_set(&frame_end_loads, loads);
        qapi_event_send_xemu_state_loaded(load_source, load_name,
                                          xemu_frame_count(), loads);
    }
    load_frame_ended = false;
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

/*
 * The frame on screen changes when the GPU writes the buffer at the CRTC
 * start, not when the guest writes the start (Azurik writes the same one
 * every vblank and resolves each frame into it): so its age is the load
 * count and frame at the GPU's last write into that buffer, counting only
 * writes after the end of a frame begun after that load. A load or
 * checkpoint restore drops the renderer's surfaces, so what the guest
 * draws or resolves until a frame begun after the load has ended is built
 * on what the loaded RAM holds: the frame of the save, at surface scale 2
 * upscaled from guest resolution (older and softer than the frame the run
 * that went on from the save shows). Recent colour targets are kept here,
 * written by the renderer thread under pgraph.lock (picture_spin covers
 * any other) and read by the query under the seqlock. The entry holding
 * the CRTC start is never the one evicted, so the buffer on screen keeps
 * its date however many other targets a frame writes (render-to-texture
 * passes, several sizes at one address).
 */
#define PICTURE_TARGETS 64

typedef struct PictureTarget {
    uint64_t start, end;
    int64_t loads, frame;
    uint64_t seq;
} PictureTarget;

static PictureTarget picture_targets[PICTURE_TARGETS];
static unsigned picture_last;
static uint64_t picture_seq;
static QemuSpin picture_spin;
static QemuSeqLock picture_sl;

/* The entry to overwrite with [start, end): its own, else the oldest. */
static unsigned picture_victim(uint64_t start, uint64_t end)
{
    uint64_t scan = nv2a_get_crtc_start();
    int victim = -1, oldest = 0;

    for (unsigned i = 0; i < PICTURE_TARGETS; i++) {
        const PictureTarget *e = &picture_targets[i];

        if (e->start == start && e->end == end) {
            return i;
        }
        if (e->seq < picture_targets[oldest].seq) {
            oldest = i;
        }
        if (e->start <= scan && scan < e->end) {
            continue;   /* the buffer on screen keeps its date */
        }
        if (victim < 0 || e->seq < picture_targets[victim].seq) {
            victim = i;
        }
    }
    return victim < 0 ? oldest : victim;
}

void xemu_events_gpu_write(uint64_t start, uint64_t size)
{
    int64_t l = qatomic_read(&loads);
    int64_t f = xemu_frame_count();
    uint64_t end = start + size;
    PictureTarget *t = &picture_targets[qatomic_read(&picture_last)];

    /*
     * Until a frame begun after the last load has ended, a write finishes
     * the frame the load caught or resolves one drawn before the save
     * (Azurik resolves each frame into the scanout as the next begins), so
     * the buffer keeps its old date. Before the first load (loads 0)
     * every write counts.
     */
    if (l && (qatomic_read(&frame_end_loads) != l ||
              qatomic_read(&frame_ends) < 2)) {
        return;
    }
    /* Nearly every draw: the same target, frame and load as the last. */
    if (!size || (t->start == start && t->end == end && t->loads == l &&
                  t->frame == f)) {
        return;
    }
    qemu_spin_lock(&picture_spin);
    seqlock_write_begin(&picture_sl);
    unsigned victim = picture_victim(start, end);
    t = &picture_targets[victim];
    t->start = start;
    t->end = end;
    t->loads = l;
    t->frame = f;
    t->seq = ++picture_seq;
    qatomic_set(&picture_last, victim);
    seqlock_write_end(&picture_sl);
    qemu_spin_unlock(&picture_spin);
}

/* The last write into the buffer holding @addr, or false. */
static bool picture_age(uint64_t addr, int64_t *l, int64_t *f)
{
    unsigned seq;
    bool found;

    do {
        uint64_t best = 0;

        seq = seqlock_read_begin(&picture_sl);
        found = false;
        for (unsigned i = 0; i < PICTURE_TARGETS; i++) {
            const PictureTarget *e = &picture_targets[i];

            if (e->seq > best && e->start <= addr && addr < e->end) {
                best = e->seq;
                *l = e->loads;
                *f = e->frame;
                found = true;
            }
        }
    } while (seqlock_read_retry(&picture_sl, seq));
    return found;
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
    info->has_picture_loads = info->has_picture_frame =
        picture_age(info->scanout, &info->picture_loads,
                    &info->picture_frame);
    /*
     * Stale: no GPU write into the buffer on screen counted since the
     * last load (none at all, after a load, means the picture is built
     * from the loaded RAM). Present whenever the GPU draws pictures.
     */
    if (nv2a_renderer_draws()) {
        info->has_picture_stale = true;
        info->picture_stale = info->has_picture_loads
                                  ? info->picture_loads < loads
                                  : loads > 0;
    }
    return info;
}
