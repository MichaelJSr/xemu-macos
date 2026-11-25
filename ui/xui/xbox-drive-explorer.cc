//
// xemu User Interface - Xbox hard drive explorer
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xbox-drive-explorer.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>

#include "common.hh"
#include "IconsFontAwesome6.h"
#include "misc.hh"
#include "viewport-manager.hh"
#include "xbox-drive-qemu.hh"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"

// The interface is English-only here (this fork has no translation layer):
// tr()/trid() are the modern-gui's i18n hooks, kept as identity so the
// ported strings and window IDs read exactly as written.
#define tr(x) (x)
#define trid(x) (x)

namespace fs = std::filesystem;

namespace
{

// A static: its destructor writes nothing (the drive is closed under the
// BQL when the window closes and at shutdown, see ShutdownDrive)
XboxDrive g_drive;
std::string g_open_err;
uint64_t g_open_tried = 0; // ticks of the last failed open
unsigned g_drive_loads = 0; // snapshot loads when the drive was opened
char g_vol = 'E';
std::string g_path = "/";
std::vector<XboxFileInfo> g_list;
std::string g_list_err;
bool g_list_dirty = true;
int g_sel = -1;
std::string g_status;
bool g_status_error = false;
bool g_window_open = false;

void SetStatus(const std::string &msg, bool error = false)
{
    g_status = msg;
    g_status_error = error;
}

// Where "Back up now" keeps game-save copies. The modern-gui routes this
// through its game library (GameLibrarySaveBackupDir); without one, a
// "save-backups" folder beside the settings, as the library uses.
std::string SaveBackupDir()
{
    return (fs::u8path(xemu_settings_get_base_path()) / "save-backups").u8string();
}

std::string FormatSize(uint64_t n)
{
    char b[32];
    if (n < 1024) {
        snprintf(b, sizeof(b), "%llu B", (unsigned long long)n);
    } else if (n < (1ull << 20)) {
        snprintf(b, sizeof(b), "%.1f KB", n / 1024.0);
    } else if (n < (1ull << 30)) {
        snprintf(b, sizeof(b), "%.1f MB", n / 1048576.0);
    } else {
        snprintf(b, sizeof(b), "%.2f GB", n / 1073741824.0);
    }
    return b;
}

std::string FormatTime(time_t t)
{
    if (t <= 0) {
        return "";
    }
    char b[32];
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv);
    return b;
}

void ShutdownDrive();

bool EnsureOpen()
{
    if (g_drive.IsOpen()) {
        return true;
    }
    uint64_t now = SDL_GetTicks();
    if (g_open_tried && now - g_open_tried < 2000) {
        return false;
    }
    g_open_err.clear();
    if (!g_drive.Open(XboxDiskOpenEmulated(&g_open_err), &g_open_err)) {
        g_open_tried = now;
        return false;
    }
    g_open_tried = 0;
    g_drive_loads = XboxSnapshotLoads();
    static bool shutdown_hooked = false;
    if (!shutdown_hooked) {
        XboxOnShutdown(ShutdownDrive);
        shutdown_hooked = true;
    }
    if (!g_drive.Volume(g_vol)) {
        g_vol = g_drive.Volumes()[0].letter;
        g_path = "/";
    }
    g_list_dirty = true;
    return true;
}

// Re-read the disk (the running Xbox may have changed it). The free space
// counted before is kept unless keep_free is false (a full FAT scan).
void Reopen(bool keep_free = true)
{
    std::vector<XboxVolume> before = g_drive.Volumes();
    g_drive.Close();
    g_open_tried = 0;
    EnsureOpen();
    if (keep_free) {
        g_drive.KeepFreeSpace(before);
    }
    g_list_dirty = true;
}

void LoadList()
{
    g_list_dirty = false;
    g_sel = -1;
    g_list_err.clear();
    if (!g_drive.List(g_vol, g_path, &g_list, &g_list_err)) {
        // The folder may be gone: fall back to the root
        if (g_path != "/") {
            g_path = "/";
            g_drive.List(g_vol, g_path, &g_list, &g_list_err);
        }
    }
    std::sort(g_list.begin(), g_list.end(),
              [](const XboxFileInfo &a, const XboxFileInfo &b) {
                  if (a.dir != b.dir) {
                      return a.dir;
                  }
                  std::string x = a.name, y = b.name;
                  std::transform(x.begin(), x.end(), x.begin(), ::tolower);
                  std::transform(y.begin(), y.end(), y.begin(), ::tolower);
                  return x < y;
              });
}

void Navigate(char vol, const std::string &path)
{
    g_vol = vol;
    g_path = path.empty() ? "/" : path;
    if (XboxIsRunning()) {
        Reopen(); // see the Xbox's latest changes
    }
    g_list_dirty = true;
}

// ---- Jobs: copy in, copy out, delete ---------------------------------------

struct Step {
    enum Kind { XMkDir, XFile, HMkDir, HFile, XDelFile, XDelDir } kind;
    std::string host;  // host path (UTF-8)
    std::string xpath; // path on the Xbox volume
    uint64_t size = 0;
};

