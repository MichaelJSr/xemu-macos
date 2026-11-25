//
// xemu User Interface - Game library
//
// Scans the games folder for Xbox disc images (.iso / .xiso), reads the
// title metadata out of each one, and caches it so a rescan only re-reads
// the images that changed. Pure C++17 with no emulator dependencies; the
// window is in game-library-window.cc.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LibraryGame {
    std::string path; // the disc image
    std::string title; // from the XBE certificate (or the file name)
    std::string title_id; // "MS-004" style
    std::string region; // "NTSC-U", "PAL", ...
    std::string error; // why the title could not be read ("" if ok)
    uint64_t size = 0; // image size in bytes
    int64_t mtime = 0; // last write time, for the rescan cache
    uint32_t title_id_raw = 0;
    uint32_t region_raw = 0;
    bool valid = false; // the certificate was read
};

// Scans `dir` recursively for .iso / .xiso images into `out` (title order)
// and reads their titles, reusing the metadata in `previous` for images
// whose size and mtime are unchanged. Touches no library state and catches
// its own exceptions, so it may run on a worker thread. Returns the number
// of games found; on failure `error` is set and `out` is left empty.
int GameLibraryScanDir(const std::string &dir,
                       const std::vector<LibraryGame> &previous,
                       std::vector<LibraryGame> &out, std::string &error);

// Replace the list with a scan's result (UI thread).
void GameLibrarySetGames(const std::string &dir,
                         std::vector<LibraryGame> games, int64_t scanned_at);

// The games found by the last scan, or loaded from the index (title order).
const std::vector<LibraryGame> &GameLibraryGames(void);

// When the last scan finished (unix time, 0 if never).
int64_t GameLibraryScannedAt(void);

// The folder the list was built from.
const std::string &GameLibraryDirectory(void);

// Read / write the cached index next to xemu.toml. Loading seeds the rescan
// cache so unchanged images are not re-read.
void GameLibraryLoadIndex(const std::string &base_path);
void GameLibrarySaveIndex(const std::string &base_path);
