/*
 * xemu disc fingerprints: a disc image's identity by content
 *
 * A snapshot records the path of the disc in the tray and, since this
 * file, the image's fingerprint: its size and a 128-bit digest of every
 * byte (xemu-snapshots.c). The Snapshots menu loads a state with no
 * dialog when the tray's image has the fingerprint the state was saved
 * with, whatever its path (ui/xui/snapshot-manager.cc).
 *
 * Every byte, because two builds of one game can differ in one level file
 * that keeps its size and its sector (the Azurik modkit rewrites a changed
 * file in place unless it grows). The XISO directory tables hold only each
 * file's name, sector and size, so they would match, and so could any
 * sampled digest that misses the changed bytes. A false match loads a
 * state on a disc it wasn't saved with, which the dialog exists to
 * prevent; a false mismatch only asks.
 *
 * The digest is XXH3-128 of the size and of the XXH3-128 of each 32 MiB
 * chunk in order, so it doesn't depend on how many threads (up to 8) hash
 * the chunks. Each image is hashed once per process: results are cached
 * by path, size, mtime, ctime, device and inode. So the first savevm with
 * a disc in the tray pays for reading it all (stderr: "xemu: disc
 * fingerprint: ..." with the time taken), and later ones read the cache.
 * A read projected to take longer than 10 s (a network share, an optical
 * drive) is given up: that snapshot gets no fingerprint, and a menu load
 * of it asks, as before.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include <glib/gstdio.h>
#include <xxhash.h>
#include "xemu-disc-fingerprint.h"

#define FP_CHUNK        (32 * MiB)
#define FP_READ         (4 * MiB)
#define FP_THREADS      8
#define FP_BUDGET_NS    (10 * NANOSECONDS_PER_SECOND)
#define FP_PROJECT_NS   NANOSECONDS_PER_SECOND  /* judge the rate after */

#if defined(__APPLE__)
#define ST_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define ST_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#elif defined(_WIN32)
#define ST_MTIME_NSEC(st) 0
#define ST_CTIME_NSEC(st) 0
#else
#define ST_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define ST_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

#ifdef _WIN32
#define fp_lseek _lseeki64
#else
#define fp_lseek lseek
#endif

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

typedef struct FileId {
    uint64_t size;
    int64_t mtime_ns;
    int64_t ctime_ns;   /* inode change, never back-dated (Windows: creation) */
    uint64_t dev;
    uint64_t ino;       /* 0 on Windows */
} FileId;

typedef struct CacheEntry {
    char *path;
    FileId id;
    bool ok;
    XemuDiscFingerprint fp;
    char *error;
} CacheEntry;

G_LOCK_DEFINE_STATIC(fp_cache);
static GPtrArray *fp_cache;

static bool file_id(const char *path, FileId *id, Error **errp)
{
    GStatBuf st;

    if (g_stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "%s", path);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error_setg(errp, "%s is not a regular file", path);
        return false;
    }
    id->size = st.st_size;
    id->mtime_ns = (int64_t)st.st_mtime * NANOSECONDS_PER_SECOND +
                   ST_MTIME_NSEC(st);
    id->ctime_ns = (int64_t)st.st_ctime * NANOSECONDS_PER_SECOND +
                   ST_CTIME_NSEC(st);
    id->dev = st.st_dev;
    id->ino = st.st_ino;
    return true;
}

static bool file_id_equal(const FileId *a, const FileId *b)
{
    return a->size == b->size && a->mtime_ns == b->mtime_ns &&
           a->ctime_ns == b->ctime_ns && a->dev == b->dev && a->ino == b->ino;
}

typedef struct HashJob {
    const char *path;
    uint64_t size;
    uint64_t nchunks;
    XXH128_canonical_t *chunks;
    uint64_t next;      /* the next chunk to take */
    uint64_t done;      /* bytes hashed so far */
    int64_t start;
    int failed;         /* the first errno; ETIMEDOUT: too slow */
} HashJob;

static void hash_fail(HashJob *job, int err)
{
    qatomic_cmpxchg(&job->failed, 0, err ? err : EIO);
}

