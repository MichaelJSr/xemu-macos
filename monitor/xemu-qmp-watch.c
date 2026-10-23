/*
 * xemu: XEMU_QMP_WATCH, a second QMP monitor for read-only queries
 *
 *   XEMU_QMP_WATCH=unix:/tmp/w.sock,server=on,wait=off   # what -qmp takes
 *
 * Every QMP monitor's commands run one at a time in the dispatcher
 * coroutine, and xemu-frame-advance, xemu-run-until and xemu-run hold it
 * while they wait for vblanks, so a client on another -qmp socket gets its
 * greeting but no reply until the advance ends. A watch monitor answers
 * at once instead: its requests run from the main loop (BQL held),
 * outside the dispatcher, and it answers only the read-only queries in
 * qmp_watch_commands (monitor/qmp.c); any other command gets an error
 * that names them. Commands on the other monitors keep their order. It
 * receives events like any QMP monitor. Opt-in: nothing opens without the
 * variable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "chardev/char.h"
#include "monitor-internal.h"
#include "qapi/error.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "qemu/error-report.h"
#include "monitor/xemu-qmp-watch.h"

void xemu_qmp_watch_open(void)
{
    const char *spec = g_getenv("XEMU_QMP_WATCH");
    Error *err = NULL;
    Chardev *chr = NULL;
    QemuOpts *opts;

    if (!spec || !spec[0]) {
        return;
    }
    opts = qemu_chr_parse_compat("xemu-qmp-watch", spec, false);
    if (!opts) {
        error_setg(&err, "not a -qmp argument");
    } else {
        /* A server socket waits for its first client unless told not to. */
        if (qemu_opt_get_bool(opts, "server", false)) {
            qemu_opt_set(opts, "wait", "off", &error_abort);
        }
        chr = qemu_chr_new_from_opts(opts, NULL, &err);
        qemu_opts_del(opts);
    }
    if (chr) {
        monitor_init_qmp_watch(chr, &err);
    }
    if (err) {
        /* As a -qmp that can't open: the harness asked for it. */
        error_reportf_err(err, "XEMU_QMP_WATCH=%s: ", spec);
        exit(1);
    }
    fprintf(stderr, "xemu: QMP watch monitor on %s\n", spec);
}
