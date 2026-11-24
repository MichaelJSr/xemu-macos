//
// xemu User Interface - Game patches window
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-patches-window.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common.hh"
#include "IconsFontAwesome6.h"
#include "actions.hh"
#include "disc-image.hh"
#include "game-patches.hh"
#include "misc.hh"
#include "viewport-manager.hh"
#include "../xemu-notifications.h"

// The interface is English-only here (this fork has no translation layer):
// tr() is the modern-gui's i18n hook, kept as identity so the ported strings
// read exactly as written.
#define tr(x) (x)

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool g_window_open = false;

// The disc image being patched (the original, never an overlay) and what the
// game on it is
std::string g_image;
DiscInfo g_info;
bool g_info_done = false;

// One patch file, in the order it is applied
struct PatchItem {
    std::string file;        // file name inside the patch folder
    bool enabled = true;
    std::vector<int> toggled; // .jmp: option indices flipped from default
};
std::vector<PatchItem> g_items;

// .jmp details, by file name
struct JmpInfo {
    bool ok = false;
    JmpPatch jmp;
    std::string error;
    JmpFit fit = JmpFit::Unverified;
};
std::map<std::string, JmpInfo> g_jmp_info;
bool g_have_xbe = false;

std::string g_status;
bool g_status_ok = false;
std::string g_confirm_remove;

fs::path BasePath()
{
    return fs::u8path(xemu_settings_get_base_path());
}

std::string TitleIdHex(uint32_t id)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", id);
    return buf;
}

// std::to_string is ambiguous under QEMU's osdep.h includes, so format here
std::string U64(uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return buf;
}

std::string I64(int64_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return buf;
}

std::string FormatTitleId(uint32_t id)
{
    char a = (char)((id >> 24) & 0xFF), b = (char)((id >> 16) & 0xFF);
    char buf[32];
    if (isalnum((unsigned char)a) && isalnum((unsigned char)b)) {
        snprintf(buf, sizeof(buf), "%c%c-%03u", a, b, id & 0xFFFF);
    } else {
        snprintf(buf, sizeof(buf), "%08X", id);
    }
    return buf;
}

std::string FormatRegion(uint32_t region)
{
    region &= 0x7;
    if (region == 0x7) {
        return "Region free";
    }
    std::string s;
    if (region & 0x1) {
        s += "NTSC-U";
    }
    if (region & 0x2) {
        s += s.empty() ? "NTSC-J" : ", NTSC-J";
    }
    if (region & 0x4) {
        s += s.empty() ? "PAL" : ", PAL";
    }
    return s.empty() ? "Unknown region" : s;
}

fs::path PatchDir(uint32_t title_id)
{
    return BasePath() / "patches" / TitleIdHex(title_id);
}

fs::path OverlayDir()
{
    return BasePath() / "patched-discs";
}

bool IsPatchFile(const fs::path &p)
{
    std::string ext = p.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".xbe" || ext == ".ips" || ext == ".bps" || ext == ".jmp";
}

const char *PatchTypeLabel(const std::string &file)
{
    std::string ext = fs::u8path(file).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == ".xbe") {
        return tr("Custom XBE");
    }
    if (ext == ".ips") {
        return tr("IPS patch");
    }
    if (ext == ".jmp") {
        return tr("Magic Patch");
    }
    return tr("BPS patch");
}

ImVec4 JmpFitColor(JmpFit fit)
{
    switch (fit) {
    case JmpFit::Works:
        return ImVec4(0.45f, 0.86f, 0.16f, 1);
    case JmpFit::Unverified:
        return ImVec4(0.75f, 0.75f, 0.75f, 1);
    default:
        return ImVec4(1.0f, 0.45f, 0.4f, 1);
    }
}

// ---------------------------------------------------------------------------
// Overlay map: overlay file -> the disc image it was built from
// ---------------------------------------------------------------------------

std::map<std::string, std::string> g_overlay_to_original;

fs::path OverlayMapPath()
{
    return OverlayDir() / "overlays.json";
}

void LoadOverlayMap()
{
    g_overlay_to_original.clear();
    std::ifstream f(OverlayMapPath());
    if (!f) {
        return;
    }
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    for (auto &[overlay, original] : j.items()) {
        if (original.is_string()) {
            g_overlay_to_original[overlay] = original.get<std::string>();
        }
    }
}

