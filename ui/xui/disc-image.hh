//
// xemu User Interface - Xbox disc image reading
//
// Reads a game's default.xbe out of an Xbox disc image (XISO, or a
// Redump-style full disc dump) and the title information in its header.
// The game patch tool uses it to find the file a patch is applied to.
//
// Pure C++17 with no emulator dependencies.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>

struct DiscInfo {
    uint32_t title_id = 0;      // from the XBE certificate
    std::string title_name;     // its UTF-16 title, as UTF-8
    uint32_t region = 0;        // certificate region flags
    uint64_t image_size = 0;
    bool ok = false;
    std::string error;
};

// Read the title id, name and region of the game on a disc image
bool ReadDiscInfo(const std::string &image_path, DiscInfo &out);

// "MS-004" style title id string
std::string FormatTitleId(uint32_t title_id);

// "NTSC-U", "PAL", "NTSC-U / NTSC-J", "Region free", ...
std::string FormatRegion(uint32_t region);