static void *hash_worker(void *opaque)
{
    HashJob *job = opaque;
    int fd = g_open(job->path, O_RDONLY | O_BINARY, 0);

    if (fd < 0) {
        hash_fail(job, errno);
        return NULL;
    }

    uint8_t *buf = g_malloc(FP_READ);
    XXH3_state_t *st = XXH3_createState();

    while (!qatomic_read(&job->failed)) {
        uint64_t i = qatomic_fetch_inc(&job->next);
        if (i >= job->nchunks) {
            break;
        }
        uint64_t off = i * FP_CHUNK;
        uint64_t left = MIN(FP_CHUNK, job->size - off);
        if (fp_lseek(fd, off, SEEK_SET) != (int64_t)off) {
            hash_fail(job, errno);
            break;
        }
        XXH3_128bits_reset(st);
        while (left && !qatomic_read(&job->failed)) {
            ssize_t n = read(fd, buf, MIN(left, FP_READ));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                hash_fail(job, n < 0 ? errno : EIO);   /* EIO: it shrank */
                break;
            }
            XXH3_128bits_update(st, buf, n);
            left -= n;

            uint64_t done = qatomic_fetch_add(&job->done, n) + n;
            int64_t el = get_clock() - job->start;
            if (el > FP_BUDGET_NS ||
                (el > FP_PROJECT_NS &&
                 (double)el * job->size > (double)FP_BUDGET_NS * done)) {
                hash_fail(job, ETIMEDOUT);
            }
        }
        if (!left) {
            XXH128_canonicalFromHash(&job->chunks[i], XXH3_128bits_digest(st));
        }
    }

    XXH3_freeState(st);
    g_free(buf);
    close(fd);
    return NULL;
}

static bool hash_file(const char *path, uint64_t size,
                      XemuDiscFingerprint *fp, Error **errp)
{
    HashJob job = {
        .path = path,
        .size = size,
        .nchunks = DIV_ROUND_UP(size, FP_CHUNK),
        .start = get_clock(),
    };
    QemuThread threads[FP_THREADS];
    int nthreads = MIN(FP_THREADS, g_get_num_processors());

    if (job.nchunks < (uint64_t)nthreads) {
        nthreads = MAX(job.nchunks, 1);
    }
    job.chunks = g_new0(XXH128_canonical_t, MAX(job.nchunks, 1));
    for (int i = 1; i < nthreads; i++) {
        qemu_thread_create(&threads[i], "disc-fp", hash_worker, &job,
                           QEMU_THREAD_JOINABLE);
    }
    hash_worker(&job);
    for (int i = 1; i < nthreads; i++) {
        qemu_thread_join(&threads[i]);
    }

    if (job.failed) {
        if (job.failed == ETIMEDOUT) {
            error_setg(errp, "%s: reading it would take over %d s",
                       path, (int)(FP_BUDGET_NS / NANOSECONDS_PER_SECOND));
        } else {
            error_setg_errno(errp, job.failed, "%s", path);
        }
        g_free(job.chunks);
        return false;
    }

    XXH3_state_t *st = XXH3_createState();
    XXH128_canonical_t whole;
    uint8_t be_size[8];

    stq_be_p(be_size, size);
    XXH3_128bits_reset(st);
    XXH3_128bits_update(st, be_size, sizeof(be_size));
    XXH3_128bits_update(st, job.chunks, job.nchunks * sizeof(*job.chunks));
    XXH128_canonicalFromHash(&whole, XXH3_128bits_digest(st));
    XXH3_freeState(st);
    g_free(job.chunks);

    QEMU_BUILD_BUG_ON(sizeof(whole.digest) != XEMU_DISC_DIGEST_LEN);
    fp->size = size;
    memcpy(fp->digest, whole.digest, XEMU_DISC_DIGEST_LEN);
    return true;
}

static void digest_hex(const XemuDiscFingerprint *fp, char *out)
{
    for (int i = 0; i < XEMU_DISC_DIGEST_LEN; i++) {
        snprintf(out + 2 * i, 3, "%02x", fp->digest[i]);
    }
}

bool xemu_disc_fingerprint(const char *path, XemuDiscFingerprint *fp,
                           Error **errp)
{
    CacheEntry *e = NULL;
    FileId id;

    if (!file_id(path, &id, errp)) {
        return false;
    }

    G_LOCK(fp_cache);
    if (!fp_cache) {
        fp_cache = g_ptr_array_new();
    }
    for (guint i = 0; i < fp_cache->len && !e;) {
        CacheEntry *c = g_ptr_array_index(fp_cache, i);
        if (strcmp(c->path, path)) {
            i++;
        } else if (file_id_equal(&c->id, &id)) {
            e = c;
        } else {
            g_ptr_array_remove_index_fast(fp_cache, i);  /* the file changed */
            g_free(c->path);
            g_free(c->error);
            g_free(c);
        }
    }
    if (!e) {
        Error *err = NULL;
        FileId after;
        int64_t t0 = get_clock();

        e = g_new0(CacheEntry, 1);
        e->path = g_strdup(path);
        e->id = id;
        e->ok = hash_file(path, id.size, &e->fp, &err);
        if (e->ok && (!file_id(path, &after, NULL) ||
                      !file_id_equal(&after, &id))) {
            e->ok = false;
            error_setg(&err, "%s changed while it was read", path);
        }
        if (e->ok) {
            char hex[2 * XEMU_DISC_DIGEST_LEN + 1];
            digest_hex(&e->fp, hex);
            fprintf(stderr, "xemu: disc fingerprint: %s: %" PRIu64
                    " bytes, xxh3-128 %s (%.2f s)\n", path, id.size, hex,
                    (get_clock() - t0) / 1e9);
        } else {
            e->error = g_strdup(error_get_pretty(err));
            fprintf(stderr, "xemu: disc fingerprint: none for %s\n",
                    e->error);
            error_free(err);
        }
        g_ptr_array_add(fp_cache, e);
    }
    bool ok = e->ok;
    if (ok) {
        *fp = e->fp;
    } else {
        error_setg(errp, "%s", e->error);
    }
    G_UNLOCK(fp_cache);
    return ok;
}

