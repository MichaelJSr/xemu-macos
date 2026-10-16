/*
 * xemu.toml's [automation] table: endpoints a tool on the player's machine
 * reads a running game through (shared guest RAM, a QMP monitor), turned
 * on from the player's own config so a Dock launch needs no arguments.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_AUTOMATION_H
#define XEMU_AUTOMATION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decide the table and open its QMP monitor. qemu_init calls it once the
 * command line's monitors exist and before the board (guest RAM) is built.
 */
void xemu_automation_apply(void);

/*
 * The shared guest RAM name to use: XEMU_RAM_SHM when set and non-empty,
 * else the table's ram_shm when the table applies, else NULL. *from_toml
 * says which.
 */
const char *xemu_automation_ram_shm(bool *from_toml);

/*
 * An endpoint that did not open: one stderr line (already formatted by
 * the caller) and an in-app notification; xemu keeps running.
 */
void xemu_automation_skipped(const char *line, const char *notification);

#ifdef __cplusplus
}
#endif

#endif
