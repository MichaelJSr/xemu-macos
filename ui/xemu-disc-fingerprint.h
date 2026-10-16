/*
 * xemu disc fingerprints: a disc image's identity by content
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_DISC_FINGERPRINT_H
#define XEMU_DISC_FINGERPRINT_H

#include "qapi/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_DISC_DIGEST_LEN 16

typedef struct XemuDiscFingerprint {
    uint64_t size;
    uint8_t digest[XEMU_DISC_DIGEST_LEN];
} XemuDiscFingerprint;

/* The image at path, from the cache when its file hasn't changed. */
bool xemu_disc_fingerprint(const char *path, XemuDiscFingerprint *fp,
                           Error **errp);

/*
 * Whether the image at tray_path has the content of the snapshot's disc:
 * its saved fingerprint if it has one (saved_fp), else saved_path's file
 * if that is still the file it was when the snapshot was saved
 * (saved_at, the snapshot's date). *how gets a phrase for the log.
 */
bool xemu_disc_same_content(const char *tray_path, const char *saved_path,
                            const XemuDiscFingerprint *saved_fp,
                            int64_t saved_at, char **how);

#ifdef __cplusplus
}
#endif

#endif
