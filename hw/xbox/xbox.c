/*
 * QEMU Xbox System Emulator
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/option.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "hw/hw.h"
#include "hw/loader.h"
#include "hw/i386/pc.h"
#include "hw/i386/kvm/clock.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/ide/pci.h"
#include "system/system.h"
#include "system/kvm.h"
#include "kvm/kvm_i386.h"
#include "hw/dma/i8257.h"

#include "hw/sysbus.h"
#include "system/arch_init.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/ramblock.h"
#include "cpu.h"

#include "qapi/error.h"
#include "qemu/error-report.h"

#include "hw/timer/i8254.h"
#include "hw/audio/pcspk.h"
#include "hw/rtc/mc146818rtc.h"

#include "hw/xbox/xbox_pci.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_eeprom.h"
#include "migration/vmstate.h"
#ifndef _WIN32
#include <sys/file.h>
#include <sys/mman.h>
#endif
#include "ui/xemu-automation.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/mcpx/apu/apu.h"

#include "hw/xbox/xbox.h"
#include "smbus.h"

#define MAX_IDE_BUS 2

/* FIXME: Clean this up and propagate errors to UI */
static void xbox_flash_init(MachineState *ms, MemoryRegion *rom_memory)
{
    const uint32_t rom_start = 0xFF000000;
    const char *bios_name;

    /* Locate BIOS ROM image */
    bios_name = ms->firmware ?: "bios.bin";

    int failed_to_load_bios = 1;
    char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    uint32_t bios_size = 256 * 1024;

    if (filename != NULL) {
        int bios_file_size = get_image_size(filename, NULL);
        if ((bios_file_size > 0) && ((bios_file_size % 65536) == 0)) {
            failed_to_load_bios = 0;
            bios_size = bios_file_size;
        }
    }

    char *bios_data = g_malloc(bios_size);
    assert(bios_data != NULL);

    if (!failed_to_load_bios && (filename != NULL)) {
        /* Read BIOS ROM into memory */
        failed_to_load_bios = 1;
        int fd = qemu_open(filename, O_RDONLY | O_BINARY, NULL);
        if (fd >= 0) {
            int rc = read(fd, bios_data, bios_size);
            if (rc == bios_size) {
                failed_to_load_bios = 0;
            }
            close(fd);
        }
    }

    if (failed_to_load_bios) {
        fprintf(stderr, "Failed to load BIOS '%s'\n", filename ? filename : "(null)");
        memset(bios_data, 0xff, bios_size);
    }
    if (filename != NULL) {
        g_free(filename);
    }

    /* Create BIOS region */
    MemoryRegion *bios;
    bios = g_malloc(sizeof(*bios));
    assert(bios != NULL);
    memory_region_init_rom(bios, NULL, "xbox.bios", bios_size, &error_fatal);
    rom_add_blob_fixed("xbox.bios", bios_data, bios_size, rom_start);

    /* Mirror ROM from 0xff000000 - 0xffffffff */
    uint32_t map_loc;
    for (map_loc = rom_start; map_loc >= rom_start; map_loc += bios_size) {
        MemoryRegion *map_bios = g_malloc(sizeof(*map_bios));
        memory_region_init_alias(map_bios, NULL, "pci-bios", bios, 0,
                                 bios_size);
        memory_region_add_subregion(rom_memory, map_loc, map_bios);
        memory_region_set_readonly(map_bios, true);
    }

    /* Create MCPX Boot ROM memory region
     *
     * For performance reasons, the overlay region should be page-aligned.
     * To do this, we simply make the memory region size equal to the size
     * of the BIOS image, and then overlay the boot ROM contents on top.
     *
     * Additionally, retail 1.1+ kernels have a quirk in very early boot stage
     * that depends on physical CPU WB caching behavior to briefly store a
     * computed value to a location in ROM and read it back in the next
     * instruction. Because we cannot emulate this cache behavior accurately,
     * work around this quirk by making this MCPX ROM region writable. When the
     * ROM is disabled during boot, any apparent writes to the region will be
     * discarded.
     *
     * Offending code which writes to ROM:
     *   sub ds:0FFFFD52Ch, eax
     *   mov eax, ds:0FFFFD52Ch
     */
    const char *bootrom_file =
        object_property_get_str(qdev_get_machine(), "bootrom", NULL);

    if ((bootrom_file != NULL) && *bootrom_file) {
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bootrom_file);
        assert(filename);

        int bootrom_size = get_image_size(filename, NULL);
        if (bootrom_size != 512) {
            fprintf(stderr, "MCPX bootrom should be 512 bytes, got %d\n",
                    bootrom_size);
            exit(1);
            return;
        }

        /* Read in MCPX ROM over last 512 bytes of BIOS data */
        int fd = qemu_open(filename, O_RDONLY | O_BINARY, NULL);
        assert(fd >= 0);
        int rc = read(fd, bios_data + bios_size - bootrom_size, bootrom_size);
        assert(rc == bootrom_size);
        close(fd);
        g_free(filename);
    }

    // Leave last BIOS image overlay writeable to satisfy cache dependency
    MemoryRegion *mcpx = g_malloc(sizeof(MemoryRegion));
    memory_region_init_ram(mcpx, NULL, "xbox.mcpx", bios_size, &error_fatal);
    rom_add_blob_fixed("xbox.mcpx", bios_data, bios_size, -bios_size);
    memory_region_add_subregion_overlap(rom_memory, -bios_size, mcpx, 1);

    g_free(bios_data); /* duplicated by `rom_add_blob_fixed` */
}