struct Job {
    std::string title;
    bool writes = false; // changes the Xbox drive
    char vol = 'E';
    std::vector<Step> steps;
    size_t i = 0;
    uint64_t done = 0, total = 0;
    FILE *f = nullptr;
    uint64_t off = 0;
    bool cancel = false;
    std::string err;
    std::vector<uint8_t> buf;
    int files = 0;
    uint64_t disk_writes = 0; // XboxDiskWriteCount() at the start
};

std::unique_ptr<Job> g_job;
bool g_paused_for_job = false; // a write job paused a running Xbox

// Deepest folder nesting a job walks (a damaged drive can loop)
constexpr int kMaxDepth = 64;

// Confirmation before a job that writes to the Xbox drive
std::unique_ptr<Job> g_pending_write;
bool g_open_saves = false; // open the Game saves window

bool HostToXboxSteps(const fs::path &src, const std::string &xdir, Job *job,
                     std::string *err)
{
    std::string name = src.filename().u8string();
    std::string why;
    if (!XboxValidName(name, &why)) {
        *err = "\"" + name + "\": " + why;
        return false;
    }
    std::string xpath = XboxJoin(xdir, name);
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
        job->steps.push_back({ Step::XMkDir, src.u8string(), xpath, 0 });
        std::vector<fs::path> children;
        for (auto &e : fs::directory_iterator(src, ec)) {
            children.push_back(e.path());
        }
        std::sort(children.begin(), children.end());
        for (auto &c : children) {
            if (!HostToXboxSteps(c, xpath, job, err)) {
                return false;
            }
        }
        return true;
    }
    uint64_t size = fs::file_size(src, ec);
    if (ec) {
        *err = string_format(tr("Can't read \"%s\""), src.u8string().c_str());
        return false;
    }
    if (size > 0xFFFFFFFFull) {
        *err = string_format(tr("\"%s\" is larger than 4 GB, the Xbox's file size limit"),
                             name.c_str());
        return false;
    }
    job->steps.push_back({ Step::XFile, src.u8string(), xpath, size });
    job->total += size;
    job->files++;
    return true;
}

// A name read off the Xbox drive, as a file name in a folder on this
// computer. The image can hold any bytes ("..", "/", "C:", control
// characters), so only names the Xbox itself could make pass.
bool HostSafeName(const std::string &name, std::string *why)
{
    if (!XboxValidName(name, why)) {
        return false;
    }
#ifdef _WIN32
    // Windows drops trailing dots and spaces ("..." would be the folder)
    if (name.back() == '.' || name.back() == ' ') {
        *why = "Windows can't use a name that ends in a dot or a space";
        return false;
    }
#endif
    return true;
}

bool XboxToHostSteps(char vol, const std::string &xpath, const XboxFileInfo &fi,
                     const fs::path &hdir, Job *job, std::string *err,
                     int depth = 0)
{
    std::string why;
    if (!HostSafeName(fi.name, &why)) {
        *err = "\"" + fi.name + "\" on the Xbox: " + why;
        return false;
    }
    if (depth > kMaxDepth) {
        *err = tr("Folders nested too deeply");
        return false;
    }
    fs::path hpath = hdir / fs::u8path(fi.name);
    // And it stays in the folder
    fs::path rel = hpath.lexically_normal().lexically_relative(hdir.lexically_normal());
    if (rel.empty() || rel == "." || *rel.begin() == "..") {
        *err = "\"" + fi.name + "\" on the Xbox: not a plain file name";
        return false;
    }
    if (!fi.dir) {
        job->steps.push_back({ Step::HFile, hpath.u8string(), xpath, fi.size });
        job->total += fi.size;
        job->files++;
        return true;
    }
    job->steps.push_back({ Step::HMkDir, hpath.u8string(), xpath, 0 });
    std::vector<XboxFileInfo> list;
    if (!g_drive.List(vol, xpath, &list, err)) {
        return false;
    }
    for (auto &c : list) {
        if (!XboxToHostSteps(vol, XboxJoin(xpath, c.name), c, hpath, job, err,
                             depth + 1)) {
            return false;
        }
    }
    return true;
}

bool DeleteSteps(char vol, const std::string &xpath, const XboxFileInfo &fi,
                 Job *job, std::string *err, int depth = 0)
{
    if (depth > kMaxDepth) {
        *err = tr("Folders nested too deeply");
        return false;
    }
    if (fi.dir) {
        std::vector<XboxFileInfo> list;
        if (!g_drive.List(vol, xpath, &list, err)) {
            return false;
        }
        for (auto &c : list) {
            if (!DeleteSteps(vol, XboxJoin(xpath, c.name), c, job, err, depth + 1)) {
                return false;
            }
        }
        job->steps.push_back({ Step::XDelDir, "", xpath, 0 });
    } else {
        job->steps.push_back({ Step::XDelFile, "", xpath, 0 });
        job->files++;
    }
    job->total += 1;
    return true;
}

void StartJob(std::unique_ptr<Job> job)
{
    if (g_job) {
        // An async file dialog can come back while a job runs
        SetStatus(tr("Another copy is still running"), true);
        return;
    }
    if (job->writes) {
        // The Xbox caches its file system: pause it, start from a fresh view
        // of the disk, and restart it when done. Until then it stays paused
        // and snapshots can't be saved.
        g_paused_for_job = XboxIsRunning();
        XboxPause();
        Reopen();
        if (!g_drive.IsOpen()) {
            if (g_paused_for_job) {
                XboxResume();
            }
            g_paused_for_job = false;
            SetStatus(g_open_err, true);
            return;
        }
        XboxJobBegin();
        job->disk_writes = XboxDiskWriteCount();
    }
    job->buf.resize(1 << 20);
    g_job = std::move(job);
    SetStatus(g_job->title + "...");
}

