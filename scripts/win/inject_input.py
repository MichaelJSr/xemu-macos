#!/usr/bin/env python3
"""inject_input.py - drive xemu's XEMU_INPUT_PIPE test-input channel.

Windows client for the same line protocol the POSIX helper
(.claude/skills/xemu-diagnostics-and-tooling/scripts/inject_input.sh)
speaks: "down <sdl_scancode>", "up <sdl_scancode>", "clear".

Transport. On Windows the channel is a Win32 named pipe created by
xemu itself (ui/xemu-input.c, _WIN32 arm): xemu is the SERVER, this
script is a one-shot client that opens the pipe, writes its lines and
closes. Set XEMU_INPUT_PIPE before launching xemu; the value is either
a full pipe name (\\\\.\\pipe\\xemu) or any path whose last component
becomes the pipe name, so the same env value works on macOS/Linux
(where it names a FIFO) and here. Only one client may be attached at a
time, so a busy pipe is retried for a couple of seconds.

On POSIX this script writes to the FIFO instead, so a cross-platform
harness can call one client everywhere. (The macOS scripts keep using
inject_input.sh; nothing about them changes.)

Injected keys are OR'd into the SDL keyboard state and flow through the
user's [input.keyboard_controller_scancode_map] bindings, so they work
with the xemu window unfocused - the property the movement/streaming
probe class depends on. That also means the guest must have the keyboard
bound as a controller ([input.bindings] port2 = 'keyboard'): the pipe is
polled from xemu_input_update_sdl_kbd_controller_state, so with no
keyboard controller bound nothing reads the pipe.

Usage:
    inject_input.py keys
    inject_input.py create <pipe>                 (POSIX: mkfifo; Windows: no-op)
    inject_input.py down   <pipe> <key>
    inject_input.py up     <pipe> <key>
    inject_input.py tap    <pipe> <key> [hold_ms]     default 80 ms
    inject_input.py hold   <pipe> <key> <hold_ms>     alias of tap
    inject_input.py clear  <pipe>
    inject_input.py send   <pipe> "<raw line>" [...]  escape hatch

<key> is a name from the table (case-insensitive) or a raw SDL scancode
integer 0-511.

Example (Windows, from a second shell while xemu runs):
    set XEMU_INPUT_PIPE=xemu-probe            (before launching xemu)
    python scripts/win/inject_input.py tap  \\\\.\\pipe\\xemu-probe W 200
    python scripts/win/inject_input.py down \\\\.\\pipe\\xemu-probe W
    python scripts/win/inject_input.py clear \\\\.\\pipe\\xemu-probe

stdlib only, Python >= 3.8.
"""

import os
import sys
import time

# SDL scancodes; values verified against SDL3/SDL_scancode.h (same table
# as inject_input.sh). The in-game meaning depends on the user's
# [input.keyboard_controller_scancode_map]; the fork default map is in
# the right column.
KEYS = {
    "W": (26, "left-stick up"),
    "A": (4, "left-stick left"),
    "S": (22, "left-stick down"),
    "D": (7, "left-stick right"),
    "UP": (82, "(unbound by default)"),
    "DOWN": (81, "A button"),
    "LEFT": (80, "X button"),
    "RIGHT": (79, "B button"),
    "SPACE": (44, "Y button"),
    "BACKSPACE": (42, "Start"),
    "KP1": (89, "right-stick left"),
    "KP2": (90, "right-stick down"),
    "KP3": (91, "right-stick right"),
    "KP5": (93, "right-stick up"),
    "LSHIFT": (225, "left trigger"),
    "RSHIFT": (229, "right trigger"),
}

RETRY_SECS = 3.0


def scancode(key):
    name = str(key).upper()
    if name in KEYS:
        return KEYS[name][0]
    try:
        sc = int(key)
    except ValueError:
        sys.exit("error: unknown key %r (run: inject_input.py keys)" % key)
    if not 0 <= sc <= 511:
        sys.exit("error: scancode %d out of range 0-511" % sc)
    return sc


