#!/usr/bin/env python3
"""Deterministic-mode A/B with load-robust host counters.

    bench-det.py [-n PAIRS] [-f FRAMES] [-t TAG] [-i LINE]... [--hash-at F,F]
                 [--icount ARGS] [--out FILE] NAME=APP[:K=V,K=V] ...

Each run clones the source config ($XEMU_TEST_CONFIG, else
../fixtures/xemu.toml) and the HDD and EEPROM it names into a fresh temp
dir (muted, pad stubs via harness-config.py), starts APP headless with the
null renderer under `-S -icount shift=2,align=off,sleep=off`, loads TAG
(`-` boots cold), writes the -i pipe lines, advances FRAMES frames and
hashes guest RAM (sha256[:16]: QMP xemu-ram-digest, or xemu-dump-ram's
file on builds without it). Arms run interleaved, PAIRS times.

Per run it reports wall fps and, from proc_pid_rusage, the host cycles
and instructions per guest frame across the whole process, and its CPU
and runnable seconds (on a core or waiting for one, summed over threads):
runnable well above CPU means the run waited for a core. A
deterministic run does fixed guest work, so cycles per frame hardly
feels host load from one run to the next (~0.2-0.5% back to back at
load 30, where wall fps swings 5-10%); it still drifts over minutes with
clocks and cache sharing, so compare arms only within one interleaved
session. Judge A/Bs by cycles per frame; instructions per frame (exact
to ~0.01%) is the mechanism check. Every arm must give the same RAM
hash unless the change is meant to.

Run dirs are /tmp/xbd.<bench-det pid>.*; each start removes the ones a
killed bench-det left (owner gone and no live process naming the dir).

Each xemu runs as scripts/xemu_lease.py says: `nice -n 10`, the argv
marker `-name xemu-fork-auto`, one of XEMU_FORK_SLOTS (4) slots, a lease
in /tmp/xemu-leases, XEMU_BASE_PATH set to the bench cache, and none of
the automation variables this process inherited (XEMU_RAM_SHM, ...; an
arm's own K=V stays).

Example: bench-det.py -n 4 base=/tmp/b.app new=/tmp/e.app
(an arm can add env: new=/tmp/e.app:XEMU_GUEST_MO=1)
Goldens: `-n 1 -f 300` (azurik-heavy-f1), `-t azurik-stream-e2 -f 600
-i "axis ly 1.0"`, `-t - -f 1800 --hash-at 900,1800`.

--timeline: a cold boot of the fixture disc (or --dvd) from power-on to
steady play, cut into phases: kernel, logos, prophecy (0x5F748 to
0x5F774), to-title, title, save-list, level-load (0x53750 to the first
player tick 0x8C230), steady (--steady more ticks). A marker pass on the
first arm finds each phase's first frame with gdbstub breakpoints and
walks the menus with frame-tagged input (START until the list opens, then
--route); then each arm runs -n timing passes that replay that input
with no debugger and print, per phase, guest frames, host ms and where
the host time went. Every timing pass must end on the marker pass's RAM,
or its phases aren't the same frames. --renderer offscreen runs Vulkan on
a portable clone of APP (cold caches); --hdd-io runs qemu-io commands on
the scratch HDD first ("write -z 0x800 0x200": the game rebuilds its HDD
cache); --block-log DIR sets XEMU_BLOCK_LOG on each timing run and checks
its rows against every phase's query-blockstats deltas and the first run.
Example: bench-det.py --timeline -n 2 base=/tmp/a.app new=/tmp/b.app
"""
import argparse, bisect, contextlib, csv, ctypes, ctypes.util, hashlib, importlib.util, json
import os, re, shutil
import socket, statistics, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The source config, as for every harness: $XEMU_TEST_CONFIG, else
# ../fixtures/xemu.toml beside the checkout. Its hdd_path / eeprom_path /
# dvd_path name the files to clone (else the same names beside it).
CFG = os.environ.get('XEMU_TEST_CONFIG') or os.path.join(
    os.path.dirname(REPO), 'fixtures', 'xemu.toml')
_spec = importlib.util.spec_from_file_location(
    'xemu_lease', os.path.join(REPO, 'scripts/xemu_lease.py'))
lease = importlib.util.module_from_spec(_spec)  # marker, nice, slot, lease, bench cache
_spec.loader.exec_module(lease)

_libproc = ctypes.CDLL(ctypes.util.find_library('proc') or '/usr/lib/libproc.dylib')


class _RusageV4(ctypes.Structure):
    _fields_ = [('uuid', ctypes.c_uint8 * 16)] + [(n, ctypes.c_uint64) for n in (
        'user_time system_time pkg_idle_wkups interrupt_wkups pageins wired_size '
        'resident_size phys_footprint proc_start_abstime proc_exit_abstime '
        'child_user_time child_system_time child_pkg_idle_wkups '
        'child_interrupt_wkups child_pageins child_elapsed_abstime '
        'diskio_bytesread diskio_byteswritten qos_default qos_maintenance '
        'qos_background qos_utility qos_legacy qos_user_initiated '
        'qos_user_interactive billed_system_time serviced_system_time '
        'logical_writes lifetime_max_phys_footprint instructions cycles '
        'billed_energy serviced_energy interval_max_phys_footprint '
        'runnable_time').split()]


class _Timebase(ctypes.Structure):
    _fields_ = [('numer', ctypes.c_uint32), ('denom', ctypes.c_uint32)]