void AskThenStart(std::unique_ptr<Job> job)
{
    if (job->steps.empty()) {
        return;
    }
    if (job->writes) {
        // Confirmed in a dialog first (opened by DrawPopups)
        g_pending_write = std::move(job);
        return;
    }
    StartJob(std::move(job));
}

// What a change to the drive did to the Xbox
std::string RestartNote(bool resumed)
{
    return std::string(" ") +
           (resumed ? tr("The Xbox restarted to load the changes.")
                    : tr("The Xbox was reset to load the changes and stays paused."));
}

// End the job. At shutdown the Xbox isn't restarted.
void FinishJob(bool shutdown = false)
{
    Job &j = *g_job;
    bool open_host_file = j.f != nullptr;
    if (j.f) {
        fclose(j.f);
        j.f = nullptr;
    }
    if (j.cancel && j.i < j.steps.size()) {
        // Don't leave a half-copied file behind (only the one being written:
        // a step not started yet may name a file that was already there)
        Step &s = j.steps[j.i];
        std::string e;
        if (s.kind == Step::XFile && j.off > 0) {
            g_drive.RemoveFile(j.vol, s.xpath, &e);
        } else if (s.kind == Step::HFile && open_host_file) {
            std::error_code ec;
            fs::remove(fs::u8path(s.host), ec);
        }
    }
    std::string note;
    if (j.writes) {
        g_drive.Flush();
        XboxJobEnd();
        if (!shutdown) {
            g_drive.UpdateFreeSpace(j.vol);
            // Reset only if the disk changed, and keep a paused Xbox paused
            if (XboxDiskWriteCount() != j.disk_writes) {
                XboxRestart(g_paused_for_job);
                note = RestartNote(g_paused_for_job);
            } else if (g_paused_for_job) {
                XboxResume();
            }
        }
        g_paused_for_job = false;
    }
    if (!j.err.empty()) {
        SetStatus(j.err + note, true);
    } else if (j.cancel) {
        SetStatus(tr("Cancelled.") + note);
    } else {
        SetStatus(j.title + " - " + tr("done.") + note);
    }
    g_job.reset();
    g_list_dirty = true;
}

// A snapshot load reverted the disk under the job: what libfatx has cached
// is stale, so stop without writing anything more. At shutdown the Xbox
// isn't resumed.
void AbandonJob(bool shutdown = false)
{
    Job &j = *g_job;
    if (j.f) {
        fclose(j.f);
        j.f = nullptr;
        if (j.i < j.steps.size() && j.steps[j.i].kind == Step::HFile) {
            std::error_code ec;
            fs::remove(fs::u8path(j.steps[j.i].host), ec);
        }
    }
    g_drive.Discard();
    g_open_tried = 0;
    if (j.writes) {
        XboxJobEnd();
        if (g_paused_for_job && !shutdown) {
            XboxResume();
        }
        g_paused_for_job = false;
    }
    SetStatus(tr("Stopped: a snapshot was loaded during the copy"), true);
    g_job.reset();
    g_list_dirty = true;
}

// xemu is quitting: end a job and close the drive while the block layer is
// still there (shutdown notifier, BQL held)
void ShutdownDrive()
{
    if (g_drive.IsOpen() && XboxSnapshotLoads() != g_drive_loads) {
        // A load reverted the disk since the drive was opened (and
        // XboxExplorerUpdate hasn't run since): write nothing
        if (g_job) {
            AbandonJob(true);
        }
        g_drive.Discard();
    } else if (g_job) {
        g_job->cancel = true;
        FinishJob(true);
    }
    g_pending_write.reset();
    g_drive.Close();
}