void SaveOverlayMap()
{
    std::error_code ec;
    fs::create_directories(OverlayDir(), ec);
    json j = json::object();
    for (auto &[overlay, original] : g_overlay_to_original) {
        j[overlay] = original;
    }
    std::ofstream f(OverlayMapPath(), std::ios::trunc);
    if (f) {
        f << j.dump(2);
    }
}

// The disc image behind a loaded path (an overlay maps back to its game)
std::string OriginalDiscPath(const std::string &path)
{
    auto it = g_overlay_to_original.find(path);
    return it == g_overlay_to_original.end() ? path : it->second;
}

// ---------------------------------------------------------------------------
// The saved patch list for a title
// ---------------------------------------------------------------------------

fs::path PatchesMetaPath(uint32_t title_id)
{
    return PatchDir(title_id) / "patches.json";
}

void SavePatches()
{
    if (!g_info.ok) {
        return;
    }
    std::error_code ec;
    fs::create_directories(PatchDir(g_info.title_id), ec);
    json arr = json::array();
    for (auto &pi : g_items) {
        json o;
        o["file"] = pi.file;
        o["enabled"] = pi.enabled;
        o["toggled"] = pi.toggled;
        arr.push_back(o);
    }
    json j;
    j["patches"] = arr;
    std::ofstream f(PatchesMetaPath(g_info.title_id), std::ios::trunc);
    if (f) {
        f << j.dump(2);
    }
}

// Match the saved list with the files actually in the patch folder: keep the
// user's order and on/off state, append new files (on), drop removed ones.
void SyncPatches()
{
    if (!g_info.ok) {
        return;
    }
    g_items.clear();
    std::ifstream f(PatchesMetaPath(g_info.title_id));
    if (f) {
        json j = json::parse(f, nullptr, false);
        if (j.is_object() && j.contains("patches") && j["patches"].is_array()) {
            for (auto &o : j["patches"]) {
                if (!o.is_object() || !o.contains("file") ||
                    !o["file"].is_string()) {
                    continue;
                }
                PatchItem pi;
                pi.file = o["file"].get<std::string>();
                pi.enabled = o.value("enabled", true);
                if (o.contains("toggled") && o["toggled"].is_array()) {
                    for (auto &t : o["toggled"]) {
                        if (t.is_number_integer()) {
                            pi.toggled.push_back(t.get<int>());
                        }
                    }
                }
                g_items.push_back(std::move(pi));
            }
        }
    }

    std::vector<std::string> present;
    std::error_code ec;
    for (fs::directory_iterator it(PatchDir(g_info.title_id), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && IsPatchFile(it->path())) {
            present.push_back(it->path().filename().u8string());
        }
    }
    std::sort(present.begin(), present.end());

    g_items.erase(std::remove_if(g_items.begin(), g_items.end(),
                                 [&](const PatchItem &pi) {
                                     return std::find(present.begin(),
                                                      present.end(),
                                                      pi.file) == present.end();
                                 }),
                  g_items.end());
    for (auto &name : present) {
        bool known = false;
        for (auto &pi : g_items) {
            known |= (pi.file == name);
        }
        if (!known) {
            g_items.push_back({ name, true, {} });
        }
    }
}

void RefreshJmpInfo()
{
    g_jmp_info.clear();
    g_have_xbe = false;
    if (!g_info.ok) {
        return;
    }
    std::vector<uint8_t> xbe;
    std::string err;
    g_have_xbe = ReadDefaultXbe(g_image, xbe, err);
    for (auto &pi : g_items) {
        std::string ext = fs::u8path(pi.file).extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext != ".jmp") {
            continue;
        }
        JmpInfo &info = g_jmp_info[pi.file];
        std::ifstream f(PatchDir(g_info.title_id) / fs::u8path(pi.file),
                        std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        info.ok = ParseJmp(text, info.jmp, info.error);
        if (info.ok && g_have_xbe) {
            info.fit = CheckJmp(info.jmp, xbe, g_info.title_id);
        }
    }
}

// ---------------------------------------------------------------------------
// The image being patched
// ---------------------------------------------------------------------------