_tb = _Timebase()
ctypes.CDLL(None).mach_timebase_info(ctypes.byref(_tb))
_TICK_S = _tb.numer / _tb.denom / 1e9   # rusage times are mach ticks (41.67 ns on Apple silicon)


def counters(pid, times=False):
    """(instructions, cycles) of the whole process; with times, also CPU and
    runnable seconds (runnable = on a core or waiting for one, all threads)."""
    r = _RusageV4()
    if _libproc.proc_pid_rusage(pid, 4, ctypes.byref(r)) != 0:  # RUSAGE_INFO_V4
        raise OSError('proc_pid_rusage failed')
    if times:
        return (r.instructions, r.cycles, (r.user_time + r.system_time) * _TICK_S,
                r.runnable_time * _TICK_S)
    return r.instructions, r.cycles


class QMP:
    def __init__(self, path, proc):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(1200):
            try:
                self.s.connect(path)
                break
            except OSError:
                if proc.poll() is not None:
                    raise RuntimeError('xemu exited before QMP came up')
                time.sleep(0.05)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline() or '{"error": "EOF"}')
            if 'event' in r:
                continue
            if 'error' in r:
                raise RuntimeError(f'{name}: {r["error"]}')
            return r.get('return')


def wait_log(log, text, count, proc, timeout=120):
    end = time.time() + timeout
    while open(log, errors='replace').read().count(text) < count:
        if proc.poll() is not None or time.time() > end:
            raise RuntimeError(f'no "{text}" in {log}')
        time.sleep(0.01)


def cfg_path(key):
    """A path the source config names (hdd_path, eeprom_path, dvd_path), or None."""
    m = re.search(rf"""(?m)^\s*{key}\s*=\s*['"](.*)['"]\s*$""", open(CFG).read())
    return m and m.group(1)


def scratch(d):
    """Clone the fixture HDD, EEPROM and config (muted, pads stubbed) into d."""
    for key, name, dst in (('hdd_path', 'xbox_hdd.qcow2', 'hdd.qcow2'),
                           ('eeprom_path', 'eeprom.bin', 'eeprom.bin')):
        src = cfg_path(key) or os.path.join(os.path.dirname(CFG), name)
        subprocess.run(['cp', '-c', src, os.path.join(d, dst)], check=True)
    cfg = open(CFG).read()
    for k, v in (('hdd_path', f'{d}/hdd.qcow2'), ('eeprom_path', f'{d}/eeprom.bin'),
                 ('screenshot_dir', d)):
        cfg = re.sub(rf'(?m)^{k} = .*$', f"{k} = '{v}'", cfg)
    cfg = re.sub(r'(?m)^fullscreen_on_startup = .*$', 'fullscreen_on_startup = false', cfg)
    toml = os.path.join(d, 'xemu.toml')
    open(toml, 'w').write(cfg)
    pads = subprocess.run([sys.executable, '-I', os.path.join(REPO, 'scripts/harness-config.py'),
                           toml], capture_output=True, text=True, check=True).stdout.split()
    fifo, sock, log = (os.path.join(d, n) for n in ('in.fifo', 'qmp.sock', 'log'))
    os.mkfifo(fifo)
    return toml, pads, fifo, sock, log


def scratch_dir():
    """A fresh /tmp/xbd.<our pid>.* run dir, so sweep_scratch can tell its owner."""
    return tempfile.mkdtemp(prefix=f'xbd.{os.getpid()}.', dir='/tmp')


def sweep_scratch():
    """Remove /tmp/xbd.* run dirs a killed bench-det left (each holds HDD and
    EEPROM clones): those whose owner pid is gone, or, without a pid in the
    name (older bench-det), unchanged for an hour, and in both cases named in
    no live process's arguments (an orphaned xemu still runs on it)."""
    args = subprocess.run(['ps', '-axww', '-o', 'args='], capture_output=True,
                          text=True).stdout
    gone = []
    for name in os.listdir('/tmp'):
        d = os.path.join('/tmp', name)
        m = re.fullmatch(r'xbd\.(?:(\d+)\.)?[a-z0-9_]{8}', name)
        try:
            st = os.lstat(d)
        except OSError:
            continue
        if not m or not os.path.isdir(d) or os.path.islink(d) or st.st_uid != os.getuid() \
                or d + '/' in args:
            continue
        if m.group(1):
            try:
                os.kill(int(m.group(1)), 0)
                continue          # owner alive
            except ProcessLookupError:
                pass
            except PermissionError:
                continue
        elif time.time() - st.st_mtime < 3600:
            continue
        shutil.rmtree(d, ignore_errors=True)
        gone.append(name)
    if gone:
        print(f'bench-det: removed {len(gone)} stale run dir(s): {" ".join(sorted(gone))}',
              file=sys.stderr)


def ram_sha256(q, d):
    """sha256 of guest RAM: QMP xemu-ram-digest (no file), else xemu-dump-ram's file."""
    try:
        return q.cmd('xemu-ram-digest')['sha256']
    except RuntimeError as e:
        if 'CommandNotFound' not in str(e):
            raise
    ram = os.path.join(d, 'ram.bin')
    q.cmd('xemu-dump-ram', filename=ram, **{'gpu-sync': False})
    h = hashlib.sha256(open(ram, 'rb').read()).hexdigest()
    os.remove(ram)
    return h


