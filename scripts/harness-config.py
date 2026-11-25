#!/usr/bin/env python3
"""Finish a harness's scratch xemu.toml: mute it, pin the pad topology and,
with --workdir, point it at the run's own files.

    harness-config.py <scratch xemu.toml> [--no-pad-stubs] [--set TABLE.KEY=VALUE]...
                      [--workdir D [--hdd H] [--eeprom E] [--dvd ISO] [--fullscreen]]

Edits the scratch config in place and prints the extra xemu arguments,
one per line, for the caller to append to its command line (stdout holds
nothing else; diagnostics go to stderr). Only ever point it at a per-run
copy, never at ../fixtures/xemu.toml or the user's config.

--set TABLE.KEY=VALUE (repeatable) writes one config key, the per-run A/B
knob (perf.async_pipeline_compile=false, display.quality.surface_scale=1):
the last dot splits the table from the key. VALUE is TOML when it reads
as a bool, a number, a quoted string, an array or an inline table, else
a string; a string or enum key takes any unquoted VALUE as a string
(sys.mem_limit=128). The key must be in the checkout's config_spec.yml,
with a value of its type (an enum's: one of its values), and VALUE valid
TOML (tomllib checks it on 3.11+, patterns on 3.9): else exit 2, as xemu
would warn and run on the default. It rewrites one-line keys only, and
refuses the keys this script sets itself (the mute, the pads, the work
dir's files).

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
import difflib
import json
import os
import re
import sys

try:
    import tomllib   # 3.11+; under 3.9 (/usr/bin/python3) the patterns below check
except ImportError:
    tomllib = None

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
p.add_argument('--set', action='append', default=[], metavar='TABLE.KEY=VALUE',
               help='set one config key of config_spec.yml (repeatable; VALUE is TOML, else a string)')
a = p.parse_args()
path = a.toml
pad_stubs = not a.no_pad_stubs


def die(msg):
    print('harness-config: ' + msg, file=sys.stderr)
    sys.exit(2)


def quoted(v):
    """A TOML string for v: literal unless it holds a quote."""
    return "'%s'" % v if "'" not in v and '\n' not in v else json.dumps(v)


# TOML numbers (no leading zeros; a sign only on decimals, inf and nan)
# and one-line strings, for Pythons without tomllib (3.9: /usr/bin/python3).
INT = r"[+-]?(0|[1-9](_?[0-9])*)|0x[0-9A-Fa-f](_?[0-9A-Fa-f])*|0o[0-7](_?[0-7])*|0b[01](_?[01])*"
FLOAT = r"[+-]?(0|[1-9](_?[0-9])*)(\.[0-9](_?[0-9])*)?([eE][+-]?[0-9](_?[0-9])*)?|[+-]?(inf|nan)"
STRING = r"'[^'\n]*'|\"([^\"\\\n]|\\.)*\""
ALIASES = {'bool': 'boolean', 'int': 'integer', 'float': 'number', 'str': 'string'}


def balanced(s):
    """True if s's brackets pair up and close at its end (quoted strings
    skipped): a one-line array or inline table, as far as 3.9 can tell."""
    want, q, i = [], None, 0
    while i < len(s):
        c = s[i]
        if q:
            if c == '\\' and q == '"':
                i += 1
            elif c == q:
                q = None
        elif c in '\'"':
            q = c
        elif c in '[{':
            want.append(']' if c == '[' else '}')
        elif c in ']}':
            if not want or want.pop() != c or (not want and s[i + 1:].strip()):
                return False
        i += 1
    return not want and q is None


def parsed(s):
    """The value of TOML text s, or ValueError (tomllib when there is one;
    else by form, an array or inline table as an empty one)."""
    if '\n' in s:
        raise ValueError('a value on one line')
    if tomllib:
        try:
            return tomllib.loads('v = ' + s)['v']
        except tomllib.TOMLDecodeError as e:
            raise ValueError(str(e))
    if s in ('true', 'false'):
        return s == 'true'
    if re.fullmatch(INT, s):
        return int(s.replace('_', ''), 0)
    if re.fullmatch(FLOAT, s):
        return float(s.replace('_', ''))
    if s[:1] in ('"', "'") and re.fullmatch(STRING, s):
        try:
            return json.loads(s) if s[0] == '"' else s[1:-1]
        except ValueError:   # a TOML escape JSON lacks (\U0001F600)
            return s[1:-1]
    if s[:1] in ('[', '{') and balanced(s):
        return [] if s[0] == '[' else {}
    raise ValueError('not TOML')


def toml_value(name, v, want=None):
    """--set's VALUE v as TOML text: as written when it reads as a TOML
    bool, number, quoted string, array or inline table, else as a string;
    for a string or enum key (want, config_spec.yml's type), any unquoted
    v is a string (sys.mem_limit=128). Text that starts as a quoted
    string, an array or a table and isn't valid TOML dies."""
    s = v.strip()
    if want in ('string', 'enum') and s[:1] not in ('"', "'"):
        return quoted(s)
    if s in ('true', 'false') or re.fullmatch(INT, s) or re.fullmatch(FLOAT, s) \
            or (s and s[0] in '[{"\''):
        try:
            parsed(s)
        except ValueError as e:
            die('--set %s=%s: not a valid TOML value (%s)' % (name, s, e))
        return s
    return quoted(s)


def spec_keys(path):
    """{TABLE.KEY: (type, enum values)} for config_spec.yml's keys by
    genconfig's rules (`key: TYPE`, or a mapping whose type: names it; any
    other mapping is a table), read by indentation (3.9 has no yaml
    module); None without the file."""
    try:
        text = open(path).read().splitlines()
    except OSError:
        return None
    root = {}
    stack = [(-1, root)]
    for raw in text:
        m = re.match(r"( *)([^\s:#-][^:]*?):(?:\s+(.*))?$", re.sub(r"(^|\s)#.*", "", raw).rstrip())
        if not m:
            continue
        while stack[-1][0] >= len(m.group(1)):
            stack.pop()
        node = m.group(3) or {}
        stack[-1][1][m.group(2).strip().strip("'\"")] = node
        if isinstance(node, dict):
            stack.append((len(m.group(1)), node))
    out = {}

    def walk(node, prefix):
        for k, v in node.items():
            if isinstance(v, dict) and not isinstance(v.get('type'), str):
                walk(v, prefix + k + '.')
                continue
            t = v if isinstance(v, str) else v['type']
            vals = v.get('values', '') if isinstance(v, dict) else ''
            out[prefix + k] = (ALIASES.get(t, t), [x.strip().strip("'\"") for x in
                                                   vals.strip('[]').split(',') if x.strip()])
    walk(root, '')
    return out


TYPE_OK = {'boolean': lambda x: isinstance(x, bool),
           'integer': lambda x: isinstance(x, int) and not isinstance(x, bool),
           'number': lambda x: isinstance(x, (int, float)) and not isinstance(x, bool),
           'string': lambda x: isinstance(x, str),
           'enum': lambda x: isinstance(x, str),
           'array': lambda x: isinstance(x, list)}

spec = spec_keys(os.path.join(REPO, 'config_spec.yml')) if a.set else None
sets = {}
for kv in a.set:
    k, eq, v = kv.partition('=')
    table, dot, key = k.strip().rpartition('.')
    if not eq or not re.fullmatch(r'[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*', table) \
            or not re.fullmatch(r'[A-Za-z0-9_]+', key):
        die('--set %r: want TABLE.KEY=VALUE (perf.async_pipeline_compile=false)' % kv)
    name = table + '.' + key
    want, values = (None, []) if spec is None else spec.get(name, (None, []))
    if spec is not None and want is None:
        if any(x.startswith(name + '.') for x in spec):
            die('--set %s: a table in config_spec.yml, not a key' % name)
        close = difflib.get_close_matches(name, list(spec), 3)
        die('--set %s: no such key in config_spec.yml%s' % (
            name, ' (did you mean %s?)' % ', '.join(close) if close else ''))
    text = toml_value(name, v, want)
    if want is not None:
        x = parsed(text)
        if not TYPE_OK.get(want, lambda x: True)(x):
            die('--set %s=%s: config_spec.yml types it %s' % (name, v.strip(), want))
        if want == 'enum' and values and x not in values:
            die('--set %s=%s: not one of %s' % (name, v.strip(), ', '.join(values)))
    sets[(table, key)] = text

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
for (t, k), v in sets.items():
    if (t, k) in force or t in ("sys.files", "input.bindings") or (t, k) in (
            ("input", "auto_bind"), ("general", "screenshot_dir"),
            ("display.window", "fullscreen_on_startup")):
        die("--set %s.%s: harness-config owns that key (the mute, the pads, "
            "the run's files)" % (t, k))
    force[(t, k)] = v
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
    m = re.match(r"\s*([A-Za-z0-9_]+)\s*=(.*)", line)
    if m and (section, m.group(1)) in force:
        v = m.group(2)
        if (section, m.group(1)) in sets and (v.count("[") > v.count("]") or v.count("{") > v.count("}")
                                              or "'''" in v or '"""' in v):
            die("--set %s.%s: its value spans lines here; edit it by hand" % (section, m.group(1)))
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