/*
 * Shared guest RAM: XEMU_RAM_SHM=<name>, or xemu.toml's [automation]
 * ram_shm (ui/xemu-automation.c), backs guest RAM with a named shared
 * memory object of the RAM's size, so another process can map it and read
 * guest RAM with no copy at all (Python:
 * multiprocessing.shared_memory.SharedMemory("<name>")). The RAM block
 * keeps its name, so savestates load either way.
 *
 * A name a live xemu holds is never taken: this xemu keeps private RAM
 * instead, with a stderr line and an in-app notification.
 * - POSIX ("/<name>"): the creator holds an flock on
 *   /tmp/xemu-shm-<uid>/<name>.lock for as long as it runs. A name whose
 *   lock is free is stale (a crashed run) and is replaced, unless its
 *   bytes still change (a running build older than the lock). Such a
 *   build takes no lock, so one that is paused looks stale and loses its
 *   name, and its exit unlinks the name whoever holds it by then. At
 *   exit the name is unlinked only if it is still this xemu's object.
 * - Windows ("<name>", session namespace): a named file mapping lives only
 *   while a handle to it does (a reader's too), so an existing name always
 *   has a live holder and none is ever stale.
 */
typedef enum {
    RAM_SHM_CREATED,
    RAM_SHM_HELD,   /* a live process holds the name */
    RAM_SHM_ERROR,
} RamShmResult;

#ifndef _WIN32
static struct {
    char *name;
    char *lock_path;
    int lock_fd;
    void *host;
    uint64_t size;
    dev_t dev;
    ino_t ino;
} ram_shm = { .lock_fd = -1 };

static void xbox_ram_shm_unlock(void)
{
    /*
     * Unlinked while still locked: a racer that opened this file before
     * the unlink finds, once it gets the lock, that the path no longer
     * names it, and opens the path again.
     */
    unlink(ram_shm.lock_path);
    close(ram_shm.lock_fd);
    ram_shm.lock_fd = -1;
    g_free(ram_shm.lock_path);
    ram_shm.lock_path = NULL;
}

