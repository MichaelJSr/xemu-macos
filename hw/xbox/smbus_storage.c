/*
 * QEMU SMBus Generic Storage Device
 *
 * Copyright (c) 2020 Mike Davis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_slave.h"
#include "smbus.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "migration/vmstate.h"
#include "system/reset.h"
#include "eeprom-fields.h"

#define TYPE_SMBUS_STORAGE "smbus-storage"
#define SMBUS_STORAGE(obj) OBJECT_CHECK(SMBusStorageDevice, (obj), \
    TYPE_SMBUS_STORAGE)

//#define DEBUG
#ifdef DEBUG
# define DPRINTF(format, ...) printf(format, ## __VA_ARGS__)
#else
# define DPRINTF(format, ...) do { } while (0)
#endif

typedef struct SMBusStorageDevice {
    SMBusDevice smbusdev;
    char* file;
    uint8_t *data;
    uint32_t size;
    uint8_t addr;
    uint32_t offset;
    bool persist;
} SMBusStorageDevice;

/*
 * xemu: the console language the guest reads from the EEPROM at 0x54, or
 * 0 for the EEPROM's own (XEMU_LANGUAGE, QMP xemu-set-language). The
 * device's contents stay the EEPROM's, in snapshots too: only the guest's
 * reads of the language and the user section's checksum see the override
 * (xbox_eeprom_move_language), a guest write, which the guest builds from
 * its own copy of what it read, lands on that view and moves back to the
 * EEPROM's own language (xbox_eeprom_write_viewed), and while it is set
 * guest writes stay in the machine instead of reaching the file.
 * eeprom_guest_language is the language the guest's copy holds: the
 * override from a boot (the guest reads it), a load or a set (they put
 * it in the kernel's copy), else what the guest last wrote itself.
 */
static uint32_t eeprom_language;
static uint32_t eeprom_guest_language;

uint32_t xbox_eeprom_language(void)
{
    return qatomic_read(&eeprom_language);
}

void xbox_eeprom_set_language(uint32_t lang)
{
    qatomic_set(&eeprom_language, lang);
    eeprom_guest_language = lang;
}

static void eeprom_language_reset(void *opaque)
{
    eeprom_guest_language = eeprom_language;
}

static bool eeprom_is_xbox(SMBusStorageDevice *s)
{
    return s->addr == 0x54 && s->size == XBOX_EEPROM_SIZE;
}

static void smbus_storage_realize(DeviceState *dev, Error **errp)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);
    const char *lang = getenv("XEMU_LANGUAGE");

    if (lang && lang[0] && eeprom_is_xbox(s)) {
        uint32_t v;
        const char *name;

        if (!xbox_language_parse(lang, &v)) {
            error_setg(errp, "XEMU_LANGUAGE='%s': not a language (en ja de "
                       "fr es it ko zh pt, a name such as french, or an "
                       "XC_LANGUAGE number)", lang);
            return;
        }
        xbox_eeprom_set_language(v);
        name = xbox_language_name(v, false);
        if (v) {
            fprintf(stderr, "xemu: language: %s (%u) from XEMU_LANGUAGE: "
                    "the guest reads it from the EEPROM, every load sets it "
                    "in the kernel's copy, and EEPROM writes stay out of "
                    "the file\n", name ? name : "XC_LANGUAGE", v);
        }
    }
    if (eeprom_is_xbox(s)) {
        qemu_register_reset(eeprom_language_reset, NULL);
    }
    qdev_prop_set_uint8(dev, "address", s->addr);
    s->data = g_malloc0(s->size);
    s->offset = 0;

    if (s->file) {
        int size = get_image_size(s->file, NULL);
        if (size != s->size) {
            error_setg(errp, "%s: file '%s' size of %d, expected %d\n",
                __func__, s->file, size, s->size);
            return;
        }

        int fd = qemu_open(s->file, O_RDONLY | O_BINARY, NULL);
        if (fd < 0) {
            error_setg(errp, "%s: file '%s' could not be opened\n",
                __func__, s->file);
            return;
        }

        int rc = read(fd, s->data, s->size);
        if (rc != s->size) {
            error_setg(errp, "%s: file '%s' read failure\n", __func__, s->file);
            close(fd);
            return;
        }
        close(fd);
    } else {
        error_setg(errp, "%s: file unspecified\n", __func__);
    }
}

