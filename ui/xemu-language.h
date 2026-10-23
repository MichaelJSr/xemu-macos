/*
 * xemu: the console language for a run (xemu-language.c)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_LANGUAGE_H
#define XEMU_LANGUAGE_H

/* After every load: the requested language into the kernel's EEPROM copy. */
void xemu_language_state_loaded(void);

#endif
