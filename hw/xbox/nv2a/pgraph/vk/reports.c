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

void pgraph_vk_init_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    r->num_queries_in_flight = 0;
    /*
     * Sized for whole-frame command buffers with per-rotation query
     * indices (deferred reports + in-pass queries): heavy scenes
     * measure ~200-400 rotations per frame; 2048 per slot leaves
     * ample headroom before the begin_draw capacity guard forces a
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

void pgraph_vk_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (*dma_get != *dma_put) {
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
         * XEMU_REPORTS_SYNC=1 restores the legacy synchronous
         * behavior wholesale.
         */
        static int sync_mode = -1;
        if (sync_mode < 0) {
            const char *e = getenv("XEMU_REPORTS_SYNC");
            sync_mode = (e && e[0] == '1');
        }
        if (sync_mode) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED);
            return;
        }
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (r->reports_idle_since_ns == 0) {
            r->reports_idle_since_ns = now;
        } else if (now - r->reports_idle_since_ns > 5000000ll /* 5 ms */) {
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
