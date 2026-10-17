/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "renderer.h"
#include "exec/icount.h"
#include "hw/core/cpu.h"
#include "system/tcg.h"
#include "qemu/main-loop.h"
#include "hw/xbox/nv2a/nsprof.h"

void pgraph_vk_init_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    r->num_queries_in_flight = 0;
    /*
     * Sized for whole-frame command buffers with per-rotation query
     * indices (deferred reports + in-pass queries): heavy scenes
     * measure ~200-400 rotations per frame; 2048 per slot leaves
     * ample headroom before the begin_pre_draw capacity guard forces a
     * submit. Visibility/result storage is 8 bytes per query.
     * XEMU_MAX_QUERIES overrides for testing the guard path.
     */
    r->max_queries_in_flight = 4096;
    {
        const char *e = getenv("XEMU_MAX_QUERIES");
        if (e && e[0]) {
            long v = strtol(e, NULL, 0);
            if (v >= 8 && v <= 65536) {
                r->max_queries_in_flight = (int)v;
            }
        }
    }
    r->new_query_needed = false;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;

    qemu_mutex_init(&r->det_reports.lock);
    qemu_event_init(&r->det_reports.delivered, false);
    r->det_reports.npages = 0;
    r->det_reports.pending = 0;
    r->det_reports.park_drain = false;
    r->det_reports.deliver = false;
    r->det_reports.eager = !tcg_enabled();

    r->query_results_buf = g_malloc_n(r->max_queries_in_flight, sizeof(uint64_t));
    r->report_pool = g_malloc_n(r->max_queries_in_flight, sizeof(QueryReport));
    r->report_pool_next = 0;

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        QSIMPLEQ_INIT(&r->flight[i].report_queue);
        r->flight[i].query_count = 0;
    }

    VkQueryPoolCreateInfo pool_create_info = (VkQueryPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = r->max_queries_in_flight,
    };
    VK_CHECK(
        vkCreateQueryPool(r->device, &pool_create_info, NULL, &r->query_pool));
}

void pgraph_vk_finalize_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->det_reports.npages; i++) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0),
                                          r->det_reports.pages[i].cb);
    }
    r->det_reports.npages = 0;
    r->det_reports.pending = 0;

    QSIMPLEQ_INIT(&r->report_queue);
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        QSIMPLEQ_INIT(&r->flight[i].report_queue);
        r->flight[i].query_count = 0;
    }

    g_free(r->query_results_buf);
    r->query_results_buf = NULL;
    g_free(r->report_pool);
    r->report_pool = NULL;

    vkDestroyQueryPool(r->device, r->query_pool, NULL);
}

/*
 * The report pool and query pool are partitioned per flight slot. The
 * current recording allocates from the current slot's partition; at
 * submit time pgraph_vk_finish() hands the recording's reports +
 * query count to the slot, and they are drained when the slot fence
 * is reaped instead of stalling on the just-submitted work.
 *
 * One query index is reserved beyond the report cap because a query
 * may be begun after the last report of a recording.
 */
static QueryReport *alloc_report(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->report_pool_next >= pgraph_vk_queries_per_slot(r) - 1) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_REPORTS_FULL);
    }
    /* finish() may have advanced the flight slot; re-read it. */
    int base = pgraph_vk_slot_query_base(r, r->current_flight);
    QueryReport *report = &r->report_pool[base + r->report_pool_next];
    r->report_pool_next++;
    return report;
}

/*
 * Deterministic mode (icount): lazy report delivery.
 *
 * Report RAM must change at fixed guest instruction counts. Delivering
 * every report before PFIFO parked did that, but Azurik ends each
 * visibility test with a FIFO kick, so every test became a GPU round trip
 * (~530 a flip). The guest reads the results much later, in one batch per
 * frame. So a report's slot is recorded at GET_REPORT, its page is trapped
 * (a memory access callback, as surfaces use), and the first guest or DMA
 * access to a slot with an undelivered report has PFIFO deliver every
 * pending report before the access completes. The APU's direct-pointer RAM
 * accesses are not trapped (a report slot reused as audio memory before
 * delivery would differ from eager delivery). Reports also reach RAM at
 * slot reclaim, pause (so a paused machine's RAM is complete), savevm,
 * sync_ram, before a load replaces RAM, and on the existing full-pool and
 * flush drains; none of these depend on GPU time. Report values do not
 * depend on where command buffers split (passes load/store, queries are
 * precise). The renderer's own VRAM reads and writes that could see a slot
 * check it (pgraph_vk_det_reports_guard), so a later write in the same
 * pusher run lands after the report, in hardware order. Readers that copy
 * RAM directly (hooks, frame-advance traces, value stops) check the bytes
 * they read first (pgraph_vk_det_reports_sync_read). Delivering every
 * report before PFIFO parked made each of Azurik's visibility tests a
 * finish again: 80-120 a flip in its death level whenever a hook read
 * memory.
 */
