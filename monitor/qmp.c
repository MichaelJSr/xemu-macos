/*
 * QEMU monitor
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"

#include "chardev/char-io.h"
#include "monitor-internal.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qemu/main-loop.h"
#include "trace.h"

/*
 * qmp_dispatcher_co_busy is used for synchronisation between the
 * monitor thread and the main thread to ensure that the dispatcher
 * coroutine never gets scheduled a second time when it's already
 * scheduled (scheduling the same coroutine twice is forbidden).
 *
 * It is true if the coroutine will process at least one more request
 * before going to sleep.  Either it has been kicked already, or it
 * is active and processing requests.  Additional requests may therefore
 * be pushed onto mon->qmp_requests, and @qmp_dispatcher_co_shutdown may
 * be set without further ado.  @qmp_dispatcher_co must not be woken up
 * in this case.
 *
 * If false, you have to wake up @qmp_dispatcher_co after pushing new
 * requests. You also have to set @qmp_dispatcher_co_busy to true
 * before waking up the coroutine.
 *
 * The coroutine will automatically change this variable back to false
 * before it yields.  Nobody else may set the variable to false.
 *
 * Access must be atomic for thread safety.
 */
static bool qmp_dispatcher_co_busy = true;

struct QMPRequest {
    /* Owner of the request */
    MonitorQMP *mon;
    /*
     * Request object to be handled or Error to be reported
     * (exactly one of them is non-null)
     */
    QObject *req;
    Error *err;
};
typedef struct QMPRequest QMPRequest;

QmpCommandList qmp_commands, qmp_cap_negotiation_commands;

/*
 * xemu: what a watch monitor (XEMU_QMP_WATCH) answers once negotiated:
 * read-only queries that never block, change guest state, a RAM digest or
 * guest time, so they can run from the main loop while another monitor's
 * command holds the dispatcher (xemu-frame-advance waiting for vblanks).
 * None may be a coroutine command. A new name must meet the same bar.
 */
QmpCommandList qmp_watch_commands;

static const char *const qmp_watch_names[] = {
    "qmp_capabilities",
    "query-commands",
    "query-version",
    "query-status",
    "query-xemu-frame",
    "query-xemu-status",
    "query-xemu-display",
    "query-xemu-input",
    "query-xemu-host-profile",
};

static bool qmp_oob_enabled(MonitorQMP *mon)
{
    return mon->capab[QMP_CAPABILITY_OOB];
}

static void monitor_qmp_caps_reset(MonitorQMP *mon)
{
    memset(mon->capab_offered, 0, sizeof(mon->capab_offered));
    memset(mon->capab, 0, sizeof(mon->capab));
    /* A watch monitor answers at once anyway: it offers no OOB. */
    mon->capab_offered[QMP_CAPABILITY_OOB] = mon->common.use_io_thread &&
                                             !mon->watch;
}

static void qmp_request_free(QMPRequest *req)
{
    qobject_unref(req->req);
    error_free(req->err);
    g_free(req);
}

/* Caller must hold mon->qmp.qmp_queue_lock */
static void monitor_qmp_cleanup_req_queue_locked(MonitorQMP *mon)
{
    while (!g_queue_is_empty(mon->qmp_requests)) {
        qmp_request_free(g_queue_pop_head(mon->qmp_requests));
    }
}

static void monitor_qmp_cleanup_queue_and_resume(MonitorQMP *mon)
{
    QEMU_LOCK_GUARD(&mon->qmp_queue_lock);

    /*
     * Same condition as in monitor_qmp_dispatcher_co(), but before
     * removing an element from the queue (hence no `- 1`).
     * Also, the queue should not be empty either, otherwise the
     * monitor hasn't been suspended yet (or was already resumed).
     */
    bool need_resume = (!qmp_oob_enabled(mon) ||
        mon->qmp_requests->length == QMP_REQ_QUEUE_LEN_MAX)
        && !g_queue_is_empty(mon->qmp_requests);
    /*
     * A watch monitor suspended once per queued request (one read can
     * hold several): give every one back.
     */
    guint watch_resumes = mon->watch ? mon->qmp_requests->length : 0;

    monitor_qmp_cleanup_req_queue_locked(mon);

    if (mon->watch) {
        while (watch_resumes--) {
            monitor_resume(&mon->common);
        }
        return;
    }

    if (need_resume) {
        /*
         * handle_qmp_command() suspended the monitor because the
         * request queue filled up, to be resumed when the queue has
         * space again.  We just emptied it; resume the monitor.
         *
         * Without this, the monitor would remain suspended forever
         * when we get here while the monitor is suspended.  An
         * unfortunately timed CHR_EVENT_CLOSED can do the trick.
         */
        monitor_resume(&mon->common);
    }

}