def run_once(app, env, a, purpose=''):
    d = scratch_dir()
    try:
        toml, pads, fifo, sock, log = scratch(d)
        e = dict(os.environ, XEMU_HEADLESS='null', XEMU_INPUT_PIPE=fifo,
                 XEMU_BASE_PATH=lease.bench_cache(), **env)
        cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,  # + MARK: lease.popen
               '-qmp', f'unix:{sock},server,nowait', '-S', '-icount', a.icount] + pads
        stops = [int(x) for x in a.hash_at.split(',')] if a.hash_at else [a.frames]
        with lease.popen(cmd, purpose or f'bench-det {a.tag} {stops[-1]} frames',
                         30 + stops[-1] // 20, env=e, stdout=subprocess.DEVNULL,
                         stderr=open(log, 'w')) as proc:
            wait_log(log, 'xemu: ready', 1, proc)
            q = QMP(sock, proc)
            q.cmd('stop')
            if a.tag != '-':
                q.cmd('human-monitor-command', **{'command-line': f'loadvm {a.tag}'})
                wait_log(log, 'xemu: state loaded', 1, proc)
            if a.input:
                fd = os.open(fifo, os.O_WRONLY)
                os.write(fd, ('\n'.join(a.input) + '\n').encode())
                os.close(fd)
                time.sleep(0.2)
            done, wall, ins, cyc, cpu, run, hashes = 0, 0.0, 0, 0, 0.0, 0.0, []
            for s in stops:
                i0, c0, u0, r0 = counters(proc.pid, True)
                t = time.time()
                q.cmd('xemu-frame-advance', frames=s - done)
                wall += time.time() - t
                i1, c1, u1, r1 = counters(proc.pid, True)
                ins, cyc, done = ins + i1 - i0, cyc + c1 - c0, s
                cpu, run = cpu + u1 - u0, run + r1 - r0
                hashes.append(f'f{s}=' + ram_sha256(q, d)[:16])
            try:
                q.cmd('quit')
            except Exception:
                pass
            proc.wait(timeout=30)
        return {'fps': done / wall, 'mcyc': cyc / done / 1e6, 'mins': ins / done / 1e6,
                'cpu_s': cpu, 'runnable_s': run, 'ram': ' '.join(hashes)}
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --- Boot timeline (--timeline) ---------------------------------------------
#
# A cold boot from power-on under icount, cut into phases at guest events:
# XBE entry, prophecy.bik's start and end, the title, the save list, the
# level load, the first player tick, steady play. A marker pass finds each
# event's frame with gdbstub breakpoints and walks the menus with
# frame-tagged input; timing passes replay that input with no debugger
# (breakpoints switch off TCG fast paths) and read host counters at each
# phase boundary. VAs are Azurik's USA release (the harness's
# xemu_oracle.py), or Europe's (XDK 4039) when default.xbe's entry says so
# (the Azurik harness's region table); --va overrides them.

TL_VAS = {'prophecy': 0x5F748, 'prophecy_end': 0x5F774, 'inv_reset': 0x57B70,
          'load_level': 0x53750, 'tick': 0x8C230, 'boot_state': 0x1BF61C}
TL_RELEASES = {0xE338A: ('Europe', {'prophecy': 0x5F6F8, 'prophecy_end': 0x5F724,
                                    'tick': 0x8C1E0, 'boot_state': 0x1C15BC})}
TL_TITLE, TL_LIST = 6, 8          # boot states: the title; START opened the save list
TL_PREVIEW_WAIT = 120             # frames a save-list cursor move may take to preview a save
TL_KEYS = {'start': 42, 'a': 81, 'b': 79, 'dpad_left': 37, 'dpad_right': 38,
           'dpad_up': 94, 'dpad_down': 92}   # the fixture's keyboard pad map
TL_PHASES = (('kernel', 'reset', 'xbe-entry'), ('logos', 'xbe-entry', 'prophecy-start'),
             ('prophecy', 'prophecy-start', 'prophecy-end'), ('to-title', 'prophecy-end', 'title'),
             ('title', 'title', 'save-list'), ('save-list', 'save-list', 'load-level'),
             ('level-load', 'load-level', 'first-tick'), ('steady', 'first-tick', 'steady'))


class RSP:
    """The few gdbstub packets the marker pass needs (no-ack mode)."""

    def __init__(self, path, proc):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(1200):
            try:
                self.s.connect(path)
                break
            except OSError:
                if proc.poll() is not None:
                    raise RuntimeError('xemu exited before the gdbstub came up')
                time.sleep(0.05)
        self.buf, self.ack = b'', True
        self.ack = self.cmd('QStartNoAckMode') != 'OK'

    def cmd(self, p):
        self.s.sendall(b'$%s#%02x' % (p.encode(), sum(p.encode()) & 0xff))
        while True:
            i = self.buf.find(b'$')
            j = self.buf.find(b'#', i) if i >= 0 else -1
            if j >= 0 and len(self.buf) >= j + 3:
                data, self.buf = self.buf[i + 1:j], self.buf[j + 3:]
                if self.ack:
                    self.s.sendall(b'+')
                return data.decode('latin-1')
            d = self.s.recv(65536)
            if not d:
                raise EOFError('gdbstub closed')
            self.buf += d

    def bp(self, va, on):
        r = self.cmd(('Z0,%x,1' if on else 'z0,%x,1') % va)
        if r != 'OK':
            raise RuntimeError(f'breakpoint {va:#x}: {r}')

    def reg(self, n):   # i386 'g' order: eax ecx edx ebx esp ebp esi edi eip
        h = self.cmd('g')[n * 8:n * 8 + 8]
        return int.from_bytes(bytes.fromhex(h), 'little')

    def u32(self, va):
        h = self.cmd('m%x,4' % va)
        return int.from_bytes(bytes.fromhex(h), 'little') if len(h) == 8 else None

    def cstr(self, va):
        h = self.cmd('m%x,80' % va)
        return bytes.fromhex(h if not h.startswith('E') else '').split(b'\0')[0].decode('latin-1')


