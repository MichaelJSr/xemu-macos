#!/usr/bin/env python3
"""Make a harness scratch config's pad topology deterministic.

    harness-pads.py <scratch xemu.toml>

Edits the scratch config in place and prints the extra xemu arguments,
one per line, for the caller to append to its command line.

A savestate stores the USB pad tree that was attached when it was saved
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
lines = open(path, errors="replace").readlines()

section = None
bindings = {}
drivers = {}
for line in lines:
    m = re.match(r"\s*\[([^\]]+)\]", line)
    if m:
        section = m.group(1).strip()
        continue
    if section != "input.bindings":
        continue
    m = re.match(r"\s*port([1-4])(_driver)?\s*=\s*'([^']*)'", line)
    if m:
        (drivers if m.group(2) else bindings)[int(m.group(1))] = m.group(3)

stub = [p for p in (1, 2) if bindings.get(p) != "keyboard"]

out = []
section = None
saw_auto_bind = False
for line in lines:
    m = re.match(r"\s*\[([^\]]+)\]", line)
    if m:
        if section == "input" and not saw_auto_bind:
            out.append("auto_bind = false\n")
            saw_auto_bind = True
        section = m.group(1).strip()
        out.append(line)
        continue
    if section == "input" and re.match(r"\s*auto_bind\s*=", line):
        out.append("auto_bind = false\n")
        saw_auto_bind = True
        continue
    m = re.match(r"\s*port([1-4])\s*=", line)
    if section == "input.bindings" and m and int(m.group(1)) in stub:
        continue
    out.append(line)
if not saw_auto_bind:
    out.append("\n[input]\nauto_bind = false\n" if section != "input"
               else "auto_bind = false\n")
open(path, "w").writelines(out)

for p in stub:
    hub = PORT_MAP[p]
    driver = drivers.get(p, "usb-xbox-gamepad")
    print("-device")
    print("usb-hub,port=1.%d,ports=3" % hub)
    print("-device")
    print("%s,port=1.%d.1,index=%d" % (driver, hub, p - 1))
