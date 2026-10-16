#!/usr/bin/env python3
"""Deterministic-mode A/B with load-robust host counters.

    bench-det.py [-n PAIRS] [-f FRAMES] [-t TAG] [-i LINE]... [--hash-at F,F]
                 [--icount ARGS] [--out FILE] NAME=APP[:K=V,K=V] ...

Each run clones the fixture HDD, EEPROM and config into a fresh temp dir
(muted, pad stubs via harness-config.py), starts APP headless with the
null renderer under `-S -icount shift=2,align=off,sleep=off`, loads TAG
(`-` boots cold), writes the -i pipe lines, advances FRAMES frames and
hashes guest RAM (sha256[:16]: QMP xemu-ram-digest, or xemu-dump-ram's
file on builds without it). Arms run interleaved, PAIRS times.

Per run it reports wall fps and, from proc_pid_rusage, the host cycles
and instructions per guest frame across the whole process. A
deterministic run does fixed guest work, so cycles per frame hardly
feels host load from one run to the next (~0.2-0.5% back to back at
load 30, where wall fps swings 5-10%); it still drifts over minutes with
clocks and cache sharing, so compare arms only within one interleaved
session. Judge A/Bs by cycles per frame; instructions per frame (exact
to ~0.01%) is the mechanism check. Every arm must give the same RAM
hash unless the change is meant to.

Each xemu runs as scripts/xemu_lease.py says: `nice -n 10`, the argv
marker `-name xemu-fork-auto`, one of XEMU_FORK_SLOTS (4) slots, a lease
in /tmp/xemu-leases, and XEMU_BASE_PATH set to the bench cache.

Example: bench-det.py -n 4 base=/tmp/a.app new=/tmp/a.app:XEMU_FOO=0
Goldens: `-n 1 -f 300` (azurik-heavy-f1), `-t azurik-stream-e2 -f 600
-i "axis ly 1.0"`, `-t - -f 1800 --hash-at 900,1800`.
"""
import argparse, ctypes, ctypes.util, hashlib, importlib.util, json, os, re, shutil
import socket, statistics, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.environ.get('XEMU_FIXTURES', os.path.join(os.path.dirname(REPO), 'fixtures'))
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


def counters(pid):
    r = _RusageV4()
    if _libproc.proc_pid_rusage(pid, 4, ctypes.byref(r)) != 0:  # RUSAGE_INFO_V4
        raise OSError('proc_pid_rusage failed')
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


def scratch(d):
    """Clone the fixture HDD, EEPROM and config (muted, pads stubbed) into d."""
    for src, dst in (('xbox_hdd.qcow2', 'hdd.qcow2'), ('eeprom.bin', 'eeprom.bin')):
        subprocess.run(['cp', '-c', os.path.join(FIX, src), os.path.join(d, dst)], check=True)
    cfg = open(os.path.join(FIX, 'xemu.toml')).read()
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
    d = tempfile.mkdtemp(prefix='xbd.', dir='/tmp')
    try:
        toml, pads, fifo, sock, log = scratch(d)
        e = dict(os.environ, XEMU_HEADLESS='null', XEMU_INPUT_PIPE=fifo,
                 XEMU_BASE_PATH=lease.bench_cache(), **env)
        cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,
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
            done, wall, ins, cyc, hashes = 0, 0.0, 0, 0, []
            for s in stops:
                i0, c0 = counters(proc.pid)
                t = time.time()
                q.cmd('xemu-frame-advance', frames=s - done)
                wall += time.time() - t
                i1, c1 = counters(proc.pid)
                ins, cyc, done = ins + i1 - i0, cyc + c1 - c0, s
                hashes.append(f'f{s}=' + ram_sha256(q, d)[:16])
            try:
                q.cmd('quit')
            except Exception:
                pass
            proc.wait(timeout=30)
        return {'fps': done / wall, 'mcyc': cyc / done / 1e6, 'mins': ins / done / 1e6,
                'ram': ' '.join(hashes)}
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('-n', '--pairs', type=int, default=4)
    p.add_argument('-f', '--frames', type=int, default=1200)
    p.add_argument('-t', '--tag', default='azurik-heavy-f1')
    p.add_argument('-i', '--input', action='append', default=[])
    p.add_argument('--hash-at', default='')
    p.add_argument('--icount', default='shift=2,align=off,sleep=off')
    p.add_argument('--out', default='')
    p.add_argument('arms', nargs='+', help='NAME=APP[:K=V,K=V]')
    a = p.parse_args()
    arms = []
    for spec in a.arms:
        name, rest = spec.split('=', 1)
        app, _, envs = rest.partition(':')
        arms.append((name, app, dict(kv.split('=', 1) for kv in envs.split(',') if kv)))
    res = {n: [] for n, _, _ in arms}
    out = open(a.out, 'a') if a.out else None
    for i in range(a.pairs):
        for name, app, env in arms:
            r = run_once(app, env, a, f'bench-det {name} pair {i + 1}/{a.pairs} '
                                      f'{a.tag} {a.frames if not a.hash_at else a.hash_at} frames')
            res[name].append(r)
            line = (f'{i + 1} {name}: {r["fps"]:.1f} fps  {r["mcyc"]:.3f} Mcyc/f  '
                    f'{r["mins"]:.3f} Mins/f  {r["ram"]}')
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