void qmp_send_response(MonitorQMP *mon, const QDict *rsp)
{
    const QObject *data = QOBJECT(rsp);
    GString *json;

    json = qobject_to_json_pretty(data, mon->pretty);
    assert(json != NULL);
    trace_monitor_qmp_respond(mon, json->str);

    g_string_append_c(json, '\n');
    monitor_puts(&mon->common, json->str);

    g_string_free(json, true);
}

/*
 * Emit QMP response @rsp to @mon.
 * Null @rsp can only happen for commands with QCO_NO_SUCCESS_RESP.
 * Nothing is emitted then.
 */
static void monitor_qmp_respond(MonitorQMP *mon, QDict *rsp)
{
    if (rsp) {
        qmp_send_response(mon, rsp);
    }
}

/*
 * Runs outside of coroutine context for OOB commands, but in
 * coroutine context for everything else.
 */
static void monitor_qmp_dispatch(MonitorQMP *mon, QObject *req)
{
    QDict *rsp;
    QDict *error;

    rsp = qmp_dispatch(mon->commands, req, qmp_oob_enabled(mon),
                       &mon->common);

    if (mon->commands == &qmp_cap_negotiation_commands) {
        error = qdict_get_qdict(rsp, "error");
        if (error
            && !g_strcmp0(qdict_get_try_str(error, "class"),
                    QapiErrorClass_str(ERROR_CLASS_COMMAND_NOT_FOUND))) {
            /* Provide a more useful error message */
            qdict_del(error, "desc");
            qdict_put_str(error, "desc", "Expecting capabilities negotiation"
                          " with 'qmp_capabilities'");
        }
    } else if (mon->watch) {
        /* Name the whitelist when a command isn't on it. */
        QDict *dict = qobject_to(QDict, req);
        const char *name = dict ? qdict_get_try_str(dict, "execute") : NULL;

        error = qdict_get_qdict(rsp, "error");
        if (error && name
            && !g_strcmp0(qdict_get_try_str(error, "class"),
                    QapiErrorClass_str(ERROR_CLASS_COMMAND_NOT_FOUND))
            && !qmp_find_command(&qmp_watch_commands, name)) {
            g_autoptr(GString) desc = g_string_new(NULL);

            g_string_printf(desc, "The command %s is not available on a "
                            "watch monitor (XEMU_QMP_WATCH), which answers "
                            "only", name);
            for (size_t i = 0; i < ARRAY_SIZE(qmp_watch_names); i++) {
                g_string_append_printf(desc, "%s %s", i ? "," : "",
                                       qmp_watch_names[i]);
            }
            qdict_del(error, "desc");
            qdict_put_str(error, "desc", desc->str);
        }
    }

    monitor_qmp_respond(mon, rsp);
    qobject_unref(rsp);
}

/*
 * Pop a QMP request from a monitor request queue.
 * Return the request, or NULL all request queues are empty.
 * We are using round-robin fashion to pop the request, to avoid
 * processing commands only on a very busy monitor.  To achieve that,
 * when we process one request on a specific monitor, we put that
 * monitor to the end of mon_list queue.
 *
 * Note: if the function returned with non-NULL, then the caller will
 * be with qmp_mon->qmp_queue_lock held, and the caller is responsible
 * to release it.
 */
static QMPRequest *monitor_qmp_requests_pop_any_with_lock(void)
{
    QMPRequest *req_obj = NULL;
    Monitor *mon;
    MonitorQMP *qmp_mon;

    QTAILQ_FOREACH(mon, &mon_list, entry) {
        if (!monitor_is_qmp(mon)) {
            continue;
        }

        qmp_mon = container_of(mon, MonitorQMP, common);
        if (qmp_mon->watch) {
            continue;   /* served by its own BH, monitor_qmp_watch_bh */
        }
        qemu_mutex_lock(&qmp_mon->qmp_queue_lock);
        req_obj = g_queue_pop_head(qmp_mon->qmp_requests);
        if (req_obj) {
            /* With the lock of corresponding queue held */
            break;
        }
        qemu_mutex_unlock(&qmp_mon->qmp_queue_lock);
    }

    if (req_obj) {
        /*
         * We found one request on the monitor. Degrade this monitor's
         * priority to lowest by re-inserting it to end of queue.
         */
        QTAILQ_REMOVE(&mon_list, mon, entry);
        QTAILQ_INSERT_TAIL(&mon_list, mon, entry);
    }

    return req_obj;
}

