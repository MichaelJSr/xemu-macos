//
// xemu User Interface - the emulated hard disk as an XboxDiskIO
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xbox-drive-qemu.hh"
#include "xbox-hdd-blk.h"

namespace
{
class QemuDiskIO : public XboxDiskIO
{
public:
    bool Read(uint64_t offset, void *buf, size_t len) override
    {
        return xemu_hdd_read(offset, buf, len);
    }
    bool Write(uint64_t offset, const void *buf, size_t len) override
    {
        return xemu_hdd_write(offset, buf, len);
    }
    uint64_t Size() override
    {
        return xemu_hdd_size();
    }
    void Flush() override
    {
        xemu_hdd_flush();
    }
};
} // namespace

std::unique_ptr<XboxDiskIO> XboxDiskOpenEmulated(std::string *err)
{
    if (!xemu_hdd_available()) {
        *err = "No hard disk image is loaded (Settings > System > Hard Disk)";
        return nullptr;
    }
    return std::make_unique<QemuDiskIO>();
}

bool XboxDiskEmulatedWritable(void)
{
    return xemu_hdd_writable();
}

bool XboxIsRunning(void)
{
    return xemu_xbox_running();
}

void XboxPause(void)
{
    xemu_xbox_pause();
}

void XboxResume(void)
{
    xemu_xbox_resume();
}

void XboxRestart(bool resume)
{
    xemu_xbox_restart(resume);
}

uint64_t XboxDiskWriteCount(void)
{
    return xemu_hdd_write_count();
}

void XboxJobBegin(void)
{
    xemu_hdd_job_begin();
}

void XboxJobEnd(void)
{
    xemu_hdd_job_end();
}

bool XboxTakeResumeRefused(void)
{
    return xemu_hdd_take_resume_refused();
}

unsigned XboxSnapshotLoads(void)
{
    return xemu_hdd_snapshot_loads();
}

void XboxOnShutdown(void (*fn)(void))
{
    xemu_hdd_on_shutdown(fn);
}
