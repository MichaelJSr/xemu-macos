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

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

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

// A scan runs on its own thread: a big tree or a network share can take
// seconds, and the HUD runs holding the BQL. The UI thread and the worker
// share the job, and whichever lets go last frees it, so a scan still
// running at exit touches nothing freed and exit never waits for it.
struct ScanJob {
    GMutex lock;
    gint refs = 2;
    std::string dir; // set before the thread starts, then read-only
    std::vector<LibraryGame> previous;
    bool done = false; // the rest is under lock
    std::vector<LibraryGame> result;
    std::string error;
    int64_t scanned_at = 0;
};
ScanJob *g_job; // the scan in flight, UI thread only

void ScanJobUnref(ScanJob *job)
{
    if (g_atomic_int_dec_and_test(&job->refs)) {
        g_mutex_clear(&job->lock);
        delete job;
    }
}

gpointer ScanThread(gpointer opaque)
{
    ScanJob *job = (ScanJob *)opaque;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    std::vector<LibraryGame> games;
    std::string error;
    GameLibraryScanDir(job->dir, job->previous, games, error);
    g_mutex_lock(&job->lock);
    job->result = std::move(games);
    job->error = std::move(error);
    job->scanned_at = (int64_t)time(nullptr);
    job->done = true;
    g_mutex_unlock(&job->lock);
    ScanJobUnref(job);
    return nullptr;
}

void Rescan()
{
    if (g_job) {
        return;
    }
    const char *dir = g_config.general.games_dir;
    ScanJob *job = new ScanJob;
    g_mutex_init(&job->lock);
    job->dir = dir ? dir : "";
    job->previous = GameLibraryGames();
    GError *gerr = nullptr;
    GThread *t = g_thread_try_new("gamelib-scan", ScanThread, job, &gerr);
    if (!t) {
        g_status = std::string("Could not start the scan: ") + gerr->message;
        g_status_ok = false;
        g_error_free(gerr);
        g_mutex_clear(&job->lock);
        delete job;
        return;
    }
    g_thread_unref(t); // detached
    g_job = job;
    g_status = "Scanning...";
    g_status_ok = true;
}

// Take a finished scan's result, on the UI thread
void PollScan()
{
    if (!g_job) {
        return;
    }
    std::string dir, err;
    std::vector<LibraryGame> games;
    int64_t at = 0;
    g_mutex_lock(&g_job->lock);
    bool done = g_job->done;
    if (done) {
        dir = g_job->dir;
        games = std::move(g_job->result);
        err = std::move(g_job->error);
        at = g_job->scanned_at;
    }
    g_mutex_unlock(&g_job->lock);
    if (!done) {
        return;
    }
    ScanJobUnref(g_job);
    g_job = nullptr;

    const char *want = g_config.general.games_dir;
    if (dir != (want ? want : "")) {
        // The games folder changed while it ran
        g_status.clear();
        if (g_window_open) {
            Rescan();
        }
        return;
    }
    if (!err.empty()) {
        GameLibrarySetGames(dir, {}, 0);
        g_status = err;
        g_status_ok = false;
        return;
    }
    int n = (int)games.size();
    GameLibrarySetGames(dir, std::move(games), at);
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

    ImGui::BeginDisabled(g_job != nullptr);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Rescan")) {
        Rescan();
    }
    ImGui::EndDisabled();
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

        if (shown == 0 && games.empty() && g_job) {
            // The status line already says "Scanning..."
        } else if (shown == 0) {
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
    // A scan in flight replaces the list when it lands
    if (!g_job) {
        GameLibraryLoadIndex(BasePath().u8string());

        const char *dir = g_config.general.games_dir;
        std::string want = dir ? dir : "";
        if (GameLibraryGames().empty() || GameLibraryDirectory() != want) {
            Rescan();
        }
    }

    g_window_open = true;
    ImGui::SetWindowFocus("Game Library");
}

void GameLibraryWindowDraw(void)
{
    PollScan(); // also while closed, so a scan always lands
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