static QMPRequest *monitor_qmp_dispatcher_pop_any(void)
{
    while (true) {
        /*
         * To avoid double scheduling, busy is true on entry to
         * monitor_qmp_dispatcher_co(), and must be set again before
         * aio_co_wake()-ing it.
         */
        assert(qatomic_read(&qmp_dispatcher_co_busy) == true);

        /*
         * Mark the dispatcher as not busy already here so that we
         * don't miss any new requests coming in the middle of our
         * processing.
         *
         * Clear qmp_dispatcher_co_busy before reading request.
         */
        qatomic_set_mb(&qmp_dispatcher_co_busy, false);

        WITH_QEMU_LOCK_GUARD(&monitor_lock) {
            QMPRequest *req_obj;

            /* On shutdown, don't take any more requests from the queue */
            if (qmp_dispatcher_co_shutdown) {
                return NULL;
            }

            req_obj = monitor_qmp_requests_pop_any_with_lock();
            if (req_obj) {
                return req_obj;
            }
        }

        /*
         * No more requests to process.  Wait to be reentered from
         * handle_qmp_command() when it pushes more requests, or
         * from monitor_cleanup() when it requests shutdown.
         */
        qemu_coroutine_yield();
    }
}

void coroutine_fn monitor_qmp_dispatcher_co(void *data)
{
    QMPRequest *req_obj;
    QDict *rsp;
    bool oob_enabled;
    MonitorQMP *mon;

    while ((req_obj = monitor_qmp_dispatcher_pop_any()) != NULL) {
        trace_monitor_qmp_in_band_dequeue(req_obj,
                                          req_obj->mon->qmp_requests->length);

        /*
         * @req_obj has a request, we hold req_obj->mon->qmp_queue_lock
         */

        mon = req_obj->mon;

        /*
         * We need to resume the monitor if handle_qmp_command()
         * suspended it.  Two cases:
         * 1. OOB enabled: mon->qmp_requests has no more space
         *    Resume right away, so that OOB commands can get executed while
         *    this request is being processed.
         * 2. OOB disabled: always
         *    Resume only after we're done processing the request,
         * We need to save qmp_oob_enabled() for later, because
         * qmp_qmp_capabilities() can change it.
         */
        oob_enabled = qmp_oob_enabled(mon);
        if (oob_enabled
            && mon->qmp_requests->length == QMP_REQ_QUEUE_LEN_MAX - 1) {
            monitor_resume(&mon->common);
        }

        /*
         * Drop the queue mutex now, before yielding, otherwise we might
         * deadlock if the main thread tries to lock it.
         */
        qemu_mutex_unlock(&mon->qmp_queue_lock);

        if (qatomic_xchg(&qmp_dispatcher_co_busy, true) == true) {
            /*
             * Someone rescheduled us (probably because a new requests
             * came in), but we didn't actually yield. Do that now,
             * only to be immediately reentered and removed from the
             * list of scheduled coroutines.
             */
            qemu_coroutine_yield();
        }

        /* Process request */
        if (req_obj->req) {
            if (trace_event_get_state(TRACE_MONITOR_QMP_CMD_IN_BAND)) {
                QDict *qdict = qobject_to(QDict, req_obj->req);
                QObject *id = qdict ? qdict_get(qdict, "id") : NULL;
                GString *id_json;

                id_json = id ? qobject_to_json(id) : g_string_new(NULL);
                trace_monitor_qmp_cmd_in_band(id_json->str);
                g_string_free(id_json, true);
            }
            monitor_qmp_dispatch(mon, req_obj->req);
        } else {
            assert(req_obj->err);
            trace_monitor_qmp_err_in_band(error_get_pretty(req_obj->err));
            rsp = qmp_error_response(req_obj->err);
            req_obj->err = NULL;
            monitor_qmp_respond(mon, rsp);
            qobject_unref(rsp);
        }

        if (!oob_enabled) {
            monitor_resume(&mon->common);
        }

        qmp_request_free(req_obj);
    }
    qatomic_set(&qmp_dispatcher_co, NULL);
}