def xbe_entry(iso):
    """default.xbe's entry point (retail key) from an XISO image, or None."""
    try:
        f = open(iso, 'rb')
        for base in (0, 0x18300000, 0x2080000, 0x0FD90000):   # xiso, redump game partitions
            f.seek(base + 0x10000)
            hdr = f.read(28)
            if hdr[:20] == b'MICROSOFT*XBOX*MEDIA':
                break
        else:
            return None
        sec, size = int.from_bytes(hdr[20:24], 'little'), int.from_bytes(hdr[24:28], 'little')
        f.seek(base + sec * 2048)
        d, todo = f.read(size), [0]
        while todo:
            o = todo.pop() * 4
            if o + 14 > len(d) or d[o:o + 2] == b'\xff\xff':
                continue
            left, right = int.from_bytes(d[o:o + 2], 'little'), int.from_bytes(d[o + 2:o + 4], 'little')
            name = d[o + 14:o + 14 + d[o + 13]].decode('latin-1').lower()
            todo += [x for x in (left, right) if x]
            if name == 'default.xbe':
                f.seek(base + int.from_bytes(d[o + 4:o + 8], 'little') * 2048)
                x = f.read(0x130)
                return int.from_bytes(x[0x128:0x12C], 'little') ^ 0xA8FC57AB
    except OSError:
        pass
    return None


_TL_LIST_THREADS, _TL_THREAD_INFO = 6, 5


class _ThreadInfo(ctypes.Structure):
    _fields_ = ([('user', ctypes.c_uint64), ('system', ctypes.c_uint64)] +
                [(n, ctypes.c_int32) for n in 'cpu policy run flags sleep cur pri maxpri'.split()] +
                [('name', ctypes.c_char * 64)])


def thread_cpu(pid):
    """{thread handle: user+system ns} of every thread in pid."""
    buf = (ctypes.c_uint64 * 1024)()
    n = _libproc.proc_pidinfo(pid, _TL_LIST_THREADS, 0, buf, ctypes.sizeof(buf))
    out = {}
    for h in buf[:max(n, 0) // 8]:
        ti = _ThreadInfo()
        if _libproc.proc_pidinfo(pid, _TL_THREAD_INFO, ctypes.c_uint64(h), ctypes.byref(ti),
                                 ctypes.sizeof(ti)) == ctypes.sizeof(ti):
            out[h] = ti.user + ti.system
    return out


def tl_launch(app, env, a, d, gdb, stack):
    """Start a timeline xemu in d: cold, paused, under icount, via lease.popen
    (marker, nice, slot, lease); stack.close() ends it."""
    toml, pads, fifo, sock, log = scratch(d)
    base = lease.bench_cache()
    if a.renderer == 'offscreen':
        # Offscreen Vulkan writes shader caches to the base path: keep them
        # in d (cold every run). The portable clone covers binaries that
        # predate XEMU_BASE_PATH.
        base = d
        clone = os.path.join(d, 'xemu.app')
        subprocess.run(['cp', '-Rc', app, clone], check=True)
        open(os.path.join(clone, 'Contents/Resources/xemu.toml'), 'w').close()
        app = clone
    e = dict(os.environ, XEMU_HEADLESS='null' if a.renderer == 'null' else '1',
             XEMU_INPUT_PIPE=fifo, XEMU_BASE_PATH=base, **env)
    cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,  # + MARK: lease.popen
           '-qmp', f'unix:{sock},server,nowait', '-S', '-icount', a.icount] + pads
    if a.dvd:
        cmd += ['-dvd_path', a.dvd]
    if gdb:
        cmd += ['-gdb', f'unix:{os.path.join(d, "gdb.sock")},server=on,wait=off']
    proc = stack.enter_context(lease.popen(
        cmd, f'bench-det --timeline {"markers" if gdb else "timing"} ({a.renderer})', 180,
        env=e, stdout=subprocess.DEVNULL, stderr=open(log, 'w')))
    wait_log(log, 'xemu: ready', 1, proc)
    q = QMP(sock, proc)
    q.cmd('stop')
    for c in a.hdd_io:   # on the scratch clone, before power-on
        q.cmd('human-monitor-command', **{'command-line': f'qemu-io ide0-hd0 "{c}"'})
    return proc, q, fifo, log


def tl_ram(q, d):
    return ram_sha256(q, d)[:16]


