/*
 * xemu Presentation Backend Selection
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * The presentation backend (which API draws the final frame + UI into
 * the main window) is selected once at startup, because the SDL window
 * type (SDL_WINDOW_OPENGL vs SDL_WINDOW_METAL) is fixed at creation.
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
#ifndef XEMU_PRESENT_H
#define XEMU_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True when the main window presents via Metal (CAMetalLayer + ImGui
 * Metal). False everywhere except macOS with the Metal backend
 * resolved at startup. Stable for the lifetime of the process after
 * the first call (which must happen after config load — guaranteed
 * because the first caller is window creation).
 */
bool xemu_present_is_metal(void);

/*
 * XEMU_HEADLESS, read once at startup. Unset or empty: windowed, as
 * always. "1": offscreen -- hidden window, no Dock icon, no UI drawn,
 * no audio device; the guest still renders, so QMP screendump returns
 * real frames. "null": offscreen plus the null renderer (no GPU work;
 * screendump falls back to the VGA surface, usually blank).
 */
typedef enum XemuHeadlessMode {
    XEMU_HEADLESS_OFF,
    XEMU_HEADLESS_OFFSCREEN,
    XEMU_HEADLESS_NULL,
} XemuHeadlessMode;

XemuHeadlessMode xemu_headless_mode(void);

/*
 * Refresh period of the display the Metal window is on, in ns, as the
 * UI thread last read it (every 500 ms); 0 while unknown (GL backend,
 * headless, no mode). Any thread.
 */
uint64_t xemu_present_refresh_ns(void);

/*
 * Fit frame interpolation to the display (default): run only the modes
 * whose steps it can show, and hold steps whole refreshes.
 * XEMU_INTERP_GATE=0: the configured mode every flip, holds as computed.
 */
bool xemu_present_interp_gate(void);

#ifdef __cplusplus
}
#endif

#endif