void qmp_dispatcher_co_wake(void)
{
    /* Write request before reading qmp_dispatcher_co_busy.  */
    smp_mb__before_rmw();

    if (!qatomic_xchg(&qmp_dispatcher_co_busy, true) &&
            qatomic_read(&qmp_dispatcher_co)) {
        aio_co_wake(qmp_dispatcher_co);
    }
}

static void handle_qmp_command(void *opaque, QObject *req, Error *err)
{
    MonitorQMP *mon = opaque;
    QDict *qdict = qobject_to(QDict, req);
    QMPRequest *req_obj;

    assert(!req != !err);

    if (req && trace_event_get_state_backends(TRACE_HANDLE_QMP_COMMAND)) {
        GString *req_json = qobject_to_json(req);
        trace_handle_qmp_command(mon, req_json->str);
        g_string_free(req_json, true);
    }

    if (qdict && qmp_is_oob(qdict)) {
        /* OOB commands are executed immediately */
        if (trace_event_get_state(TRACE_MONITOR_QMP_CMD_OUT_OF_BAND)) {
            QObject *id = qdict_get(qdict, "id");
            GString *id_json;

            id_json = id ? qobject_to_json(id) : g_string_new(NULL);
            trace_monitor_qmp_cmd_out_of_band(id_json->str);
            g_string_free(id_json, true);
        }
        monitor_qmp_dispatch(mon, req);
        qobject_unref(req);
        return;
    }

    req_obj = g_new0(QMPRequest, 1);
    req_obj->mon = mon;
    req_obj->req = req;
    req_obj->err = err;

    /* Protect qmp_requests and fetching its length. */
    WITH_QEMU_LOCK_GUARD(&mon->qmp_queue_lock) {

        /*
         * Suspend the monitor when we can't queue more requests after
         * this one.  Dequeuing in monitor_qmp_dispatcher_co() or
         * monitor_qmp_cleanup_queue_and_resume() will resume it.
         * Note that when OOB is disabled, we queue at most one command,
         * for backward compatibility.
         */
        if (!qmp_oob_enabled(mon) ||
            mon->qmp_requests->length == QMP_REQ_QUEUE_LEN_MAX - 1) {
            monitor_suspend(&mon->common);
        }

        /*
         * Put the request to the end of queue so that requests will be
         * handled in time order.  Ownership for req_obj, req,
         * etc. will be delivered to the handler side.
         */
        trace_monitor_qmp_in_band_enqueue(req_obj, mon,
                                          mon->qmp_requests->length);
        assert(mon->qmp_requests->length < QMP_REQ_QUEUE_LEN_MAX);
        g_queue_push_tail(mon->qmp_requests, req_obj);
    }

    if (mon->watch) {
        qemu_bh_schedule(mon->watch_bh);
        return;
    }

    /* Kick the dispatcher routine */
    qmp_dispatcher_co_wake();
}

/*
 * xemu: a watch monitor's requests, in order, on the main loop's
 * iohandler context (BQL held; only the main loop polls it, so no nested
 * event loop in a block job or a vCPU wait runs this), outside any
 * coroutine and outside the dispatcher: the dispatcher may be parked in
 * another monitor's command (xemu-frame-advance) for as long as that
 * runs. Only qmp_watch_commands are found here, none a coroutine
 * command, so qmp_dispatch calls each handler directly.
 */
static void monitor_qmp_watch_bh(void *opaque)
{
    MonitorQMP *mon = opaque;
    QMPRequest *req_obj;

    for (;;) {
        WITH_QEMU_LOCK_GUARD(&mon->qmp_queue_lock) {
            req_obj = g_queue_pop_head(mon->qmp_requests);
        }
        if (!req_obj) {
            return;
        }
        if (req_obj->req) {
            /*
             * qmp_dispatch asserts that no monitor is current for this
             * (the main loop's) coroutine; a caller up the stack that set
             * one keeps it.
             */
            Monitor *outer = monitor_set_cur(qemu_coroutine_self(), NULL);

            monitor_qmp_dispatch(mon, req_obj->req);
            monitor_set_cur(qemu_coroutine_self(), outer);
        } else {
            QDict *rsp = qmp_error_response(req_obj->err);

            req_obj->err = NULL;
            monitor_qmp_respond(mon, rsp);
            qobject_unref(rsp);
        }
        /* handle_qmp_command suspended once for it (no OOB here). */
        monitor_resume(&mon->common);
        qmp_request_free(req_obj);
    }
}

