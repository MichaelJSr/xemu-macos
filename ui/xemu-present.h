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

#ifdef __cplusplus
}
#endif

#endif
