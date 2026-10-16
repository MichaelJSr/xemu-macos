/*
 * xemu RAM digest: QMP xemu-ram-digest
 *
 * Guest RAM hashed in place, with no file and no GPU write-back: the
 * sha256 of all of it (what xemu-dump-ram's file hashes to), optionally an
 * 8-byte hash per 4 KiB page, or the pages whose bytes differ from a
 * checkpoint's RAM image (xemu-checkpoint-ram.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "system/runstate.h"
#include "xemu-checkpoints.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include "crypto/hash.h"
#endif

#define SHA256_LEN 32
#define PAGE_HASH_LEN 8

int64_t xemu_frame_count(void);

/*
 * sha256: CommonCrypto on macOS, where it runs on the CPU's SHA-2
 * instructions (about 2.4 GB/s on Apple silicon, against a few hundred
 * MB/s for glib's, the crypto backend this build has); QEMU's crypto
 * layer elsewhere.
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
    return qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, buf, len, &out,
                              &out_len, errp) == 0;
#endif
}

XemuRamDigest *qmp_xemu_ram_digest(bool has_sha256, bool sha256,
                                   bool has_pages, bool pages,
                                   const char *against, Error **errp)
{
    RAMBlock *rb;
    const uint8_t *ram;
    uint64_t ram_size;
    const uint8_t *const *image = NULL;
    uint64_t image_pages = 0;

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

    if (sha256) {
        uint8_t md[SHA256_LEN];

        ok = sha256_bytes(ram, ram_size, md, errp);
        if (ok) {
            info->sha256 = g_string_free(qemu_hexdump_line(NULL, md,
                                                           SHA256_LEN, 0, 0),
                                         false);
        }
    }
    if (ok && pages) {
        GString *hex = g_string_sized_new(npages * PAGE_HASH_LEN * 2 + 1);
        uint8_t md[SHA256_LEN];

        for (uint64_t i = 0; ok && i < npages; i++) {
            ok = sha256_bytes(ram + i * page, page, md, errp);
            qemu_hexdump_line(hex, md, PAGE_HASH_LEN, 0, 0);
        }
        info->page_hashes = g_string_free(hex, !ok);
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