#define DET_GRANULE 16

static DetReportPage *det_report_page(PGRAPHVkState *r, uint32_t page)
{
    for (int i = 0; i < r->det_reports.npages; i++) {
        if (r->det_reports.pages[i].ram_page == page) {
            return &r->det_reports.pages[i];
        }
    }
    return NULL;
}

static void report_access_callback(void *opaque, MemoryRegion *mr,
                                   hwaddr addr, hwaddr len, bool write);

/* vCPU safe work queued after the trap's insert and its TLB refill. */
static void det_report_page_armed(CPUState *cpu, run_on_cpu_data data)
{
    qatomic_store_release(&((DetReportPage *)data.host_ptr)->armed, true);
}

/* With det_reports.lock held. */
static void det_report_page_init(NV2AState *d, DetReportPage *p,
                                 uint32_t page)
{
    memset(p, 0, sizeof(*p));
    p->ram_page = page;
    /*
     * The trap goes in as safe work at the vCPU's next TB boundary, which
     * is not at a fixed instruction. Until it is armed, this page's reports
     * are delivered before PFIFO rests, as eagerly.
     */
    p->cb = mem_access_callback_insert(qemu_get_cpu(0), d->vram, page, 4096,
                                       report_access_callback, d);
    async_safe_run_on_cpu(qemu_get_cpu(0), det_report_page_armed,
                          RUN_ON_CPU_HOST_PTR(p));
}

/* PFIFO thread, at GET_REPORT, with pgraph.lock held. */
static void det_report_track(NV2AState *d, QueryReport *report)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    uint32_t addr = report->ram_addr;
    uint32_t page = addr & ~4095u;
    DetReportPage *p;

    if (r->det_reports.eager || (addr & (DET_GRANULE - 1))) {
        r->det_reports.park_drain = true;
        return;
    }
    qemu_mutex_lock(&r->det_reports.lock);
    p = det_report_page(r, page);
    if (!p && r->det_reports.npages < DET_REPORT_PAGES) {
        p = &r->det_reports.pages[r->det_reports.npages++];
        det_report_page_init(d, p, page);
    } else if (!p) {
        /*
         * Full (reports moved, say after a reboot): reuse a page whose
         * trap is armed (so its arming work has run) and has nothing
         * pending. Its old trap finds no page once the entry changes.
         */
        for (int i = 0; i < DET_REPORT_PAGES && !p; i++) {
            DetReportPage *v = &r->det_reports.pages[i];
            if (qatomic_load_acquire(&v->armed) && !v->total) {
                mem_access_callback_remove_by_ref(qemu_get_cpu(0), v->cb);
                p = v;
                det_report_page_init(d, p, page);
            }
        }
    }
    if (p) {
        p->pending[(addr & 4095) / DET_GRANULE]++;
        p->total++;
        qatomic_set(&r->det_reports.pending, r->det_reports.pending + 1);
        report->det_tracked = true;
        if (!qatomic_load_acquire(&p->armed)) {
            r->det_reports.park_drain = true;
        }
    } else {
        warn_report_once("nv2a: reports pending on more than %d pages; "
                         "reports are delivered before PFIFO rests from "
                         "here on", DET_REPORT_PAGES);
        r->det_reports.eager = true;
        r->det_reports.park_drain = true;
    }
    qemu_mutex_unlock(&r->det_reports.lock);
}

static void det_report_untrack(PGRAPHVkState *r, QueryReport *report)
{
    qemu_mutex_lock(&r->det_reports.lock);
    DetReportPage *p = det_report_page(r, report->ram_addr & ~4095u);
    assert(p && p->pending[(report->ram_addr & 4095) / DET_GRANULE]);
    p->pending[(report->ram_addr & 4095) / DET_GRANULE]--;
    p->total--;
    /* After the report's RAM stores: pairs with det_reports_overlap. */
    qatomic_store_release(&r->det_reports.pending, r->det_reports.pending - 1);
    qemu_mutex_unlock(&r->det_reports.lock);
    report->det_tracked = false;
}

