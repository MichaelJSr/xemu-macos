/*
 * xemu: host-time counters (QMP query-xemu-host-profile)
 *
 * The counters live in include/qemu/xemu-hostprof.h's xemu_hostprof; the
 * vCPU thread adds to them from tb_gen_code, the deterministic APU tick
 * and the icount idle warp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/xemu-hostprof.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"

XemuHostProf xemu_hostprof;

XemuHostProfile *qmp_query_xemu_host_profile(Error **errp)
{
    XemuHostProfile *p = g_new0(XemuHostProfile, 1);

    p->translate_ns = qatomic_read(&xemu_hostprof.translate_ns);
    p->translations = qatomic_read(&xemu_hostprof.translations);
    p->apu_ns = qatomic_read(&xemu_hostprof.apu_ns);
    p->apu_ticks = qatomic_read(&xemu_hostprof.apu_ticks);
    p->idle_ns = qatomic_read(&xemu_hostprof.idle_ns);
    p->idle_warps = qatomic_read(&xemu_hostprof.idle_warps);
    return p;
}
