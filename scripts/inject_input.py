#!/usr/bin/env python3
"""inject_input.py - drive xemu's XEMU_INPUT_PIPE test-input channel.

One client for every host. xemu ORs "down <sdl_scancode>",
"up <sdl_scancode>" and "clear" lines into its SDL keyboard state
(ui/xemu-input.c); `send` writes any other line the protocol takes
("axis lx 0.5", "+0 clear", "@<frame> down 81", ...).

Transport.
  POSIX    a FIFO made with `create` before xemu starts with
           XEMU_INPUT_PIPE=<fifo>; xemu opens a non-blocking reader on its
           first input poll and holds it open. Each call opens the FIFO,
           writes its lines and closes. While nobody reads the FIFO (xemu
           still queued for a lease slot or starting, or gone) the open is
           retried for up to 60 s, then reported (exit 1) instead of
           blocking forever.
  Windows  a Win32 named pipe xemu creates itself (ui/xemu-input.c,
           _WIN32 arm): xemu is the server, this is a one-shot client.
           The value is either a full pipe name (\\\\.\\pipe\\xemu) or any
           path whose last component becomes the pipe name, so one
           /tmp/xemu.fifo value works on both hosts. Only one client may
           be attached at a time, so a missing or busy pipe is retried for
           3 s; wait for xemu (QMP, scripts/win/xemu_ctl.py wait) before
           the first call.

Injected keys flow through the user's [input.keyboard_controller_scancode_map]
bindings, so they work with the xemu window unfocused (macOS drops
synthetic key events for background apps). That also means the keyboard
must be bound as a controller ([input.bindings] port2 = 'keyboard').

Usage:
    inject_input.py keys
    inject_input.py create <pipe>                 (POSIX: mkfifo; Windows: no-op)
    inject_input.py down   <pipe> <key>
    inject_input.py up     <pipe> <key>
    inject_input.py tap    <pipe> <key> [hold_ms]     default 80 ms
    inject_input.py hold   <pipe> <key> <hold_ms>     alias of tap
    inject_input.py clear  <pipe>
    inject_input.py send   <pipe> "<raw line>" [...]  any protocol line

<key> is a name from the table (case-insensitive) or a raw SDL scancode
integer 0-511.

Example (macOS; $W holds a bundle clone and a muted scratch config, see
xemu-operate Run):
    scripts/inject_input.py create /tmp/xemu.fifo
    python3 -I scripts/xemu_lease.py run --env XEMU_INPUT_PIPE=/tmp/xemu.fifo \\
        --purpose 'pipe test' -- $W/xemu.app/Contents/MacOS/xemu \\
        -config_path $W/xemu.toml &
    scripts/inject_input.py tap  /tmp/xemu.fifo W 200    # walk forward 200 ms
    scripts/inject_input.py down /tmp/xemu.fifo DOWN     # hold the A button
    scripts/inject_input.py clear /tmp/xemu.fifo
The first tap waits for xemu's reader (up to 60 s). If the lease may
queue longer ("slots busy; waiting"), wait for xemu to be up first.
Windows: XEMU_INPUT_PIPE=xemu-probe before launching, then
    py -3 scripts\\inject_input.py tap \\\\.\\pipe\\xemu-probe W 200

stdlib only, Python >= 3.8.
"""

import os
import stat
import sys
import time

# SDL scancodes; values verified against SDL3/SDL_scancode.h. The in-game
# meaning depends on the user's [input.keyboard_controller_scancode_map];
# the fork default map is in the right column.
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

# How long one call keeps retrying the open before it gives up. POSIX: the
# open fails with ENXIO until xemu's first input poll opens the reader,
# which comes after any xemu_lease.py slot wait and the emulator start-up,
# so allow a cold start on a loaded box (the old shell client blocked
# here indefinitely). Windows: ENOENT until xemu creates the pipe, then
# ERROR_PIPE_BUSY for about a frame while it recycles the single instance.
RETRY_SECS = 3.0 if os.name == "nt" else 60.0


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


def require_fifo(path):
    try:
        is_fifo = stat.S_ISFIFO(os.stat(path).st_mode)
    except OSError:
        is_fifo = False
    if not is_fifo:
        sys.exit("error: %s is not a FIFO (run: inject_input.py create %s)"
                 % (path, path))


class Channel(object):
    """One open write session. Windows: a named-pipe client connection;
    POSIX: the FIFO opened for writing."""

    def __init__(self, path):
        self.name = pipe_name(path)
        self.f = None
        if os.name != "nt":
            require_fifo(self.name)
        deadline = time.time() + RETRY_SECS
        last = None
        while True:
            try:
                self.f = self._open()
                return
            except OSError as e:
                # Windows 2/ENOENT: xemu not up yet (or launched without
                # the env var); 231 ERROR_PIPE_BUSY: another client still
                # owns the single instance, or xemu has not recycled it yet
                # - it does so on its next input poll (~1 frame). POSIX
                # ENXIO: no reader holds the FIFO open (xemu not up yet,
                # or gone).
                last = e
                if time.time() >= deadline:
                    break
                time.sleep(0.05)
        sys.exit("error: cannot open %s after %g s: %s\n"
                 "       (is xemu running with XEMU_INPUT_PIPE set?)"
                 % (self.name, RETRY_SECS, last))

    def _open(self):
        if os.name == "nt":
            return open(self.name, "wb", buffering=0)
        # O_NONBLOCK only for the open, so a FIFO nobody reads fails with
        # ENXIO instead of hanging; the writes block as usual.
        fd = os.open(self.name, os.O_WRONLY | os.O_NONBLOCK)
        os.set_blocking(fd, True)
        return os.fdopen(fd, "wb", buffering=0)

    def send(self, line):
        try:
            self.f.write((line + "\n").encode("ascii"))
            self.f.flush()
        except OSError as e:
            # xemu exited (POSIX: EPIPE), or on Windows never bound a
            # keyboard controller and so never polls: report it instead of
            # a traceback.
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
        if not stat.S_ISFIFO(os.stat(path).st_mode):
            sys.exit("error: %s exists and is not a FIFO; refusing to touch "
                     "it" % path)
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
        line = "%s %d" % (cmd, scancode(argv[3]))
        with Channel(path) as ch:
            ch.send(line)
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
