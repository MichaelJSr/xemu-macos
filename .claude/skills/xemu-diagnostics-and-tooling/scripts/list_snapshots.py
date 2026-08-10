#!/usr/bin/env python3
"""list_snapshots.py — list QEMU savestates (snapshots) inside a qcow2 image
without qemu-img and without taking the image's write lock.

xemu stores savestates inside the game hdd qcow2 (e.g.
~/Documents/Xemu/xbox_hdd.qcow2). qemu-img also lists them, but a RUNNING
xemu holds the image's write lock and qemu-img then refuses to open it.
This script only reads bytes, so it is always safe — but on an image that is
being actively written, clone first (APFS instant copy):

    cp -c /path/to/xbox_hdd.qcow2 /tmp/snap-list.qcow2
    list_snapshots.py /tmp/snap-list.qcow2

qcow2 layout parsed (all integers big-endian):
  header offset  0: magic "QFI\\xfb", offset 4: version (2 or 3)
  header offset 60: nb_snapshots (u32)
  header offset 64: snapshots_offset (u64)
  each table entry, 8-byte aligned file offset:
    40-byte fixed struct '>QIHHIIQII' =
      l1_table_offset u64, l1_size u32, id_str_size u16, name_size u16,
      date_sec u32, date_nsec u32, vm_clock_nsec u64,
      vm_state_size u32 (legacy 32-bit), extra_data_size u32
    then: extra_data (extra_data_size bytes; if >=8 its first u64 is the
          64-bit vm_state_size that supersedes the legacy field; if >=16
          the next u64 is the virtual disk size at snapshot time)
    then: id_str (id_str_size bytes), name (name_size bytes)

Usage:
    list_snapshots.py IMAGE.qcow2          # human table (qemu-img-like)
    list_snapshots.py IMAGE.qcow2 --json   # machine-readable

stdlib only. Read-only: opens the file with mode 'rb' and never writes.
"""

import argparse
import datetime
import json
import struct
import sys

QCOW_MAGIC = b"QFI\xfb"
FIXED_FMT = ">QIHHIIQII"
FIXED_LEN = struct.calcsize(FIXED_FMT)  # 40


def human_size(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def vm_clock_str(nsec):
    total = nsec // 1_000_000_000
    h, rem = divmod(total, 3600)
    m, s = divmod(rem, 60)
    ms = (nsec % 1_000_000_000) // 1_000_000
    return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"


def read_snapshots(path):
    with open(path, "rb") as f:
        hdr = f.read(72)
        if len(hdr) < 72 or hdr[0:4] != QCOW_MAGIC:
            sys.exit(f"error: {path} is not a qcow2 image (bad magic)")
        version = struct.unpack(">I", hdr[4:8])[0]
        nb_snapshots = struct.unpack(">I", hdr[60:64])[0]
        snapshots_offset = struct.unpack(">Q", hdr[64:72])[0]

        snaps = []
        pos = snapshots_offset
        for i in range(nb_snapshots):
            pos = (pos + 7) & ~7  # entries start 8-byte aligned
            f.seek(pos)
            raw = f.read(FIXED_LEN)
            if len(raw) < FIXED_LEN:
                sys.exit(f"error: truncated snapshot table at entry {i}")
            (l1_table_offset, l1_size, id_str_size, name_size,
             date_sec, date_nsec, vm_clock_nsec, vm_state_size32,
             extra_data_size) = struct.unpack(FIXED_FMT, raw)
            extra = f.read(extra_data_size)
            id_str = f.read(id_str_size).decode("utf-8", "replace")
            name = f.read(name_size).decode("utf-8", "replace")
            pos = f.tell()

            vm_state_size = vm_state_size32
            disk_size = None
            if extra_data_size >= 8:
                vm_state_size = struct.unpack(">Q", extra[0:8])[0]
            if extra_data_size >= 16:
                disk_size = struct.unpack(">Q", extra[8:16])[0]

            snaps.append({
                "id": id_str,
                "name": name,
                "vm_state_size": vm_state_size,
                "date_sec": date_sec,
                "date_nsec": date_nsec,
                "vm_clock_nsec": vm_clock_nsec,
                "disk_size": disk_size,
                "l1_table_offset": l1_table_offset,
                "l1_size": l1_size,
            })
        return version, nb_snapshots, snaps


def main():
    ap = argparse.ArgumentParser(
        description="List QEMU snapshots (savestates) in a qcow2 image, "
                    "read-only, without qemu-img.")
    ap.add_argument("image", help="qcow2 image path")
    ap.add_argument("--json", action="store_true", help="JSON output")
    args = ap.parse_args()

    version, nb, snaps = read_snapshots(args.image)
    if args.json:
        json.dump({"qcow2_version": version, "nb_snapshots": nb,
                   "snapshots": snaps}, sys.stdout, indent=2)
        print()
        return

    print(f"{args.image}: qcow2 v{version}, {nb} snapshot(s)")
    if not snaps:
        return
    idw = max(2, max(len(s["id"]) for s in snaps))
    namew = max(4, max(len(s["name"]) for s in snaps))
    print(f"{'ID':<{idw}}  {'NAME':<{namew}}  {'VMSTATE':>10}  "
          f"{'DATE':<19}  {'VMCLOCK':>12}")
    for s in snaps:
        date = datetime.datetime.fromtimestamp(s["date_sec"]).strftime(
            "%Y-%m-%d %H:%M:%S")
        print(f"{s['id']:<{idw}}  {s['name']:<{namew}}  "
              f"{human_size(s['vm_state_size']):>10}  {date:<19}  "
              f"{vm_clock_str(s['vm_clock_nsec']):>12}")


if __name__ == "__main__":
    main()