def tl_markers(app, env, a, va):
    """Marker pass: each event's frame, the input schedule, the end RAM."""
    d = scratch_dir()
    stack = contextlib.ExitStack()
    try:
        proc, q, fifo, log = tl_launch(app, env, a, d, True, stack)
        g = RSP(os.path.join(d, 'gdb.sock'), proc)
        fd = os.open(fifo, os.O_RDWR)
        marks, info, schedule, armed = {'reset': 0}, {}, [], set()
        frame, state, stage, limit = 0, None, 'xbe-entry', a.phase_frames

        def arm(v, on=True):
            if on != (v in armed):
                g.bp(v, on)
                (armed.add if on else armed.discard)(v)

        def press(key, at):
            lines = [f'@{at} down {TL_KEYS[key]}', f'@{at + 6} up {TL_KEYS[key]}']
            os.write(fd, ('\n'.join(lines) + '\n').encode())
            schedule.extend(lines)

        def mark(name):
            marks[name] = frame
            print(f'  marker {name:15s} frame {frame:6d}', flush=True)

        entry = va.get('xbe_entry')
        if entry:
            arm(entry)
        else:
            stage = 'prophecy-start'
            arm(va['prophecy'])
        due, move = None, None            # next press: (frame, key); cursor move: (key, frame)
        route = list(a.route)
        tick_ent, ticks, deadline = None, 0, limit
        while stage != 'done':
            step = 30 if due is None else max(1, min(30, due[0] - frame - 1))
            r = q.cmd('xemu-frame-advance', frames=step,
                      trace=[{'address': va['boot_state'], 'size': 4, 'virtual': True}])
            for s in r.get('trace', []):
                v = int.from_bytes(bytes.fromhex(s['data'][0]), 'little') if s['data'][0] else None
                if stage == 'title' and v == TL_TITLE and 'title' not in marks:
                    marks['title'] = s['frame'] - 1
                    print(f'  marker {"title":15s} frame {marks["title"]:6d}', flush=True)
                    due = (s['frame'] + 1, 'start')
                state = v
            frame = r['frame']
            if frame > deadline:
                raise RuntimeError(f'timeline: no {stage} within {limit} frames of the last marker')
            if r.get('debug-stop'):
                eip = g.reg(8)
                if stage == 'xbe-entry' and eip == entry:
                    mark('xbe-entry'); arm(entry, False); arm(va['prophecy'])
                    stage = 'prophecy-start'
                elif stage == 'prophecy-start' and eip == va['prophecy']:
                    mark('prophecy-start'); arm(va['prophecy'], False); arm(va['prophecy_end'])
                    stage, due = 'prophecy-end', None if a.watch_movie else (frame + 2, 'start')
                elif stage == 'prophecy-end' and eip == va['prophecy_end']:
                    mark('prophecy-end'); arm(va['prophecy_end'], False); arm(va['inv_reset'])
                    stage, due = 'title', None
                elif stage == 'title' and eip == va['inv_reset']:
                    if 'title' not in marks:   # the list opened before a traced vblank saw the title
                        marks['title'] = frame
                    mark('save-list'); arm(va['inv_reset'], False)
                    stage, due = 'save-list', (frame + 30, route[0])
                elif stage == 'save-list' and eip == va['inv_reset']:
                    arm(va['inv_reset'], False)    # the cursor's move previewed its save
                    print(f'  {move[0]} previewed a save after {frame - move[1]} frames', flush=True)
                    due, move = (frame + 30, route[0]), None
                elif stage == 'save-list' and eip == va['load_level']:
                    esp = g.reg(4)
                    p = g.u32(esp + 8)
                    info['level'] = g.cstr(p) if p else None
                    mark('load-level'); arm(va['load_level'], False); arm(va['tick'])
                    stage, due = 'first-tick', None
                elif stage in ('first-tick', 'steady') and eip == va['tick']:
                    ent = g.reg(1)
                    if stage == 'first-tick':
                        mark('first-tick')
                        tick_ent, stage = ent, 'steady'
                    ticks += ent == tick_ent
                    if ticks > a.steady:
                        mark('steady'); arm(va['tick'], False)
                        stage = 'done'
                        continue
                    arm(va['tick'], False)    # step off the breakpoint, then re-arm it
                    g.cmd('s')
                    arm(va['tick'])
                    continue
                else:
                    raise RuntimeError(f'timeline: unexpected stop at {eip:#x} waiting for {stage}')
                deadline = frame + limit
            if due is not None and frame >= due[0] - 1:
                at, key = max(due[0], frame + 1), due[1]
                if move:
                    print(f'  {move[0]} previewed no save in {at - move[1]} frames', flush=True)
                    move = None
                press(key, at)
                if stage == 'prophecy-end':
                    due = (at + 90, 'start')          # START every 1.5 s until the movie ends
                elif stage == 'title':
                    due = (at + a.title_every, 'start')   # until the list opens
                elif stage == 'save-list' and len(route) > 1:
                    arm(va['inv_reset'])              # a cursor move: wait for its preview,
                    route.pop(0)                      # but "Start New Game" shows none, so the
                    due = (at + TL_PREVIEW_WAIT, route[0])   # next key goes in after a while
                    move = (key, at)
                elif stage == 'save-list':
                    arm(va['inv_reset'], False)
                    arm(va['load_level'])
                    due = (at + 1500, key)            # pressed again after 25 s without a load
                else:
                    due = None
        r = q.cmd('xemu-frame-advance', frames=1)     # on to a vblank
        info['end'] = r['frame']
        info['ram'] = tl_ram(q, d)
        return marks, info, schedule
    finally:
        stack.close()   # kills a xemu still running, drops its lease and slot
        shutil.rmtree(d, ignore_errors=True)


def tl_snap(q, pid, hdd):
    bs = {}
    for dev in q.cmd('query-blockstats'):
        name = {'ide0-hd0': 'hdd', 'ide0-cd1': 'dvd'}.get(dev.get('device'), dev.get('device'))
        bs[name] = dev['stats']
    try:
        hp = q.cmd('query-xemu-host-profile')
    except RuntimeError:
        hp = None
    ins, cyc = counters(pid)
    return {'t': time.monotonic(), 'ins': ins, 'cyc': cyc, 'thr': thread_cpu(pid), 'bs': bs, 'hp': hp,
            'qcow2': os.path.getsize(hdd)}


