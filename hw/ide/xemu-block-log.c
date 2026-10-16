/*
 * xemu: per-drive I/O log
 *
 * XEMU_BLOCK_LOG=<file> (from launch) or QMP xemu-block-log-start/-stop
 * write one tab-separated row per IDE or ATAPI command that reads, writes,
 * flushes or discards, when the command ends:
 *
 *   frame  drive  op  lba  sectors  bytes  status  cmd  guest_ns
 *
 * frame (the vblank counter of query-xemu-frame) and guest_ns
 * (QEMU_CLOCK_VIRTUAL) are read when the command is issued. lba and sectors
 * are what the command asked for, in the drive's sectors (512 bytes on the
 * HDD, 2048 on the DVD). bytes is what query-blockstats counted for it (the
 * drive's byte counters across the command), so over any window the rows'
 * bytes add up to the blockstats deltas, and a row whose bytes differ from
 * sectors x sector size moved less than it asked for. A command that moves
 * no data and flushes nothing gets a row only if blockstats counted bytes
 * for it (an ATAPI reply sent by DMA: op r, lba -1). No data is logged.
 *
 * Everything runs under the BQL: the IDE hooks, QMP and the run-state
 * handler. The file (ui/xemu-filelog.c) is flushed when the VM stops, when
 * the log stops, at exit, and at a row 250 ms or more after the last
 * flush. Nothing here changes what the guest sees.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "system/block-backend.h"
#include "ide-internal.h"
#include "ui/xemu-filelog.h"
#include "xemu-block-log.h"

typedef struct BlockLogRow {
    IDEState *s;                        /* the drive; NULL: slot free */
    int64_t frame, guest_ns, lba, sectors;
    uint64_t base[BLOCK_MAX_IOTYPE];    /* its blockstats bytes at issue */
    char op;                            /* r w f(lush) d(iscard), 0: other */
    uint8_t cmd;
} BlockLogRow;

static void block_log_before_close(void);

bool xemu_block_log_on;
static XemuFileLog block_log = {
    .what = "block log",
    .header = "frame\tdrive\top\tlba\tsectors\tbytes\tstatus\tcmd\t"
              "guest_ns\n",
    .before_close = block_log_before_close,
};
static BlockLogRow open_rows[8];        /* one command per drive at a time */

static void drive_bytes(IDEState *s, uint64_t *out)
{
    BlockAcctStats *stats = blk_get_stats(s->blk);

    QEMU_LOCK_GUARD(&stats->lock);
    memcpy(out, stats->nr_bytes, sizeof(stats->nr_bytes));
}

static BlockLogRow *row_of(IDEState *s, bool alloc)
{
    BlockLogRow *free_row = NULL;

    for (int i = 0; i < ARRAY_SIZE(open_rows); i++) {
        if (open_rows[i].s == s) {
            return &open_rows[i];
        }
        if (!open_rows[i].s && !free_row) {
            free_row = &open_rows[i];
        }
    }
    return alloc ? free_row : NULL;
}

static void row_write(BlockLogRow *r, const char *status)
{
    IDEState *s = r->s;
    uint64_t now[BLOCK_MAX_IOTYPE], rd, wr, unmap;
    int64_t lba = r->lba, sectors = r->sectors;
    const char *drive = blk_name(s->blk);
    char op = r->op, name[32];

    r->s = NULL;
    drive_bytes(s, now);
    rd = now[BLOCK_ACCT_READ] - r->base[BLOCK_ACCT_READ];
    wr = now[BLOCK_ACCT_WRITE] - r->base[BLOCK_ACCT_WRITE];
    unmap = now[BLOCK_ACCT_UNMAP] - r->base[BLOCK_ACCT_UNMAP];
    if (!op) {
        if (!rd && !wr && !unmap) {
            return;
        }
        op = wr ? 'w' : unmap ? 'd' : 'r';
        lba = -1;
        sectors = 0;
    }
    if (!drive[0]) {
        snprintf(name, sizeof(name), "ide%d.%d", s->bus->bus_id, s->unit);
        drive = name;
    }
    xemu_filelog_printf(&block_log, "%" PRId64 "\t%s\t%c\t%" PRId64 "\t%" PRId64
                        "\t%" PRIu64 "\t%s\t%02x\t%" PRId64 "\n", r->frame,
                        drive, op, lba, sectors, rd + wr + unmap, status,
                        r->cmd, r->guest_ns);
}

/* The drive's last command never signalled its end (a reset, an error). */
static void row_close_stale(BlockLogRow *r, IDEState *s)
{
    if (r->s) {
        row_write(r, (s->status & ERR_STAT) ? "err" : "open");
    }
}

static void row_open(BlockLogRow *r, IDEState *s, uint8_t cmd, char op)
{
    r->s = s;
    r->cmd = cmd;
    r->op = op;
    r->lba = -1;
    r->sectors = 0;
    r->frame = xemu_frame_count();
    r->guest_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    drive_bytes(s, r->base);
}

