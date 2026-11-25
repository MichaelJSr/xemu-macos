/*
 * xemu User Interface - emulated hard disk access for the drive explorer
 *
 * Small C shim over QEMU's block layer and run state, so the C++ explorer
 * doesn't include QEMU's C-only headers. Callers hold the BQL.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "migration/blocker.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "xbox-hdd-blk.h"

static uint64_t hdd_writes;

static BlockBackend *hdd_blk(void)
{
    BlockBackend *blk;

    /* Without the BQL (a late static destructor) the block layer is off
     * limits: it asserts, and may already be closed */
    if (!bql_locked()) {
        return NULL;
    }
    blk = blk_by_name("ide0-hd0");
    if (!blk || !blk_is_available(blk)) {
        return NULL;
    }
    return blk;
}

bool xemu_hdd_available(void)
{
    return hdd_blk() != NULL;
}

bool xemu_hdd_writable(void)
{
    BlockBackend *blk = hdd_blk();
    return blk && blk_is_writable(blk);
}

bool xemu_hdd_read(uint64_t offset, void *buf, size_t len)
{
    BlockBackend *blk = hdd_blk();
    return blk && blk_pread(blk, (int64_t)offset, (int64_t)len, buf, 0) >= 0;
}

bool xemu_hdd_write(uint64_t offset, const void *buf, size_t len)
{
    BlockBackend *blk = hdd_blk();
    if (!blk) {
        return false;
    }
    hdd_writes++; /* counted even if it fails: it may have written part */
    return blk_pwrite(blk, (int64_t)offset, (int64_t)len, buf, 0) >= 0;
}

uint64_t xemu_hdd_size(void)
{
    BlockBackend *blk = hdd_blk();
    int64_t n = blk ? blk_getlength(blk) : 0;
    return n > 0 ? (uint64_t)n : 0;
}

void xemu_hdd_flush(void)
{
    BlockBackend *blk = hdd_blk();
    if (blk) {
        blk_flush(blk);
    }
}

uint64_t xemu_hdd_write_count(void)
{
    return hdd_writes;
}

/* ---- Write jobs, snapshot loads, shutdown ------------------------------ */

static bool hdd_job_busy;
static bool hdd_resume_refused;
static Error *hdd_job_blocker;
/* Resets of every kind, and all but snapshot loads */
static unsigned hdd_resets_all, hdd_resets_plain;
static void (*hdd_shutdown_fn)(void);
static Notifier hdd_shutdown_notifier;

static void hdd_count_reset(void *opaque)
{
    (*(unsigned *)opaque)++;
}

/* Runs on the vCPU thread before it executes anything after a resume */
static void hdd_hold_cpu(CPUState *cpu, run_on_cpu_data data)
{
    if (hdd_job_busy) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

static void hdd_vm_state_change(void *opaque, bool running, RunState state)
{
    /* A resume during a write job (menu, hotkey, monitor, QMP): undo it
     * before the guest runs on its old view of the disk */
    if (running && hdd_job_busy && first_cpu) {
        hdd_resume_refused = true;
        async_run_on_cpu(first_cpu, hdd_hold_cpu, RUN_ON_CPU_NULL);
    }
}

static void hdd_hooks_init(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    qemu_register_reset(hdd_count_reset, &hdd_resets_all);
    qemu_register_reset_nosnapshotload(hdd_count_reset, &hdd_resets_plain);
    qemu_add_vm_change_state_handler(hdd_vm_state_change, NULL);
}

void xemu_hdd_job_begin(void)
{
    hdd_hooks_init();
    if (hdd_job_busy) {
        return;
    }
    hdd_job_busy = true;
    hdd_resume_refused = false;
    /* Snapshot saves (and checkpoints) check migration blockers */
    error_setg(&hdd_job_blocker,
               "The Xbox hard drive is being written: wait for the copy to "
               "finish");
    migrate_add_blocker_internal(&hdd_job_blocker, NULL);
}

void xemu_hdd_job_end(void)
{
    hdd_job_busy = false;
    migrate_del_blocker(&hdd_job_blocker);
}

bool xemu_hdd_take_resume_refused(void)
{
    bool r = hdd_resume_refused;
    hdd_resume_refused = false;
    return r;
}

unsigned xemu_hdd_snapshot_loads(void)
{
    hdd_hooks_init();
    return hdd_resets_all - hdd_resets_plain;
}

static void hdd_shutdown(Notifier *n, void *data)
{
    if (hdd_shutdown_fn) {
        hdd_shutdown_fn();
    }
}

void xemu_hdd_on_shutdown(void (*fn)(void))
{
    if (!hdd_shutdown_fn) {
        hdd_shutdown_notifier.notify = hdd_shutdown;
        qemu_register_shutdown_notifier(&hdd_shutdown_notifier);
    }
    hdd_shutdown_fn = fn;
}

/* ---- Run state ---------------------------------------------------------- */

bool xemu_xbox_running(void)
{
    return runstate_is_running();
}

void xemu_xbox_pause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

void xemu_xbox_resume(void)
{
    if (!runstate_is_running()) {
        vm_start();
    }
}

void xemu_xbox_restart(bool resume)
{
    /* The Xbox caches its file system: reset it while it is stopped, so no
     * guest code runs on its old view of the disk, then start it */
    xemu_xbox_pause();
    qemu_system_reset(SHUTDOWN_CAUSE_HOST_UI);
    if (resume) {
        vm_start();
    }
}
