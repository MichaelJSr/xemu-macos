//
// xemu User Interface - Xbox hard drive access through QEMU's block layer
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include "xbox-drive.hh"

// The emulated Xbox's hard disk image
std::unique_ptr<XboxDiskIO> XboxDiskOpenEmulated(std::string *err);
bool XboxDiskEmulatedWritable(void);

// The Xbox caches its file system, so changes are made while it is paused
// and it is restarted afterwards
bool XboxIsRunning(void);
void XboxPause(void);
void XboxResume(void);
// Reset the Xbox (while stopped), then start it if resume
void XboxRestart(bool resume);
// Disk writes so far: whether an operation changed the disk
uint64_t XboxDiskWriteCount(void);

// Around a write job that runs over several frames: snapshot saves are
// refused and a resume is undone before the Xbox runs
void XboxJobBegin(void);
void XboxJobEnd(void);
// Whether a resume was undone since the last call
bool XboxTakeResumeRefused(void);
// Snapshot loads so far (each reverts the disk under an open drive)
unsigned XboxSnapshotLoads(void);
// fn runs at shutdown, while the disk can still be written
void XboxOnShutdown(void (*fn)(void));
