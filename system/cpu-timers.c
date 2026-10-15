/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
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
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/cpus.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "system/cpu-timers.h"
#include "system/cpu-timers-internal.h"
#include "exec/icount.h"

/* clock and ticks */

static int64_t cpu_get_ticks_locked(void)
{
    int64_t ticks = timers_state.cpu_ticks_offset;
    if (timers_state.cpu_ticks_enabled) {
        ticks += cpu_get_host_ticks();
    }

    if (timers_state.cpu_ticks_prev > ticks) {
        /* Non increasing ticks may happen if the host uses software suspend. */
        timers_state.cpu_ticks_offset += timers_state.cpu_ticks_prev - ticks;
        ticks = timers_state.cpu_ticks_prev;
    }

    timers_state.cpu_ticks_prev = ticks;
    return ticks;
}

/*
 * return the time elapsed in VM between vm_start and vm_stop.
 * cpu_get_ticks() uses units of the host CPU cycle counter.
 */
int64_t cpu_get_ticks(void)
{
    int64_t ticks;

    qemu_spin_lock(&timers_state.vm_clock_lock);
    ticks = cpu_get_ticks_locked();
    qemu_spin_unlock(&timers_state.vm_clock_lock);
    return ticks;
}

int64_t cpu_get_clock_locked(void)
{
    int64_t time;

    time = timers_state.cpu_clock_offset;
    if (timers_state.cpu_ticks_enabled) {
        time += get_clock();
    }

    return time;
}

/*
 * Return the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void)
{
    int64_t ti;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        ti = cpu_get_clock_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return ti;
}

/*
 * enable cpu_get_ticks()
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_enable_ticks(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (!timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset -= cpu_get_host_ticks();
        timers_state.cpu_clock_offset -= get_clock();
        timers_state.cpu_ticks_enabled = 1;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
}

/*
 * disable cpu_get_ticks() : the clock is stopped. You must not call
 * cpu_get_ticks() after that.
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_disable_ticks(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset += cpu_get_host_ticks();
        timers_state.cpu_clock_offset = cpu_get_clock_locked();
        timers_state.cpu_ticks_enabled = 0;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static bool icount_state_needed(void *opaque)
{
    return icount_enabled();
}

static bool warp_timer_state_needed(void *opaque)
{
    TimersState *s = opaque;
    return s->icount_warp_timer != NULL;
}

static bool adjust_timers_state_needed(void *opaque)
{
    TimersState *s = opaque;
    return s->icount_rt_timer != NULL;
}

static bool icount_shift_state_needed(void *opaque)
{
    return icount_enabled() == ICOUNT_ADAPTATIVE;
}

/*
 * xemu: VMSTATE_TIMER_PTR for a timer the loading process may not have:
 * the warp timer exists only with -icount sleep=on, the adjust timers only
 * with shift=auto. The bytes on the wire are the same; a missing timer
 * just drops the deadline instead of tripping vmstate's NULL-pointer
 * assert, so such a state loads in normal mode or under sleep=off.
 */
static int get_timer_opt(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field)
{
    QEMUTimer *t = *(QEMUTimer **)pv;
    uint64_t expire_time = qemu_get_be64(f);

    if (t) {
        if (expire_time != -1) {
            timer_mod_ns(t, expire_time);
        } else {
            timer_del(t);
        }
    }
    return 0;
}

static int put_timer_opt(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field, JSONWriter *vmdesc)
{
    timer_put(f, *(QEMUTimer **)pv); /* .needed saw it non-NULL */
    return 0;
}

static const VMStateInfo vmstate_info_timer_opt = {
    .name = "timer",
    .get  = get_timer_opt,
    .put  = put_timer_opt,
};

#define VMSTATE_TIMER_PTR_OPT(_f, _s) {                                 \
    .name   = stringify(_f),                                            \
    .info   = &vmstate_info_timer_opt,                                  \
    .size   = sizeof(QEMUTimer *),                                      \
    .flags  = VMS_SINGLE,                                               \
    .offset = vmstate_offset_value(_s, _f, QEMUTimer *),                \
}

static int warp_timer_post_load(void *opaque, int version_id)
{
    TimersState *s = opaque;

    if (!s->icount_warp_timer) {
        s->vm_clock_warp_start = -1;
    }
    return 0;
}

/*
 * Subsection for warp timer migration is optional, because may not be created
 */