def pipe_name(path):
    """Mirror ui/xemu-input.c's normalisation: a value already naming a
    pipe is used verbatim, otherwise the last path component becomes the
    pipe name."""
    if os.name != "nt":
        return path
    if path.startswith("\\\\"):
        return path
    base = path
    for sep in ("/", "\\", ":"):
        base = base.rsplit(sep, 1)[-1]
    if not base:
        sys.exit("error: XEMU_INPUT_PIPE value %r has no pipe name" % path)
    return "\\\\.\\pipe\\" + base


class Channel(object):
    """One open write session. Windows: a named-pipe client connection;
    POSIX: the FIFO opened for writing."""

    def __init__(self, path):
        self.name = pipe_name(path)
        self.f = None
        deadline = time.time() + RETRY_SECS
        last = None
        while True:
            try:
                self.f = open(self.name, "wb", buffering=0)
                return
            except OSError as e:
                # 2/ENOENT: xemu not up yet (or launched without the env
                # var). 231 ERROR_PIPE_BUSY: another client still owns the
                # single instance, or xemu has not recycled it yet - it
                # does so on its next input poll (~1 frame).
                last = e
                if time.time() >= deadline:
                    break
                time.sleep(0.05)
        sys.exit("error: cannot open %s: %s\n"
                 "       (is xemu running with XEMU_INPUT_PIPE set?)"
                 % (self.name, last))

    def send(self, line):
        try:
            self.f.write((line + "\n").encode("ascii"))
            self.f.flush()
        except OSError as e:
            # xemu exited (or never bound a keyboard controller, so it never
            # polls): report it instead of a traceback. A dead FIFO reader
            # on POSIX blocks forever - here the write just fails.
            sys.exit("error: write to %s failed: %s (is xemu still running?)"
                     % (self.name, e))

    def close(self):
        if self.f is not None:
            self.f.close()
            self.f = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def cmd_keys():
    print("name       scancode  (fork default map)")
    for name, (sc, meaning) in KEYS.items():
        print("%-10s %-9d %s" % (name, sc, meaning))
    return 0


def cmd_create(path):
    if os.name == "nt":
        print("note: on Windows xemu creates the pipe itself (%s); "
              "nothing to do" % pipe_name(path))
        return 0
    if os.path.exists(path):
        print("already exists: %s" % path)
        return 0
    os.mkfifo(path, 0o600)
    print("created: %s" % path)
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd = argv[1]
    if cmd == "keys":
        return cmd_keys()
    if len(argv) < 3:
        sys.exit("error: %s needs a pipe path (run: inject_input.py --help)"
                 % cmd)
    path = argv[2]
    if cmd == "create":
        return cmd_create(path)
    if cmd == "clear":
        with Channel(path) as ch:
            ch.send("clear")
        return 0
    if cmd == "send":
        with Channel(path) as ch:
            for line in argv[3:]:
                ch.send(line)
        return 0
    if cmd in ("down", "up"):
        if len(argv) < 4:
            sys.exit("error: %s needs a key" % cmd)
        with Channel(path) as ch:
            ch.send("%s %d" % (cmd, scancode(argv[3])))
        return 0
    if cmd in ("tap", "hold"):
        if len(argv) < 4:
            sys.exit("error: %s needs a key" % cmd)
        sc = scancode(argv[3])
        hold_ms = float(argv[4]) if len(argv) > 4 else 80.0
        # One session for the whole tap: fewer connect/recycle cycles for
        # xemu's single pipe instance, and the release cannot be lost to a
        # busy-retry timeout.
        with Channel(path) as ch:
            ch.send("down %d" % sc)
            time.sleep(hold_ms / 1000.0)
            ch.send("up %d" % sc)
        return 0
    sys.exit("error: unknown command %r (run: inject_input.py --help)" % cmd)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