/*
 * xemu: QMP input is read in chunks of up to MONITOR_QMP_READ_MAX bytes
 * instead of one byte per chardev read (monitor_can_read's 1), which
 * cost a main-loop or I/O-thread iteration per byte: a 70 KB
 * xemu-input-queue request took 0.9 s to arrive. The parser still gets
 * one byte at a time and stops at the byte where a request suspends the
 * monitor, exactly where the one-byte reads stopped; the rest waits in
 * in_pending and is parsed first when the monitor resumes, ahead of any
 * new read. So requests are queued and dispatched as before.
 *
 * Only the chardev context touches in_pending (see monitor-internal.h).
 * A connect or disconnect, which may run on another thread (a write that
 * fails on the main loop disconnects there), only bumps in_drop_gen; the
 * chardev context then stops feeding and drops what it read before.
 */
#define MONITOR_QMP_READ_MAX 4096

static int monitor_qmp_can_read(void *opaque)
{
    MonitorQMP *mon = opaque;

    return qatomic_read(&mon->common.suspend_cnt) ? 0 : MONITOR_QMP_READ_MAX;
}

static void monitor_qmp_feed(MonitorQMP *mon, unsigned gen,
                             const uint8_t *buf, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (qatomic_read(&mon->in_drop_gen) != gen) {
            return;   /* the connection ended under us: drop the rest */
        }
        if (qatomic_read(&mon->common.suspend_cnt)) {
            assert(!mon->in_pending);
            mon->in_pending = g_byte_array_sized_new(size - i);
            g_byte_array_append(mon->in_pending, buf + i, size - i);
            mon->in_pending_gen = gen;
            return;
        }
        json_message_parser_feed(&mon->parser, (const char *)buf + i, 1);
    }
}

/* Chardev context: feed what a suspend left over, unless it is stale. */
void monitor_qmp_feed_pending(MonitorQMP *mon)
{
    GByteArray *p = mon->in_pending;

    if (!p) {
        return;
    }
    mon->in_pending = NULL;
    if (mon->in_pending_gen == qatomic_read(&mon->in_drop_gen)) {
        monitor_qmp_feed(mon, mon->in_pending_gen, p->data, p->len);
    }
    g_byte_array_unref(p);
}

static void monitor_qmp_read(void *opaque, const uint8_t *buf, int size)
{
    MonitorQMP *mon = opaque;
    unsigned gen = qatomic_read(&mon->in_drop_gen);

    if (mon->in_pending) {
        /*
         * Read after a resume, before monitor_accept_input fed the rest:
         * the older bytes go first (or are dropped if stale).
         */
        if (mon->in_pending_gen == gen) {
            g_byte_array_append(mon->in_pending, buf, size);
            monitor_qmp_feed_pending(mon);
            return;
        }
        monitor_qmp_feed_pending(mon);
    }
    monitor_qmp_feed(mon, gen, buf, size);
}

static QDict *qmp_greeting(MonitorQMP *mon)
{
    QList *cap_list = qlist_new();
    QObject *ver = NULL;
    QDict *args;
    QMPCapability cap;

    args = qdict_new();
    qmp_marshal_query_version(args, &ver, NULL);
    qobject_unref(args);

    for (cap = 0; cap < QMP_CAPABILITY__MAX; cap++) {
        if (mon->capab_offered[cap]) {
            qlist_append_str(cap_list, QMPCapability_str(cap));
        }
    }

    return qdict_from_jsonf_nofail(
        "{'QMP': {'version': %p, 'capabilities': %p}}",
        ver, cap_list);
}

static void monitor_qmp_event(void *opaque, QEMUChrEvent event)
{
    QDict *data;
    MonitorQMP *mon = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        qatomic_inc(&mon->in_drop_gen);   /* xemu: drop bytes read before */
        mon->commands = &qmp_cap_negotiation_commands;
        monitor_qmp_caps_reset(mon);
        data = qmp_greeting(mon);
        qmp_send_response(mon, data);
        qobject_unref(data);
        break;
    case CHR_EVENT_CLOSED:
        /*
         * Note: this is only useful when the output of the chardev
         * backend is still open.  For example, when the backend is
         * stdio, it's possible that stdout is still open when stdin
         * is closed.
         */
        qatomic_inc(&mon->in_drop_gen);   /* xemu: drop bytes read before */
        monitor_qmp_cleanup_queue_and_resume(mon);
        json_message_parser_destroy(&mon->parser);
        json_message_parser_init(&mon->parser, handle_qmp_command,
                                 mon, NULL);
        monitor_fdsets_cleanup();
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;
    }
}

