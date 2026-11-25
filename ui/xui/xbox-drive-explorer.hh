//
// xemu User Interface - Xbox hard drive explorer
//
// Browse the emulated Xbox hard drive (C, E, F, X, Y, Z) and copy files and
// folders in and out, create folders, rename and delete. Shown as a window
// (Machine > Xbox Hard Drive).
//
// Writing changes the Xbox's file system underneath it, and the Xbox caches
// its file system, so writes happen while the Xbox is paused and it restarts
// when they are done.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

// Draw the explorer into the current window
void XboxExplorerDraw(void);
// Run copy/delete jobs a little each frame (call every frame)
void XboxExplorerUpdate(void);
// The standalone window
void XboxExplorerWindowDraw(void);
void XboxExplorerOpenWindow(void);
bool XboxExplorerBusy(void);

// Game saves: copy E:\UDATA and E:\TDATA to host_dir (a new folder; the
// Xbox keeps running), or put a backup back (replacing those folders; the
// Xbox pauses and restarts). False with *err if it can't start.
bool XboxExplorerBackupSaves(const std::string &host_dir, std::string *err);
bool XboxExplorerRestoreSaves(const std::string &host_dir, std::string *err);

// Last status line ("Copying...", "... - done.", an error), and progress 0-1
std::string XboxExplorerStatus(bool *is_error);
float XboxExplorerProgress(void);
