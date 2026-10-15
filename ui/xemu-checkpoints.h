/*
 * xemu checkpoints: whole-machine states in host memory
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_CHECKPOINTS_H
#define XEMU_CHECKPOINTS_H

#include "system/block-backend.h"

/*
 * Before the guest writes sectors [sector, sector + count) of @blk: keep
 * what they hold now for every checkpoint that has no copy yet.
 */
void xemu_checkpoint_disk_prewrite(BlockBackend *blk, int64_t sector,
                                   int64_t count);

#endif
