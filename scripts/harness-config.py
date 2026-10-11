#!/usr/bin/env python3
"""Finish a harness's scratch xemu.toml: mute it and pin the pad topology.

    harness-config.py <scratch xemu.toml> [--no-pad-stubs]

Edits the scratch config in place and prints the extra xemu arguments,
one per line, for the caller to append to its command line. Only ever
point it at a per-run copy, never at ../fixtures/xemu.toml or the user's
config.

Audio: sets [audio] volume_limit = 0. Harness runs play on the owner's
machine while they work (owner request, 2026-10-11); the guest's APU
and DSP still run in full, only the host output is silenced.

Pads (skipped with --no-pad-stubs, for harnesses that never loadvm):
a savestate stores the USB pad tree that was attached when it was saved
(a hub at 1.P plus a gamepad at 1.P.1 for each bound player, P from
ui/xemu-input.c's port_map {3,4,1,2}), and loadvm fails with
"Unknown section or instance 'pci.0:02.0/1.3/usb-hub'" when the running
machine lacks one. xemu only builds a player's pad while the controller
bound to that port is connected, so a tag saved with a physical pad on
player 1 (every azurik-* tag: player 1 = the owner's pad, player 2 =
the keyboard) cannot load in a harness run while that pad is off.

For each of players 1 and 2 that is not bound to the keyboard, this
drops the binding and stubs the pad with -device args instead: an
unbound pad reports neutral input (hw/xbox/xid.c). It also sets
[input] auto_bind = false. Without that, a pad connecting mid-run would
bind to the stubbed port and xemu_input_bind() would abort on the
occupied USB port. A stub on a tag saved without that player (the
vm-2026070* tags carry only player 2) is a device the stream does not
mention, which loadvm tolerates.
"""
import re
import sys

PORT_MAP = {1: 3, 2: 4, 3: 1, 4: 2}  # player -> hub port, ui/xemu-input.c

path = sys.argv[1]
pad_stubs = "--no-pad-stubs" not in sys.argv[2:]
lines = open(path, errors="replace").readlines()


def header(line):
    m = re.match(r"\s*\[([^\]]+)\]", line)
    return m.group(1).strip() if m else None


bindings = {}
drivers = {}
section = None
for line in lines:
    if header(line):
        section = header(line)
        continue
    m = re.match(r"\s*port([1-4])(_driver)?\s*=\s*'([^']*)'", line)
    if section == "input.bindings" and m:
        (drivers if m.group(2) else bindings)[int(m.group(1))] = m.group(3)

stub = [p for p in (1, 2) if pad_stubs and bindings.get(p) != "keyboard"]

# (table, key) -> value to force; each lands in its table, which is
# created at the end of the file if the config has none.
force = {("audio", "volume_limit"): "0"}
if pad_stubs:
    force[("input", "auto_bind")] = "false"
done = set()


def flush(table, out):
    for (t, k), v in force.items():
        if t == table and (t, k) not in done:
            out.append("%s = %s\n" % (k, v))
            done.add((t, k))


out = []
section = None
for line in lines:
    if header(line):
        flush(section, out)
        section = header(line)
        out.append(line)
        continue
    m = re.match(r"\s*([A-Za-z0-9_]+)\s*=", line)
    if m and (section, m.group(1)) in force:
        if (section, m.group(1)) not in done:
            out.append("%s = %s\n" % (m.group(1), force[(section, m.group(1))]))
            done.add((section, m.group(1)))
        continue
    m = re.match(r"\s*port([1-4])\s*=", line)
    if section == "input.bindings" and m and int(m.group(1)) in stub:
        continue
    out.append(line)
flush(section, out)
for table in sorted({t for (t, k) in force if (t, k) not in done}):
    out.append("\n[%s]\n" % table)
    flush(table, out)
open(path, "w").writelines(out)

for p in stub:
    hub = PORT_MAP[p]
    driver = drivers.get(p, "usb-xbox-gamepad")
    print("-device")
    print("usb-hub,port=1.%d,ports=3" % hub)
    print("-device")
    print("%s,port=1.%d.1,index=%d" % (driver, hub, p - 1))
