/*
 * xemu checkpoints: whole-machine states in host memory
 *
 * QMP xemu-checkpoint-save/-restore/-delete and query-xemu-checkpoints.
 * A checkpoint is the device sections of a savevm stream in a host
 * buffer, written and read by the savevm/loadvm code itself, and an image
 * of guest RAM (xemu-checkpoint-ram.c) copied and written back where that
 * code would put RAM in the stream, so a restore is a loadvm without the
 * qcow2. An image saved against a base holds only the pages that differ
 * from it; an incremental restore writes back only the pages the dirty
 * log says may differ. loadvm also reverts the disks; here every sector
 * the guest writes after a checkpoint is kept as it was at the checkpoint
 * (xemu_checkpoint_disk_prewrite, from the IDE write paths) and written
 * back on restore.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "migration/snapshot.h"
#include "system/block-backend.h"
#include "block/block.h"
#include "xemu-checkpoints.h"
#include "xemu-events.h"

#define SECTOR 512

typedef struct Checkpoint {
    char *name;
    uint8_t *state;         /* the device sections, mmap'd (keep_state) */
    size_t size;
    XemuRamImage *ram;
    char *base;             /* saved against it, if not NULL */
    int64_t frame;
    /* BlockBackend * -> (sector -> its SECTOR bytes at the checkpoint) */
    GHashTable *disks;
} Checkpoint;

static GPtrArray *checkpoints;

int64_t xemu_frame_count(void);

/*
 * A state's mapping: whole host pages, as qemu_ram_mmap's unmap of its
 * alignment slack after the mapping's end needs (munmap refuses an address
 * off a page boundary; on Linux each save would leave up to 2 MiB reserved).
 */
static size_t state_map_size(size_t size)
{
    return ROUND_UP(size, qemu_real_host_page_size());
}

static void checkpoint_free(gpointer p)
{
    Checkpoint *c = p;

    g_free(c->name);
    qemu_anon_ram_free(c->state, state_map_size(c->size));
    xemu_ram_image_unref(c->ram);
    g_free(c->base);
    g_hash_table_unref(c->disks);
    g_free(c);
}

static Checkpoint *checkpoint_find(const char *name)
{
    for (guint i = 0; checkpoints && i < checkpoints->len; i++) {
        Checkpoint *c = g_ptr_array_index(checkpoints, i);
        if (!strcmp(c->name, name)) {
            return c;
        }
    }
    return NULL;
}

static GHashTable *checkpoint_disk(Checkpoint *c, BlockBackend *blk)
{
    GHashTable *t = g_hash_table_lookup(c->disks, blk);
    if (!t) {
        t = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
        g_hash_table_insert(c->disks, blk, t);
    }
    return t;
}

static void journal_add(GHashTable *t, int64_t sector, const uint8_t *data)
{
    int64_t *key = g_new(int64_t, 1);
    *key = sector;
    g_hash_table_insert(t, key, g_memdup2(data, SECTOR));
}

void xemu_checkpoint_disk_prewrite(BlockBackend *blk, int64_t sector,
                                   int64_t count)
{
    g_autofree uint8_t *old = NULL;

    if (!checkpoints || !checkpoints->len || count <= 0) {
        return;
    }
    for (guint i = 0; i < checkpoints->len; i++) {
        GHashTable *t = checkpoint_disk(g_ptr_array_index(checkpoints, i),
                                        blk);
        for (int64_t s = sector; s < sector + count; s++) {
            if (g_hash_table_contains(t, &s)) {
                continue;
            }
            if (!old) {
                old = g_malloc(count * SECTOR);
                if (blk_pread(blk, sector * SECTOR, count * SECTOR, old,
                              0) < 0) {
                    /* Unreadable: the write would fail the same way. */
                    return;
                }
            }
            journal_add(t, s, old + (s - sector) * SECTOR);
        }
    }
}

typedef struct Revert {
    Checkpoint *c;
    int64_t sectors;
    bool failed;
    bool incremental, verify;
    XemuRamStats ram;
} Revert;