/* Take the name's lock: CREATED (ours), HELD or ERROR (*why says). */
static RamShmResult xbox_ram_shm_lock(const char *name, char **why)
{
    g_autofree char *dir = g_strdup_printf("/tmp/xemu-shm-%u",
                                           (unsigned)getuid());
    g_autofree char *base = g_strdup(name + 1);
    struct stat st;

    if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
        *why = g_strdup_printf("cannot create %s: %s", dir, strerror(errno));
        return RAM_SHM_ERROR;
    }
    if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid()) {
        *why = g_strdup_printf("%s is not a directory of this user's", dir);
        return RAM_SHM_ERROR;
    }

    g_strdelimit(base, "/", '_');
    char *path = g_strdup_printf("%s/%s.lock", dir, base);

    for (int tries = 0; tries < 16; tries++) {
        int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            *why = g_strdup_printf("cannot open %s: %s", path,
                                   strerror(errno));
            g_free(path);
            return RAM_SHM_ERROR;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            int err = errno;
            char pid[32];
            ssize_t n = pread(fd, pid, sizeof(pid) - 1, 0);

            pid[MAX(n, 0)] = '\0';
            g_strstrip(pid);
            if (err == EWOULDBLOCK) {
                *why = pid[0] ?
                    g_strdup_printf("%s is held by a live xemu (pid %s)",
                                    name, pid) :
                    g_strdup_printf("%s is held by a live xemu", name);
            } else {
                *why = g_strdup_printf("cannot lock %s: %s", path,
                                       strerror(err));
            }
            close(fd);
            g_free(path);
            return err == EWOULDBLOCK ? RAM_SHM_HELD : RAM_SHM_ERROR;
        }

        struct stat a, b;
        if (fstat(fd, &a) == 0 && stat(path, &b) == 0 &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
            g_autofree char *pid = g_strdup_printf("%ld\n", (long)getpid());
            if (ftruncate(fd, 0) == 0) {
                ssize_t w = pwrite(fd, pid, strlen(pid), 0); /* messages only */
                (void)w;
            }
            ram_shm.lock_fd = fd;
            ram_shm.lock_path = path;
            return RAM_SHM_CREATED;
        }
        close(fd); /* its holder unlinked it meanwhile: open the path again */
    }
    *why = g_strdup_printf("%s keeps changing", path);
    g_free(path);
    return RAM_SHM_ERROR;
}

/* Whether an object's bytes change within 100 ms: an xemu still runs on it. */
static bool xbox_ram_shm_changing(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0 || st.st_size < 8) {
        return false;
    }
    size_t len = MIN((uint64_t)st.st_size, (uint64_t)256 << 20) & ~(size_t)7;
    const uint64_t *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        return false;
    }
    uint64_t sum[2] = { 0, 0 };
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            g_usleep(100 * 1000);
        }
        for (size_t i = 0; i < len / 8; i++) {
            sum[pass] = (sum[pass] ^ p[i]) * 0x100000001b3ull;
        }
    }
    munmap((void *)p, len);
    return sum[0] != sum[1];
}

/* 1: the name is still this xemu's object, 0: another's, -1: gone. */
static int xbox_ram_shm_still_ours(void)
{
    int fd = shm_open(ram_shm.name, O_RDONLY, 0);
    struct stat st;
    int ours = 0;

    if (fd < 0) {
        return errno == ENOENT ? -1 : 0;
    }
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_size == ram_shm.size) {
        if (ram_shm.ino) {
            /* Linux: a /dev/shm file has an identity. */
            ours = st.st_dev == ram_shm.dev && st.st_ino == ram_shm.ino;
        } else {
            /*
             * macOS gives none: the same object shows the same bytes (again
             * if a thread wrote RAM between the two reads of a byte).
             */
            void *p = mmap(NULL, ram_shm.size, PROT_READ, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) {
                for (int i = 0; i < 3 && !ours; i++) {
                    ours = !memcmp(p, ram_shm.host, ram_shm.size);
                }
                munmap(p, ram_shm.size);
            }
        }
    }
    close(fd);
    return ours;
}

static void xbox_ram_shm_exit(void)
{
    int ours = xbox_ram_shm_still_ours();

    if (ours > 0) {
        shm_unlink(ram_shm.name);
    } else if (ours == 0) {
        fprintf(stderr, "xemu: shared memory %s is no longer this xemu's; "
                "left in place\n", ram_shm.name);
    }
    xbox_ram_shm_unlock();
}

