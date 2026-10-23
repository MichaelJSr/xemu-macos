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

#endif