/* Whether [addr, addr + len) of VRAM holds an undelivered report. */
static bool det_reports_overlap(PGRAPHVkState *r, hwaddr addr, hwaddr len)
{
    bool hit = false;

    /* A trap that sees nothing pending then reads the delivered bytes. */
    if (!qatomic_load_acquire(&r->det_reports.pending) || !len) {
        return false;
    }
    qemu_mutex_lock(&r->det_reports.lock);
    for (int i = 0; i < r->det_reports.npages && !hit; i++) {
        DetReportPage *p = &r->det_reports.pages[i];
        hwaddr start = MAX(addr, (hwaddr)p->ram_page);
        hwaddr end = MIN(addr + len, (hwaddr)p->ram_page + 4096);
        for (hwaddr a = start & ~(hwaddr)(DET_GRANULE - 1); a < end;
             a += DET_GRANULE) {
            if (p->pending[(a & 4095) / DET_GRANULE]) {
                hit = true;
                break;
            }
        }
    }
    qemu_mutex_unlock(&r->det_reports.lock);
    return hit;
}

/* PFIFO thread, pgraph.lock held, outside a draw. */
static void det_reports_deliver_all(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->in_command_buffer && !QSIMPLEQ_EMPTY(&r->report_queue)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED); /* drains all */
    } else {
        pgraph_vk_drain_all_pending_reports(d);
    }
}

/*
 * Have PFIFO deliver every pending report and wait until it has (not on
 * the PFIFO thread). Drops the BQL while it waits, as
 * surface_access_callback does.
 */
static void det_reports_await(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    bool had_bql = bql_locked();
    if (had_bql) {
        bql_unlock();
    }
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_event_reset(&r->det_reports.delivered);
    qatomic_set(&r->det_reports.deliver, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&r->det_reports.delivered);
    if (had_bql) {
        bql_lock();
    }
}

/*
 * The guest or a DMA is about to touch a trapped report page: if the
 * access covers a slot with an undelivered report, have PFIFO deliver all
 * of them first (as surface_access_callback does for surfaces).
 */
static void report_access_callback(void *opaque, MemoryRegion *mr,
                                   hwaddr addr, hwaddr len, bool write)
{
    NV2AState *d = opaque;
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    hwaddr end = MIN(addr + len, (addr & ~(hwaddr)4095) + 4096);

    if (!r || !det_reports_overlap(r, addr, end - addr)) {
        return;
    }
    if (qemu_thread_is_self(&d->pfifo.thread)) {
        warn_report_once("nv2a: PFIFO touched an undelivered report");
        return;
    }
    nsprof_event(NSPROF_EV_REP_TRAP);
    det_reports_await(d);
}

/*
 * Renderer op: a reader on the vCPU thread is about to copy [addr, addr +
 * len) of guest RAM directly, past the traps (guest PC hooks, frame-advance
 * trace samples, xemu-run value stops). Deliver first if a pending report
 * sits there, as the trap does for a guest access, so the reader sees
 * what the guest would and lazy delivery stays on while it reads.
 */
void pgraph_vk_det_reports_sync_read(NV2AState *d, hwaddr addr, hwaddr len)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (r && det_reports_overlap(r, addr, len)) {
        nsprof_event(NSPROF_EV_REP_TRAP);
        det_reports_await(d);
    }
}

/*
 * Renderer op: deliver pending reports at the next PFIFO iteration (the
 * caller waits with pfifo_icount_sync).
 */
void pgraph_vk_request_report_delivery(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&r->det_reports.deliver, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

/*
 * After a load replaced RAM: drop the previous session's reports instead of
 * writing them, and its query counts too, so the occlusion sum restarts at
 * zero as in a fresh process. A plain finish would leave the last queries
 * on the GPU, to be added to the sum at the slot's reclaim, in the first
 * frame after the load.
 */
void pgraph_vk_det_reports_discard(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->det_reports.discard = true;
    if (r->in_command_buffer) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    }
    pgraph_vk_drain_all_pending_reports(d);
    r->det_reports.discard = false;
    r->det_reports.park_drain = false;
    r->zpass_pixel_count_result = 0;
}

/* Before RAM is saved or synced (pgraph_vk_download_dirty_surfaces). */
void pgraph_vk_det_reports_deliver(NV2AState *d)
{
    if (icount_enabled()) {
        det_reports_deliver_all(d);
    }
}

