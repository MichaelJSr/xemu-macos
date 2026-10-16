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

#include "apu_int.h"

struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
bool g_dbg_snapshot;

/*
 * Frames publish g_dbg to g_dbg_cache (~18.5 KB, 1500 times a second) only
 * while someone reads it: the Audio debug window, which calls
 * mcpx_apu_get_debug_info every UI frame while it is open, or a voice
 * isolation (voice_should_mute reads the snapshot). A read re-arms a
 * countdown of APU frames.
 */
#define DBG_SNAPSHOT_FRAMES 3000 /* 2 s */
static int g_dbg_snapshot_frames;

const struct McpxApuDebug *mcpx_apu_get_debug_info(void)
{
    qatomic_set(&g_dbg_snapshot_frames, DBG_SNAPSHOT_FRAMES);
    return &g_dbg_cache;
}

void mcpx_debug_begin_frame(void)
{
    int n = qatomic_read(&g_dbg_snapshot_frames);
    if (n > 0) {
        qatomic_set(&g_dbg_snapshot_frames, n - 1);
    }
    g_dbg_snapshot = n > 0 || g_dbg_voice_monitor >= 0;
    if (!g_dbg_snapshot) {
        return;
    }
    for (int i = 0; i < MCPX_HW_MAX_VOICES; i++) {
        g_dbg.vp.v[i].active = false;
        g_dbg.vp.v[i].multipass_dst_voice = 0xFFFF;
    }
}

void mcpx_debug_end_frame(void)
{
    if (g_dbg_snapshot) {
        g_dbg_cache = g_dbg;
    }
}

void mcpx_apu_debug_set_gp_realtime_enabled(bool run)
{
    g_state->gp.realtime = run;
}

void mcpx_apu_debug_set_ep_realtime_enabled(bool run)
{
    g_state->ep.realtime = run;
}

McpxApuDebugMonitorPoint mcpx_apu_debug_get_monitor(void)
{
    return g_state->monitor.point;
}

void mcpx_apu_debug_set_monitor(McpxApuDebugMonitorPoint monitor)
{
    g_state->monitor.point = monitor;
}

void mcpx_apu_debug_isolate_voice(uint16_t v)
{
    g_dbg_voice_monitor = v;
}

void mcpx_apu_debug_clear_isolations(void)
{
    g_dbg_voice_monitor = -1;
}

bool mcpx_apu_debug_is_muted(uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    return g_dbg_muted_voices[v / 64] & (1LL << (v % 64));
}

void mcpx_apu_debug_toggle_mute(uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    g_dbg_muted_voices[v / 64] ^= (1LL << (v % 64));
}
