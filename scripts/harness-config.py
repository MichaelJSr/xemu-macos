#!/usr/bin/env python3
"""Finish a harness's scratch xemu.toml: mute it, pin the pad topology and,
with --workdir, point it at the run's own files.

    harness-config.py <scratch xemu.toml> [--no-pad-stubs]
                      [--workdir D [--hdd H] [--eeprom E] [--dvd ISO] [--fullscreen]]

Edits the scratch config in place and prints the extra xemu arguments,
one per line, for the caller to append to its command line (stdout holds
nothing else; diagnostics go to stderr). Only ever point it at a per-run
copy, never at ../fixtures/xemu.toml or the user's config.

Audio: sets [audio] volume_limit = 0. Harness runs play on the owner's
machine while they work (owner request, 2026-10-11); the guest's APU
and DSP still run in full, only the host output is silenced.

--workdir D (the run's work dir; the toml must lie inside it, and must not
be a source config: $XEMU_TEST_CONFIG, ../fixtures/xemu.toml,
$AZURIK_FX/xemu.toml or the user's) also
writes, each in its own table (added when the config lacks it):
  [general]        screenshot_dir = D (the fixture's names the owner's
                   pictures folder: an F12 or a menu screenshot during a
                   bench landed there)
  [display.window] fullscreen_on_startup = false (true with --fullscreen)
  [sys.files]      hdd_path = H, eeprom_path = E, dvd_path = ISO, each
                   only when given
It never clones: the caller passes the clones it made (cp -c) and keeps
the source paths for its receipts.

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
bind to the stubbed port and drive the stub. A stub on a tag saved
without that player (the vm-2026070* tags carry only player 2) is a
device the stream does not mention, which loadvm tolerates.
"""
import argparse
import json
import os
import re
import sys

PORT_MAP = {1: 3, 2: 4, 3: 1, 4: 2}  # player -> hub port, ui/xemu-input.c
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
p.add_argument('toml')
p.add_argument('--no-pad-stubs', action='store_true')
p.add_argument('--workdir', default='')
p.add_argument('--hdd', default='')
p.add_argument('--eeprom', default='')
p.add_argument('--dvd', default='')
p.add_argument('--fullscreen', action='store_true')
a = p.parse_args()
path = a.toml
pad_stubs = not a.no_pad_stubs


def die(msg):
    print('harness-config: ' + msg, file=sys.stderr)
    sys.exit(2)


def quoted(v):
    """A TOML string for v: literal unless it holds a quote."""
    return "'%s'" % v if "'" not in v and '\n' not in v else json.dumps(v)


if not a.workdir and (a.hdd or a.eeprom or a.dvd or a.fullscreen):
    die('--hdd, --eeprom, --dvd and --fullscreen need --workdir')
if a.workdir:
    real, work = os.path.realpath(path), os.path.realpath(a.workdir)
    if not real.startswith(work.rstrip('/') + '/'):
        die('%s is not inside the work dir %s' % (path, a.workdir))
    fx = os.environ.get('AZURIK_FX', '')   # the Modkit's D98 fixtures (bench-det --check d98)
    sources = [os.environ.get('XEMU_TEST_CONFIG', ''),
               os.path.join(os.path.dirname(REPO), 'fixtures', 'xemu.toml'),
               fx and os.path.join(fx, 'xemu.toml'),
               os.path.expanduser('~/Library/Application Support/xemu/xemu/xemu.toml')]
    if any(s and os.path.exists(s) and os.path.samefile(s, real) for s in sources):
        die('%s is a source config, not a scratch copy' % path)

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

# (table, key) -> value to force; each lands in its table (an existing key
# is rewritten in place, a missing one is added at the table's end), which
# is created at the end of the file if the config has none.
force = {("audio", "volume_limit"): "0"}
if pad_stubs:
    force[("input", "auto_bind")] = "false"
if a.workdir:
    force[("general", "screenshot_dir")] = quoted(a.workdir)
    force[("display.window", "fullscreen_on_startup")] = "true" if a.fullscreen else "false"
    for key, v in (("hdd_path", a.hdd), ("eeprom_path", a.eeprom), ("dvd_path", a.dvd)):
        if v:
            force[("sys.files", key)] = quoted(v)
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
