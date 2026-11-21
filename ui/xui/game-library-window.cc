//
// xemu User Interface - Game library window
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-library-window.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "common.hh"
#include "IconsFontAwesome6.h"
#include "actions.hh"
#include "game-library.hh"
#include "viewport-manager.hh"

namespace fs = std::filesystem;

namespace {

bool g_window_open = false;
std::string g_filter;
std::string g_selected; // path of the highlighted game
std::string g_status;
bool g_status_ok = true;

fs::path BasePath()
{
    return fs::u8path(xemu_settings_get_base_path());
}

std::string HumanSize(uint64_t v)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double d = (double)v;
    int i = 0;
    while (d >= 1024.0 && i < 4) {
        d /= 1024.0;
        i++;
    }
    char buf[32];
    if (i == 0) {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)v);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", d, unit[i]);
    }
    return buf;
}

bool ContainsNoCase(const std::string &hay, const std::string &needle)
{
    if (needle.empty()) {
        return true;
    }
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return tolower((unsigned char)a) ==
                                     tolower((unsigned char)b);
                          });
    return it != hay.end();
}

bool Matches(const LibraryGame &g)
{
    return ContainsNoCase(g.title, g_filter) ||
           ContainsNoCase(g.title_id, g_filter) ||
           ContainsNoCase(g.region, g_filter) ||
           ContainsNoCase(g.path, g_filter);
}

const LibraryGame *FindSelected()
{
    for (const LibraryGame &g : GameLibraryGames()) {
        if (g.path == g_selected) {
            return &g;
        }
    }
    return nullptr;
}

void Rescan()
{
    const char *dir = g_config.general.games_dir;
    std::string err;
    int n = GameLibraryScan(dir ? dir : "", err);
    if (!err.empty()) {
        g_status = err;
        g_status_ok = false;
        return;
    }
    GameLibrarySaveIndex(BasePath().u8string());
    char buf[64];
    snprintf(buf, sizeof(buf), "Found %d game%s", n, n == 1 ? "" : "s");
    g_status = buf;
    g_status_ok = true;
}

void LoadGame(const LibraryGame &g)
{
    ActionLoadDiscFile(g.path.c_str());
    g_status = "Loading " + g.title;
    g_status_ok = true;
}

void OpenGamesFolder()
{
    const char *dir = g_config.general.games_dir;
    if (dir && dir[0]) {
        SDL_OpenURL(("file:///" + fs::u8path(dir).generic_u8string()).c_str());
    }
}

void DrawWindow()
{
    const float s = g_viewport_mgr.m_scale;
    const char *dir = g_config.general.games_dir;

    ImGui::TextDisabled("%s", (dir && dir[0]) ? dir : "(no games folder set)");
    ImGui::SetItemTooltip("The folder scanned for .iso / .xiso images. Set it "
                          "in Settings > General.");

    if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Rescan")) {
        Rescan();
    }
    ImGui::SetItemTooltip("Look for disc images again. Images whose size and "
                          "modification time are unchanged are not re-read.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder")) {
        OpenGamesFolder();
    }
    ImGui::SameLine();

    const std::vector<LibraryGame> &games = GameLibraryGames();
    ImGui::BeginDisabled(g_selected.empty());
    if (ImGui::Button(ICON_FA_PLAY "  Load")) {
        const LibraryGame *g = FindSelected();
        if (g) {
            LoadGame(*g);
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Put the selected disc image in the DVD drive.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240 * s);
    ImGui::InputTextWithHint("##gamelib-filter",
                             ICON_FA_MAGNIFYING_GLASS " Filter", &g_filter);

    if (!g_status.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(g_status_ok ? ImVec4(0.45f, 0.86f, 0.16f, 1) :
                                         ImVec4(1.0f, 0.45f, 0.4f, 1),
                           "%s", g_status.c_str());
        ImGui::PopTextWrapPos();
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##gamelib", 4, flags,
                          ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()))) {
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Title ID", ImGuiTableColumnFlags_WidthFixed,
                                80 * s);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed,
                                130 * s);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed,
                                90 * s);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int shown = 0;
        for (const LibraryGame &g : games) {
            if (!Matches(g)) {
                continue;
            }
            shown++;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            ImGui::PushID(g.path.c_str());
            if (!g.valid) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.65f, 0.65f, 0.65f, 1.0f));
            }
            if (ImGui::Selectable(g.title.c_str(), g_selected == g.path,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                g_selected = g.path;
            }
            if (!g.valid) {
                ImGui::PopStyleColor();
            }
            if (!g.valid && !g.error.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", g.error.c_str());
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g_selected = g.path;
                LoadGame(g);
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(g.title_id.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(g.region.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(HumanSize(g.size).c_str());
        }
        ImGui::EndTable();

        if (shown == 0) {
            ImGui::TextDisabled(games.empty() ?
                                    "No games found. Check the games folder "
                                    "in Settings > General." :
                                    "No game matches the filter.");
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d of %d games", shown,
                     (int)games.size());
            if (GameLibraryScannedAt()) {
                char when[64];
                time_t t = (time_t)GameLibraryScannedAt();
                struct tm tmv;
                localtime_r(&t, &tmv);
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
                ImGui::TextDisabled("%s - last scan %s", buf, when);
            } else {
                ImGui::TextDisabled("%s", buf);
            }
        }
    }
}

} // namespace

void GameLibraryOpenWindow(void)
{
    GameLibraryLoadIndex(BasePath().u8string());

    const char *dir = g_config.general.games_dir;
    std::string want = dir ? dir : "";
    if (GameLibraryGames().empty() || GameLibraryDirectory() != want) {
        Rescan();
    }

    g_window_open = true;
    ImGui::SetWindowFocus("Game Library");
}

void GameLibraryWindowDraw(void)
{
    if (!g_window_open) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(760 * s, 520 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Game Library", &g_window_open)) {
        DrawWindow();
    }
    ImGui::End();
}