// Do one piece of work; false when the job has ended
bool RunStep(Job &j)
{
    if (j.cancel || !j.err.empty() || j.i >= j.steps.size()) {
        return false;
    }
    Step &s = j.steps[j.i];
    std::string err;
    switch (s.kind) {
    case Step::XMkDir:
        if (!g_drive.MakeDir(j.vol, s.xpath, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.i++;
        return true;
    case Step::HMkDir: {
        std::error_code ec;
        fs::create_directories(fs::u8path(s.host), ec);
        if (ec) {
            j.err = string_format(tr("Can't create \"%s\""), s.host.c_str());
            return false;
        }
        j.i++;
        return true;
    }
    case Step::XDelFile:
        if (!g_drive.RemoveFile(j.vol, s.xpath, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.done++;
        j.i++;
        return true;
    case Step::XDelDir:
        if (!g_drive.DeleteEmptyDir(j.vol, s.xpath, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.done++;
        j.i++;
        return true;
    case Step::XFile: {
        if (!j.f) {
            j.f = fopen(s.host.c_str(), "rb");
#ifdef _WIN32
            if (!j.f) {
                j.f = _wfopen(fs::u8path(s.host).wstring().c_str(), L"rb");
            }
#endif
            if (!j.f) {
                j.err = string_format(tr("Can't read \"%s\""), s.host.c_str());
                return false;
            }
            if (!g_drive.NewFile(j.vol, s.xpath, &err)) {
                j.err = s.xpath + ": " + err;
                return false;
            }
            j.off = 0;
        }
        size_t n = fread(j.buf.data(), 1, j.buf.size(), j.f);
        if (n > 0 &&
            !g_drive.WriteAt(j.vol, s.xpath, j.off, j.buf.data(), n, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.off += n;
        j.done += n;
        if (n < j.buf.size()) {
            fclose(j.f);
            j.f = nullptr;
            j.off = 0;
            j.i++;
        }
        return true;
    }
    case Step::HFile: {
        if (!j.f) {
#ifdef _WIN32
            j.f = _wfopen(fs::u8path(s.host).wstring().c_str(), L"wb");
#else
            j.f = fopen(s.host.c_str(), "wb");
#endif
            if (!j.f) {
                j.err = string_format(tr("Can't write \"%s\""), s.host.c_str());
                return false;
            }
            j.off = 0;
        }
        size_t want = (size_t)std::min<uint64_t>(j.buf.size(), s.size - j.off);
        int64_t n = want ? g_drive.ReadAt(j.vol, s.xpath, j.off, j.buf.data(), want)
                         : 0;
        if (n < 0 || (want && n == 0) ||
            (n > 0 && fwrite(j.buf.data(), 1, (size_t)n, j.f) != (size_t)n)) {
            j.err = string_format(tr("Couldn't copy %s"), s.xpath.c_str());
            return false;
        }
        j.off += n;
        j.done += n;
        if (j.off >= s.size) {
            fclose(j.f);
            j.f = nullptr;
            j.off = 0;
            j.i++;
        }
        return true;
    }
    }
    return false;
}

// ---- Actions ---------------------------------------------------------------

const XboxFileInfo *Selected()
{
    if (g_sel < 0 || g_sel >= (int)g_list.size()) {
        return nullptr;
    }
    return &g_list[g_sel];
}

void AddHostPaths(const std::vector<std::string> &paths)
{
    if (g_job) {
        SetStatus(tr("Another copy is still running"), true);
        return;
    }
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = g_vol;
    std::string err;
    for (auto &p : paths) {
        if (!HostToXboxSteps(fs::u8path(p), g_path, job.get(), &err)) {
            SetStatus(err, true);
            return;
        }
    }
    const XboxVolume *v = g_drive.Volume(g_vol);
    if (v && v->free_known && job->total > v->free) {
        SetStatus(string_format(tr("Not enough space on %c: (%s needed, %s free)"), g_vol,
                                FormatSize(job->total).c_str(),
                                FormatSize(v->free).c_str()),
                  true);
        return;
    }
    job->title = string_format(tr("Copying files to %c: (%d)"), g_vol, job->files);
    AskThenStart(std::move(job));
}

void ExportSelected(const std::string &host_dir)
{
    const XboxFileInfo *fi = Selected();
    if (!fi) {
        return;
    }
    if (g_job) {
        SetStatus(tr("Another copy is still running"), true);
        return;
    }
    auto job = std::make_unique<Job>();
    job->vol = g_vol;
    std::string err;
    if (!XboxToHostSteps(g_vol, XboxJoin(g_path, fi->name), *fi,
                         fs::u8path(host_dir), job.get(), &err)) {
        SetStatus(err, true);
        return;
    }
    job->title = string_format(tr("Copying \"%s\" to your PC"), fi->name.c_str());
    AskThenStart(std::move(job));
}

void DeleteSelected()
{
    const XboxFileInfo *fi = Selected();
    if (!fi) {
        return;
    }
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = g_vol;
    std::string err;
    if (!DeleteSteps(g_vol, XboxJoin(g_path, fi->name), *fi, job.get(), &err)) {
        SetStatus(err, true);
        return;
    }
    job->title = string_format(tr("Deleting \"%s\""), fi->name.c_str());
    // Already confirmed by the delete dialog
    StartJob(std::move(job));
}

// Single quick changes (new folder, rename) run as a one-step write job so
// they get the same pause/restart handling
void SimpleWrite(const std::string &title, std::function<bool(std::string *)> op)
{
    if (g_job) {
        SetStatus(tr("Another copy is still running"), true);
        return;
    }
    bool was_running = XboxIsRunning();
    XboxPause();
    Reopen();
    uint64_t writes = XboxDiskWriteCount();
    std::string err;
    bool ok = g_drive.IsOpen() && op(&err);
    if (g_drive.IsOpen()) {
        g_drive.Flush();
    }
    // Reset only if the disk changed (a failed op may have written part of
    // it), and keep a paused Xbox paused
    std::string note;
    if (XboxDiskWriteCount() != writes) {
        XboxRestart(was_running);
        note = RestartNote(was_running);
    } else if (was_running) {
        XboxResume();
    }
    SetStatus(ok ? title + " - " + tr("done.") + note
                 : (err.empty() ? g_open_err : err) + note,
              !ok);
    g_list_dirty = true;
}

// Esc or B in a dialog: cancel it without also closing the dashboard
bool CancelPressed()
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        return true;
    }
    return false;
}

char g_name_buf[64];
bool g_focus_name = false;

// ---- Drawing ---------------------------------------------------------------

void DrawVolumes()
{
    const float s = g_viewport_mgr.m_scale;
    for (auto &v : g_drive.Volumes()) {
        ImGui::PushID(v.letter);
        bool cur = v.letter == g_vol;
        std::string label =
            string_format(ICON_FA_HARD_DRIVE "  %c:  %s", v.letter, v.label);
        ImVec2 size(ImGui::CalcTextSize(label.c_str()).x + 28 * s, 0);
        if (cur) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label.c_str(), size) && !g_job) {
            Navigate(v.letter, "/");
        }
        if (cur) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered()) {
            if (v.free_known) {
                ImGui::SetTooltip(tr("%s free of %s"), FormatSize(v.free).c_str(),
                                  FormatSize(v.size).c_str());
            } else {
                ImGui::SetTooltip("%s", FormatSize(v.size).c_str());
            }
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::NewLine();
}

void DrawToolbar()
{
    const float s = g_viewport_mgr.m_scale;
    bool busy = g_job != nullptr;
    // Breadcrumbs: E: > Apps > Foo
    if (ImGui::SmallButton(string_format("%c:", g_vol).c_str()) && !busy) {
        Navigate(g_vol, "/");
    }
    std::string acc;
    size_t start = 1;
    while (start < g_path.size()) {
        size_t end = g_path.find('/', start);
        if (end == std::string::npos) {
            end = g_path.size();
        }
        std::string part = g_path.substr(start, end - start);
        acc += "/" + part;
        ImGui::SameLine(0, 4 * s);
        ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
        ImGui::SameLine(0, 4 * s);
        ImGui::PushID(acc.c_str());
        if (ImGui::SmallButton(part.c_str()) && !busy) {
            Navigate(g_vol, acc);
        }
        ImGui::PopID();
        start = end + 1;
    }
    const XboxVolume *v = g_drive.Volume(g_vol);
    if (v && v->free_known) {
        std::string fr = string_format(tr("%s free"), FormatSize(v->free).c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x -
                        ImGui::CalcTextSize(fr.c_str()).x);
        ImGui::TextDisabled("%s", fr.c_str());
    }

    ImGui::BeginDisabled(busy);
    const XboxFileInfo *sel = Selected();
    if (ImGui::Button(tr(ICON_FA_ARROW_UP " Up")) && g_path != "/") {
        Navigate(g_vol, XboxParent(g_path));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_ROTATE " Refresh"))) {
        Reopen(false);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FOLDER_PLUS " New folder"))) {
        g_name_buf[0] = 0;
        g_focus_name = true;
        ImGui::OpenPopup("##xdrive-newfolder");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FILE_IMPORT " Add files"))) {
        ShowOpenFilesDialog(nullptr, [](const std::vector<std::string> &p) {
            AddHostPaths(p);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FOLDER_OPEN " Add folder"))) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { AddHostPaths({ p }); });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(tr(ICON_FA_FILE_EXPORT " Copy to PC"))) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { ExportSelected(p); });
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_PEN " Rename")) && sel) {
        snprintf(g_name_buf, sizeof(g_name_buf), "%s", sel->name.c_str());
        g_focus_name = true;
        ImGui::OpenPopup("##xdrive-rename");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_TRASH " Delete"))) {
        ImGui::OpenPopup("##xdrive-delete");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FLOPPY_DISK " Game saves"))) {
        g_open_saves = true;
    }
    ImGui::SetItemTooltip("%s", tr("Back up the Xbox's game saves to your PC, or put "
                                   "a backup back"));
    ImGui::EndDisabled();
}