def tl_row(s0, s1, frames):
    def bd(dev, k):
        return s1['bs'].get(dev, {}).get(k, 0) - s0['bs'].get(dev, {}).get(k, 0)
    thr = {h: c - s0['thr'].get(h, 0) for h, c in s1['thr'].items()}
    row = {'frames': frames, 'ms': (s1['t'] - s0['t']) * 1e3, 'mcyc': (s1['cyc'] - s0['cyc']) / 1e6,
           'minsns': (s1['ins'] - s0['ins']) / 1e6, 'thr': thr, 'qcow2_grow': s1['qcow2'] - s0['qcow2']}
    for dev in ('hdd', 'dvd'):
        for k in ('rd_operations', 'rd_bytes', 'rd_total_time_ns', 'wr_operations', 'wr_bytes',
                  'wr_total_time_ns', 'flush_operations', 'flush_total_time_ns'):
            row[f'{dev}.{k}'] = bd(dev, k)
    if s0['hp'] and s1['hp']:
        for k, v in s1['hp'].items():
            row[f'hp.{k}'] = v - s0['hp'][k]
    return row


def tl_timing(app, env, a, marks, info, schedule):
    """Timing pass: replay the schedule without a debugger, snap each phase."""
    d = scratch_dir()
    stack = contextlib.ExitStack()
    try:
        proc, q, fifo, log = tl_launch(app, env, a, d, False, stack)
        fd = os.open(fifo, os.O_RDWR)
        os.write(fd, ('\n'.join(schedule) + '\n').encode())
        time.sleep(0.2)
        rows, frame, phases, names, start = {}, 0, [], [], 'reset'
        for n, _, e in TL_PHASES:   # a phase whose end wasn't marked joins the next
            names.append(n)
            if e in marks:
                phases.append(('+'.join(names), marks[start], marks[e]))
                names, start = [], e
        phases.append(('tail', marks['steady'], info['end']))
        hdd = os.path.join(d, 'hdd.qcow2')
        snap = tl_snap(q, proc.pid, hdd)
        for name, start, end in phases:
            assert start == frame, (name, start, frame)
            if end > frame:
                q.cmd('xemu-frame-advance', frames=end - frame)
                frame = end
            nxt = tl_snap(q, proc.pid, hdd)
            rows[name] = tl_row(snap, nxt, end - start)
            rows[name]['start'] = start
            snap = nxt
        ram = tl_ram(q, d)
        try:
            q.cmd('quit')
        except Exception:
            pass
        proc.wait(timeout=30)
        # The vCPU: the busiest thread over the run (under icount it does
        # nearly all the work; "other" is the rest: disk workers, main loop,
        # PFIFO and the renderer offscreen).
        busy = {}
        for r in rows.values():
            for h, c in r['thr'].items():
                busy[h] = busy.get(h, 0) + c
        vcpu = max(busy, key=busy.get)
        for r in rows.values():
            thr = r.pop('thr')
            r['vcpu_ms'] = thr.get(vcpu, 0) / 1e6
            r['other_ms'] = (sum(thr.values()) - thr.get(vcpu, 0)) / 1e6
        return rows, ram
    finally:
        stack.close()   # kills a xemu still running, drops its lease and slot
        shutil.rmtree(d, ignore_errors=True)


def tl_block_log(path, rows):
    """XEMU_BLOCK_LOG's rows against each phase's query-blockstats deltas."""
    recs = list(csv.DictReader(open(path), delimiter='\t'))
    drives = {'ide0-hd0': ('hdd', 512), 'ide0-cd1': ('dvd', 2048)}
    phases = sorted((r['start'], r['frames'], p) for p, r in rows.items())   # empty ones first
    starts = [s for s, _, _ in phases]
    got, tot, short, failed = {}, {}, 0, 0
    for x in recs:
        if x['drive'] not in drives or x['op'] not in 'rw':
            continue
        dev, size = drives[x['drive']]
        lba, sectors, nbytes = int(x['lba']), int(x['sectors']), int(x['bytes'])
        ok = x['status'] == 'ok'
        failed += not ok
        short += ok and lba >= 0 and sectors * size != nbytes
        i = bisect.bisect_right(starts, int(x['frame'])) - 1
        k = (phases[max(i, 0)][2], f"{dev}.{'rd' if x['op'] == 'r' else 'wr'}_bytes")
        # A command that ended ok moved what it asked for; a failed one (err,
        # open) what blockstats counted; lba -1 is a DMA reply.
        got[k] = got.get(k, 0) + (sectors * size if ok and lba >= 0 else nbytes)
        t = tot.setdefault(k[1], [0, 0])
        t[0], t[1] = t[0] + 1, t[1] + nbytes
    bad = [f'{p} {k}: rows {got.get((p, k), 0)} B, blockstats {r[k]} B'
           for p, r in rows.items() for k in ('hdd.rd_bytes', 'hdd.wr_bytes', 'dvd.rd_bytes')
           if got.get((p, k), 0) != r[k]]
    out = (f'  block log: {len(recs)} rows; ' +
           ', '.join(f'{k[:-6]} {n} rows {b / 2**20:.1f} MiB' for k, (n, b) in sorted(tot.items())) +
           ('; sectors match query-blockstats in every phase' if not bad else
            '; MISMATCH ' + '; '.join(bad)))
    if failed:
        out += f'; {failed} rows failed (status err or open: their bytes, not their sectors)'
    if short:
        out += f'; {short} ok rows moved less than they asked for'
    return out, hashlib.sha256(open(path, 'rb').read()).hexdigest()


