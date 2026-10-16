/*
 * xemu: row logs written to a file (XEMU_BLOCK_LOG, XEMU_VOICE_LOG)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_FILELOG_H
#define XEMU_FILELOG_H

#include "qemu/thread.h"

/* The vblank counter of query-xemu-frame (ui/xemu.c): a row's frame. */
int64_t xemu_frame_count(void);

/*
 * One log. The owner fills the first four fields statically; the rest
 * belongs to the helpers below.
 */
typedef struct XemuFileLog {
    const char *what;               /* "block log", for error messages */
    const char *header;             /* first line of a new or empty file */
    QemuMutex *lock;                /* held around file access, or NULL */
    void (*before_close)(void);     /* runs first when the log closes */

    FILE *file;
    char *path;                     /* the last log's, kept after close */
    int64_t rows;                   /* the last log's rows */
    int64_t last_flush_ms;
    bool registered;
} XemuFileLog;

/*
 * Open @path (truncated unless @append; binary mode, so rows end in \n on
 * every host) and write the header if the file is empty. Fails while
 * @log is open. The file is flushed when the VM stops and closed at exit.
 */
bool xemu_filelog_open(XemuFileLog *log, const char *path, bool append,
                       Error **errp);

/*
 * Write one row (a whole line) if the log is open. The file is flushed
 * at a row 250 ms or more after its last flush.
 */
void xemu_filelog_printf(XemuFileLog *log, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

/*
 * Run before_close, then close the file. Fails when no log is open or a
 * write failed; path and rows still describe the closed log.
 */
bool xemu_filelog_close(XemuFileLog *log, Error **errp);

#endif