/* PFIFO thread, pgraph.lock held (pgraph_vk_process_pending). */
void pgraph_vk_det_reports_process(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_xchg(&r->det_reports.deliver, false)) {
        det_reports_deliver_all(d);
        qemu_event_set(&r->det_reports.delivered);
    }
}

/*
 * The renderer is about to read [addr, addr + len) of VRAM itself (texture
 * or vertex upload, blit), at a point where it may also download a surface,
 * so it may finish: deliver first if a pending report sits there.
 */
void pgraph_vk_det_reports_guard(NV2AState *d, hwaddr addr, hwaddr len)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (det_reports_overlap(r, addr, len)) {
        nsprof_event(NSPROF_EV_REP_GPU_TOUCH);
        det_reports_deliver_all(d);
    }
}

void pgraph_vk_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report = alloc_report(pg);
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

void pgraph_vk_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    QueryReport *report = alloc_report(pg);
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->num_queries_in_flight;
    report->det_tracked = false;
    if (icount_enabled()) {
        report->ram_addr = pgraph_report_vram_offset(d, parameter);
        det_report_track(d, report);
    }
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

/*
 * Write out a queue of reports against fetched query results.
 * query_results holds num_queries results from a single submission;
 * report->query_count indices are local to that submission.
 */
static void write_reports_from_queue(NV2AState *d, QueryReportQueue *queue,
                                     const uint64_t *query_results,
                                     int num_queries)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    int num_results_counted = 0;
    const int result_divisor =
        pg->surface_scale_factor * pg->surface_scale_factor;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(queue)) != NULL) {
        assert(report->query_count >= num_results_counted);
        assert(report->query_count <= num_queries);

        while (num_results_counted < report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[num_results_counted++];
        }

        if (report->clear) {
            NV2A_VK_DPRINTF("Cleared");
            r->zpass_pixel_count_result = 0;
        } else if (icount_enabled()) {
            /* The slot as of GET_REPORT; dropped if RAM was reloaded. */
            if (!r->det_reports.discard) {
                pgraph_write_zpass_pixel_cnt_report_at(
                    d, report->ram_addr,
                    r->zpass_pixel_count_result / result_divisor);
            }
            if (report->det_tracked) {
                det_report_untrack(r, report);
            }
        } else {
            pgraph_write_zpass_pixel_cnt_report(
                d, report->parameter,
                r->zpass_pixel_count_result / result_divisor);
        }

        QSIMPLEQ_REMOVE_HEAD(queue, entry);
    }

    // Add remaining results
    while (num_results_counted < num_queries) {
        r->zpass_pixel_count_result += query_results[num_results_counted++];
    }
}

/*
 * Drain a slot's deferred queries and reports. The caller must ensure
 * the slot's submission has completed on the GPU (fence waited) — the
 * WAIT_BIT below is then effectively free and only kept as a safety
 * net.
 */
void pgraph_vk_drain_slot_reports(NV2AState *d, int slot)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    int num_queries = r->flight[slot].query_count;
    if (num_queries == 0 && QSIMPLEQ_EMPTY(&r->flight[slot].report_queue)) {
        return;
    }

    NV2A_VK_DGROUP_BEGIN("Processing queries");

    uint64_t *query_results = r->query_results_buf;
    if (num_queries > 0) {
        size_t size_of_results = num_queries * sizeof(uint64_t);
        VkResult result;
        do {
            result = vkGetQueryPoolResults(
                r->device, r->query_pool,
                pgraph_vk_slot_query_base(r, slot), num_queries,
                size_of_results, query_results, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        } while (result == VK_NOT_READY);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkGetQueryPoolResults failed: %d\n", result);
            memset(query_results, 0, size_of_results);
        }
    }

    write_reports_from_queue(d, &r->flight[slot].report_queue, query_results,
                             num_queries);
    r->flight[slot].query_count = 0;

    NV2A_VK_DGROUP_END();
}

/*
 * Synchronously deliver every outstanding report: submitted slots in
 * submission order (oldest first — with round-robin slot reuse the
 * next slot to be reclaimed is the oldest submission), then any
 * query-less reports still attached to a not-yet-begun recording.
 */
