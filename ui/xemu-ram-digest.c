/*
 * xemu RAM digest: QMP xemu-ram-digest
 *
 * Guest RAM hashed in place, with no file and no GPU write-back: the
 * sha256 of all of it (what xemu-dump-ram's file hashes to) or its
 * XXH3-128, optionally an 8-byte hash per 4 KiB page, or the pages whose
 * bytes differ from a checkpoint's RAM image (xemu-checkpoint-ram.c).
 *
 * The VM is paused throughout, so page hashes run on worker threads
 * (dispatch_apply on macOS, QEMU's thread pool elsewhere), each over its
 * own range of pages. The whole-RAM hashes are sequential by definition,
 * and the comparison is memory-bound (threads only add CPU time), so both
 * stay on the calling thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "system/runstate.h"
#include "xemu-checkpoints.h"
/*
 * xxHash inline, with its own endianness detection: the in-tree library
 * is built with every 'auto' feature disabled (configure
 * --without-default-features), XXH_CPU_LITTLE_ENDIAN=0 included, so its
 * XXH3 reads little-endian hosts' words byte-swapped and matches no other
 * xxHash. Its own users (fast_hash) only compare hashes they made.
 */
#undef XXH_CPU_LITTLE_ENDIAN
#undef XXH_NO_INLINE_HINTS
#define XXH_INLINE_ALL
#include <xxhash.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#else
#include "block/thread-pool.h"
#include "crypto/hash.h"
#endif

#define SHA256_LEN 32
#define XXH128_LEN 16
#define PAGE_HASH_LEN 8
/* Pages per parallel task: 2 MiB, 64 tasks for 128 MiB. */
#define TASK_PAGES 512

int64_t xemu_frame_count(void);

/*
 * sha256: CommonCrypto on macOS, where it runs on the CPU's SHA-2
 * instructions (about 2.4 GB/s on Apple silicon, against a few hundred
 * MB/s for glib's, the crypto backend this build has); QEMU's crypto
 * layer elsewhere. Thread-safe: each call has its own context.
 */
static bool sha256_bytes(const uint8_t *buf, size_t len,
                         uint8_t out[SHA256_LEN], Error **errp)
{
#ifdef __APPLE__
    CC_SHA256_CTX ctx;

    CC_SHA256_Init(&ctx);
    while (len) {
        CC_LONG n = MIN(len, 1u << 30);
        CC_SHA256_Update(&ctx, buf, n);
        buf += n;
        len -= n;
    }
    CC_SHA256_Final(out, &ctx);
    return true;
#else
    size_t out_len = SHA256_LEN;
    uint8_t *res = out;
    return qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, buf, len, &res,
                              &out_len, errp) == 0;
#endif
}

typedef struct DigestJob {
    const uint8_t *ram;
    size_t page;
    uint64_t npages;
    bool xxh3;
    char *page_hex;                 /* 2 * PAGE_HASH_LEN per page */
    bool failed;
} DigestJob;

/* One task: pages [k * TASK_PAGES, (k + 1) * TASK_PAGES) of @j. */
static void digest_task(DigestJob *j, size_t k)
{
    static const char hexdig[] = "0123456789abcdef";
    uint64_t first = k * TASK_PAGES;
    uint64_t end = MIN(first + TASK_PAGES, j->npages);

    for (uint64_t i = first; i < end; i++) {
        const uint8_t *p = j->ram + i * j->page;
        uint8_t md[SHA256_LEN];
        char *out = j->page_hex + i * 2 * PAGE_HASH_LEN;

        if (j->xxh3) {
            /* Canonical (big-endian) form, as xxhsum prints it. */
            stq_be_p(md, XXH3_64bits(p, j->page));
        } else if (!sha256_bytes(p, j->page, md, NULL)) {
            qatomic_set(&j->failed, true);
            return;
        }
        for (int b = 0; b < PAGE_HASH_LEN; b++) {
            out[2 * b] = hexdig[md[b] >> 4];
            out[2 * b + 1] = hexdig[md[b] & 15];
        }
    }
}

#ifdef __APPLE__
static void digest_task_apply(void *opaque, size_t k)
{
    digest_task(opaque, k);
}

static void digest_parallel(DigestJob *j, size_t ntasks)
{
    dispatch_apply_f(ntasks,
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                     j, digest_task_apply);
}
#else
typedef struct DigestTask {
    DigestJob *j;
    size_t k;
} DigestTask;

static int digest_task_pool(void *opaque)
{
    DigestTask *t = opaque;

    digest_task(t->j, t->k);
    return 0;
}

