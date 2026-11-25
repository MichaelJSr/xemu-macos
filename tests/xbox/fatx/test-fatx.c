/*
 * Tests for libfatx (ui/thirdparty/libfatx), which the drive explorer uses
 * to write into the player's HDD image. Each test formats a volume in
 * memory and works on it through the io_read/io_write callbacks, as
 * ui/xui/xbox-drive.cc does, covering the xemu changes in README.xemu.md.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "ui/thirdparty/libfatx/fatx.h"

#define VOL_SIZE    ((uint64_t)8 * MiB)
#define SECTOR_SIZE 512
#define FAT_OFFSET  4096 /* FATX_FAT_OFFSET (fatx_internal.h) */
#define DIRENT_SIZE 64   /* sizeof(struct fatx_raw_directory_entry) */
#define FAT_TYPE_16 1    /* FATX_FAT_TYPE_16 */

typedef struct Vol {
    struct fatx_fs fs;
    uint8_t *disk;
} Vol;

static int mem_read(void *opaque, uint64_t offset, void *buf, size_t len)
{
    Vol *v = opaque;

    if (offset > VOL_SIZE || len > VOL_SIZE - offset) {
        return -1;
    }
    memcpy(buf, v->disk + offset, len);
    return 0;
}

static int mem_write(void *opaque, uint64_t offset, const void *buf,
                     size_t len)
{
    Vol *v = opaque;

    if (offset > VOL_SIZE || len > VOL_SIZE - offset) {
        return -1;
    }
    memcpy(v->disk + offset, buf, len);
    return 0;
}

static void vol_init_fs(Vol *v)
{
    memset(&v->fs, 0, sizeof(v->fs));
    v->fs.io_opaque = v;
    v->fs.io_read = mem_read;
    v->fs.io_write = mem_write;
}

static void vol_open(Vol *v)
{
    vol_init_fs(v);
    g_assert_cmpint(fatx_open_device(&v->fs, "mem", 0, VOL_SIZE, SECTOR_SIZE,
                                     FATX_READ_FROM_SUPERBLOCK),
                    ==, FATX_STATUS_SUCCESS);
}

/* Flushes the FAT cache to the disk */
static void vol_close(Vol *v)
{
    g_assert_cmpint(fatx_close_device(&v->fs), ==, FATX_STATUS_SUCCESS);
    free(v->fs.fat_cache.data);
    v->fs.fat_cache.data = NULL;
}

/* A formatted, open volume. 32 sectors per cluster is the retail 16 KiB. */
static void vol_new(Vol *v, size_t sectors_per_cluster)
{
    v->disk = g_malloc0(VOL_SIZE);
    vol_init_fs(v);
    g_assert_cmpint(fatx_disk_format_partition(&v->fs, "mem", 0, VOL_SIZE,
                                               SECTOR_SIZE,
                                               sectors_per_cluster),
                    ==, FATX_STATUS_SUCCESS);
    free(v->fs.fat_cache.data);
    vol_open(v);
}

static void vol_free(Vol *v)
{
    vol_close(v);
    g_free(v->disk);
}

static size_t per_cluster(Vol *v)
{
    return v->fs.bytes_per_cluster / DIRENT_SIZE;
}

/* Clusters are numbered from 1 */
static uint8_t *cluster_ptr(Vol *v, size_t cluster)
{
    return v->disk + v->fs.cluster_offset +
           (cluster - 1) * v->fs.bytes_per_cluster;
}

static size_t cluster_of(Vol *v, const char *path)
{
    struct fatx_attr attr;

    g_assert_cmpint(fatx_get_attr(&v->fs, path, &attr), ==,
                    FATX_STATUS_SUCCESS);
    return attr.first_cluster;
}

/* The FAT16 on the disk; valid between vol_close() and vol_open() */
static uint16_t *fat16(Vol *v)
{
    g_assert_cmpint(v->fs.fat_type, ==, FAT_TYPE_16);
    return (uint16_t *)(v->disk + FAT_OFFSET);
}

