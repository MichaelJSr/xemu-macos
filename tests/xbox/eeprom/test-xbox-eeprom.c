/*
 * Xbox EEPROM field decoders (hw/xbox/eeprom-fields.h)
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/xbox/eeprom-fields.h"

static void put32(uint8_t *e, unsigned off, uint32_t v)
{
    e[off] = v;
    e[off + 1] = v >> 8;
    e[off + 2] = v >> 16;
    e[off + 3] = v >> 24;
}

static bool pal50(uint32_t standard, uint32_t flags)
{
    uint8_t e[XBOX_EEPROM_SIZE] = { 0 };

    put32(e, XBOX_EEPROM_VIDEO_STANDARD, standard);
    put32(e, XBOX_EEPROM_VIDEO_FLAGS, flags);
    return xbox_eeprom_is_pal50(e);
}

static void test_pal50(void)
{
    g_assert_false(pal50(0x00400100, 0));          /* NTSC-M */
    g_assert_false(pal50(0x00400200, 0));          /* NTSC-J */
    g_assert_false(pal50(0x00400400, 0));          /* PAL-M */
    g_assert_true(pal50(0x00800300, 0));           /* PAL-I */
    g_assert_true(pal50(0x00800300, 0x00010000));  /* PAL-I, widescreen */
    g_assert_false(pal50(0x00800300, 0x00400000)); /* PAL-I, PAL-60 */
    g_assert_false(pal50(0, 0));
    g_assert_false(pal50(0xffffffff, 0));
}

/*
 * The user section's checksum: xemu's EEPROM generator's and the kernel's
 * check, which adds the checksum word to the section's 23 words, carries
 * folded, and wants all ones.
 */
static uint32_t kernel_check(const uint8_t *e)
{
    uint32_t high = 0, low = 0;

    for (unsigned off = XBOX_EEPROM_USER_CHECKSUM;
         off < XBOX_EEPROM_USER_SUM_START + XBOX_EEPROM_USER_SUM_LEN;
         off += 4) {
        uint32_t v = xbox_eeprom_le32(e, off);
        uint64_t sum = ((uint64_t)high << 32) | low;

        high = (sum + v) >> 32;
        low += v;
    }
    return high + low;
}

static void test_language(void)
{
    uint8_t e[XBOX_EEPROM_SIZE];

    for (unsigned i = 0; i < sizeof(e); i++) {
        e[i] = i * 151 + 7;
    }
    xbox_eeprom_put_language(e, 1);
    g_assert_cmphex(xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE), ==, 1);
    g_assert_cmphex(kernel_check(e), ==, 0xffffffff);
    for (uint32_t lang = 2; lang <= 9; lang++) {
        uint8_t f[XBOX_EEPROM_SIZE];

        memcpy(f, e, sizeof(f));
        xbox_eeprom_put_language(f, lang);
        g_assert_cmphex(kernel_check(f), ==, 0xffffffff);
        /* Only the language and the checksum change. */
        for (unsigned i = 0; i < sizeof(f); i++) {
            if ((i < XBOX_EEPROM_USER_CHECKSUM ||
                 i >= XBOX_EEPROM_USER_CHECKSUM + 4) &&
                (i < XBOX_EEPROM_LANGUAGE || i >= XBOX_EEPROM_LANGUAGE + 4)) {
                g_assert_cmpuint(f[i], ==, e[i]);
            }
        }
        xbox_eeprom_put_language(f, 1);
        g_assert_cmpmem(f, sizeof(f), e, sizeof(e));
    }
}

static void own_image(uint8_t *e, uint32_t lang)
{
    for (unsigned i = 0; i < XBOX_EEPROM_SIZE; i++) {
        e[i] = i * 151 + 7;
    }
    xbox_eeprom_put_language(e, lang);
}