def tl_print(name, runs, base=None):
    """Per phase, the mean over runs; returns the means for the next arm's deltas."""
    phases = [p for p in runs[0] if p != 'tail']
    means = {}
    for p in phases:
        means[p] = {k: statistics.mean(r[p][k] for r in runs) for k in runs[0][p]}
    means['total'] = {k: sum(means[p][k] for p in phases) for k in means[phases[0]]}
    means['total']['start'] = 0
    print(f'{name}: mean of {len(runs)} run(s)')
    print(f'  {"phase":12s} {"start":>6s} {"frames":>6s} {"ms":>7s} {"ms/f":>6s} {"vcpu":>6s} '
          f'{"exec":>6s} {"xlate":>6s} {"apu":>5s} {"other":>6s} {"idle%":>5s}  '
          f'{"hdd rd n/MiB/ms":>16s} {"hdd wr n/MiB/ms":>16s} {"grow":>5s} {"flush":>6s} '
          f'{"dvd rd n/MiB/ms":>16s}' + (f'  {"vs " + base[0]:>8s}' if base else ''))
    for p in phases + ['total']:
        r, frames, start = means[p], means[p]['frames'], int(means[p]['start'])
        if not frames:
            print(f'  {p:12s} {start:6d} {0:6d}       -')
            continue
        def io(dev, op):
            return (f"{r[f'{dev}.{op}_operations']:.0f}/{r[f'{dev}.{op}_bytes'] / 2**20:.1f}/"
                    f"{r[f'{dev}.{op}_total_time_ns'] / 1e6:.0f}")
        hp = lambda k: r.get(f'hp.{k}', 0)
        xlate, apu = hp('translate-ns') / 1e6, hp('apu-ns') / 1e6
        grow = r['qcow2_grow'] / 2**20
        line = (f'  {p:12s} {start:6d} {frames:6.0f} {r["ms"]:7.0f} {r["ms"] / frames:6.2f} '
                f'{r["vcpu_ms"]:6.0f} {r["vcpu_ms"] - xlate - apu:6.0f} {xlate:6.0f} {apu:5.0f} '
                f'{r["other_ms"]:6.0f} {hp("idle-ns") / (frames * 1e9 / 60) * 100:5.1f}  '
                f'{io("hdd", "rd"):>16s} {io("hdd", "wr"):>16s} {grow:5.1f} '
                f'{r["hdd.flush_operations"]:.0f}/{r["hdd.flush_total_time_ns"] / 1e6:<4.0f} '
                f'{io("dvd", "rd"):>16s}')
        if base and base[1].get(p, {}).get('ms'):
            line += f'  {(r["ms"] / base[1][p]["ms"] - 1) * 100:+7.1f}%'
        print(line)
    return means