static unsigned free_clusters(Vol *v)
{
    const uint16_t *fat;
    unsigned n = 0;

    vol_close(v);
    fat = fat16(v);
    for (size_t i = 2; i < v->fs.num_clusters; i++) {
        n += fat[i] == 0;
    }
    vol_open(v);
    return n;
}

static void make_files(Vol *v, const char *dir, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        g_autofree char *path = g_strdup_printf("%s/f%03zu", dir, i);
        g_assert_cmpint(fatx_mknod(&v->fs, path), ==, FATX_STATUS_SUCCESS);
    }
}

/*
 * Walks a directory as the explorer does: the live entries, or -1 when the
 * walk fails. A looped cluster chain has to fail within a few laps.
 */
static int list_dir(Vol *v, const char *path)
{
    struct fatx_dir dir;
    struct fatx_dirent ent, *res;
    struct fatx_attr attr;
    int n = 0;

    if (fatx_open_dir(&v->fs, path, &dir) != FATX_STATUS_SUCCESS) {
        return -1;
    }
    for (int steps = 0; ; steps++) {
        int st;

        g_assert_cmpint(steps, <, 10000);
        st = fatx_read_dir(&v->fs, &dir, &ent, &attr, &res);
        if (st == FATX_STATUS_END_OF_DIR) {
            return n;
        }
        if (st == FATX_STATUS_SUCCESS) {
            n++;
        } else if (st != FATX_STATUS_FILE_DELETED) {
            return -1;
        }
        if (fatx_next_dir_entry(&v->fs, &dir) != FATX_STATUS_SUCCESS) {
            return -1;
        }
    }
}

/* A file over three retail clusters, and a write across a cluster edge */
static void test_round_trip(void)
{
    const size_t len = 40 * KiB;
    const off_t edge = 16 * KiB - 50;
    g_autofree uint8_t *data = g_malloc(len);
    g_autofree uint8_t *back = g_malloc0(len);
    uint8_t patch[100];
    Vol v;

    vol_new(&v, 32);
    for (size_t i = 0; i < len; i++) {
        data[i] = i * 7 + (i >> 8);
    }
    g_assert_cmpint(fatx_mkdir(&v.fs, "/d"), ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_mknod(&v.fs, "/d/a"), ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_write(&v.fs, "/d/a", 0, len, data), ==, len);

    memset(patch, 0x5a, sizeof(patch));
    memcpy(data + edge, patch, sizeof(patch));
    g_assert_cmpint(fatx_write(&v.fs, "/d/a", edge, sizeof(patch), patch),
                    ==, sizeof(patch));

    vol_close(&v);
    vol_open(&v);
    g_assert_cmpint(fatx_read(&v.fs, "/d/a", 0, len, back), ==, len);
    g_assert_true(memcmp(data, back, len) == 0);
    g_assert_cmpint(list_dir(&v, "/d"), ==, 1);
    vol_free(&v);
}

