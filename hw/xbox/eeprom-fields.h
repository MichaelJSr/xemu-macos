/*
 * Xbox EEPROM fields the machine model reads (xemu)
 *
 * Pure decoders over the 256-byte EEPROM image (XboxEEPROM in
 * eeprom_generation.h), shared by the device model and its unit test.
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_EEPROM_FIELDS_H
#define HW_XBOX_EEPROM_FIELDS_H

#define XBOX_EEPROM_SIZE 256

/* Factory section: the AV region (video standard), e.g. 0x00800300. */
#define XBOX_EEPROM_VIDEO_STANDARD 0x58
/* User section: XC_VIDEO flags. */
#define XBOX_EEPROM_VIDEO_FLAGS 0x94

#define XBOX_VIDEO_STANDARD_PAL_I 3       /* (standard >> 8) & 0xff */
#define XBOX_VIDEO_FLAG_PAL_60HZ 0x00400000

#define XBOX_VBLANK_NS_60HZ 16666666LL
#define XBOX_VBLANK_NS_50HZ 20000000LL

static inline uint32_t xbox_eeprom_le32(const uint8_t *e, unsigned off)
{
    return e[off] | e[off + 1] << 8 | e[off + 2] << 16 |
           (uint32_t)e[off + 3] << 24;
}

/*
 * A PAL50 console: the video standard is PAL-I and the user hasn't set
 * PAL-60, the test PAL titles make before they pick 25 or 50 fps
 * timing (Azurik Europe's frame-rate latch). NTSC-M, NTSC-J and PAL-M
 * consoles, and PAL-I with PAL-60, run at 60 Hz.
 */
static inline bool xbox_eeprom_is_pal50(const uint8_t *e)
{
    uint32_t std = xbox_eeprom_le32(e, XBOX_EEPROM_VIDEO_STANDARD);
    uint32_t flags = xbox_eeprom_le32(e, XBOX_EEPROM_VIDEO_FLAGS);

    return ((std >> 8) & 0xff) == XBOX_VIDEO_STANDARD_PAL_I &&
           !(flags & XBOX_VIDEO_FLAG_PAL_60HZ);
}

/*
 * The console language (XC_LANGUAGE) and the user section's checksum,
 * which covers 0x64..0xbf; the kernel refuses a section whose checksum
 * fails (ExQueryNonVolatileSetting then returns an error and XAPI's
 * XGetLanguage 0).
 */
#define XBOX_EEPROM_USER_CHECKSUM 0x60
#define XBOX_EEPROM_USER_SUM_START 0x64
#define XBOX_EEPROM_USER_SUM_LEN 0x5c
#define XBOX_EEPROM_LANGUAGE 0x90

static inline uint32_t xbox_eeprom_sum(const uint8_t *p, unsigned len)
{
    uint32_t high = 0, low = 0;

    for (unsigned i = 0; i + 4 <= len; i += 4) {
        uint32_t v = xbox_eeprom_le32(p, i);
        uint64_t sum = ((uint64_t)high << 32) | low;

        high = (sum + v) >> 32;
        low += v;
    }
    return ~(high + low);
}

static inline void xbox_eeprom_put_le32(uint8_t *e, unsigned off, uint32_t v)
{
    e[off] = v;
    e[off + 1] = v >> 8;
    e[off + 2] = v >> 16;
    e[off + 3] = v >> 24;
}

/*
 * Set the image's language and recompute the user section's checksum (a
 * section that failed its check passes after this).
 */
static inline void xbox_eeprom_put_language(uint8_t *e, uint32_t lang)
{
    xbox_eeprom_put_le32(e, XBOX_EEPROM_LANGUAGE, lang);
    xbox_eeprom_put_le32(e, XBOX_EEPROM_USER_CHECKSUM,
                         xbox_eeprom_sum(e + XBOX_EEPROM_USER_SUM_START,
                                         XBOX_EEPROM_USER_SUM_LEN));
}

/* The checksum the image's user section sums to. */
static inline uint32_t xbox_eeprom_user_sum(const uint8_t *e)
{
    return xbox_eeprom_sum(e + XBOX_EEPROM_USER_SUM_START,
                           XBOX_EEPROM_USER_SUM_LEN);
}

/*
 * Set the image's language and move the user checksum with it: a section
 * whose checksum matched still matches, one that failed fails by the same
 * bits. Moving back to the old language restores the image exactly, so
 * this maps the EEPROM to the guest's view of it and back.
 */
static inline void xbox_eeprom_move_language(uint8_t *e, uint32_t lang)
{
    uint32_t off_by = xbox_eeprom_le32(e, XBOX_EEPROM_USER_CHECKSUM) ^
                      xbox_eeprom_user_sum(e);

    xbox_eeprom_put_le32(e, XBOX_EEPROM_LANGUAGE, lang);
    xbox_eeprom_put_le32(e, XBOX_EEPROM_USER_CHECKSUM,
                         off_by ^ xbox_eeprom_user_sum(e));
}

/*
 * A guest write of len bytes at off (wrapping at the end) into the image
 * e while the guest's own copy of the EEPROM (the kernel's, which it
 * builds its writes from) holds the language *guest, not e's: the write
 * lands on e moved to that language, which then moves back to e's own,
 * unless the write changed the field (the guest set a language itself:
 * e and *guest take it). So e's user checksum matches after the write
 * exactly when the guest's does, whatever order the guest writes a
 * setting's bytes and its checksum in, and a write that leaves the
 * language alone changes in e only the bytes it wrote, bar the checksum.
 */
static inline void xbox_eeprom_write_viewed(uint8_t *e, uint32_t *guest,
                                            unsigned off, const uint8_t *buf,
                                            unsigned len)
{
    uint32_t own = xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE);
    uint32_t was = *guest, now;

    xbox_eeprom_move_language(e, was);
    for (unsigned i = 0; i < len; i++) {
        e[(off + i) % XBOX_EEPROM_SIZE] = buf[i];
    }
    now = xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE);
    *guest = now;
    xbox_eeprom_move_language(e, now == was ? own : now);
}

/* XC_LANGUAGE values, 1-9, by code and name; NULL outside them. */
static inline const char *xbox_language_name(uint32_t v, bool code)
{
    static const char *const names[][2] = {
        { "en", "English" }, { "ja", "Japanese" }, { "de", "German" },
        { "fr", "French" }, { "es", "Spanish" }, { "it", "Italian" },
        { "ko", "Korean" }, { "zh", "Chinese" }, { "pt", "Portuguese" },
    };
    return v >= 1 && v <= 9 ? names[v - 1][code ? 0 : 1] : NULL;
}

/*
 * A language a person typed: a code (fr), a name (french, any case) or a
 * number (4, 0x4; any 32-bit value goes to the guest as is). 0 for off,
 * none or an empty string; false when not understood.
 */
static inline bool xbox_language_parse(const char *s, uint32_t *out)
{
    char *end;
    unsigned long long n;

    if (!s[0] || !g_ascii_strcasecmp(s, "off") ||
        !g_ascii_strcasecmp(s, "none")) {
        *out = 0;
        return true;
    }
    for (uint32_t v = 1; v <= 9; v++) {
        if (!g_ascii_strcasecmp(s, xbox_language_name(v, true)) ||
            !g_ascii_strcasecmp(s, xbox_language_name(v, false))) {
            *out = v;
            return true;
        }
    }
    errno = 0;
    n = strtoull(s, &end, 0);
    if (g_ascii_isdigit(s[0]) && !*end && !errno && n <= UINT32_MAX) {
        *out = n;
        return true;
    }
    return false;
}

#endif