def timeline(a, arms):
    if a.tag != '-':
        sys.exit('--timeline boots cold: leave -t at its default or pass -t -')
    va = dict(TL_VAS)
    if not a.dvd:
        iso = cfg_path('dvd_path')
    else:
        iso = a.dvd
    va['xbe_entry'] = xbe_entry(iso) if iso else None
    release, vas = TL_RELEASES.get(va['xbe_entry'], ('USA', {}))
    va.update(vas)
    for kv in a.va:
        k, v = kv.split('=', 1)
        va[k] = int(v, 0)
    name, app, env = arms[0]
    print(f'timeline: markers on {name} ({a.renderer}); disc {iso}; XBE entry '
          f'{va["xbe_entry"] and hex(va["xbe_entry"])} ({release} VAs)', flush=True)
    t = time.time()
    marks, info, schedule = tl_markers(app, env, a, va)
    print(f'  level {info.get("level")}; end frame {info["end"]} ram {info["ram"]}; '
          f'{len(schedule) // 2} presses; {time.time() - t:.1f} s', flush=True)
    res = {n: [] for n, _, _ in arms}
    out = open(a.out, 'a') if a.out else None
    first_log = None
    if a.block_log:
        os.makedirs(a.block_log, exist_ok=True)
    for i in range(a.pairs):
        for n, app, env in arms:
            if a.block_log:
                blog = os.path.join(a.block_log, f'{n}-{i + 1}.tsv')
                env = dict(env, XEMU_BLOCK_LOG=blog)
            rows, ram = tl_timing(app, env, a, marks, info, schedule)
            res[n].append(rows)
            tot = sum(r['ms'] for p, r in rows.items() if p != 'tail')
            same = 'same RAM' if ram == info['ram'] else f'RAM {ram} DIFFERS from the marker pass'
            print(f'{i + 1} {n}: boot to steady {tot / 1e3:.2f} s host, {same}', flush=True)
            if out:
                out.write(json.dumps({'pair': i + 1, 'arm': n, 'marks': marks, 'info': info,
                                      'ram': ram, 'rows': {p: {k: v for k, v in r.items() if k != 'thr'}
                                                           for p, r in rows.items()}}) + '\n')
            if ram != info['ram']:
                print('timeline: the replay left the marker pass\'s path; phase frames are not '
                      'comparable', file=sys.stderr)
            if a.block_log:
                line, digest = tl_block_log(blog, rows)
                first_log = first_log or digest
                print(line + ('; same rows as the first run' if digest == first_log else
                              '; rows DIFFER from the first run'), flush=True)
    load = os.getloadavg()
    print(f'load {load[0]:.1f} {load[1]:.1f} {load[2]:.1f}')
    print('ms: host wall time; vcpu: CPU ms of the vCPU thread (ms - vcpu = waiting: disk '
          'round trips, host load), split into exec (guest code, helpers, devices), xlate '
          '(translating) and apu (APU ticks); other: CPU ms of all other threads; idle%: guest '
          'time warped while halted (no host cost); hdd/dvd: requests/MiB/request ms (submit '
          'to completion); grow: MiB the qcow2 file grew (new clusters allocated); '
          'flush: count/ms')
    base = None
    for n, _, _ in arms:
        means = tl_print(n, res[n], base)
        base = base or (n, means)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('-n', '--pairs', type=int, default=4)
    p.add_argument('-f', '--frames', type=int, default=1200)
    p.add_argument('-t', '--tag', default='azurik-heavy-f1')
    p.add_argument('-i', '--input', action='append', default=[])
    p.add_argument('--hash-at', default='')
    p.add_argument('--icount', default='shift=2,align=off,sleep=off')
    p.add_argument('--out', default='')
    t = p.add_argument_group('boot timeline (--timeline)')
    t.add_argument('--timeline', action='store_true',
                   help='cold boot to steady play, phase by phase (-n timing runs per arm)')
    t.add_argument('--renderer', choices=('null', 'offscreen'), default='null')
    t.add_argument('--dvd', default='', help='disc to boot (default: the fixture config\'s)')
    t.add_argument('--route', default='a',
                   help='save-list presses, comma-separated (dpad_left,a: new game); each key '
                        'but the last waits for its save preview, at most %d frames' % TL_PREVIEW_WAIT)
    t.add_argument('--steady', type=int, default=120, help='player ticks after the first')
    t.add_argument('--watch-movie', action='store_true',
                   help="don't press START to skip prophecy.bik (times Bink decoding)")
    t.add_argument('--title-every', type=int, default=30, help='frames between STARTs at the title')
    t.add_argument('--phase-frames', type=int, default=6000, help='frames a phase may take')
    t.add_argument('--hdd-io', action='append', default=[],
                   help='qemu-io command on the scratch HDD before power-on; '
                        '"write -z 0x800 0x200" empties the cache-partition table, '
                        'so the game copies its HDD cache again')
    t.add_argument('--block-log', default='', metavar='DIR',
                   help='XEMU_BLOCK_LOG per timing run into DIR/<arm>-<run>.tsv; checks the rows '
                        'against each phase\'s query-blockstats deltas and against the first run')
    t.add_argument('--va', action='append', default=[],
                   help='NAME=ADDR override: ' + ', '.join(TL_VAS) + ', xbe_entry')
    p.add_argument('arms', nargs='+', help='NAME=APP[:K=V,K=V]')
    a = p.parse_args()
    sweep_scratch()
    arms = []
    for spec in a.arms:
        name, rest = spec.split('=', 1)
        app, _, envs = rest.partition(':')
        arms.append((name, app, dict(kv.split('=', 1) for kv in envs.split(',') if kv)))
    if a.timeline:
        a.route = a.route.split(',')
        if a.tag == 'azurik-heavy-f1':
            a.tag = '-'
        return timeline(a, arms)
    res = {n: [] for n, _, _ in arms}
    out = open(a.out, 'a') if a.out else None
    for i in range(a.pairs):
        for name, app, env in arms:
            r = run_once(app, env, a, f'bench-det {name} pair {i + 1}/{a.pairs} '
                                      f'{a.tag} {a.frames if not a.hash_at else a.hash_at} frames')
            res[name].append(r)
            line = (f'{i + 1} {name}: {r["fps"]:.1f} fps  {r["mcyc"]:.3f} Mcyc/f  '
                    f'{r["mins"]:.3f} Mins/f  {r["cpu_s"]:.2f}/{r["runnable_s"]:.2f} cpu/runnable s  '
                    f'{r["ram"]}')
            print(line, flush=True)
            if out:
                out.write(json.dumps({'pair': i + 1, 'arm': name, **r}) + '\n')
    load = os.getloadavg()
    print(f'load {load[0]:.1f} {load[1]:.1f} {load[2]:.1f}')
    base = arms[0][0]
    for key, label, better in (('mcyc', 'Mcycles/frame', -1), ('mins', 'Minsns/frame', -1),
                               ('fps', 'wall fps', 1)):
        print(label)
        b = [r[key] for r in res[base]]
        for name, _, _ in arms:
            v = [r[key] for r in res[name]]
            sd = statistics.stdev(v) if len(v) > 1 else 0.0
            gain = better * (statistics.mean(v) / statistics.mean(b) - 1) * 100
            wins = sum(1 for x, y in zip(v, b) if (x - y) * better > 0)
            print(f'  {name:10s} {statistics.mean(v):9.3f} sd {sd:.3f}  '
                  f'{gain:+.2f}% faster than {base}  wins {wins}/{len(v)}')
    hashes = {r['ram'] for rs in res.values() for r in rs}
    if len(hashes) > 1:
        print('RAM hashes differ:', ', '.join(sorted(hashes)))
        sys.exit(2)


if __name__ == '__main__':
    main()
