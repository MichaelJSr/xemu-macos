/*
 * xemu User Interface - emulated hard disk access for the drive explorer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XBOX_HDD_BLK_H
#define XBOX_HDD_BLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool xemu_hdd_available(void);
bool xemu_hdd_writable(void);
bool xemu_hdd_read(uint64_t offset, void *buf, size_t len);
bool xemu_hdd_write(uint64_t offset, const void *buf, size_t len);
uint64_t xemu_hdd_size(void);
void xemu_hdd_flush(void);
/* Writes issued so far: whether an operation changed the disk */
uint64_t xemu_hdd_write_count(void);

/* While a write job runs over several frames: snapshot saves are refused
 * and a resume is undone before the guest runs */
void xemu_hdd_job_begin(void);
void xemu_hdd_job_end(void);
/* Whether a resume was undone since the last call */
bool xemu_hdd_take_resume_refused(void);
/* Snapshot loads so far (each one reverts the disk) */
unsigned xemu_hdd_snapshot_loads(void);
/* fn runs at shutdown, with the BQL, while the block layer is still open */
void xemu_hdd_on_shutdown(void (*fn)(void));

bool xemu_xbox_running(void);
void xemu_xbox_pause(void);
void xemu_xbox_resume(void);
/* Reset while stopped, then start if resume */
void xemu_xbox_restart(bool resume);

#ifdef __cplusplus
}
#endif

#endif