void monitor_data_destroy_qmp(MonitorQMP *mon)
{
    if (mon->watch_bh) {
        qemu_bh_delete(mon->watch_bh);
        mon->watch_bh = NULL;
    }
    /* The I/O thread has stopped (monitor_cleanup). */
    if (mon->in_pending) {
        g_byte_array_unref(mon->in_pending);
        mon->in_pending = NULL;
    }
    json_message_parser_destroy(&mon->parser);
    qemu_mutex_destroy(&mon->qmp_queue_lock);
    monitor_qmp_cleanup_req_queue_locked(mon);
    g_queue_free(mon->qmp_requests);
}

static void monitor_qmp_setup_handlers_bh(void *opaque)
{
    MonitorQMP *mon = opaque;
    GMainContext *context;

    assert(mon->common.use_io_thread);
    context = iothread_get_g_main_context(mon_iothread);
    assert(context);
    qemu_chr_fe_set_handlers(&mon->common.chr, monitor_qmp_can_read,
                             monitor_qmp_read, monitor_qmp_event,
                             NULL, &mon->common, context, true);
    monitor_list_append(&mon->common);
}

static void monitor_init_qmp_common(Chardev *chr, bool pretty, bool watch,
                                    Error **errp)
{
    MonitorQMP *mon = g_new0(MonitorQMP, 1);

    if (!qemu_chr_fe_init(&mon->common.chr, chr, errp)) {
        g_free(mon);
        return;
    }
    if (watch) {
        mon->watch = true;
        mon->watch_bh = aio_bh_new(iohandler_get_aio_context(),
                                   monitor_qmp_watch_bh, mon);
    }
    qemu_chr_fe_set_echo(&mon->common.chr, true);

    /* Note: we run QMP monitor in I/O thread when @chr supports that */
    monitor_data_init(&mon->common, true, false,
                      qemu_chr_has_feature(chr, QEMU_CHAR_FEATURE_GCONTEXT));

    mon->pretty = pretty;

    qemu_mutex_init(&mon->qmp_queue_lock);
    mon->qmp_requests = g_queue_new();

    json_message_parser_init(&mon->parser, handle_qmp_command, mon, NULL);
    if (mon->common.use_io_thread) {
        /*
         * Make sure the old iowatch is gone.  It's possible when
         * e.g. the chardev is in client mode, with wait=on.
         */
        remove_fd_in_watch(chr);
        /*
         * We can't call qemu_chr_fe_set_handlers() directly here
         * since chardev might be running in the monitor I/O
         * thread.  Schedule a bottom half.
         */
        aio_bh_schedule_oneshot(iothread_get_aio_context(mon_iothread),
                                monitor_qmp_setup_handlers_bh, mon);
        /* The bottom half will add @mon to @mon_list */
    } else {
        qemu_chr_fe_set_handlers(&mon->common.chr, monitor_qmp_can_read,
                                 monitor_qmp_read, monitor_qmp_event,
                                 NULL, &mon->common, NULL, true);
        monitor_list_append(&mon->common);
    }
}

void monitor_init_qmp(Chardev *chr, bool pretty, Error **errp)
{
    monitor_init_qmp_common(chr, pretty, false, errp);
}

/*
 * xemu: a QMP monitor that negotiates and then answers only
 * qmp_watch_commands, at once, whatever another monitor's command is
 * doing (XEMU_QMP_WATCH, monitor/xemu-qmp-watch.c).
 */
void monitor_init_qmp_watch(Chardev *chr, Error **errp)
{
    if (QTAILQ_EMPTY(&qmp_watch_commands)) {
        QTAILQ_INIT(&qmp_watch_commands);
        for (size_t i = 0; i < ARRAY_SIZE(qmp_watch_names); i++) {
            const QmpCommand *cmd = qmp_find_command(&qmp_commands,
                                                     qmp_watch_names[i]);

            /* The handlers run outside coroutine context. */
            assert(cmd && !(cmd->options & QCO_COROUTINE));
            qmp_register_command(&qmp_watch_commands, cmd->name, cmd->fn,
                                 cmd->options, cmd->features);
        }
    }
    monitor_init_qmp_common(chr, false, true, errp);
}