/* What ide_cmd_lba48_transform and ide_get_sector will make of the regs. */
static void ata_range(IDEState *s, bool lba48, int64_t *lba, int64_t *count)
{
    if (!lba48) {
        *count = s->nsector ? s->nsector : 256;
    } else if (!s->nsector && !s->hob_nsector) {
        *count = 65536;
    } else {
        *count = (s->hob_nsector << 8) | s->nsector;
    }
    if (!(s->select & ATA_DEV_LBA)) {
        *lba = ((s->hcyl << 8) | s->lcyl) * s->heads * s->sectors +
               (s->select & ATA_DEV_HS) * s->sectors + (s->sector - 1);
    } else if (lba48) {
        *lba = ((int64_t)s->hob_hcyl << 40) | ((int64_t)s->hob_lcyl << 32) |
               ((int64_t)s->hob_sector << 24) | ((int64_t)s->hcyl << 16) |
               ((int64_t)s->lcyl << 8) | s->sector;
    } else {
        *lba = ((s->select & ATA_DEV_LBA_MSB) << 24) | (s->hcyl << 16) |
               (s->lcyl << 8) | s->sector;
    }
}

void xemu_block_log_ata(IDEState *s, uint8_t cmd)
{
    BlockLogRow *r = s->blk ? row_of(s, true) : NULL;
    bool lba48 = false;
    char op = 0;

    if (!r) {
        return;
    }
    row_close_stale(r, s);
    if (cmd == WIN_PACKETCMD) {
        return;                     /* the packet opens the row */
    }
    switch (cmd) {
    case WIN_READ_EXT:
    case WIN_READDMA_EXT:
    case WIN_MULTREAD_EXT:
        lba48 = true;
        /* fall through */
    case WIN_READ:
    case WIN_READ_ONCE:
    case WIN_READDMA:
    case WIN_READDMA_ONCE:
    case WIN_MULTREAD:
        op = 'r';
        break;
    case WIN_WRITE_EXT:
    case WIN_WRITEDMA_EXT:
    case WIN_MULTWRITE_EXT:
        lba48 = true;
        /* fall through */
    case WIN_WRITE:
    case WIN_WRITE_ONCE:
    case WIN_WRITE_VERIFY:
    case WIN_WRITEDMA:
    case WIN_WRITEDMA_ONCE:
    case WIN_MULTWRITE:
    case CFA_WRITE_SECT_WO_ERASE:
    case CFA_WRITE_MULTI_WO_ERASE:
        op = 'w';
        break;
    case WIN_FLUSH_CACHE:
    case WIN_FLUSH_CACHE_EXT:
        op = 'f';
        break;
    case WIN_DSM:
        op = 'd';
        break;
    }
    row_open(r, s, cmd, op);
    if (op == 'r' || op == 'w') {
        ata_range(s, lba48, &r->lba, &r->sectors);
    }
}

void xemu_block_log_atapi(IDEState *s, const uint8_t *buf)
{
    BlockLogRow *r = s->blk ? row_of(s, true) : NULL;

    if (!r) {
        return;
    }
    row_close_stale(r, s);
    row_open(r, s, buf[0], 0);
    switch (buf[0]) {
    case GPCMD_READ_10:
        r->sectors = lduw_be_p(buf + 7);
        break;
    case GPCMD_READ_12:
        r->sectors = (uint32_t)ldl_be_p(buf + 6);
        break;
    case GPCMD_READ_CD:
        r->sectors = (buf[6] << 16) | (buf[7] << 8) | buf[8];
        break;
    default:
        return;
    }
    r->op = 'r';
    r->lba = (uint32_t)ldl_be_p(buf + 2);
}

void xemu_block_log_done(IDEState *s)
{
    BlockLogRow *r = row_of(s, false);

    if (r) {
        row_write(r, (s->status & ERR_STAT) ? "err" : "ok");
    }
}

/* Rows still open are written with status "open". */
static void block_log_before_close(void)
{
    xemu_block_log_on = false;
    for (int i = 0; i < ARRAY_SIZE(open_rows); i++) {
        if (open_rows[i].s) {
            row_write(&open_rows[i], "open");
        }
    }
}

static bool block_log_open(const char *path, bool append, Error **errp)
{
    if (!xemu_filelog_open(&block_log, path, append, errp)) {
        return false;
    }
    memset(open_rows, 0, sizeof(open_rows));
    xemu_block_log_on = true;
    return true;
}

void qmp_xemu_block_log_start(const char *filename, bool has_append,
                              bool append, Error **errp)
{
    block_log_open(filename, has_append && append, errp);
}

XemuBlockLogInfo *qmp_xemu_block_log_stop(Error **errp)
{
    XemuBlockLogInfo *info;

    if (!xemu_filelog_close(&block_log, errp)) {
        return NULL;
    }
    info = g_new0(XemuBlockLogInfo, 1);
    info->filename = g_strdup(block_log.path);
    info->rows = block_log.rows;
    return info;
}

static void block_log_env_init(void)
{
    const char *path = getenv("XEMU_BLOCK_LOG");
    Error *err = NULL;

    if (path && path[0] && !block_log_open(path, false, &err)) {
        error_report_err(err);
    }
}
opts_init(block_log_env_init);