static RamShmResult xbox_ram_shm_create(const char *name, uint64_t size,
                                        void **host, char **why)
{
    RamShmResult r = xbox_ram_shm_lock(name, why);
    if (r != RAM_SHM_CREATED) {
        return r;
    }

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno == EEXIST) {
        /* Its lock is free: stale, unless an older build runs on it. */
        int old = shm_open(name, O_RDONLY, 0);
        bool live = old >= 0 && xbox_ram_shm_changing(old);
        if (old >= 0) {
            close(old);
        }
        if (live) {
            *why = g_strdup_printf("%s is held by a running xemu that takes "
                                   "no lock (an older build)", name);
            xbox_ram_shm_unlock();
            return RAM_SHM_HELD;
        }
        shm_unlink(name);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            fprintf(stderr, "xemu: shared memory %s: replaced a stale "
                    "object\n", name);
        }
    }
    if (fd < 0) {
        *why = g_strdup_printf("cannot create %s: %s", name, strerror(errno));
        xbox_ram_shm_unlock();
        return RAM_SHM_ERROR;
    }

    struct stat st = {};
    void *p = MAP_FAILED;
    if (ftruncate(fd, size) == 0 && fstat(fd, &st) == 0) {
        p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    int err = errno;
    close(fd);
    if (p == MAP_FAILED) {
        *why = g_strdup_printf("cannot map %s: %s", name, strerror(err));
        shm_unlink(name);
        xbox_ram_shm_unlock();
        return RAM_SHM_ERROR;
    }

    ram_shm.name = g_strdup(name);
    ram_shm.host = p;
    ram_shm.size = size;
    ram_shm.dev = st.st_dev;
    ram_shm.ino = st.st_ino;
    atexit(xbox_ram_shm_exit);
    *host = p;
    return RAM_SHM_CREATED;
}
#else
static RamShmResult xbox_ram_shm_create(const char *name, uint64_t size,
                                        void **host, char **why)
{
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  (DWORD)(size >> 32),
                                  (DWORD)(size & 0xffffffffu), name);
    DWORD err = GetLastError();
    void *p = NULL;

    if (h && err == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        *why = g_strdup_printf("%s is held by a live process", name);
        return RAM_SHM_HELD;
    }
    if (h) {
        p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
        if (!p) {
            err = GetLastError();
            CloseHandle(h);
        }
    }
    if (!p) {
        g_autofree char *msg = g_win32_error_message((gint)err);
        *why = g_strdup_printf("cannot create %s: %s", name, msg);
        return RAM_SHM_ERROR;
    }
    /* The handle stays open: the name lives as long as a handle does. */
    *host = p;
    return RAM_SHM_CREATED;
}
#endif

static bool xbox_ram_shm_init(MemoryRegion *ram, uint64_t size)
{
    bool from_toml;
    const char *e = xemu_automation_ram_shm(&from_toml);

    if (!e) {
        return false;
    }
#ifdef _WIN32
    g_autofree char *name = g_strdup(e[0] == '/' ? e + 1 : e);
#else
    g_autofree char *name = g_strdup_printf("%s%s", e[0] == '/' ? "" : "/",
                                            e);
#endif
    g_autofree char *why = NULL;
    void *host = NULL;
    RamShmResult r = xbox_ram_shm_create(name, size, &host, &why);

    if (r == RAM_SHM_CREATED) {
        memory_region_init_ram_ptr(ram, NULL, "xbox.ram", size, host);
        vmstate_register_ram_global(ram);
        if (from_toml) {
            fprintf(stderr, "xemu: [automation] ram_shm = \"%s\": applied: "
                    "guest RAM in shared memory %s\n", e, name);
        } else {
            fprintf(stderr, "xemu: guest RAM in shared memory %s\n", name);
        }
        return true;
    }
    if (r == RAM_SHM_ERROR && !from_toml) {
        error_report("XEMU_RAM_SHM: %s", why);
        exit(1);
    }

    g_autofree char *line = from_toml ?
        g_strdup_printf("xemu: [automation] ram_shm = \"%s\": skipped: %s",
                        e, why) :
        g_strdup_printf("xemu: XEMU_RAM_SHM: %s; guest RAM stays private",
                        why);
    g_autofree char *toast = g_strdup_printf("Shared RAM not opened: %s",
                                             why);
    xemu_automation_skipped(line, toast);
    return false;
}

/* The most guest RAM: up to NV2A BAR1 at 0xF0000000 (3840 MiB) */
#define XBOX_RAM_SIZE_MAX 0xF0000000ULL