static gint cmp_sector(gconstpointer a, gconstpointer b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/*
 * Write @c's sectors of @blk back, a run of contiguous ones at a time. A
 * checkpoint without a copy of one keeps what it holds now first: the
 * sector has not changed since that checkpoint.
 */
static bool revert_disk(Checkpoint *c, BlockBackend *blk, GHashTable *table,
                        int64_t *count)
{
    g_autoptr(GArray) sectors = g_array_sized_new(false, false,
                                                  sizeof(int64_t),
                                                  g_hash_table_size(table));
    GHashTableIter si;
    gpointer key;

    g_hash_table_iter_init(&si, table);
    while (g_hash_table_iter_next(&si, &key, NULL)) {
        g_array_append_val(sectors, *(int64_t *)key);
    }
    g_array_sort(sectors, cmp_sector);

    for (guint i = 0; i < sectors->len;) {
        int64_t first = g_array_index(sectors, int64_t, i);
        guint n = 1;
        while (i + n < sectors->len &&
               g_array_index(sectors, int64_t, i + n) == first + n) {
            n++;
        }
        g_autofree uint8_t *cur = NULL;
        g_autofree uint8_t *buf = g_malloc(n * SECTOR);

        for (guint k = 0; k < checkpoints->len; k++) {
            Checkpoint *o = g_ptr_array_index(checkpoints, k);
            GHashTable *t;
            if (o == c) {
                continue;
            }
            t = checkpoint_disk(o, blk);
            for (guint j = 0; j < n; j++) {
                int64_t s = first + j;
                if (g_hash_table_contains(t, &s)) {
                    continue;
                }
                if (!cur) {
                    cur = g_malloc(n * SECTOR);
                    if (blk_pread(blk, first * SECTOR, n * SECTOR, cur,
                                  0) < 0) {
                        return false;
                    }
                }
                journal_add(t, s, cur + j * SECTOR);
            }
        }
        for (guint j = 0; j < n; j++) {
            int64_t s = first + j;
            memcpy(buf + j * SECTOR, g_hash_table_lookup(table, &s), SECTOR);
        }
        if (blk_pwrite(blk, first * SECTOR, n * SECTOR, buf, 0) < 0) {
            return false;
        }
        *count += n;
        i += n;
    }
    return !sectors->len || blk_flush(blk) >= 0;
}

static void checkpoint_revert_disks(void *opaque)
{
    Revert *r = opaque;
    GHashTableIter di;
    gpointer blk, table;

    g_hash_table_iter_init(&di, r->c->disks);
    while (g_hash_table_iter_next(&di, &blk, &table)) {
        if (!revert_disk(r->c, blk, table, &r->sectors)) {
            r->failed = true;
            return;
        }
    }
}

static void checkpoint_load_ram(void *opaque)
{
    Revert *r = opaque;

    xemu_ram_image_load(r->c->ram, r->incremental, r->verify, &r->ram);
}

static XemuCheckpointInfo *checkpoint_info(Checkpoint *c, int64_t elapsed_ns)
{
    XemuCheckpointInfo *info = g_new0(XemuCheckpointInfo, 1);
    GHashTableIter di;
    gpointer blk, table;
    uint64_t pages = xemu_ram_image_own_pages(c->ram);

    info->name = g_strdup(c->name);
    info->size = c->size + pages * xemu_ram_image_page_size();
    info->ram_pages = pages;
    info->base = g_strdup(c->base);
    info->frame = c->frame;
    g_hash_table_iter_init(&di, c->disks);
    while (g_hash_table_iter_next(&di, &blk, &table)) {
        info->disk_sectors += g_hash_table_size(table);
    }
    if (elapsed_ns >= 0) {
        info->has_elapsed_us = true;
        info->elapsed_us = elapsed_ns / SCALE_US;
    }
    return info;
}

typedef struct Capture {
    XemuRamImage *base;
    bool verify;
    XemuRamImage *ram;
    XemuRamStats st;
} Capture;

/*
 * Move a save's device sections from the heap into mmap'd memory, which a
 * delete gives back: Apple's malloc keeps a freed large block resident for
 * good (deleted checkpoints left about 7 MiB each). NULL if out of memory.
 */
static uint8_t *keep_state(uint8_t *heap, size_t size)
{
    uint8_t *kept = qemu_anon_ram_alloc(state_map_size(size), NULL, false,
                                        false);

    if (kept) {
        memcpy(kept, heap, size);
    }
    g_free(heap);
    return kept;
}

static void checkpoint_save_ram(void *opaque)
{
    Capture *cap = opaque;

    cap->ram = xemu_ram_image_capture(cap->base, cap->verify, &cap->st);
}

XemuCheckpointInfo *qmp_xemu_checkpoint_save(const char *name,
                                             const char *base,
                                             bool has_verify, bool verify,
                                             Error **errp)
{
    int64_t t0 = get_clock();
    Checkpoint *old = checkpoint_find(name);
    Checkpoint *parent = base ? checkpoint_find(base) : NULL;
    Capture cap = { .verify = verify };
    size_t size = 0;
    /* A save is about the size of the last one's device sections. */
    static size_t last_size = 3 * MiB;
    size_t hint = (old ? old->size : last_size) + MiB;
    uint8_t *state;
    XemuCheckpointInfo *info;

    if (base && !parent) {
        error_setg(errp, "no checkpoint '%s'", base);
        return NULL;
    }
    if (parent) {
        /* Later saves against a base, and restores, compare less. */
        xemu_ram_log_start();
        cap.base = parent->ram;
    }
    state = xemu_checkpoint_save_state(hint, &size, checkpoint_save_ram, &cap,
                                       errp);
    if (state && !(state = keep_state(state, size))) {
        error_setg(errp, "out of memory for a %zu-byte checkpoint", size);
    }
    if (!state) {
        xemu_ram_image_unref(cap.ram);
        return NULL;
    }
    last_size = size;
    if (!checkpoints) {
        checkpoints = g_ptr_array_new_with_free_func(checkpoint_free);
    }
    if (old) {
        g_ptr_array_remove(checkpoints, old);
        xemu_ram_images_compact();
    }

    Checkpoint *c = g_new0(Checkpoint, 1);
    c->name = g_strdup(name);
    c->state = state;
    c->size = size;
    c->ram = cap.ram;
    c->base = g_strdup(base);
    c->frame = xemu_frame_count();
    c->disks = g_hash_table_new_full(NULL, NULL, NULL,
                                     (GDestroyNotify)g_hash_table_unref);
    g_ptr_array_add(checkpoints, c);
    info = checkpoint_info(c, get_clock() - t0);
    info->has_pages_checked = true;
    info->pages_checked = cap.st.checked;
    if (base && verify) {
        info->has_pages_missed = true;
        info->pages_missed = cap.st.missed;
    }
    return info;
}

XemuCheckpointInfo *qmp_xemu_checkpoint_restore(const char *name,
                                                bool has_incremental,
                                                bool incremental,
                                                bool has_verify, bool verify,
                                                Error **errp)
{
    int64_t t0 = get_clock();
    Checkpoint *c = checkpoint_find(name);
    Revert r = { .c = c, .incremental = incremental, .verify = verify };
    XemuCheckpointInfo *info;
    bool ok;

    if (!c) {
        error_setg(errp, "no checkpoint '%s'", name);
        return NULL;
    }
    if (incremental) {
        xemu_ram_log_start();
    }
    /* Names the load in XEMU_STATE_LOADED; the load itself resets it. */
    xemu_events_load_source(XEMU_LOAD_SOURCE_CHECKPOINT, name);
    xemu_ram_restore_begin();
    ok = xemu_checkpoint_load_state(c->state, c->size,
                                    checkpoint_revert_disks,
                                    checkpoint_load_ram, &r, errp);
    xemu_ram_restore_end(ok);
    if (!ok) {
        xemu_events_load_source(XEMU_LOAD_SOURCE_INCOMING, NULL);
        return NULL;
    }
    if (r.failed) {
        error_setg(errp, "checkpoint '%s': reverting the disk failed; the "
                   "machine is restored, its disk is not", name);
        return NULL;
    }
    info = checkpoint_info(c, get_clock() - t0);
    info->has_pages_checked = info->has_pages_copied = true;
    info->pages_checked = r.ram.checked;
    info->pages_copied = r.ram.differ;
    if (incremental && verify) {
        info->has_pages_missed = true;
        info->pages_missed = r.ram.missed;
    }
    return info;
}

void qmp_xemu_checkpoint_delete(const char *name, Error **errp)
{
    Checkpoint *c = checkpoint_find(name);

    if (!c) {
        error_setg(errp, "no checkpoint '%s'", name);
        return;
    }
    g_ptr_array_remove(checkpoints, c);
    if (!checkpoints->len) {
        xemu_ram_log_stop();
    }
    xemu_ram_images_compact();
}

XemuCheckpointInfoList *qmp_query_xemu_checkpoints(Error **errp)
{
    XemuCheckpointInfoList *list = NULL, **tail = &list;

    for (guint i = 0; checkpoints && i < checkpoints->len; i++) {
        QAPI_LIST_APPEND(tail, checkpoint_info(g_ptr_array_index(checkpoints,
                                                                 i), -1));
    }
    return list;
}

XemuRamImage *xemu_checkpoint_ram(const char *name)
{
    Checkpoint *c = checkpoint_find(name);

    return c ? c->ram : NULL;
}
