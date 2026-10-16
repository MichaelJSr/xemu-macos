//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "viewport-manager.hh"
#include "xemu-hud.h"

ViewportManager g_viewport_mgr;

ViewportManager::ViewportManager() {
    m_scale = 1;
    m_pixel_density = 1;
    m_extents.x = 25 * m_scale; // Distance from Left
    m_extents.y = 25 * m_scale; // '' Top
    m_extents.z = 25 * m_scale; // '' Right
    m_extents.w = 25 * m_scale; // '' Bottom
}

ImVec4 ViewportManager::GetExtents()
{
    return m_extents;
}

#if 0
void ViewportManager::DrawExtents()
{
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 tl(m_extents.x, m_extents.y);
    ImVec2 tr(io.DisplaySize.x - m_extents.z, m_extents.y);
    ImVec2 br(io.DisplaySize.x - m_extents.z, io.DisplaySize.y - m_extents.w);
    ImVec2 bl(m_extents.x, io.DisplaySize.y - m_extents.w);

    auto dl = ImGui::GetForegroundDrawList();
    ImU32 color = 0xffff00ff;
    dl->AddLine(tl, tr, color, 2.0);
    dl->AddLine(tr, br, color, 2.0);
    dl->AddLine(br, bl, color, 2.0);
    dl->AddLine(bl, tl, color, 2.0);
    dl->AddLine(tl, br, color, 2.0);
    dl->AddLine(bl, tr, color, 2.0);
}
#endif

ImVec2 ViewportManager::Scale(const ImVec2 vec2)
{
    return ImVec2(vec2.x * m_scale, vec2.y * m_scale);
}

void ViewportManager::Update()
{
    ImGuiIO &io = ImGui::GetIO();

    SDL_Window *window = xemu_get_window();
    m_pixel_density = fmaxf(SDL_GetWindowPixelDensity(window), 1.0f);

    if (g_config.display.ui.auto_scale) {
        float window_display_scale = fmaxf(SDL_GetWindowDisplayScale(window), 1.0f);
        m_scale = window_display_scale / m_pixel_density;
    } else {
        m_scale = g_config.display.ui.scale;
    }

    m_scale = fmaxf(m_scale, 1.0);

    /*
     * Shrink the UI in windows smaller than the menus need at this scale
     * (about 700x480 points at 1x: the main menu's tab column and setting
     * descriptions, and its eight tabs plus margins), so it stays inside the
     * window instead of spilling past its edges. The factor moves in 5%
     * steps, so a live resize rebuilds the font atlas a few times rather
     * than on every pixel.
     */
    int win_w, win_h;
    if (SDL_GetWindowSize(window, &win_w, &win_h) && win_w > 0 && win_h > 0) {
        float fit = fminf(win_w / (700.0f * m_scale), win_h / (480.0f * m_scale));
        if (fit < 1.0f) {
            m_scale *= fmaxf(floorf(fit * 20.0f) / 20.0f, 0.4f);
        }
    }

    if (io.DisplaySize.x > 640*m_scale) {
        m_extents.x = 25 * m_scale; // Distance from Left
        m_extents.y = 25 * m_scale; // '' Top
        m_extents.z = 25 * m_scale; // '' Right
        m_extents.w = 25 * m_scale; // '' Bottom
    } else {
        m_extents = ImVec4(0,0,0,0);
    }
}