void DrawList(float height)
{
    const float s = g_viewport_mgr.m_scale;
    if (g_list_dirty && !g_job) {
        LoadList();
    }
    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##xdrive-files", 3, flags, ImVec2(0, height))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 110 * s);
    ImGui::TableSetupColumn(tr("Modified"), ImGuiTableColumnFlags_WidthFixed, 160 * s);
    ImGui::TableHeadersRow();
    if (!g_list_err.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g_list_err.c_str());
    } else if (g_list.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled(tr("This folder is empty"));
    }
    std::string open_dir;
    for (int i = 0; i < (int)g_list.size(); i++) {
        const XboxFileInfo &fi = g_list[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(i);
        std::string label = std::string(fi.dir ? ICON_FA_FOLDER : ICON_FA_FILE) +
                            "  " + fi.name;
        bool clicked = ImGui::Selectable(label.c_str(), g_sel == i,
                                         ImGuiSelectableFlags_SpanAllColumns |
                                             ImGuiSelectableFlags_AllowDoubleClick);
        if (clicked) {
            g_sel = i;
            bool mouse = ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            if (fi.dir && !g_job &&
                (!mouse || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                open_dir = XboxJoin(g_path, fi.name);
            }
        }
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) {
            g_sel = i;
        }
        ImGui::TableSetColumnIndex(1);
        if (!fi.dir) {
            ImGui::TextDisabled("%s", FormatSize(fi.size).c_str());
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", FormatTime(fi.modified).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!open_dir.empty()) {
        Navigate(g_vol, open_dir);
    }
}

void DrawFooter()
{
    const float s = g_viewport_mgr.m_scale;
    if (g_job) {
        Job &j = *g_job;
        float frac = j.total ? (float)((double)j.done / (double)j.total) : 0.0f;
        std::string cur =
            j.i < j.steps.size() ? j.steps[j.i].xpath : std::string();
        ImGui::ProgressBar(frac, ImVec2(-120 * s, 0), j.title.c_str());
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(-1, 0))) {
            j.cancel = true;
        }
        ImGui::TextDisabled("%s", cur.c_str());
        return;
    }
    if (!g_status.empty()) {
        if (g_status_error) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), ICON_FA_TRIANGLE_EXCLAMATION
                               "  %s", g_status.c_str());
        } else {
            ImGui::TextDisabled("%s", g_status.c_str());
        }
    } else if (XboxIsRunning()) {
        ImGui::TextDisabled(tr("Copying to the Xbox pauses it and restarts it when done."));
    } else {
        ImGui::TextDisabled(tr("Double-click a folder to open it."));
    }
}

