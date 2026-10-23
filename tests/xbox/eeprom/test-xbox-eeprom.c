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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/eeprom/pal50", test_pal50);
    return g_test_run();
}
