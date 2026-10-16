/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
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
#ifndef HW_XBOX_MCPX_APU_INT_H
#define HW_XBOX_MCPX_APU_INT_H

#include "qemu/osdep.h"
#include <math.h>
#include <SDL3/SDL.h>
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/runstate.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "ui/xemu-settings.h"

#include "trace.h"
#include "apu.h"
#include "apu_regs.h"
#include "apu_debug.h"
#include "fpconv.h"
#include "vp/vp.h"
#include "dsp/gp_ep.h"

#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))

#define SET_MASK(v, mask, val)                                       \
    do {                                                             \
        (v) &= ~(mask);                                              \
        (v) |= ((val) << ctz32(mask)) & (mask);                      \
    } while (0)

#define CASE_4(v, step)                                              \
    case (v):                                                        \
    case (v)+(step):                                                 \
    case (v)+(step)*2:                                               \
    case (v)+(step)*3

// #define DEBUG_MCPX

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define MCPX_APU_DEVICE(obj) \
    OBJECT_CHECK(MCPXAPUState, (obj), "mcpx-apu")

typedef struct MCPXAPUState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    bool exiting;
    bool set_irq;

    QemuThread apu_thread;
    QemuMutex lock;
    QemuCond cond;
    QemuCond idle_cond;
    bool pause_requested;
    bool is_idle;

    MemoryRegion *ram;
    uint8_t *ram_ptr;
    size_t ram_size;
    MemoryRegion mmio;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    int ep_frame_div;

    /*
     * Deterministic mode (icount): frames run from a virtual-clock timer
     * on the vCPU thread instead of the host-paced frame thread (apu.c).
     */
    struct {
        bool enabled;
        bool in_burst;
        bool rearm;     /* after realize or a load: restart on the grid */
        bool loaded;    /* after a load: the DSPs take the loaded state */
        bool voices_inline; /* the tick is running voices on the vCPU */
        QEMUTimer *timer;
        /* mcpx-apu/history, filled before a save and read after a load */
        int64_t deadline;
        uint32_t dsp_cycles[2];
        bool dsp_idle[2];
    } det;

    int frame_work_acc_us;
    int frame_count;
    int64_t frame_count_time_us;
    int64_t next_frame_time_us;

    struct {
        struct {
            int backoff, ok, speedup;
        } pacing;
        struct {
            int64_t last_us;
            int64_t min_us, max_us, sum_us;
            int count;
        } deviation;
        int queued_bytes_min, queued_bytes_max;
        int64_t queued_bytes_sum;
        int queued_bytes_count;
    } throttle;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2]; // 1 EP frame (0x400 bytes)
        SDL_AudioStream *stream;
        int queued_bytes_low, queued_bytes_high;
    } monitor;
} MCPXAPUState;

/*
 * Past guest RAM the accessors go through the memory API, which takes the
 * BQL for MMIO. Deterministic voice processing drops such accesses (a guest
 * bug either way): a read returns 0 and a write is lost. It used to run on
 * workers that could not take the BQL the tick holds, and now runs on the
 * vCPU with the same result. Realtime voices run on the frame thread, which
 * holds no BQL, and still take the memory API.
 */
static inline bool ram_oob_dropped(MCPXAPUState *d)
{
    return d->det.enabled && (d->det.voices_inline || !bql_locked());
}

static inline uint32_t ram_ldl(MCPXAPUState *d, hwaddr addr)
{
    if (__builtin_expect(addr + 4 <= d->ram_size, 1)) {
        return ldl_le_p(&d->ram_ptr[addr]);
    }
    if (ram_oob_dropped(d)) {
        return 0;
    }
    return ldl_le_phys(&address_space_memory, addr);
}

extern MCPXAPUState *g_state; // Used via debug handlers
extern struct McpxApuDebug g_dbg, g_dbg_cache;
extern int g_dbg_voice_monitor;
extern uint64_t g_dbg_muted_voices[4];

void mcpx_debug_begin_frame(void);
void mcpx_debug_end_frame(void);

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp);
void mcpx_apu_monitor_finalize(MCPXAPUState *d);
void mcpx_apu_monitor_frame(MCPXAPUState *d);

#endif