/* 42-character names, whole-component path match, the name-length clamp */
static void test_names(void)
{
    g_autofree char *n42 = g_strnfill(FATX_MAX_FILENAME_LEN, 'n');
    g_autofree char *m42 = g_strnfill(FATX_MAX_FILENAME_LEN, 'r');
    g_autofree char *p42 = g_strdup_printf("/d/%s", n42);
    g_autofree char *p43 = g_strdup_printf("/d/%sx", n42);
    g_autofree char *r42 = g_strdup_printf("/d/%s", m42);
    g_autofree char *r43 = g_strdup_printf("/d/%sx", m42);
    struct {
        struct fatx_dirent ent;
        uint8_t guard[64];
    } probe;
    struct fatx_dirent *res;
    struct fatx_attr attr;
    struct fatx_dir dir;
    unsigned before;
    Vol v;

    vol_new(&v, 1);
    g_assert_cmpint(fatx_mkdir(&v.fs, "/d"), ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_mknod(&v.fs, p42), ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_get_attr(&v.fs, p42, &attr), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpstr(attr.filename, ==, n42);
    before = free_clusters(&v);
    g_assert_cmpint(fatx_mknod(&v.fs, p43), !=, FATX_STATUS_SUCCESS);
    g_assert_cmpuint(free_clusters(&v), ==, before);

    g_assert_cmpint(fatx_mknod(&v.fs, "/d/a"), ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_rename(&v.fs, "/d/a", r42, false, false), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_get_attr(&v.fs, r42, &attr), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_rename(&v.fs, r42, r43, false, false), !=,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_get_attr(&v.fs, r42, &attr), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(list_dir(&v, "/d"), ==, 2);

    /* "Halo" must not match "Halo 2" */
    g_assert_cmpint(fatx_mkdir(&v.fs, "/Halo 2"), ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_mknod(&v.fs, "/Halo 2/save"), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_open_dir(&v.fs, "/Halo", &dir), ==,
                    FATX_STATUS_FILE_NOT_FOUND);
    g_assert_cmpint(fatx_get_attr(&v.fs, "/Halo/save", &attr), !=,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_get_attr(&v.fs, "/Halo 2/save", &attr), ==,
                    FATX_STATUS_SUCCESS);

    /* A damaged length byte reads as at most 42 characters */
    cluster_ptr(&v, cluster_of(&v, "/d"))[0] = 0x50;
    memset(&probe, 0xa5, sizeof(probe));
    g_assert_cmpint(fatx_open_dir(&v.fs, "/d", &dir), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_read_dir(&v.fs, &dir, &probe.ent, &attr, &res), ==,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpstr(probe.ent.filename, ==, n42);
    g_assert_cmpstr(attr.filename, ==, n42);
    for (size_t i = 0; i < sizeof(probe.guard); i++) {
        g_assert_cmphex(probe.guard[i], ==, 0xa5);
    }
    vol_free(&v);
}

/*
 * A directory cluster whose last slot holds the end marker: the next entry
 * takes that slot and the marker goes to a new cluster, not into the
 * cluster after this one on the disk.
 */
static void test_full_cluster(void)
{
    g_autofree uint8_t *next = NULL;
    size_t x, per;
    Vol v;

    vol_new(&v, 1);
    per = per_cluster(&v);
    g_assert_cmpint(fatx_mkdir(&v.fs, "/full"), ==, FATX_STATUS_SUCCESS);
    x = cluster_of(&v, "/full");
    make_files(&v, "/full", 0, per - 1);
    next = g_memdup2(cluster_ptr(&v, x + 1), v.fs.bytes_per_cluster);

    make_files(&v, "/full", per - 1, per + 1);
    g_assert_true(memcmp(next, cluster_ptr(&v, x + 1),
                         v.fs.bytes_per_cluster) == 0);
    g_assert_cmpint(list_dir(&v, "/full"), ==, per + 1);
    vol_close(&v);
    vol_open(&v);
    g_assert_cmpint(list_dir(&v, "/full"), ==, per + 1);
    vol_free(&v);
}

/* A full directory cluster with no end marker, as the Xbox can leave one */
static void test_full_cluster_no_marker(void)
{
    g_autofree uint8_t *next = NULL;
    struct fatx_attr attr;
    uint8_t *last;
    size_t x, per;
    Vol v;

    vol_new(&v, 1);
    per = per_cluster(&v);
    g_assert_cmpuint(per, ==, 8);
    g_assert_cmpint(fatx_mkdir(&v.fs, "/full"), ==, FATX_STATUS_SUCCESS);
    x = cluster_of(&v, "/full");
    make_files(&v, "/full", 0, per - 1);

    /* An entry over the end marker: f000's, renamed f007 */
    last = cluster_ptr(&v, x) + (per - 1) * DIRENT_SIZE;
    memcpy(last, cluster_ptr(&v, x), DIRENT_SIZE);
    g_assert_cmpmem(last + 2, 4, "f000", 4);
    last[2 + 3] = '7';
    g_assert_cmpint(fatx_get_attr(&v.fs, "/full/f007", &attr), ==,
                    FATX_STATUS_SUCCESS);
    next = g_memdup2(cluster_ptr(&v, x + 1), v.fs.bytes_per_cluster);

    make_files(&v, "/full", per, per + 2);
    g_assert_true(memcmp(next, cluster_ptr(&v, x + 1),
                         v.fs.bytes_per_cluster) == 0);
    g_assert_cmpint(list_dir(&v, "/full"), ==, per + 2);
    vol_free(&v);
}

/* A directory chain that loops: walks and allocations fail, and stop */
static void test_looped_chain(void)
{
    struct fatx_attr attr;
    struct fatx_dir dir;
    size_t x, n1, per;
    unsigned before;
    Vol v;

    vol_new(&v, 1);
    per = per_cluster(&v);
    g_assert_cmpint(fatx_mkdir(&v.fs, "/loop"), ==, FATX_STATUS_SUCCESS);
    x = cluster_of(&v, "/loop");

    /* Two full clusters, then one with the end marker; loop the second
     * back to the first */
    make_files(&v, "/loop", 0, 2 * per);
    vol_close(&v);
    n1 = fat16(&v)[x];
    g_assert_cmpuint(n1, !=, 0xffff);
    g_assert_cmpuint(fat16(&v)[n1], !=, 0xffff);
    fat16(&v)[n1] = x;
    vol_open(&v);
    before = free_clusters(&v);

    g_assert_cmpint(list_dir(&v, "/loop"), ==, -1);
    g_assert_cmpint(fatx_get_attr(&v.fs, "/loop/missing", &attr), !=,
                    FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_open_dir(&v.fs, "/loop/sub", &dir), ==,
                    FATX_STATUS_ERROR);
    g_assert_cmpint(fatx_mknod(&v.fs, "/loop/new"), !=, FATX_STATUS_SUCCESS);
    g_assert_cmpuint(free_clusters(&v), ==, before);
    vol_free(&v);
}

/* Rename and unlink survive a reopen; a new entry reuses the freed slot */
static void test_rename_unlink(void)
{
    char back[6] = { 0 };
    struct fatx_attr attr;
    const uint8_t *slot;
    unsigned before;
    Vol v;

    vol_new(&v, 1);
    g_assert_cmpint(fatx_mkdir(&v.fs, "/d"), ==, FATX_STATUS_SUCCESS);
    make_files(&v, "/d", 0, 3);
    g_assert_cmpint(fatx_write(&v.fs, "/d/f000", 0, 5, "hello"), ==, 5);
    g_assert_cmpint(fatx_rename(&v.fs, "/d/f000", "/d/kept", false, false),
                    ==, FATX_STATUS_SUCCESS);
    g_assert_cmpint(fatx_unlink(&v.fs, "/d/f001"), ==, FATX_STATUS_SUCCESS);

    vol_close(&v);
    vol_open(&v);
    g_assert_cmpint(fatx_get_attr(&v.fs, "/d/f000", &attr), ==,
                    FATX_STATUS_FILE_NOT_FOUND);
    g_assert_cmpint(fatx_get_attr(&v.fs, "/d/f001", &attr), ==,
                    FATX_STATUS_FILE_NOT_FOUND);
    g_assert_cmpint(fatx_read(&v.fs, "/d/kept", 0, 5, back), ==, 5);
    g_assert_cmpstr(back, ==, "hello");
    g_assert_cmpint(list_dir(&v, "/d"), ==, 2);

    before = free_clusters(&v);
    make_files(&v, "/d", 3, 4);
    slot = cluster_ptr(&v, cluster_of(&v, "/d")) + DIRENT_SIZE;
    g_assert_cmpuint(slot[0], ==, 4);
    g_assert_cmpmem(slot + 2, 4, "f003", 4);
    g_assert_cmpint(list_dir(&v, "/d"), ==, 3);
    g_assert_cmpuint(free_clusters(&v), ==, before - 1);
    vol_free(&v);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/fatx/round-trip", test_round_trip);
    g_test_add_func("/xbox/fatx/names", test_names);
    g_test_add_func("/xbox/fatx/full-cluster", test_full_cluster);
    g_test_add_func("/xbox/fatx/full-cluster-no-marker",
                    test_full_cluster_no_marker);
    g_test_add_func("/xbox/fatx/looped-chain", test_looped_chain);
    g_test_add_func("/xbox/fatx/rename-unlink", test_rename_unlink);
    return g_test_run();
}