// Read the title info of g_image; if it is an overlay, resolve back first
void ReadImageInfo()
{
    g_info = DiscInfo();
    g_info_done = false;
    if (g_image.empty()) {
        return;
    }
    std::string original = OriginalDiscPath(g_image);
    if (!ReadDiscInfo(original, g_info)) {
        g_info_done = true;
        return;
    }
    g_image = original;
    g_info_done = true;
    SyncPatches();
    RefreshJmpInfo();
}

// Adopt whatever the DVD drive currently has loaded
void AdoptCurrentDisc()
{
    const char *path = g_config.sys.files.dvd_path;
    if (!path || !path[0]) {
        return;
    }
    if (g_image == path) {
        return;
    }
    g_image = path;
    ReadImageInfo();
}

std::vector<PatchSpec> EnabledPatches()
{
    std::vector<PatchSpec> out;
    for (auto &pi : g_items) {
        if (pi.enabled) {
            out.push_back({ (PatchDir(g_info.title_id) / fs::u8path(pi.file))
                                .u8string(),
                            pi.toggled });
        }
    }
    return out;
}

int EnabledPatchCount()
{
    int n = 0;
    for (auto &pi : g_items) {
        n += pi.enabled ? 1 : 0;
    }
    return n;
}

// Overlay file name depends on the image and the exact patch files, so any
// change produces a fresh overlay.
std::string OverlayPathFor(const std::vector<PatchSpec> &patches)
{
    std::error_code ec;
    std::string sig = g_image;
    sig += "|" + U64(fs::file_size(fs::u8path(g_image), ec));
    sig += "|" + I64(fs::last_write_time(fs::u8path(g_image), ec)
                         .time_since_epoch()
                         .count());
    for (auto &spec : patches) {
        fs::path fp = fs::u8path(spec.path);
        sig += "|" + spec.path + "|" + U64(fs::file_size(fp, ec)) + "|" +
               I64(fs::last_write_time(fp, ec).time_since_epoch().count());
        for (int t : spec.toggled) {
            sig += "," + std::to_string(t);
        }
    }
    char hash[32];
    snprintf(hash, sizeof(hash), "%016llx",
             (unsigned long long)std::hash<std::string>()(sig));
    return (OverlayDir() / (TitleIdHex(g_info.title_id) + "-" + hash +
                            ".qcow2"))
        .u8string();
}

// Build the patched overlay for the enabled patches, or return the original
// image if there are none.
bool PrepareImage(std::string &load_path, std::string &error)
{
    load_path = g_image;
    if (!g_info.ok) {
        error = g_info.error.empty() ? tr("No disc image is loaded")
                                     : g_info.error;
        return false;
    }
    std::vector<PatchSpec> patches = EnabledPatches();
    if (patches.empty()) {
        return true;
    }
    std::string overlay = OverlayPathFor(patches);
    std::error_code ec;
    if (!fs::exists(fs::u8path(overlay), ec) &&
        !BuildPatchedOverlay(g_image, patches, overlay, error)) {
        return false;
    }
    g_overlay_to_original[overlay] = g_image;
    // Remove this game's older overlays (the file just built is kept)
    std::string prefix = TitleIdHex(g_info.title_id) + "-";
    for (fs::directory_iterator it(OverlayDir(), ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string name = it->path().filename().u8string();
        if (name.compare(0, prefix.size(), prefix) == 0 &&
            it->path().u8string() != overlay) {
            std::error_code rec;
            if (fs::remove(it->path(), rec)) {
                g_overlay_to_original.erase(it->path().u8string());
            }
        }
    }
    SaveOverlayMap();
    load_path = overlay;
    return true;
}

void AddPatchFile()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Game patches (*.xbe, *.ips, *.bps, *.jmp)", "xbe;ips;bps;jmp" },
        { "All Files", "*" },
    };
    ShowOpenFileDialog(filters, 2, nullptr, [](const char *path) {
        if (!g_info.ok) {
            return;
        }
        fs::path src = fs::u8path(path);
        if (!IsPatchFile(src)) {
            xemu_queue_error_message(
                tr("Unsupported patch file. Use .xbe, .ips, .bps or .jmp "
                   "files."));
            return;
        }
        std::error_code ec;
        fs::path dir = PatchDir(g_info.title_id);
        fs::create_directories(dir, ec);
        fs::path dst = dir / src.filename();
        for (int n = 2; fs::exists(dst, ec) && n < 100; n++) {
            dst = dir / fs::u8path(src.stem().u8string() + " (" +
                                   std::to_string(n) + ")" +
                                   src.extension().u8string());
        }
        if (!fs::copy_file(src, dst, ec)) {
            xemu_queue_error_message(tr("Could not copy the patch file."));
            return;
        }
        SyncPatches();
        RefreshJmpInfo();
        SavePatches();
        g_status.clear();
    });
}

