/*
 * xemu: the console language for a run (XEMU_LANGUAGE, QMP
 * xemu-set-language)
 *
 * The guest reads its language from the EEPROM: XAPI's XGetLanguage asks
 * the kernel's ExQueryNonVolatileSetting(XC_LANGUAGE), which answers from
 * a copy of the whole EEPROM the kernel reads over SMBus once per boot
 * (the 4627 kernel keeps it at 0x8003AEE8, behind a valid flag at
 * 0x8003AEE0). So the device alone covers a boot: smbus_storage.c shows
 * the guest the requested language and a matching user-section checksum
 * at every read. After a load the kernel's copy is the state's, with the
 * state's language, so the load also sets the language (and the checksum)
 * in every copy of the EEPROM image it finds in guest RAM: the image as
 * the device holds it, bar those two fields. That changes guest RAM, only
 * behind the opt-in and only when the state's language differs; a load
 * of a state already in that language leaves RAM as a plain load does.
 *
 * Titles that ask once per boot keep what they asked then: Azurik asks
 * in its main() only, so a loaded state keeps the language its session
 * booted with until the next boot.
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ramblock.h"
#include "system/runstate.h"
#include "hw/xbox/smbus.h"
#include "hw/xbox/eeprom-fields.h"
#include "xemu-language.h"

/* Copies looked for (the kernel keeps one) and where to look first. */
#define LANGUAGE_MAX_COPIES 4
#define LANGUAGE_LOW_SCAN (16 * MiB)

static int64_t said_copy = -2;   /* the copy the last stderr line named */

static bool image_matches(const uint8_t *p, const uint8_t *e)
{
    return !memcmp(p, e, XBOX_EEPROM_USER_CHECKSUM) &&
           !memcmp(p + XBOX_EEPROM_USER_SUM_START,
                   e + XBOX_EEPROM_USER_SUM_START,
                   XBOX_EEPROM_LANGUAGE - XBOX_EEPROM_USER_SUM_START) &&
           !memcmp(p + XBOX_EEPROM_LANGUAGE + 4, e + XBOX_EEPROM_LANGUAGE + 4,
                   XBOX_EEPROM_SIZE - XBOX_EEPROM_LANGUAGE - 4);
}

static int scan(const uint8_t *ram, uint64_t lo, uint64_t hi,
                const uint8_t *e, int64_t *found, int n)
{
    uint32_t w;

    memcpy(&w, e, 4);
    for (uint64_t off = lo; off + XBOX_EEPROM_SIZE <= hi &&
         n < LANGUAGE_MAX_COPIES; off += 4) {
        uint32_t v;

        memcpy(&v, ram + off, 4);
        if (v == w && image_matches(ram + off, e)) {
            found[n++] = off;
        }
    }
    return n;
}

static void set_word(uint64_t pa, const uint8_t *ram, const uint8_t *img,
                     unsigned off)
{
    if (memcmp(ram + pa + off, img + off, 4)) {
        address_space_write(&address_space_memory, pa + off,
                            MEMTXATTRS_UNSPECIFIED, img + off, 4);
    }
}

/*
 * Set lang (0: the device's own) in the guest's copies of the EEPROM
 * image. The VM is stopped, so this lands at a fixed point of a
 * deterministic run. Returns the copies found, -1 without an EEPROM.
 */