bool xemu_disc_same_content(const char *tray_path, const char *saved_path,
                            const XemuDiscFingerprint *saved_fp,
                            int64_t saved_at, char **how)
{
    XemuDiscFingerprint tray, saved;
    FileId tid, sid;
    Error *err = NULL;
    char hex[2 * XEMU_DISC_DIGEST_LEN + 1];

    if (!file_id(tray_path, &tid, &err)) {
        *how = g_strdup_printf("the tray's image can't be read (%s)",
                               error_get_pretty(err));
        error_free(err);
        return false;
    }

    if (saved_fp) {
        if (saved_fp->size != tid.size) {
            *how = g_strdup_printf("another disc: %" PRIu64 " bytes, "
                                   "saved with %" PRIu64, tid.size,
                                   saved_fp->size);
            return false;
        }
        if (!xemu_disc_fingerprint(tray_path, &tray, &err)) {
            *how = g_strdup_printf("no fingerprint for the tray's image (%s)",
                                   error_get_pretty(err));
            error_free(err);
            return false;
        }
        if (memcmp(tray.digest, saved_fp->digest, XEMU_DISC_DIGEST_LEN)) {
            *how = g_strdup("another disc: the same size, other content");
            return false;
        }
        digest_hex(&tray, hex);
        *how = g_strdup_printf("the saved fingerprint: %" PRIu64
                               " bytes, xxh3-128 %s", tid.size, hex);
        return true;
    }

    /*
     * Saved before fingerprints: trust the saved path's file only while
     * it is the file that was there at the save. ctime moves on any write,
     * replacement or copy over it, even one that keeps an older mtime.
     */
    if (!file_id(saved_path, &sid, &err)) {
        *how = g_strdup_printf("the snapshot has no disc fingerprint and "
                               "its image can't be read (%s)",
                               error_get_pretty(err));
        error_free(err);
        return false;
    }
#ifdef _WIN32
    /*
     * No inode change time here: st_ctime is the creation time, which a
     * copy over the file keeps (as it keeps the copy's older mtime), so
     * an unchanged file can't be told from a replaced one.
     */
    *how = g_strdup("the snapshot has no disc fingerprint, and on Windows "
                    "its image can't be shown unchanged since the save");
    return false;
#endif
    if (MAX(sid.mtime_ns, sid.ctime_ns) / NANOSECONDS_PER_SECOND > saved_at) {
        *how = g_strdup("the snapshot has no disc fingerprint and its image "
                        "changed after it was saved");
        return false;
    }
    if (sid.size != tid.size) {
        *how = g_strdup_printf("another disc: %" PRIu64 " bytes, the saved "
                               "image %" PRIu64, tid.size, sid.size);
        return false;
    }
    if (sid.ino && sid.ino == tid.ino && sid.dev == tid.dev) {
        *how = g_strdup("the saved image's own file (no fingerprint saved)");
        return true;
    }
    if (!xemu_disc_fingerprint(tray_path, &tray, &err) ||
        !xemu_disc_fingerprint(saved_path, &saved, &err)) {
        *how = g_strdup_printf("the snapshot has no disc fingerprint and "
                               "the images can't be compared (%s)",
                               error_get_pretty(err));
        error_free(err);
        return false;
    }
    if (memcmp(tray.digest, saved.digest, XEMU_DISC_DIGEST_LEN)) {
        *how = g_strdup("another disc: the saved image's content differs "
                        "(no fingerprint saved)");
        return false;
    }
    digest_hex(&tray, hex);
    *how = g_strdup_printf("the saved image's content, xxh3-128 %s "
                           "(no fingerprint saved)", hex);
    return true;
}
