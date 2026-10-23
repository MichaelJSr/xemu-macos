/*
 * xemu: XEMU_QMP_WATCH, a second QMP monitor for read-only queries
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_QMP_WATCH_H
#define XEMU_QMP_WATCH_H

/*
 * Open the watch monitor XEMU_QMP_WATCH names (a -qmp argument), after the
 * command line's monitors; exits on a spec that can't open. No-op unset.
 */
void xemu_qmp_watch_open(void);

#endif
