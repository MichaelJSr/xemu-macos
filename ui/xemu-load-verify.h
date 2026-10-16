/*
 * XEMU_LOAD_VERIFY=1: report guest RAM written between a load and the
 * VM's start
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_LOAD_VERIFY_H
#define XEMU_LOAD_VERIFY_H

/*
 * At the start of every load, before anything replaces RAM (a second call
 * before the end does nothing), and at the end of every successful one.
 */
void xemu_load_verify_loading(void);
void xemu_load_verify_loaded(void);

#endif