static void digest_parallel(DigestJob *j, size_t ntasks)
{
    static ThreadPool *pool;
    g_autofree DigestTask *t = g_new(DigestTask, ntasks);

    if (!pool) {
        pool = thread_pool_new();
        thread_pool_set_max_threads(pool, MAX(1, MIN(g_get_num_processors(),
                                                     16)));
    }
    for (size_t k = 0; k < ntasks; k++) {
        t[k] = (DigestTask) { .j = j, .k = k };
        thread_pool_submit(pool, digest_task_pool, &t[k], NULL);
    }
    thread_pool_wait(pool);
}
#endif

XemuRamDigest *qmp_xemu_ram_digest(bool has_sha256, bool sha256,
                                   bool has_pages, bool pages,
                                   const char *against,
                                   bool has_algorithm,
                                   XemuDigestAlgorithm algorithm,
                                   Error **errp)
{
    RAMBlock *rb;
    const uint8_t *ram;
    uint64_t ram_size;
    const uint8_t *const *image = NULL;
    uint64_t image_pages = 0;
    bool xxh3 = has_algorithm && algorithm == XEMU_DIGEST_ALGORITHM_XXH3_128;

    WITH_RCU_READ_LOCK_GUARD() {
        rb = qemu_ram_block_by_name("xbox.ram");
    }
    if (!rb) {
        error_setg(errp, "no xbox.ram block");
        return NULL;
    }
    ram = qemu_ram_get_host_addr(rb);
    ram_size = qemu_ram_get_used_length(rb);
    if (!has_sha256) {
        sha256 = !against;
    }
    if (against) {
        XemuRamImage *img = xemu_checkpoint_ram(against);
        if (!img) {
            error_setg(errp, "no checkpoint '%s'", against);
            return NULL;
        }
        image = xemu_ram_image_block(img, rb, &image_pages);
        if (!image || image_pages != ram_size / qemu_target_page_size()) {
            error_setg(errp, "checkpoint '%s' holds no %s", against,
                       qemu_ram_get_idstr(rb));
            return NULL;
        }
    }

    /* Pause, as xemu-dump-ram does, but never write GPU surfaces back. */
    bool resume = runstate_is_running();
    if (resume) {
        vm_stop(RUN_STATE_PAUSED);
    }

    int64_t t0 = get_clock();
    size_t page = qemu_target_page_size();
    uint64_t npages = ram_size / page;
    XemuRamDigest *info = g_new0(XemuRamDigest, 1);
    bool ok = true;

    if (sha256 && xxh3) {
        XXH128_canonical_t c;

        XXH128_canonicalFromHash(&c, XXH3_128bits(ram, ram_size));
        info->xxh3_128 = g_string_free(qemu_hexdump_line(NULL, c.digest,
                                                         XXH128_LEN, 0, 0),
                                       false);
    } else if (sha256) {
        uint8_t md[SHA256_LEN];

        ok = sha256_bytes(ram, ram_size, md, errp);
        if (ok) {
            info->sha256 = g_string_free(qemu_hexdump_line(NULL, md,
                                                           SHA256_LEN, 0, 0),
                                         false);
        }
    }
    if (ok && pages) {
        g_autofree char *hex = g_malloc0(npages * 2 * PAGE_HASH_LEN + 1);
        DigestJob j = {
            .ram = ram, .page = page, .npages = npages, .xxh3 = xxh3,
            .page_hex = hex,
        };

        digest_parallel(&j, DIV_ROUND_UP(npages, TASK_PAGES));
        if (j.failed) {
            error_setg(errp, "hashing a RAM page failed");
            ok = false;
        } else {
            info->page_hashes = g_steal_pointer(&hex);
        }
    }
    if (ok && against) {
        intList **tail = &info->differ;

        for (uint64_t i = 0; i < npages; i++) {
            const uint8_t *p = ram + i * page;
            if (image[i] ? memcmp(p, image[i], page)
                         : !buffer_is_zero(p, page)) {
                QAPI_LIST_APPEND(tail, i);
            }
        }
        info->has_differ = true;
    }
    int64_t elapsed_us = (get_clock() - t0) / SCALE_US;

    if (resume) {
        vm_start();
    }
    if (!ok) {
        qapi_free_XemuRamDigest(info);
        return NULL;
    }
    info->ram_size = ram_size;
    info->frame = xemu_frame_count();
    info->elapsed_us = elapsed_us;
    return info;
}