static const VMStateDescription icount_vmstate_warp_timer = {
    .name = "timer/icount/warp_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = warp_timer_state_needed,
    .post_load = warp_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(vm_clock_warp_start, TimersState),
        VMSTATE_TIMER_PTR_OPT(icount_warp_timer, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription icount_vmstate_adjust_timers = {
    .name = "timer/icount/timers",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = adjust_timers_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR_OPT(icount_rt_timer, TimersState),
        VMSTATE_TIMER_PTR_OPT(icount_vm_timer, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

/* xemu: only an adaptive (shift=auto) process takes a saved shift. */
static int16_t icount_shift_configured;

static int icount_shift_pre_load(void *opaque)
{
    TimersState *s = opaque;

    icount_shift_configured = s->icount_time_shift;
    return 0;
}

static int icount_shift_post_load(void *opaque, int version_id)
{
    TimersState *s = opaque;

    if (icount_enabled() != ICOUNT_ADAPTATIVE) {
        s->icount_time_shift = icount_shift_configured;
    }
    return 0;
}

static const VMStateDescription icount_vmstate_shift = {
    .name = "timer/icount/shift",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = icount_shift_state_needed,
    .pre_load = icount_shift_pre_load,
    .post_load = icount_shift_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT16(icount_time_shift, TimersState),
        VMSTATE_INT64(last_delta, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * This is a subsection for icount migration.
 */
static const VMStateDescription icount_vmstate_timers = {
    .name = "timer/icount",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = icount_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(qemu_icount_bias, TimersState),
        VMSTATE_INT64(qemu_icount, TimersState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &icount_vmstate_warp_timer,
        &icount_vmstate_adjust_timers,
        &icount_vmstate_shift,
        NULL
    }
};

/*
 * xemu: the migrated cpu_clock_offset is QEMU_CLOCK_VIRTUAL at save time
 * (the VM is stopped, so cpu_disable_ticks() froze it there), but only
 * without icount; under icount the virtual clock is the instruction
 * count. Save that instead, and under icount re-base the instruction
 * clock onto the saved value on every load: a state saved without icount
 * has no "timer/icount" subsection, and precise mode does not migrate the
 * shift. Device timers restored after this section then keep their
 * distance from "now" instead of lying minutes ahead. Without icount the
 * stream and the restored clock are unchanged.
 */
static int timers_pre_save(void *opaque)
{
    TimersState *s = opaque;

    s->vm_clock_saved = icount_enabled() ? icount_get() : s->cpu_clock_offset;
    return 0;
}

static int timers_pre_load(void *opaque)
{
    TimersState *s = opaque;

    s->vm_clock_saved = s->cpu_clock_offset;
    return 0;
}

static int timers_post_load(void *opaque, int version_id)
{
    TimersState *s = opaque;

    s->cpu_clock_offset = s->vm_clock_saved;
    if (icount_enabled()) {
        seqlock_write_lock(&s->vm_clock_seqlock, &s->vm_clock_lock);
        qatomic_set_i64(&s->qemu_icount_bias,
                        s->vm_clock_saved - icount_to_ns(s->qemu_icount));
        seqlock_write_unlock(&s->vm_clock_seqlock, &s->vm_clock_lock);
    }
    return 0;
}

static const VMStateDescription vmstate_timers = {
    .name = "timer",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = timers_pre_save,
    .pre_load = timers_pre_load,
    .post_load = timers_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(cpu_ticks_offset, TimersState),
        VMSTATE_UNUSED(8),
        VMSTATE_INT64_V(vm_clock_saved, TimersState, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &icount_vmstate_timers,
        NULL
    }
};

static void do_nothing(CPUState *cpu, run_on_cpu_data unused)
{
}

void qemu_timer_notify_cb(void *opaque, QEMUClockType type)
{
    if (!icount_enabled() || type != QEMU_CLOCK_VIRTUAL) {
        qemu_notify_event();
        return;
    }

    if (qemu_in_vcpu_thread()) {
        /*
         * A CPU is currently running; send it out of the
         * tcg_cpu_exec() loop so it will recalculate its
         * icount deadline immediately.
         */
        cpu_exit(current_cpu);
    } else if (first_cpu) {
        /*
         * cpu_exit() is not enough to kick a halted CPU out of
         * qemu_tcg_wait_io_event.  async_run_on_cpu, instead,
         * causes cpu_thread_is_idle to return false.  This way,
         * handle_icount_deadline can run.
         * If we have no CPUs at all for some reason, we don't
         * need to do anything.
         */
        async_run_on_cpu(first_cpu, do_nothing, RUN_ON_CPU_NULL);
    }
}

TimersState timers_state;

/* initialize timers state and the cpu throttle for convenience */
void cpu_timers_init(void)
{
    seqlock_init(&timers_state.vm_clock_seqlock);
    qemu_spin_init(&timers_state.vm_clock_lock);
    vmstate_register(NULL, 0, &vmstate_timers, &timers_state);
}