static void xbox_memory_init(PCMachineState *pcms,
                             MemoryRegion *system_memory,
                             MemoryRegion *rom_memory,
                             MemoryRegion **ram_memory)
{
    // int linux_boot, i;
    MemoryRegion *ram;//, *option_rom_mr;
    // FWCfgState *fw_cfg;
    MachineState *machine = MACHINE(pcms);
    // PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);

    // linux_boot = (machine->kernel_filename != NULL);

    /*
     * More than 128 MiB is experimental (mem_limit '256'/'512', or -m,
     * which wins over mem_limit): RAM for a kernel that manages it; BAR1
     * and VGA cover the first 128 MiB (nv2a_aperture_size), NV2A DMA
     * offsets all of it (nv2a_dma_addr_mask). Such a size must be a
     * multiple of 16 MiB: the aarch64 sub-page store prefilter compares
     * page indexes with the RAM's page count as an arithmetic immediate.
     * RAM must also end below 0xF0000000, where the kernel puts NV2A BAR1
     * and, above it, the other PCI devices and the flash: RAM sits over
     * the PCI hole, so it would hide them.
     */
    if (machine->ram_size > 128 * MiB &&
        (machine->ram_size % (16 * MiB) ||
         machine->ram_size > XBOX_RAM_SIZE_MAX)) {
        error_report("-m %" PRIu64 " MiB: guest RAM above 128 MiB must be "
                     "a multiple of 16 MiB, %" PRIu64 " MiB at most",
                     machine->ram_size / MiB, XBOX_RAM_SIZE_MAX / MiB);
        exit(1);
    }

    /* Allocate RAM.  We allocate it as a single memory region and use
     * aliases to address portions of it, mostly for backwards compatibility
     * with older qemus that used qemu_ram_alloc().
     */
    ram = g_malloc(sizeof(*ram));
    if (!xbox_ram_shm_init(ram, machine->ram_size)) {
        memory_region_init_ram(ram, NULL, "xbox.ram",
                               machine->ram_size, &error_fatal);
    }

    *ram_memory = ram;
    memory_region_add_subregion(system_memory, 0, ram);

    xbox_flash_init(machine, rom_memory);
    pc_system_flash_cleanup_unused(pcms);
}

/*
 * The PC machine's default RAM backend, pc.ram (machine->ram), is never
 * mapped: the guest runs on xbox.ram. It stays in the RAM list, so every
 * savestate carries it as zero pages, but loads and saves no longer read
 * its 128 MiB (which faulted it all into the footprint on macOS).
 */
static void xbox_unused_ram_done(Notifier *n, void *data)
{
    MachineState *machine = MACHINE(qdev_get_machine());

    if (machine->ram && machine->ram->ram_block &&
        !memory_region_is_mapped(machine->ram)) {
        machine->ram->ram_block->xemu_zero = true;
    }
}

static Notifier xbox_unused_ram_notifier = {
    .notify = xbox_unused_ram_done,
};

/* PC hardware initialisation */
static void xbox_init(MachineState *machine)
{
    xbox_init_common(machine, NULL, NULL);
}