// Build the overlay and put it in the DVD drive
void BuildAndLoad()
{
    std::string load, error;
    if (!PrepareImage(load, error)) {
        g_status_ok = false;
        g_status = error;
        return;
    }
    int n = EnabledPatchCount();
    ActionLoadDiscFile(load.c_str());
    g_status_ok = true;
    g_status = n ? string_format(tr("Loaded the patched disc (%d patch(es))."),
                                 n)
                 : tr("Loaded the disc (no patches enabled).");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void DrawJmpDetails(PatchItem &pi, const JmpInfo &info)
{
    const float s = g_viewport_mgr.m_scale;
    if (!info.ok) {
        ImGui::TextColored(JmpFitColor(JmpFit::OtherGame),
                           tr("Can't read: %s"), info.error.c_str());
        return;
    }
    const JmpPatch &jmp = info.jmp;
    if (g_have_xbe) {
        ImGui::TextColored(JmpFitColor(info.fit), "%s", JmpFitLabel(info.fit));
        if (info.fit == JmpFit::Unverified) {
            ImGui::SetItemTooltip(
                tr("The patch has no checksum, so xemu can't confirm it was "
                   "made for your version of the game. Try it; if the game "
                   "misbehaves, switch it off."));
        }
        if (!jmp.author.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled(tr("by %s"), jmp.author.c_str());
        }
    } else if (!jmp.author.empty()) {
        ImGui::TextDisabled(tr("by %s"), jmp.author.c_str());
    }
    if (!jmp.notes.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO);
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(400 * s);
            ImGui::TextUnformatted(jmp.notes.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    std::vector<bool> on = JmpOptionStates(jmp, pi.toggled);
    int n_on = 0, n_usable = 0;
    for (size_t k = 0; k < jmp.options.size(); k++) {
        if (!jmp.options[k].other_xbe) {
            n_usable++;
            n_on += on[k] ? 1 : 0;
        }
    }
    if (jmp.options.size() <= 1) {
        return;
    }
    std::string label =
        string_format(tr("Options (%d of %d on)###opts"), n_on, n_usable);
    if (!ImGui::TreeNode(label.c_str())) {
        return;
    }
    for (size_t k = 0; k < jmp.options.size(); k++) {
        const JmpOption &o = jmp.options[k];
        ImGui::PushID((int)k);
        bool v = on[k] && !o.other_xbe;
        ImGui::BeginDisabled(o.other_xbe);
        if (ImGui::Checkbox(o.name.c_str(), &v)) {
            auto t = std::find(pi.toggled.begin(), pi.toggled.end(), (int)k);
            if (t == pi.toggled.end()) {
                pi.toggled.push_back((int)k);
            } else {
                pi.toggled.erase(t);
            }
            std::sort(pi.toggled.begin(), pi.toggled.end());
            SavePatches();
            g_status.clear();
        }
        ImGui::EndDisabled();
        if (o.other_xbe) {
            ImGui::SetItemTooltip(tr("Changes another program on the disc; "
                                     "only default.xbe can be patched"));
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void DrawWindow()
{
    const float s = g_viewport_mgr.m_scale;

    ImGui::TextUnformatted(g_info.ok ? g_info.title_name.c_str()
                                     : tr("Game Patches"));
    if (g_info.ok) {
        ImGui::TextDisabled("%s  -  %s  -  patch folder: patches\\%s",
                            FormatTitleId(g_info.title_id).c_str(),
                            FormatRegion(g_info.region).c_str(),
                            TitleIdHex(g_info.title_id).c_str());
    } else if (!g_image.empty() && g_info_done) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s",
                           g_info.error.c_str());
    } else {
        ImGui::TextDisabled("%s", tr("No disc image is loaded."));
    }

    ImGui::TextDisabled("%s", g_image.empty() ? tr("(no image)")
                                              : g_image.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Choose image..."))) {
        static const SDL_DialogFileFilter filters[] = {
            { "Disc Image Files (*.iso, *.xiso)", "iso;xiso" },
            { "All Files", "*" },
        };
        const char *def = g_config.sys.files.dvd_path;
        ShowOpenFileDialog(filters, 2, def, [](const char *path) {
            g_image = path;
            ReadImageInfo();
            g_status.clear();
        });
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Use loaded disc"))) {
        AdoptCurrentDisc();
        g_status.clear();
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600 * s);
    ImGui::TextDisabled(
        tr("Enabled patches are applied, top to bottom, when you press Build "
           "& Load. Your disc image is never modified: the patched "
           "default.xbe goes into a qcow2 overlay that is loaded instead. A "
           "custom .xbe replaces the game's default.xbe; .ips, .bps and .jmp "
           "patches modify it."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (!g_info.ok) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted(
            tr("Load or choose a disc image to see its patches."));
        ImGui::Dummy(ImVec2(0, 4 * s));
        return;
    }

    if (g_items.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted(
            tr("No patches yet. Add a .xbe, .ips, .bps or .jmp file."));
        ImGui::Dummy(ImVec2(0, 4 * s));
    } else if (ImGui::BeginTable("##patches", 4,
                                 ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp,
                                 ImVec2(600 * s, 0))) {
        ImGui::TableSetupColumn(tr("On"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Patch"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        int move_from = -1, move_to = -1;
        std::string remove;
        for (size_t i = 0; i < g_items.size(); i++) {
            PatchItem &pi = g_items[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &pi.enabled)) {
                SavePatches();
                g_status.clear();
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(pi.file.c_str());
            auto info_it = g_jmp_info.find(pi.file);
            if (info_it != g_jmp_info.end()) {
                DrawJmpDetails(pi, info_it->second);
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", PatchTypeLabel(pi.file));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                move_from = (int)i;
                move_to = (int)i - 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2 * s);
            ImGui::BeginDisabled(i + 1 == g_items.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                move_from = (int)i;
                move_to = (int)i + 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (g_confirm_remove == pi.file) {
                if (ImGui::SmallButton(tr("Delete file?"))) {
                    remove = pi.file;
                }
            } else if (ImGui::SmallButton(ICON_FA_XMARK)) {
                g_confirm_remove = pi.file;
            }
            ImGui::SetItemTooltip(
                tr("Remove this patch from the patch folder"));
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (move_from >= 0) {
            std::swap(g_items[move_from], g_items[move_to]);
            SavePatches();
            g_status.clear();
        }
        if (!remove.empty()) {
            std::error_code ec;
            fs::remove(PatchDir(g_info.title_id) / fs::u8path(remove), ec);
            g_confirm_remove.clear();
            SyncPatches();
            RefreshJmpInfo();
            SavePatches();
            g_status.clear();
        }
    }

    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::Button(tr(ICON_FA_FOLDER_OPEN "  Add patch..."))) {
        AddPatchFile();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Open patch folder"))) {
        std::error_code ec;
        fs::create_directories(PatchDir(g_info.title_id), ec);
        SDL_OpenURL(("file:///" + PatchDir(g_info.title_id).generic_u8string())
                        .c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Refresh"))) {
        SyncPatches();
        RefreshJmpInfo();
        g_status.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_PLAY "  Build & Load"))) {
        BuildAndLoad();
    }
    ImGui::SetItemTooltip(
        tr("Build the patched overlay and put it in the DVD drive."));

    if (!g_status.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600 * s);
        ImGui::TextColored(g_status_ok ? ImVec4(0.45f, 0.86f, 0.16f, 1)
                                       : ImVec4(1.0f, 0.45f, 0.4f, 1),
                           "%s", g_status.c_str());
        ImGui::PopTextWrapPos();
    }
}

} // namespace

void GamePatchesOpenWindow(void)
{
    LoadOverlayMap();
    AdoptCurrentDisc();
    g_window_open = true;
    ImGui::SetWindowFocus(tr("Game Patches"));
}

void GamePatchesWindowDraw(void)
{
    if (!g_window_open) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(660 * s, 520 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(tr("Game Patches"), &g_window_open)) {
        DrawWindow();
    }
    ImGui::End();
}
