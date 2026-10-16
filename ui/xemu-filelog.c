/*
 * xemu: row logs written to a file (XEMU_BLOCK_LOG, XEMU_VOICE_LOG)
 *
 * The file plumbing the per-drive I/O log and the voice log share: open
 * (append or truncate, a header line in an empty file), rows flushed every
 * 250 ms of host time at most, a flush when the VM stops, a close at exit,
 * and the {filename, rows} their QMP stop commands return. Callers that
 * write rows from several threads give the log a lock.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "system/runstate.h"
#include "xemu-filelog.h"

#define FILELOG_FLUSH_MS 250

static GSList *filelogs;            /* every log ever opened */

static void filelog_lock(XemuFileLog *log)
{
    if (log->lock) {
        qemu_mutex_lock(log->lock);
    }
}

static void filelog_unlock(XemuFileLog *log)
{
    if (log->lock) {
        qemu_mutex_unlock(log->lock);
    }
}

static void filelog_vm_state(void *opaque, bool running, RunState state)
{
    XemuFileLog *log = opaque;

    if (!running) {
        filelog_lock(log);
        if (log->file) {
            fflush(log->file);
        }
        filelog_unlock(log);
    }
}

static void filelog_exit(void)
{
    for (GSList *l = filelogs; l; l = l->next) {
        XemuFileLog *log = l->data;

        if (log->file) {
            xemu_filelog_close(log, NULL);
        }
    }
}

bool xemu_filelog_open(XemuFileLog *log, const char *path, bool append,
                       Error **errp)
{
    FILE *f;

    if (log->file) {
        error_setg(errp, "a %s is already open: '%s'", log->what, log->path);
        return false;
    }
    f = fopen(path, append ? "ab" : "wb");
    if (!f) {
        error_setg_errno(errp, errno, "can't open %s '%s'", log->what, path);
        return false;
    }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) <= 0) {
        fputs(log->header, f);
    }
    if (!log->registered) {
        qemu_add_vm_change_state_handler(filelog_vm_state, log);
        if (!filelogs) {
            atexit(filelog_exit);
        }
        filelogs = g_slist_prepend(filelogs, log);
        log->registered = true;
    }
    filelog_lock(log);
    g_free(log->path);
    log->path = g_strdup(path);
    log->rows = 0;
    log->file = f;
    filelog_unlock(log);
    return true;
}

void xemu_filelog_printf(XemuFileLog *log, const char *fmt, ...)
{
    va_list ap;

    filelog_lock(log);
    if (log->file) {
        int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        va_start(ap, fmt);
        vfprintf(log->file, fmt, ap);
        va_end(ap);
        log->rows++;
        /*
         * No QEMU timer: while one is pending, every main-loop wakeup arms
         * a kqueue timer, which costs more host time than the rows.
         */
        if (ms - log->last_flush_ms >= FILELOG_FLUSH_MS) {
            fflush(log->file);
            log->last_flush_ms = ms;
        }
    }
    filelog_unlock(log);
}

bool xemu_filelog_close(XemuFileLog *log, Error **errp)
{
    bool ok;

    if (!log->file) {
        error_setg(errp, "no %s is open", log->what);
        return false;
    }
    if (log->before_close) {
        log->before_close();
    }
    filelog_lock(log);
    ok = !ferror(log->file);
    ok &= fclose(log->file) == 0;
    log->file = NULL;
    filelog_unlock(log);
    if (!ok) {
        error_setg(errp, "writing %s '%s' failed", log->what, log->path);
    }
    return ok;
}