static int language_set_copies(uint32_t lang, int64_t *where)
{
    uint8_t e[XBOX_EEPROM_SIZE];
    int64_t found[LANGUAGE_MAX_COPIES];
    RAMBlock *rb;
    const uint8_t *ram;
    uint64_t size;
    int n = 0;

    if (!xbox_eeprom_read(0, e, sizeof(e))) {
        return -1;
    }
    if (!lang) {
        lang = xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE);
    }
    WITH_RCU_READ_LOCK_GUARD() {
        rb = qemu_ram_block_by_name("xbox.ram");
    }
    if (!rb) {
        return 0;
    }
    ram = qemu_ram_get_host_addr(rb);
    size = qemu_ram_get_used_length(rb);
    /*
     * A full scan every time, never a remembered address, so what a load
     * patches depends on the loaded RAM alone, as in a fresh process.
     */
    n = scan(ram, 0, MIN(LANGUAGE_LOW_SCAN, size), e, found, 0);
    if (!n) {
        n = scan(ram, LANGUAGE_LOW_SCAN, size, e, found, 0);
    }
    for (int i = 0; i < n; i++) {
        uint8_t img[XBOX_EEPROM_SIZE];

        memcpy(img, ram + found[i], sizeof(img));
        xbox_eeprom_move_language(img, lang);
        set_word(found[i], ram, img, XBOX_EEPROM_USER_CHECKSUM);
        set_word(found[i], ram, img, XBOX_EEPROM_LANGUAGE);
    }
    *where = n ? found[0] : -1;
    return n;
}

static const char *language_label(uint32_t v)
{
    const char *name = xbox_language_name(v, false);
    return name ? name : "XC_LANGUAGE";
}

void xemu_language_state_loaded(void)
{
    uint32_t lang = xbox_eeprom_language();
    int64_t where;
    int n;

    if (!lang) {
        return;
    }
    xbox_eeprom_set_language(lang); /* the guest's copy holds it again */
    n = language_set_copies(lang, &where);
    if (n > 0 && where != said_copy) {
        fprintf(stderr, "xemu: language: %s (%u) set in the kernel's EEPROM "
                "copy at 0x%" PRIx64 "%s (load)\n", language_label(lang),
                lang, where, n > 1 ? " and others" : "");
        said_copy = where;
    } else if (n == 0 && said_copy != -1) {
        fprintf(stderr, "xemu: language: %s (%u): no EEPROM copy in guest "
                "RAM (load); the guest reads the device's\n",
                language_label(lang), lang);
        said_copy = -1;
    }
}

XemuLanguage *qmp_xemu_set_language(const char *language, Error **errp)
{
    XemuLanguage *info;
    uint8_t e[XBOX_EEPROM_SIZE];
    uint32_t lang = xbox_eeprom_language();
    int64_t where = -1;
    int n = 0;

    if (!xbox_eeprom_read(0, e, sizeof(e))) {
        error_setg(errp, "the machine has no Xbox EEPROM");
        return NULL;
    }
    if (language) {
        if (!xbox_language_parse(language, &lang)) {
            error_setg(errp, "'%s' is not a language (en ja de fr es it ko "
                       "zh pt, a name such as french, an XC_LANGUAGE "
                       "number, or off)", language);
            return NULL;
        }
        if (runstate_is_running()) {
            error_setg(errp, "the VM is running: pause it first (the "
                       "language goes into guest RAM)");
            return NULL;
        }
        xbox_eeprom_set_language(lang);
        n = MAX(language_set_copies(lang, &where), 0);
        said_copy = n ? where : -1;
        if (lang) {
            fprintf(stderr, "xemu: language: %s (%u) from xemu-set-language;"
                    " %d kernel EEPROM %s set\n", language_label(lang), lang,
                    n, n == 1 ? "copy" : "copies");
        } else {
            fprintf(stderr, "xemu: language: off from xemu-set-language (the "
                    "EEPROM's own, %u); %d kernel EEPROM %s set\n",
                    xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE), n,
                    n == 1 ? "copy" : "copies");
        }
    }

    info = g_new0(XemuLanguage, 1);
    info->value = lang;
    info->language = !lang ? g_strdup("off") :
                     xbox_language_name(lang, true) ?
                     g_strdup(xbox_language_name(lang, true)) :
                     g_strdup_printf("%u", lang);
    info->eeprom = xbox_eeprom_le32(e, XBOX_EEPROM_LANGUAGE);
    info->kernel_copies = n;
    return info;
}
