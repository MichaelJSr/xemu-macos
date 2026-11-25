//
// xemu User Interface - Xbox disc image reading
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "disc-image.hh"
#include "game-patches.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

const size_t kSectorSize = 2048;
const char kXdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";
const size_t kXdvdfsMagicLen = 20;

// Where the XDVDFS game partition can start inside an image:
// plain XISO, XGD3, XGD2 and XGD1 (Redump-style full disc dumps).
const uint64_t kPartitionOffsets[] = { 0, 0x2080000, 0xFD90000, 0x18300000 };

uint16_t Le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

class ImageFile
{
public:
    explicit ImageFile(const std::string &path)
        : f(fs::u8path(path), std::ios::binary)
    {
        if (f) {
            f.seekg(0, std::ios::end);
            size = (uint64_t)f.tellg();
        }
    }
    bool Ok() const
    {
        return (bool)f;
    }
    uint64_t Size() const
    {
        return size;
    }
    bool Read(uint64_t off, size_t len, std::vector<uint8_t> &buf)
    {
        if (off + len > size) {
            return false;
        }
        buf.resize(len);
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char *)buf.data(), (std::streamsize)len);
        return (size_t)f.gcount() == len;
    }

private:
    std::ifstream f;
    uint64_t size = 0;
};

bool NameEquals(const uint8_t *name, size_t len, const char *want)
{
    if (len != strlen(want)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (tolower(name[i]) != tolower((unsigned char)want[i])) {
            return false;
        }
    }
    return true;
}

// Walk the XDVDFS directory binary tree looking for `want`.
bool FindInDirectory(const std::vector<uint8_t> &dir, const char *want,
                     uint32_t &sector, uint32_t &size,
                     uint32_t *entry_pos = nullptr)
{
    std::vector<uint32_t> stack = { 0 };
    std::unordered_set<uint32_t> visited;
    while (!stack.empty()) {
        uint32_t pos = stack.back() * 4;
        stack.pop_back();
        if (!visited.insert(pos).second || pos + 14 > dir.size()) {
            continue;
        }
        const uint8_t *e = &dir[pos];
        uint16_t left = Le16(e), right = Le16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF) {
            continue; // padding
        }
        uint8_t name_len = e[13];
        if (pos + 14 + name_len <= dir.size() &&
            NameEquals(e + 14, name_len, want)) {
            sector = Le32(e + 4);
            size = Le32(e + 8);
            if (entry_pos) {
                *entry_pos = pos;
            }
            return true;
        }
        if (left && left != 0xFFFF) {
            stack.push_back(left);
        }
        if (right && right != 0xFFFF) {
            stack.push_back(right);
        }
    }
    return false;
}

std::string Utf16ToUtf8(const uint8_t *p, size_t max_chars)
{
    std::string out;
    for (size_t i = 0; i < max_chars; i++) {
        uint32_t c = Le16(p + i * 2);
        if (c == 0) {
            break;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < max_chars) {
            uint32_t lo = Le16(p + (i + 1) * 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD; // unpaired surrogate: not encodable as UTF-8
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

} // namespace

bool LocateDefaultXbe(const std::string &image_path, XbeLocation &loc,
                      std::string &error)
{
    ImageFile img(image_path);
    if (!img.Ok()) {
        error = "Cannot open the disc image";
        return false;
    }
    std::vector<uint8_t> buf;
    uint64_t base = 0;
    bool found = false;
    for (uint64_t off : kPartitionOffsets) {
        if (img.Read(off + 32 * kSectorSize, kSectorSize, buf) &&
            memcmp(buf.data(), kXdvdfsMagic, kXdvdfsMagicLen) == 0) {
            base = off;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "Not an Xbox disc image";
        return false;
    }
    uint32_t root_sector = Le32(&buf[20]);
    uint32_t root_size = Le32(&buf[24]);
    std::vector<uint8_t> dir;
    if (root_size == 0 || root_size > 64 * 1024 * 1024 ||
        !img.Read(base + (uint64_t)root_sector * kSectorSize, root_size, dir)) {
        error = "Cannot read the disc's root directory";
        return false;
    }
    uint32_t sector, size, pos;
    if (!FindInDirectory(dir, "default.xbe", sector, size, &pos)) {
        error = "The disc has no default.xbe";
        return false;
    }
    loc.image_size = img.Size();
    loc.xbe_offset = base + (uint64_t)sector * kSectorSize;
    loc.xbe_size = size;
    loc.size_field_offset =
        base + (uint64_t)root_sector * kSectorSize + pos + 8;
    if (loc.xbe_offset + size > loc.image_size) {
        error = "default.xbe extends past the end of the image";
        return false;
    }
    return true;
}

bool ReadDiscInfo(const std::string &image_path, DiscInfo &out)
{
    XbeLocation loc;
    if (!LocateDefaultXbe(image_path, loc, out.error)) {
        return false;
    }
    ImageFile img(image_path);
    std::vector<uint8_t> hdr;
    if (!img.Read(loc.xbe_offset, std::min<uint32_t>(loc.xbe_size, 0x1000),
                  hdr) ||
        hdr.size() < 0x178 || memcmp(hdr.data(), "XBEH", 4) != 0) {
        out.error = "Invalid XBE";
        return false;
    }
    uint32_t base_addr = Le32(&hdr[0x104]);
    uint32_t headers_size = Le32(&hdr[0x108]);
    if (headers_size < 0x178 || headers_size > loc.xbe_size ||
        headers_size > 16 * 1024 * 1024 ||
        !img.Read(loc.xbe_offset, headers_size, hdr)) {
        out.error = "Invalid XBE headers";
        return false;
    }
    uint32_t cert_addr = Le32(&hdr[0x118]);
    uint64_t cert = (uint64_t)cert_addr - base_addr;
    if (cert_addr < base_addr || cert + 0xB0 > hdr.size()) {
        out.error = "Invalid XBE certificate";
        return false;
    }
    const uint8_t *c = &hdr[cert];
    out.title_id = Le32(c + 0x08);
    out.title_name = Utf16ToUtf8(c + 0x0C, 40);
    out.region = Le32(c + 0xA0);
    out.image_size = loc.image_size;
    out.ok = true;
    return true;
}

std::string FormatTitleId(uint32_t title_id)
{
    char a = (char)((title_id >> 24) & 0xFF);
    char b = (char)((title_id >> 16) & 0xFF);
    char buf[32];
    if (isalnum((unsigned char)a) && isalnum((unsigned char)b)) {
        snprintf(buf, sizeof(buf), "%c%c-%03u", a, b, title_id & 0xFFFF);
    } else {
        snprintf(buf, sizeof(buf), "%08X", title_id);
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
        s += s.empty() ? "NTSC-J" : " / NTSC-J";
    }
    if (region & 0x4) {
        s += s.empty() ? "PAL" : " / PAL";
    }
    return s.empty() ? "Unknown" : s;
}
