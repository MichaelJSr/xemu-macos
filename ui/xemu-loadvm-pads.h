/*
 * XEMU_LOADVM_ADD_PADS=1 (ui/xemu-input-bind.c): loads that plug the pads
 * a snapshot names. No SDL here: migration/savevm.c and system/vl.c use it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_LOADVM_PADS_H
#define XEMU_LOADVM_PADS_H

/*
 * The load stopped at section idstr, which the machine lacks: plug the pad
 * or pad hub it names and return true, so that the load can start over.
 */
bool xemu_input_add_snapshot_pad(const char *idstr);

/*
 * CLI -loadvm: true when xemu takes it over, to load it (and then start
 * the VM when start) once the config's pads are bound, as a loadvm sent
 * after "xemu: ready" is. xemu_loadvm_run_deferred does it (BQL held).
 */
bool xemu_loadvm_defer(const char *name, bool start);
void xemu_loadvm_run_deferred(void);

#endif