void xbox_init_common(MachineState *machine,
                      PCIBus **pci_bus_out,
                      ISABus **isa_bus_out)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    // MemoryRegion *system_io = get_system_io();

    PCIBus *pci_bus;
    ISABus *isa_bus;

    // qemu_irq *i8259;
    // qemu_irq smi_irq; // XBOX_TODO: SMM support?

    GSIState *gsi_state;

    // DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    // BusState *idebus[MAX_IDE_BUS];
    MC146818RtcState *rtc_state;
    ISADevice *pit = NULL;
    int pit_isa_irq = 0;
    qemu_irq pit_alt_irq = NULL;
    // ISADevice *pit;

    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;

    I2CBus *smbus;
    PCIBus *agp_bus;

    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
    rom_memory = pci_memory;

    // pc_guest_info_init(pcms);

    /* allocate ram and load rom/bios */
    xbox_memory_init(pcms, system_memory, rom_memory, &ram_memory);
    qemu_add_machine_init_done_notifier(&xbox_unused_ram_notifier);

    gsi_state = pc_gsi_create(&x86ms->gsi, pcmc->pci_enabled);

    xbox_pci_init(x86ms->gsi,
                  get_system_memory(), get_system_io(),
                  pci_memory, ram_memory, rom_memory,
                  &pci_bus,
                  &isa_bus,
                  &smbus,
                  &agp_bus);

    pcms->pcibus = pci_bus;

    isa_bus_register_input_irqs(isa_bus, x86ms->gsi);

    pc_i8259_create(isa_bus, gsi_state->i8259_irq);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    /* init basic PC hardware */
    rtc_state = mc146818_rtc_init(isa_bus, 2000, NULL);
    x86ms->rtc = ISA_DEVICE(rtc_state);

    if (kvm_pit_in_kernel()) {
        pit = kvm_pit_init(isa_bus, 0x40);
    } else {
        pit = i8254_pit_init(isa_bus, 0x40, pit_isa_irq, pit_alt_irq);
    }

    i8257_dma_init(OBJECT(machine), isa_bus, 0);

    object_property_set_link(OBJECT(pcms->pcspk), "pit",
                             OBJECT(pit), &error_fatal);
    isa_realize_and_unref(pcms->pcspk, isa_bus, &error_fatal);

    PCIDevice *dev = pci_create_simple(pci_bus, PCI_DEVFN(9, 0), "piix3-ide");
    pci_ide_create_devs(dev);
    // idebus[0] = qdev_get_child_bus(&dev->qdev, "ide.0");
    // idebus[1] = qdev_get_child_bus(&dev->qdev, "ide.1");

    /* smbus devices */
    smbus_xbox_smc_init(smbus, 0x10);

    const char *video_encoder =
        object_property_get_str(qdev_get_machine(), "video-encoder", NULL);

    if (strcmp(video_encoder, "xcalibur") != 0) {
        if (!strcmp(video_encoder, "conexant")) {
            smbus_cx25871_init(smbus, 0x45);
        } else if (!strcmp(video_encoder, "focus")) {
            smbus_fs454_init(smbus, 0x6A);
        }
        smbus_adm1032_init(smbus, 0x4C);
    } else {
        smbus_xcalibur_init(smbus, 0x70);
    }

    /* USB */
    PCIDevice *usb1 = pci_new(PCI_DEVFN(3, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb1->qdev, "num-ports", 4);
    pci_realize_and_unref(usb1, pci_bus, &error_fatal);

    PCIDevice *usb0 = pci_new(PCI_DEVFN(2, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb0->qdev, "num-ports", 4);
    pci_realize_and_unref(usb0, pci_bus, &error_fatal);

    /* Ethernet! */
    PCIDevice *nvnet = pci_new(PCI_DEVFN(4, 0), "nvnet");
    qemu_configure_nic_device(DEVICE(nvnet), true, "nvnet");
    pci_realize_and_unref(nvnet, pci_bus, &error_fatal);

    /* APU! */
    mcpx_apu_init(pci_bus, PCI_DEVFN(5, 0), ram_memory);

    /* ACI! */
    pci_create_simple(pci_bus, PCI_DEVFN(6, 0), "mcpx-aci");

    /* GPU! */
    nv2a_init(agp_bus, PCI_DEVFN(0, 0), ram_memory);

    /* FIXME: Stub the memory controller */
    pci_create_simple(pci_bus, PCI_DEVFN(0, 3), "pci-testdev");

    if (pci_bus_out) {
        *pci_bus_out = pci_bus;
    }
    if (isa_bus_out) {
        *isa_bus_out = isa_bus;
    }
}

static char *machine_get_bootrom(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->bootrom);
}

static void machine_set_bootrom(Object *obj, const char *value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    g_free(ms->bootrom);
    ms->bootrom = g_strdup(value);
}

static char *machine_get_avpack(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->avpack);
}

static void machine_set_avpack(Object *obj, const char *value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (!xbox_smc_avpack_to_reg(value, NULL)) {
        error_setg(errp, "-machine avpack=%s: unsupported option", value);
        xbox_smc_append_avpack_hint(errp);
        return;
    }

    g_free(ms->avpack);
    ms->avpack = g_strdup(value);
}

