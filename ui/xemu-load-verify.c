/*
 * XEMU_LOAD_VERIFY=1: nothing may write guest RAM between a load and the
 * moment the VM starts running from it
 *
 * After every successful load (loadvm, xemu-checkpoint-restore, an
 * incoming migration) keep a copy of guest RAM, and when the VM next
 * starts compare it with RAM: one stderr line names the pages that
 * changed. Until the vCPU runs, a loaded VM's RAM is the state's; a write
 * in between (a device timer firing in the stopped VM, a thread, a
 * gdbstub or QMP write) makes the run start from other RAM than the same
 * load in a fresh process. The check runs first among the start's
 * run-state handlers, before the vCPU resumes, so the device work that
 * the start itself does on the vCPU (overdue device timers, PFIFO
 * catching up) is not counted: it is the same on every load of that
 * state. A load that replaces a paused VM's RAM before it ran checks it
 * first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "system/runstate.h"
#include "xemu-load-verify.h"

#define PAGES_LISTED 8

static int enabled = -1;
static bool armed;
static uint8_t *copy;
static uint64_t copy_size;

int64_t xemu_frame_count(void);

static const uint8_t *guest_ram(uint64_t *size)
{
    RAMBlock *rb;

    WITH_RCU_READ_LOCK_GUARD() {
        rb = qemu_ram_block_by_name("xbox.ram");
    }
    if (!rb) {
        return NULL;
    }
    *size = qemu_ram_get_used_length(rb);
    return qemu_ram_get_host_addr(rb);
}

/* Compare RAM with the copy the last load left; @until names the moment. */
static void load_verify_check(const char *until)
{
    uint64_t size, changed = 0;
    const uint8_t *ram;
    size_t page = qemu_target_page_size();
    g_autoptr(GString) pages = g_string_new(NULL);

    if (!armed) {
        return;
    }
    armed = false;
    ram = guest_ram(&size);
    if (!ram || size != copy_size) {
        return;
    }
    for (uint64_t off = 0; off < size; off += page) {
        if (memcmp(ram + off, copy + off, page)) {
            if (changed++ < PAGES_LISTED) {
                g_string_append_printf(pages, " 0x%" PRIx64, off);
            }
        }
    }
    if (changed) {
        fprintf(stderr, "xemu: load verify: RAM changed between the load "
                "and %s (frame %" PRId64 "): %" PRIu64 " page%s%s%s\n",
                until, xemu_frame_count(), changed, changed == 1 ? "" : "s",
                pages->str, changed > PAGES_LISTED ? " ..." : "");
    }
}

static void load_verify_vm_state(void *opaque, bool running, RunState state)
{
    if (running) {
        load_verify_check("the VM start");
    }
}

void xemu_load_verify_loading(void)
{
    load_verify_check("the next load");
}

void xemu_load_verify_loaded(void)
{
    uint64_t size;
    const uint8_t *ram;

    if (enabled < 0) {
        const char *e = getenv("XEMU_LOAD_VERIFY");
        enabled = e && e[0] == '1';
        if (enabled) {
            /* Before every other handler on a start (low runs first). */
            qemu_add_vm_change_state_handler_prio(load_verify_vm_state,
                                                  NULL, INT_MIN);
        }
    }
    if (!enabled) {
        return;
    }
    ram = guest_ram(&size);
    if (!ram) {
        return;
    }
    if (size != copy_size) {
        g_free(copy);
        copy = g_malloc(size);
        copy_size = size;
    }
    memcpy(copy, ram, size);
    armed = true;
}
