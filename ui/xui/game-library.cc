//
// xemu User Interface - Game library
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-library.hh"
#include "disc-image.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::vector<LibraryGame> g_games;
int64_t g_scanned_at = 0;
std::string g_dir;

std::string Lower(std::string s)
{
    for (char &c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

bool LessNoCase(const std::string &a, const std::string &b)
{
    return Lower(a) < Lower(b);
}

// Every .iso / .xiso below `dir`, without descending into an extracted game
// (a folder that holds default.xbe, whose thousands of files are not disc
// images).
void CollectImages(const fs::path &dir, int depth, std::vector<fs::path> &out)
{
    if (depth > 6) {
        return;
    }
    std::error_code ec;
    fs::directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return;
    }
    bool extracted_game = false;
    std::vector<fs::path> here, subdirs;
    for (const fs::directory_entry &e : it) {
        std::string name = Lower(e.path().filename().u8string());
        if (!name.empty() && name[0] == '.') {
            continue; // skip hidden entries (.Trash, .DS_Store, ...)
        }
        std::error_code fec;
        if (e.is_directory(fec)) {
            subdirs.push_back(e.path());
        } else if (e.is_regular_file(fec)) {
            if (name == "default.xbe") {
                extracted_game = true;
            }
            std::string ext = Lower(e.path().extension().u8string());
            if (ext == ".iso" || ext == ".xiso") {
                here.push_back(e.path());
            }
        }
    }
    if (extracted_game) {
        return; // an extracted game, not a place for disc images
    }
    out.insert(out.end(), here.begin(), here.end());
    for (const fs::path &s : subdirs) {
        CollectImages(s, depth + 1, out);
    }
}

fs::path IndexPath(const std::string &base_path)
{
    return fs::u8path(base_path) / "game-library.json";
}

} // namespace

int GameLibraryScan(const std::string &dir, std::string &error)
{
    error.clear();
    g_dir = dir;

    // The current list seeds the cache: an image whose size and mtime are
    // unchanged keeps its metadata and is not opened again.
    std::map<std::string, LibraryGame> cache;
    for (const LibraryGame &g : g_games) {
        cache[g.path] = g;
    }

    if (dir.empty() || !fs::is_directory(fs::u8path(dir))) {
        error = dir.empty() ? "No games folder is configured" :
                              "The games folder does not exist";
        g_games.clear();
        g_scanned_at = 0;
        return 0;
    }

    std::vector<fs::path> images;
    CollectImages(fs::u8path(dir), 0, images);

    std::vector<LibraryGame> found;
    found.reserve(images.size());
    for (const fs::path &p : images) {
        LibraryGame g;
        g.path = p.u8string();
        std::error_code fec;
        g.size = (uint64_t)fs::file_size(p, fec);
        if (fec) {
            g.size = 0;
            fec.clear();
        }
        auto t = fs::last_write_time(p, fec);
        g.mtime = fec ? 0 : (int64_t)t.time_since_epoch().count();
        fec.clear();

        auto ci = cache.find(g.path);
        if (ci != cache.end() && ci->second.size == g.size &&
            ci->second.mtime == g.mtime) {
            g = ci->second; // unchanged, reuse the cached metadata
        } else {
            DiscInfo info;
            if (ReadDiscInfo(g.path, info)) {
                g.title = info.title_name;
                g.title_id_raw = info.title_id;
                g.region_raw = info.region;
                g.valid = true;
            } else {
                g.title = p.filename().u8string();
                g.error = info.error;
            }
            g.title_id = FormatTitleId(g.title_id_raw);
            g.region = FormatRegion(g.region_raw);
        }
        found.push_back(std::move(g));
    }

    std::sort(found.begin(), found.end(),
              [](const LibraryGame &a, const LibraryGame &b) {
                  if (LessNoCase(a.title, b.title)) {
                      return true;
                  }
                  if (LessNoCase(b.title, a.title)) {
                      return false;
                  }
                  return a.path < b.path;
              });

    g_games = std::move(found);
    g_scanned_at = (int64_t)time(nullptr);
    return (int)g_games.size();
}

const std::vector<LibraryGame> &GameLibraryGames(void)
{
    return g_games;
}

int64_t GameLibraryScannedAt(void)
{
    return g_scanned_at;
}

const std::string &GameLibraryDirectory(void)
{
    return g_dir;
}

void GameLibraryLoadIndex(const std::string &base_path)
{
    std::ifstream f(IndexPath(base_path), std::ios::binary);
    if (!f) {
        return;
    }
    json j;
    try {
        f >> j;
    } catch (...) {
        return; // a damaged index is rebuilt by the next scan
    }
    if (!j.is_object()) {
        return;
    }
    g_dir = j.value("dir", std::string());
    g_scanned_at = j.value("scanned_at", (int64_t)0);

    std::vector<LibraryGame> games;
    for (const json &e : j.value("games", json::array())) {
        if (!e.is_object()) {
            continue;
        }
        LibraryGame g;
        g.path = e.value("path", std::string());
        if (g.path.empty()) {
            continue;
        }
        g.size = e.value("size", (uint64_t)0);
        g.mtime = e.value("mtime", (int64_t)0);
        g.title = e.value("title", std::string());
        g.title_id = e.value("title_id", std::string());
        g.region = e.value("region", std::string());
        g.error = e.value("error", std::string());
        g.title_id_raw = e.value("title_id_raw", 0u);
        g.region_raw = e.value("region_raw", 0u);
        g.valid = e.value("valid", false);
        games.push_back(std::move(g));
    }
    g_games = std::move(games);
}

void GameLibrarySaveIndex(const std::string &base_path)
{
    json j;
    j["version"] = 1;
    j["dir"] = g_dir;
    j["scanned_at"] = g_scanned_at;

    json games = json::array();
    for (const LibraryGame &g : g_games) {
        json e;
        e["path"] = g.path;
        e["size"] = g.size;
        e["mtime"] = g.mtime;
        e["title"] = g.title;
        e["title_id"] = g.title_id;
        e["region"] = g.region;
        e["title_id_raw"] = g.title_id_raw;
        e["region_raw"] = g.region_raw;
        e["valid"] = g.valid;
        if (!g.error.empty()) {
            e["error"] = g.error;
        }
        games.push_back(std::move(e));
    }
    j["games"] = std::move(games);

    std::ofstream f(IndexPath(base_path), std::ios::binary);
    if (!f) {
        return;
    }
    f << j.dump(2);
}
