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

/* Guest RAM images (xemu-checkpoint-ram.c). */
typedef struct XemuRamImage XemuRamImage;

typedef struct XemuRamStats {
    uint64_t checked;   /* pages compared */
    uint64_t differ;    /* pages that differed: stored or written back */
    uint64_t missed;    /* with verify: differing pages the log missed */
} XemuRamStats;

/*
 * Copy guest RAM while the VM is stopped. With @base, share its pages
 * where RAM still holds them and copy only the others: those the dirty
 * log says may differ (plus, with @verify, every other page compared,
 * counted in @st->missed).
 */
XemuRamImage *xemu_ram_image_capture(XemuRamImage *base, bool verify,
                                     XemuRamStats *st);
/*
 * Write @img into guest RAM, after the reset of a restore: every page, or
 * with @incremental only the pages the dirty log says may differ (plus,
 * with @verify, every other page compared, counted in @st->missed).
 */
void xemu_ram_image_load(XemuRamImage *img, bool incremental, bool verify,
                         XemuRamStats *st);
XemuRamImage *xemu_ram_image_ref(XemuRamImage *img);
void xemu_ram_image_unref(XemuRamImage *img);
uint64_t xemu_ram_image_own_pages(XemuRamImage *img);
size_t xemu_ram_image_page_size(void);
/* @img's pages of block @rb (NULL entries are zero pages), or NULL. */
const uint8_t *const *xemu_ram_image_block(XemuRamImage *img, RAMBlock *rb,
                                           uint64_t *pages);

/*
 * After a checkpoint goes: move the page copies other images still use
 * out of chunks that are at least half unused, and free those chunks.
 */
void xemu_ram_images_compact(void);
/*
 * The dirty log for incremental restores and saves against a base; stop
 * it when the last checkpoint goes (which also drops RAM's image).
 */
void xemu_ram_log_start(void);
void xemu_ram_log_stop(void);
/* Around a checkpoint restore's load: a failed one leaves RAM unknown. */
void xemu_ram_restore_begin(void);
void xemu_ram_restore_end(bool ok);
/* From every qemu_loadvm_state: other loads leave RAM unknown. */
void xemu_checkpoint_ram_loading(void);

/* The RAM image checkpoint @name holds, or NULL. */
XemuRamImage *xemu_checkpoint_ram(const char *name);

#endif