bool NamePopup(const char *id, const char *title, const char *button,
               std::string *out)
{
    bool done = false;
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(title);
        if (g_focus_name) {
            ImGui::SetKeyboardFocusHere();
            g_focus_name = false;
        }
        bool enter = ImGui::InputText("##name", g_name_buf, sizeof(g_name_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        std::string why;
        bool valid = XboxValidName(g_name_buf, &why);
        if (!valid && g_name_buf[0]) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", why.c_str());
        }
        ImGui::TextDisabled(tr("The Xbox pauses and restarts to make the change."));
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(button) || (enter && valid)) {
            *out = g_name_buf;
            done = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return done;
}

// ---- Game saves window ----

struct SaveBackup {
    std::string name, path;
    uint64_t bytes = 0;
    int files = 0;
};
std::vector<SaveBackup> g_backups;
std::string g_restore_path; // asking to restore this one

void ListBackups()
{
    g_backups.clear();
    std::error_code ec;
    fs::path dir = fs::u8path(SaveBackupDir());
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        SaveBackup b;
        b.name = e.path().filename().u8string();
        b.path = e.path().u8string();
        for (auto &f : fs::recursive_directory_iterator(e.path(), ec)) {
            if (f.is_regular_file(ec)) {
                b.bytes += f.file_size(ec);
                b.files++;
            }
        }
        g_backups.push_back(b);
    }
    // Named by date and time, so newest first is reverse order
    std::sort(g_backups.begin(), g_backups.end(),
              [](const SaveBackup &a, const SaveBackup &b) { return a.name > b.name; });
}

void OpenHostFolder(const std::string &path)
{
    std::string url = "file:///" + fs::u8path(path).generic_u8string();
    SDL_OpenURL(url.c_str());
}