static void machine_set_short_animation(Object *obj, bool value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    ms->short_animation = value;
}

static bool machine_get_short_animation(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);
    return ms->short_animation;
}

static char *machine_get_smc_version(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->smc_version);
}

static void machine_set_smc_version(Object *obj, const char *value,
                                    Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (strlen(value) != 3) {
        error_setg(errp, "-machine smc-version=%s: unsupported option", value);
        xbox_smc_append_smc_version_hint(errp);
        return;
    }

    g_free(ms->smc_version);
    ms->smc_version = g_strdup(value);
}

static char *machine_get_video_encoder(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->video_encoder);
}

static void machine_set_video_encoder(Object *obj, const char *value,
                                      Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (strcmp(value, "conexant") != 0 && strcmp(value, "focus") != 0 &&
        strcmp(value, "xcalibur") != 0) {
        error_setg(errp, "-machine video_encoder=%s: unsupported option",
                   value);
        error_append_hint(
            errp, "Valid options are: conexant (default), focus, xcalibur\n");
        return;
    }

    g_free(ms->video_encoder);
    ms->video_encoder = g_strdup(value);
}

static void xbox_machine_options(MachineClass *m)
{
    ObjectClass *oc = OBJECT_CLASS(m);

    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    m->desc              = "Microsoft Xbox";
    m->max_cpus          = 1;
    m->option_rom_has_mr = true;
    m->rom_file_has_mr   = false;
    m->no_floppy         = 1,
    m->no_cdrom          = 1,
    m->default_cpu_type  = X86_CPU_TYPE_NAME("pentium3");
    m->is_default        = true;
    m->default_nic       = "nvnet";

    pcmc->pci_enabled         = true;
    pcmc->has_acpi_build      = false;
    pcmc->smbios_defaults     = false;
    pcmc->gigabyte_align      = false;
    pcmc->smbios_legacy_mode  = true;
    pcmc->has_reserved_memory = false;

    object_class_property_add_str(oc, "bootrom", machine_get_bootrom,
                                  machine_set_bootrom);
    object_class_property_set_description(oc, "bootrom", "Xbox bootrom file");

    object_class_property_add_str(oc, "avpack", machine_get_avpack,
                                  machine_set_avpack);
    object_class_property_set_description(
        oc, "avpack",
        "Xbox video connector: composite, scart, svideo, vga, rfu, hdtv "
        "(default), none");

    object_class_property_add_bool(oc, "short-animation",
                                   machine_get_short_animation,
                                   machine_set_short_animation);
    object_class_property_set_description(oc, "short-animation",
                                          "Skip Xbox boot animation");

    object_class_property_add_str(oc, "smc-version", machine_get_smc_version,
                                  machine_set_smc_version);
    object_class_property_set_description(
        oc, "smc-version", "Set the SMC version number, default is P01");

    object_class_property_add_str(oc, "video-encoder",
                                  machine_get_video_encoder,
                                  machine_set_video_encoder);
    object_class_property_set_description(
        oc, "video-encoder",
        "Set the encoder presented to the OS: conexant (default), focus, "
        "xcalibur");
}

static inline void xbox_machine_initfn(Object *obj)
{
    object_property_set_str(obj, "avpack", "hdtv", &error_fatal);
    object_property_set_bool(obj, "short-animation", false, &error_fatal);
    object_property_set_str(obj, "smc-version", "P01", &error_fatal);
    object_property_set_str(obj, "video-encoder", "conexant", &error_fatal);
}

static void xbox_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    xbox_machine_options(mc);
    mc->init = xbox_init;
}

static const TypeInfo pc_machine_type_xbox = {
    .name = TYPE_XBOX_MACHINE,
    .parent = TYPE_PC_MACHINE,
    .abstract = false,
    .instance_size = sizeof(XboxMachineState),
    .instance_init = xbox_machine_initfn,
    .class_size = sizeof(XboxMachineClass),
    .class_init = xbox_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
         // { TYPE_HOTPLUG_HANDLER },
         // { TYPE_NMI },
         { }
    },
};

static void pc_machine_init_xbox(void)
{
    type_register_static(&pc_machine_type_xbox);
}

type_init(pc_machine_init_xbox)
