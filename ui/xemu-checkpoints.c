/*
 * xemu checkpoints: whole-machine states in host memory
 *
 * QMP xemu-checkpoint-save/-restore/-delete and query-xemu-checkpoints.
 * A checkpoint is the savevm stream (RAM and devices) in a host buffer,
 * written and read by the savevm/loadvm code itself, so a restore is a
 * loadvm without the qcow2. loadvm also reverts the disks; here every
 * sector the guest writes after a checkpoint is kept as it was at the
 * checkpoint (xemu_checkpoint_disk_prewrite, from the IDE write paths) and
 * written back on restore.
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

#define SECTOR 512

typedef struct Checkpoint {
    char *name;
    uint8_t *state;
    size_t size;
    int64_t frame;
    /* BlockBackend * -> (sector -> its SECTOR bytes at the checkpoint) */
    GHashTable *disks;
} Checkpoint;

static GPtrArray *checkpoints;

int64_t xemu_frame_count(void);

static void checkpoint_free(gpointer p)
{
    Checkpoint *c = p;

    g_free(c->name);
    g_free(c->state);
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

static XemuCheckpointInfo *checkpoint_info(Checkpoint *c, int64_t elapsed_ns)
{
    XemuCheckpointInfo *info = g_new0(XemuCheckpointInfo, 1);
    GHashTableIter di;
    gpointer blk, table;

    info->name = g_strdup(c->name);
    info->size = c->size;
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

XemuCheckpointInfo *qmp_xemu_checkpoint_save(const char *name, Error **errp)
{
    int64_t t0 = get_clock();
    Checkpoint *old = checkpoint_find(name);
    size_t size = 0;
    /* A save is about the size of the last one, or of RAM plus devices. */
    size_t hint = old ? old->size + (1 << 20) : 160 * MiB;
    uint8_t *state = xemu_checkpoint_save_state(hint, &size, errp);

    if (!state) {
        return NULL;
    }
    if (!checkpoints) {
        checkpoints = g_ptr_array_new_with_free_func(checkpoint_free);
    }
    if (old) {
        g_ptr_array_remove(checkpoints, old);
    }

    Checkpoint *c = g_new0(Checkpoint, 1);
    c->name = g_strdup(name);
    c->state = state;
    c->size = size;
    c->frame = xemu_frame_count();
    c->disks = g_hash_table_new_full(NULL, NULL, NULL,
                                     (GDestroyNotify)g_hash_table_unref);
    g_ptr_array_add(checkpoints, c);
    return checkpoint_info(c, get_clock() - t0);
}

XemuCheckpointInfo *qmp_xemu_checkpoint_restore(const char *name,
                                                Error **errp)
{
    int64_t t0 = get_clock();
    Checkpoint *c = checkpoint_find(name);
    Revert r = { .c = c };

    if (!c) {
        error_setg(errp, "no checkpoint '%s'", name);
        return NULL;
    }
    if (!xemu_checkpoint_load_state(c->state, c->size,
                                    checkpoint_revert_disks, &r, errp)) {
        return NULL;
    }
    if (r.failed) {
        error_setg(errp, "checkpoint '%s': reverting the disk failed; the "
                   "machine is restored, its disk is not", name);
        return NULL;
    }
    return checkpoint_info(c, get_clock() - t0);
}

void qmp_xemu_checkpoint_delete(const char *name, Error **errp)
{
    Checkpoint *c = checkpoint_find(name);

    if (!c) {
        error_setg(errp, "no checkpoint '%s'", name);
        return;
    }
    g_ptr_array_remove(checkpoints, c);
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