void pgraph_vk_drain_all_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        int slot = (r->current_flight + i) % NUM_FLIGHT_SLOTS;
        if (r->flight[slot].query_count == 0 &&
            QSIMPLEQ_EMPTY(&r->flight[slot].report_queue)) {
            continue;
        }
        pgraph_vk_wait_slot_fence(pg, slot);
        pgraph_vk_drain_slot_reports(d, slot);
    }

    if (!r->in_command_buffer && !QSIMPLEQ_EMPTY(&r->report_queue)) {
        /*
         * Reports queued while no command buffer was recording cannot
         * reference unfinished queries; write them out directly.
         */
        assert(r->num_queries_in_flight == 0);
        write_reports_from_queue(d, &r->report_queue, NULL, 0);
        r->report_pool_next = 0;
    }
}

/* Continuous FIFO-idle time before the reports safety-valve submit. */
#define REPORTS_IDLE_BUDGET_NS (300 * 1000)

void pgraph_vk_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (icount_enabled()) {
        /*
         * Deterministic mode delivers as an infinitely fast GPU would:
         * every report the pusher has issued reaches RAM before this
         * thread parks, also while the pusher is stalled on a flip, and
         * pfifo_icount_sync waits for the park. So report RAM changes only
         * within the pusher run a guest register write started, never on
         * host or GPU time (no idle budget, no fence-status poll). Report
         * sums do not depend on where command buffers split: passes always
         * load/store and the queries are precise.
         */
        if (r->det_reports.eager || r->det_reports.park_drain) {
            r->det_reports.park_drain = false;
            det_reports_deliver_all(d);
        }
        return;
    }

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (*dma_get != qatomic_read(dma_put)) {
        return;
    }

    if (r->in_command_buffer && !QSIMPLEQ_EMPTY(&r->report_queue)) {
        /*
         * Deferred report delivery. The old behavior finished +
         * fully drained here (STALLED) every time the FIFO idled
         * with a report pending — measured 23+ full GPU syncs per
         * flip on report-heavy titles, and together with
         * per-rotation render-pass teardown it held a heavy scene
         * at ~16 fps. Engines overwhelmingly consume last frame's
         * occlusion counts, so by default reports simply ride the
         * next natural submission (flip) and deliver at slot
         * reclaim: zero extra submits, zero waits (measured 35.6
         * fps in the same scene, render passes 376 -> 14 per flip).
         *
         * Safety valve for a guest that truly spin-waits on the
         * value with an idle FIFO: if reports stay pending while
         * the FIFO remains continuously idle past a wall-clock
         * budget, submit once (no synchronous drain) and let the
         * fence-status poll below deliver on GPU completion.
         *
         * For a guest that consumes a report value mid-frame, the
         * budget is a direct stall on the guest's critical path: it
         * spin-waits on the value while the FIFO idles, so every
         * elapsed budget microsecond is lost frame time (measured:
         * scenes stall on it 1-2.2x per flip). A budget that is too
         * small submits each time the FIFO goes briefly idle mid-frame
         * with reports pending (50 us: -12.1 fps, a submit storm).
         * 300 us beat the pre-v0.10.1 5 ms by 5.5% fps.
         */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (r->reports_idle_since_ns == 0) {
            r->reports_idle_since_ns = now;
        } else if (now - r->reports_idle_since_ns >
                   REPORTS_IDLE_BUDGET_NS) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_REPORTS_SUBMIT);
            r->reports_idle_since_ns = 0;
        }
    } else {
        r->reports_idle_since_ns = 0;
    }

    /*
     * Deliver deferred reports for any slot whose submission has
     * completed. Non-blocking: slots still executing are skipped and
     * re-checked on the next idle iteration.
     */
    for (int i = 0; i < NUM_FLIGHT_SLOTS; i++) {
        int slot = (r->current_flight + i) % NUM_FLIGHT_SLOTS;
        if (r->flight[slot].query_count == 0 &&
            QSIMPLEQ_EMPTY(&r->flight[slot].report_queue)) {
            continue;
        }
        if (r->flight[slot].submitted &&
            vkGetFenceStatus(r->device, r->flight[slot].fence) !=
                VK_SUCCESS) {
            continue;
        }
        pgraph_vk_wait_slot_fence(pg, slot); /* signaled: immediate */
        pgraph_vk_drain_slot_reports(d, slot);
    }

    if (!r->in_command_buffer && !QSIMPLEQ_EMPTY(&r->report_queue)) {
        /*
         * Reports queued while no command buffer was recording cannot
         * reference unfinished queries; write them out directly.
         */
        assert(r->num_queries_in_flight == 0);
        write_reports_from_queue(d, &r->report_queue, NULL, 0);
        r->report_pool_next = 0;
    }
}