static void test_move_language(void)
{
    uint8_t e[XBOX_EEPROM_SIZE], v[XBOX_EEPROM_SIZE];

    own_image(e, 1);
    memcpy(v, e, sizeof(v));
    xbox_eeprom_move_language(v, 4);
    g_assert_cmphex(xbox_eeprom_le32(v, XBOX_EEPROM_LANGUAGE), ==, 4);
    g_assert_cmphex(kernel_check(v), ==, 0xffffffff);
    xbox_eeprom_move_language(v, 1);
    g_assert_cmpmem(v, sizeof(v), e, sizeof(e));

    /* A section that fails its check still fails, by the same bits. */
    e[XBOX_EEPROM_USER_CHECKSUM + 1] ^= 0x5a;
    memcpy(v, e, sizeof(v));
    xbox_eeprom_move_language(v, 3);
    g_assert_cmphex(kernel_check(v), !=, 0xffffffff);
    g_assert_cmphex(xbox_eeprom_le32(v, XBOX_EEPROM_USER_CHECKSUM) ^
                    xbox_eeprom_user_sum(v), ==,
                    xbox_eeprom_le32(e, XBOX_EEPROM_USER_CHECKSUM) ^
                    xbox_eeprom_user_sum(e));
    xbox_eeprom_move_language(v, 1);
    g_assert_cmpmem(v, sizeof(v), e, sizeof(e));
}

/*
 * The kernel saves a setting from its copy of the EEPROM k, which holds
 * the language the guest read and a checksum over it: the bytes that
 * differ, one SMBus write each, in either order, or the whole section.
 */
enum { BYTES_UP, BYTES_DOWN, WHOLE_SECTION };

static void kernel_save(uint8_t *e, uint32_t *guest, uint8_t *k,
                        unsigned off, uint32_t value, int how)
{
    uint8_t old[XBOX_EEPROM_SIZE];
    const unsigned lo = XBOX_EEPROM_USER_CHECKSUM;
    const unsigned hi = XBOX_EEPROM_USER_SUM_START + XBOX_EEPROM_USER_SUM_LEN;

    memcpy(old, k, sizeof(old));
    put32(k, off, value);
    put32(k, XBOX_EEPROM_USER_CHECKSUM, xbox_eeprom_user_sum(k));
    if (how == WHOLE_SECTION) {
        xbox_eeprom_write_viewed(e, guest, lo, k + lo, hi - lo);
        return;
    }
    for (unsigned n = 0; n < hi - lo; n++) {
        unsigned i = how == BYTES_UP ? lo + n : hi - 1 - n;
        if (k[i] != old[i]) {
            xbox_eeprom_write_viewed(e, guest, i, k + i, 1);
        }
    }
}

/* What the guest reads with the override: the image moved to lang. */
static void guest_view(uint8_t *k, const uint8_t *e, uint32_t lang)
{
    memcpy(k, e, XBOX_EEPROM_SIZE);
    xbox_eeprom_move_language(k, lang);
}