static int smbus_storage_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    DPRINTF("%s: addr=0x%02x cmd=0x%02x val=0x%02x\n",
        __func__, s->addr, buf[0], buf[1]);

    /* len is guaranteed to be > 0 */
    s->offset = buf[0];
    buf++;
    len--;

    bool changed = len > 0;
    uint32_t lang = xbox_eeprom_language();
    if (unlikely(lang) && changed && eeprom_is_xbox(s)) {
        /* Built from the guest's copy: keep the EEPROM's own language. */
        xbox_eeprom_write_viewed(s->data, &eeprom_guest_language, s->offset,
                                 buf, len);
        s->offset = (s->offset + len) % s->size;
        len = 0;
    }
    for (; len > 0; len--) {
        s->data[s->offset] = *buf++;
        DPRINTF("%s: addr=0x%02x off=0x%02x, data=0x%02x\n",
            __func__, s->addr, s->offset, s->data[s->offset]);
        s->offset = (s->offset + 1) % s->size;
    }

    if (changed && s->file && s->persist && lang && eeprom_is_xbox(s)) {
        static bool said;
        if (!said) {
            said = true;
            fprintf(stderr, "xemu: language: a guest EEPROM write stays out "
                    "of '%s' while the language is set\n", s->file);
        }
    } else if (changed && s->file && s->persist) {
        int fd = qemu_open(s->file, O_WRONLY | O_BINARY, NULL);
        if (fd < 0) {
            DPRINTF("%s: file '%s' could not be opened\n", __func__, s->file);
            return -1;
        }

        int wc = write(fd, s->data, s->size);
        if (wc != s->size) {
            DPRINTF( "%s: file '%s' write failure\n", __func__, s->file);
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

static uint8_t smbus_storage_receive_byte(SMBusDevice *dev)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    uint8_t val = s->data[s->offset];
    uint32_t lang = xbox_eeprom_language();

    if (unlikely(lang) && eeprom_is_xbox(s) &&
        ((s->offset >= XBOX_EEPROM_USER_CHECKSUM &&
          s->offset < XBOX_EEPROM_USER_CHECKSUM + 4) ||
         (s->offset >= XBOX_EEPROM_LANGUAGE &&
          s->offset < XBOX_EEPROM_LANGUAGE + 4))) {
        uint8_t view[XBOX_EEPROM_SIZE];

        memcpy(view, s->data, sizeof(view));
        xbox_eeprom_move_language(view, lang);
        val = view[s->offset];
    }
    DPRINTF("%s: addr=0x%02x off=0x%02x val=0x%02x\n",
        __func__, s->addr, s->offset, val);
    s->offset = (s->offset + 1) % s->size;

    return val;
}

static const VMStateDescription vmstate_smbus_storage = {
    .name = "smbus-storage",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(smbusdev, SMBusStorageDevice),
        VMSTATE_VBUFFER_UINT32(data, SMBusStorageDevice, 1, NULL, size),
        VMSTATE_UINT32(offset, SMBusStorageDevice),
        VMSTATE_END_OF_LIST()
    }
};

// default xbox eeprom with persistence
static const Property smbus_storage_props[] = {
    DEFINE_PROP_UINT8("addr", SMBusStorageDevice, addr, 0x54),
    DEFINE_PROP_UINT32("size", SMBusStorageDevice, size, 256),
    DEFINE_PROP_BOOL("persist", SMBusStorageDevice, persist, true),
    DEFINE_PROP_STRING("file", SMBusStorageDevice, file),
};

static void smbus_storage_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *sc = SMBUS_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_smbus_storage;
    dc->realize = smbus_storage_realize;
    sc->receive_byte = smbus_storage_receive_byte;
    sc->write_data = smbus_storage_write_data;
    device_class_set_props(dc, smbus_storage_props);
}

/*
 * xemu: the Xbox EEPROM, the smbus-storage device at 0x54 (vl.c adds it):
 * len bytes of its contents as the machine holds them (a load restores
 * them; guest writes change them). False with no such device.
 */
bool xbox_eeprom_read(uint32_t offset, void *buf, uint32_t len)
{
    Object *obj = object_resolve_path_type("", TYPE_SMBUS_STORAGE, NULL);
    SMBusStorageDevice *s = obj ? SMBUS_STORAGE(obj) : NULL;

    if (!s || !s->data || s->addr != 0x54 || offset > s->size ||
        len > s->size - offset) {
        return false;
    }
    memcpy(buf, s->data + offset, len);
    return true;
}

static TypeInfo smbus_storage_info = {
    .name = TYPE_SMBUS_STORAGE,
    .parent = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(SMBusStorageDevice),
    .class_init = smbus_storage_class_init,
};

static void smbus_storage_register_devices(void)
{
    type_register_static(&smbus_storage_info);
}

type_init(smbus_storage_register_devices)
