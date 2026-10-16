/*
 * xemu: per-drive I/O log (XEMU_BLOCK_LOG, QMP xemu-block-log-start/-stop)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_IDE_XEMU_BLOCK_LOG_H
#define HW_IDE_XEMU_BLOCK_LOG_H

#include "hw/ide/ide-dev.h"

/* A log is open. The hooks cost one branch on it per command otherwise. */
extern bool xemu_block_log_on;

/* An ATA command is about to run (ide_bus_exec_cmd, before its handler). */
void xemu_block_log_ata(IDEState *s, uint8_t cmd);
/* An ATAPI packet command is about to run (ide_atapi_cmd). */
void xemu_block_log_atapi(IDEState *s, const uint8_t *packet);
/* The drive's command ended (ide_cmd_done). */
void xemu_block_log_done(IDEState *s);

#endif