static void test_write_viewed(void)
{
    uint8_t own[XBOX_EEPROM_SIZE], e[XBOX_EEPROM_SIZE], k[XBOX_EEPROM_SIZE];

    own_image(own, 1);
    for (int how = BYTES_UP; how <= WHOLE_SECTION; how++) {
        uint32_t guest = 4;

        /* A setting saved under French keeps the EEPROM's English. */
        memcpy(e, own, sizeof(e));
        guest_view(k, e, 4);
        kernel_save(e, &guest, k, 0x98, 0x00010003, how);
        g_assert_cmphex(xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE), ==, 1);
        g_assert_cmphex(xbox_eeprom_le32(e, 0x98), ==, 0x00010003);
        g_assert_cmphex(kernel_check(e), ==, 0xffffffff);
        g_assert_cmpuint(guest, ==, 4);
        for (unsigned i = 0; i < sizeof(e); i++) {
            if (i < XBOX_EEPROM_USER_CHECKSUM ||
                (i >= XBOX_EEPROM_USER_CHECKSUM + 4 && (i < 0x98 || i >= 0x9c))) {
                g_assert_cmpuint(e[i], ==, own[i]);
            }
        }

        /* The guest setting a language itself sets it, then saves more. */
        kernel_save(e, &guest, k, XBOX_EEPROM_LANGUAGE, 3, how);
        g_assert_cmphex(xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE), ==, 3);
        g_assert_cmphex(kernel_check(e), ==, 0xffffffff);
        g_assert_cmpuint(guest, ==, 3);
        kernel_save(e, &guest, k, 0x64, 0x01020304, how);
        g_assert_cmphex(xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE), ==, 3);
        g_assert_cmphex(xbox_eeprom_le32(e, 0x64), ==, 0x01020304);
        g_assert_cmphex(kernel_check(e), ==, 0xffffffff);
        /* Back to the override's language: the guest chose it. */
        kernel_save(e, &guest, k, XBOX_EEPROM_LANGUAGE, 4, how);
        g_assert_cmphex(xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE), ==, 4);
        g_assert_cmphex(kernel_check(e), ==, 0xffffffff);
    }

    /*
     * Between a setting's bytes and its checksum the section fails its
     * check, as on the plain device; only the checksum differs from it.
     */
    {
        uint8_t w[4] = { 0x11, 0x22, 0x33, 0x44 };
        uint8_t plain[XBOX_EEPROM_SIZE];
        uint32_t guest = 4;

        memcpy(e, own, sizeof(e));
        memcpy(plain, own, sizeof(plain));
        memcpy(plain + 0x64, w, 4);
        xbox_eeprom_write_viewed(e, &guest, 0x64, w, 4);
        g_assert_cmphex(kernel_check(e), !=, 0xffffffff);
        g_assert_cmpmem(e + XBOX_EEPROM_USER_SUM_START,
                        sizeof(e) - XBOX_EEPROM_USER_SUM_START,
                        plain + XBOX_EEPROM_USER_SUM_START,
                        sizeof(plain) - XBOX_EEPROM_USER_SUM_START);
        g_assert_cmpmem(e, XBOX_EEPROM_USER_CHECKSUM,
                        plain, XBOX_EEPROM_USER_CHECKSUM);
    }

    /* Outside the user section a write lands as it is. */
    {
        uint8_t w[3] = { 0xaa, 0xbb, 0xcc };
        uint32_t guest = 4;

        memcpy(e, own, sizeof(e));
        xbox_eeprom_write_viewed(e, &guest, 0x10, w, 3);
        memcpy(own + 0x10, w, 3);
        g_assert_cmpmem(e, sizeof(e), own, sizeof(own));
    }
}

static void test_language_parse(void)
{
    uint32_t v = 99;

    g_assert_true(xbox_language_parse("fr", &v) && v == 4);
    g_assert_true(xbox_language_parse("French", &v) && v == 4);
    g_assert_true(xbox_language_parse("DE", &v) && v == 3);
    g_assert_true(xbox_language_parse("german", &v) && v == 3);
    g_assert_true(xbox_language_parse("en", &v) && v == 1);
    g_assert_true(xbox_language_parse("pt", &v) && v == 9);
    g_assert_true(xbox_language_parse("4", &v) && v == 4);
    g_assert_true(xbox_language_parse("0x10", &v) && v == 16);
    g_assert_true(xbox_language_parse("off", &v) && v == 0);
    g_assert_true(xbox_language_parse("", &v) && v == 0);
    g_assert_false(xbox_language_parse("klingon", &v));
    g_assert_false(xbox_language_parse("-1", &v));
    g_assert_false(xbox_language_parse("4x", &v));
    g_assert_false(xbox_language_parse("99999999999", &v));
    g_assert_cmpstr(xbox_language_name(4, true), ==, "fr");
    g_assert_cmpstr(xbox_language_name(3, false), ==, "German");
    g_assert_null(xbox_language_name(0, true));
    g_assert_null(xbox_language_name(10, false));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/eeprom/pal50", test_pal50);
    g_test_add_func("/xbox/eeprom/language", test_language);
    g_test_add_func("/xbox/eeprom/language-parse", test_language_parse);
    g_test_add_func("/xbox/eeprom/move-language", test_move_language);
    g_test_add_func("/xbox/eeprom/write-viewed", test_write_viewed);
    return g_test_run();
}