void DrawSavesPopup()
{
    const float s = g_viewport_mgr.m_scale;
    if (g_open_saves) {
        g_open_saves = false;
        ListBackups();
        ImGui::OpenPopup(trid("Game saves"));
    }
    ImGui::SetNextWindowSize(ImVec2(560 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(trid("Game saves"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushTextWrapPos(540 * s);
    ImGui::TextUnformatted(tr("Copies the Xbox's game saves (E:\\UDATA and E:\\TDATA) to "
                              "a folder on your PC, so you can get them back if a save "
                              "or the hard disk image goes wrong."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * s));
    bool busy = XboxExplorerBusy();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr(ICON_FA_FLOPPY_DISK "  Back up now"))) {
        char stamp[32];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", tm ? tm : gmtime(&now));
        std::string err;
        std::string dest = (fs::u8path(SaveBackupDir()) / stamp).u8string();
        if (!XboxExplorerBackupSaves(dest, &err)) {
            SetStatus(err, true);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FOLDER_OPEN "  Open backups folder"))) {
        std::error_code ec;
        fs::create_directories(fs::u8path(SaveBackupDir()), ec);
        OpenHostFolder(SaveBackupDir());
    }
    // Progress and the result of the last backup or restore
    bool status_err = false;
    std::string status = XboxExplorerStatus(&status_err);
    if (busy) {
        ImGui::ProgressBar(XboxExplorerProgress(), ImVec2(-FLT_MIN, 0), status.c_str());
    } else if (!status.empty()) {
        ImGui::TextColored(status_err ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.6f, 0.9f, 0.5f, 1),
                           "%s", status.c_str());
    }
    static bool was_busy = false;
    if (was_busy && !busy) {
        ListBackups(); // a backup just finished
    }
    was_busy = busy;

    ImGui::Separator();
    if (g_backups.empty()) {
        ImGui::TextDisabled(tr("No backups yet."));
    } else {
        ImGui::TextDisabled(tr("Backups (newest first)"));
        float rows = ImGui::GetFrameHeightWithSpacing() * g_backups.size();
        ImGui::BeginChild("##backups", ImVec2(0, std::min(260 * s, rows + 4 * s)),
                          ImGuiChildFlags_NavFlattened);
        for (auto &b : g_backups) {
            ImGui::PushID(b.path.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(tr("%d files, %s"), b.files, FormatSize(b.bytes).c_str());
            float bw = ImGui::CalcTextSize(tr("Restore")).x + ImGui::CalcTextSize(tr("Show")).x +
                       ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Restore"))) {
                g_restore_path = b.path;
                ImGui::OpenPopup("##restore-saves");
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Show"))) {
                OpenHostFolder(b.path);
            }
            if (ImGui::BeginPopupModal("##restore-saves", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text(tr("Put back the game saves from %s?"),
                            fs::u8path(g_restore_path).filename().u8string().c_str());
                ImGui::PushTextWrapPos(460 * s);
                ImGui::TextDisabled("%s", tr("The saves on the Xbox are replaced by the "
                                             "backup: anything saved since then is lost. "
                                             "Back up first if you're not sure. The Xbox "
                                             "pauses and restarts, so save your game "
                                             "first."));
                ImGui::PopTextWrapPos();
                if (ImGui::Button(tr("Restore"))) {
                    std::string err;
                    if (!XboxExplorerRestoreSaves(g_restore_path, &err)) {
                        SetStatus(err, true);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel")) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();
    if (ImGui::Button(tr("Close")) ||
        (!ImGui::IsPopupOpen("##restore-saves", ImGuiPopupFlags_AnyPopupId) &&
         ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawPopups()
{
    DrawSavesPopup();
    std::string name;
    if (NamePopup("##xdrive-newfolder", tr("New folder name:"), tr("Create"), &name)) {
        std::string path = XboxJoin(g_path, name);
        char vol = g_vol;
        SimpleWrite(tr("New folder"), [vol, path](std::string *err) {
            return g_drive.MakeDir(vol, path, err);
        });
    }
    if (NamePopup("##xdrive-rename", tr("New name:"), tr("Rename"), &name)) {
        const XboxFileInfo *fi = Selected();
        if (fi && fi->name != name) {
            std::string from = XboxJoin(g_path, fi->name);
            std::string to = XboxJoin(g_path, name);
            char vol = g_vol;
            SimpleWrite(tr("Rename"), [vol, from, to](std::string *err) {
                return g_drive.Rename(vol, from, to, err);
            });
        }
    }
    if (ImGui::BeginPopupModal("##xdrive-delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const XboxFileInfo *fi = Selected();
        ImGui::Text(tr("Delete \"%s\"%s from %c:?"), fi ? fi->name.c_str() : "",
                    fi && fi->dir ? tr(" and everything in it") : "", g_vol);
        ImGui::TextDisabled(tr("This can't be undone. The Xbox pauses and "
                            "restarts to make the change."));
        if (ImGui::Button(tr(ICON_FA_TRASH " Delete"))) {
            ImGui::CloseCurrentPopup();
            DeleteSelected();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (g_pending_write && !ImGui::IsPopupOpen("##xdrive-confirm-write")) {
        ImGui::OpenPopup("##xdrive-confirm-write");
    }
    if (ImGui::BeginPopupModal("##xdrive-confirm-write", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", g_pending_write ? g_pending_write->title.c_str() : "");
        if (g_pending_write) {
            ImGui::TextDisabled("%s", FormatSize(g_pending_write->total).c_str());
        }
        ImGui::Spacing();
        ImGui::TextWrapped(tr("The Xbox pauses while the files are written and "
                           "restarts when they're done. Progress in a running "
                           "game that hasn't been saved will be lost."));
        // Waits for a job that started while a file dialog was open
        ImGui::BeginDisabled(g_job != nullptr);
        if (ImGui::Button(tr("Copy to the Xbox"))) {
            ImGui::CloseCurrentPopup();
            if (g_pending_write) {
                StartJob(std::move(g_pending_write));
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || CancelPressed()) {
            g_pending_write.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

void XboxExplorerUpdate(void)
{
    if (XboxTakeResumeRefused()) {
        xemu_queue_notification(
            tr("The Xbox stays paused until the copy to its hard drive is done"));
    }
    if (g_drive.IsOpen() && XboxSnapshotLoads() != g_drive_loads) {
        // A snapshot load reverted the disk under the open drive
        if (g_job) {
            AbandonJob();
        } else {
            g_drive.Discard();
            g_open_tried = 0;
            g_list_dirty = true;
        }
    }
    if (!g_job) {
        return;
    }
    if (g_job->writes && XboxIsRunning()) {
        XboxPause(); // resumed past XboxJobBegin's hold: stop it again
    }
    // Work per frame, holding the BQL: about 12 ms while the Xbox is paused,
    // 2 ms while it runs (a read job) so it keeps its frame rate. The step
    // limit only matters if the clock is coarse.
    uint64_t budget_ns = (g_job->writes || !XboxIsRunning()) ? 12000000 : 2000000;
    uint64_t start = SDL_GetTicksNS();
    for (int n = 0; n < 256 && SDL_GetTicksNS() - start < budget_ns; n++) {
        if (!RunStep(*g_job)) {
            FinishJob();
            return;
        }
    }
}

bool XboxExplorerBusy(void)
{
    return g_job != nullptr;
}

// ---- Game save backups -------------------------------------------------------

namespace
{
const char *const kSaveFolders[] = { "UDATA", "TDATA" };

bool SameNoCase(const std::string &a, const char *b)
{
    size_t n = strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

// The name a folder has on E: ("UDATA", "udata"...), or ""
std::string SaveFolderOnXbox(const std::vector<XboxFileInfo> &root, const char *want)
{
    for (auto &f : root) {
        if (f.dir && SameNoCase(f.name, want)) {
            return f.name;
        }
    }
    return "";
}
} // namespace

bool XboxExplorerBackupSaves(const std::string &host_dir, std::string *err)
{
    if (g_job) {
        *err = tr("Another copy is still running");
        return false;
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
        return false;
    }
    std::vector<XboxFileInfo> root;
    if (!g_drive.List('E', "/", &root, err)) {
        return false;
    }
    auto job = std::make_unique<Job>();
    job->vol = 'E';
    for (const char *want : kSaveFolders) {
        std::string name = SaveFolderOnXbox(root, want);
        if (name.empty()) {
            continue;
        }
        XboxFileInfo fi;
        for (auto &f : root) {
            if (f.name == name) {
                fi = f;
            }
        }
        if (!XboxToHostSteps('E', XboxJoin("/", name), fi, fs::u8path(host_dir), job.get(),
                             err)) {
            return false;
        }
    }
    if (job->steps.empty()) {
        *err = tr("There are no game saves on this Xbox hard drive yet");
        return false;
    }
    // Made only once there is something to copy, so a failed plan leaves
    // no empty backup behind
    std::error_code ec;
    fs::create_directories(fs::u8path(host_dir), ec);
    if (ec) {
        *err = string_format(tr("Can't make the folder %s"), host_dir.c_str());
        return false;
    }
    job->title = tr("Backing up game saves");
    StartJob(std::move(job));
    return true;
}

bool XboxExplorerRestoreSaves(const std::string &host_dir, std::string *err)
{
    if (g_job) {
        *err = tr("Another copy is still running");
        return false;
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
        return false;
    }
    if (!XboxDiskEmulatedWritable()) {
        *err = tr("The hard disk image is read-only");
        return false;
    }
    std::vector<XboxFileInfo> root;
    if (!g_drive.List('E', "/", &root, err)) {
        return false;
    }
    // A backup with no files in it would only wipe the saves: refuse
    std::error_code ec;
    int files = 0;
    for (auto &e : fs::recursive_directory_iterator(fs::u8path(host_dir), ec)) {
        files += e.is_regular_file(ec) ? 1 : 0;
    }
    if (!files) {
        *err = tr("This backup has no game saves in it");
        return false;
    }
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = 'E';
    for (const char *want : kSaveFolders) {
        fs::path src;
        for (auto &e : fs::directory_iterator(fs::u8path(host_dir), ec)) {
            if (e.is_directory(ec) &&
                SameNoCase(e.path().filename().u8string(), want)) {
                src = e.path();
            }
        }
        if (src.empty()) {
            continue;
        }
        // The folder on the Xbox goes first, then the backup's copy
        std::string name = SaveFolderOnXbox(root, want);
        if (!name.empty()) {
            XboxFileInfo fi;
            for (auto &f : root) {
                if (f.name == name) {
                    fi = f;
                }
            }
            if (!DeleteSteps('E', XboxJoin("/", name), fi, job.get(), err)) {
                return false;
            }
        }
        if (!HostToXboxSteps(src, "/", job.get(), err)) {
            return false;
        }
    }
    if (job->steps.empty()) {
        *err = tr("This backup has no game saves in it");
        return false;
    }
    job->title = tr("Restoring game saves");
    StartJob(std::move(job));
    if (!g_job) {
        *err = g_status;
        return false;
    }
    return true;
}

std::string XboxExplorerStatus(bool *is_error)
{
    if (is_error) {
        *is_error = g_status_error;
    }
    return g_status;
}

float XboxExplorerProgress(void)
{
    if (!g_job || !g_job->total) {
        return 0;
    }
    return std::min(1.0f, (float)((double)g_job->done / g_job->total));
}

void XboxExplorerDraw(void)
{
    const float s = g_viewport_mgr.m_scale;
    ImGui::PushID("xbox-drive");
    if (!EnsureOpen()) {
        ImGui::Dummy(ImVec2(0, 20 * s));
        ImGui::TextUnformatted(tr(ICON_FA_HARD_DRIVE "  Xbox hard drive"));
        ImGui::TextDisabled("%s", g_open_err.empty() ? tr("Opening...") : g_open_err.c_str());
        if (ImGui::Button(tr("Try again"))) {
            g_open_tried = 0;
        }
        ImGui::PopID();
        return;
    }
    // Free space of the current drive, counted once (kept across reopens
    // until a write or Refresh)
    const XboxVolume *v = g_drive.Volume(g_vol);
    if (v && !v->free_known && !g_job && ImGui::GetFrameCount() > 1) {
        g_drive.UpdateFreeSpace(g_vol);
    }
    DrawVolumes();
    DrawToolbar();
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    DrawList(ImGui::GetContentRegionAvail().y - footer);
    DrawFooter();
    DrawPopups();
    ImGui::PopID();
}

void XboxExplorerOpenWindow(void)
{
    g_window_open = true;
    ImGui::SetWindowFocus(trid("Xbox Hard Drive"));
}

void XboxExplorerWindowDraw(void)
{
    if (!g_window_open) {
        // Closed: let go of the drive, under the BQL (a job keeps it open
        // until it ends)
        if (g_drive.IsOpen() && !g_job) {
            g_drive.Close();
            g_open_tried = 0;
        }
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(820 * s, 560 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(trid("Xbox Hard Drive"), &g_window_open)) {
        XboxExplorerDraw();
    }
    ImGui::End();
}
