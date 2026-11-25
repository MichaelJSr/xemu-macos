#!/usr/bin/env python3
"""First-use probe: Azurik casts or a streaming walk in real time, nsprof per cast or phase.

    cast-probe.py --app APP [--walk] [--config TOML] [--hdd QCOW2] [--state TAG]
                  [--casts N] [--every S] [--window N] [--settle S]
                  [--base DIR] [--keep DIR] [--windowed]
                  [--set TABLE.KEY=VALUE ...] [K=V ...]

Loads STATE paused in a clone of APP, then runs it in real time,
offscreen (XEMU_HEADLESS=1) unless --windowed, muted, with nsprof on, and
quits through QMP. A failed load, or an xemu that dies or exits non-zero,
is exit 1.

Casts (the default): STATE vanilla-rz-w1-dark (a dark cave's entrance)
on the Modkit's D98 fixtures: $AZURIK_FX/xemu.toml, its drive and
$AZURIK_FX/vanilla.iso (as bench-det --check d98; the toml's own
dvd_path is never used). After --settle (10) s it casts --casts (5)
times, --every (6) s apart: left trigger held, B pressed (Azurik's fire
armor: a light, a glow and particles, so its first casts build new
shaders and pipelines). Per
cast, summed over the --window (3) 1 s nsprof intervals after the press:
the longest frame (flip_gap max), shader_gen and pipeline_gen (events /
total / max) and their parts, tex_upload, LRU evictions, the async_*
counters of the async pipeline compile, and the slow-creation lines.

--walk: STATE azurik-stream-e2 on $XEMU_TEST_CONFIG, else
../fixtures/xemu.toml (bench-det's source). Settle 15 s, left stick
forward 20 s into unexplored space (first-visit streaming), rest 5 s,
back 20 s over the route just walked (assets resident: the built-in
control), with a QMP screendump 10 s into the forward phase, scored by
score_shot.py (exit 3 on an artifact). Per phase: the `info jit` TB
flush and invalidate deltas (the mechanism signal: 139 flushes in 20 s
forward was the v0.11 new-area lag; healthy is 0-1) and the nsprof sums
of the 5 s intervals whose summary printed in it. Then the KosmicKrisp
gate's C2 count: post-load intervals with a frame over 50 ms (the load's
own interval is left out and shown). Static savestate A/Bs can't see
first-visit costs, so the walk is the required probe for TB lifetime or
invalidation changes. C2 is a windowed measure (KK's copy-present path;
offscreen and icount runs pin MoltenVK unless XEMU_VK_DRIVER is set):
--walk --windowed XEMU_VK_DRIVER=kosmickrisp.

Caches: the run's base path (shader and pipeline caches, the prewarm
manifest) starts empty unless --base names a folder to clone in (cp -c),
e.g. one an earlier run kept with --keep: cold and warm are different
questions. --keep DIR keeps the log, the base and the walk's shot.

The config's HDD (or --hdd instead), EEPROM and disc are cloned per run
through bench-det's scratch(). --set TABLE.KEY=VALUE (repeatable,
TOML-typed) edits the scratch config through harness-config.py --set,
e.g. --set perf.async_pipeline_compile=false. K=V pairs go to xemu's
environment and win over the probe's own (XEMU_NV2A_NSPROF=1000 gives
the walk 1 s intervals). Launches go through xemu_lease.py (marker,
nice, slot, lease); a QMP reply that takes over 60 s (a wedged xemu)
is exit 1 with the stall diagnostics.
"""
import argparse, importlib.util, os, re, shutil, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, path))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


bd = _load('bench_det', 'scripts/bench-det.py')
nsprof = _load('nsprof_summarize', 'scripts/nsprof_summarize.py')

B_KEY = 79   # B on the D98 keyboard pad (and the fixtures')
PARTS = ('glsl_gen', 'spv_load', 'spv_reflect', 'glslang', 'vk_module', 'drv_pipeline')
ASYNC = ('submit', 'skip', 'install', 'warm', 'idle', 'zpass_sync', 'drop', 'fail', 'fallback')
SHOT_AT = 10.0   # seconds into the walk's forward phase


def cast_lines():
    return ['+0 trigger lt 1.000000', '+12 down %d' % B_KEY, '+18 up %d' % B_KEY,
            '+40 trigger lt 0.000000']


def walk_phases(settle):
    """(phase, left-stick y set as it starts or None, seconds)."""
    return (('settle', None, settle), ('forward', 1.0, 20.0), ('rest', 0.0, 5.0),
            ('backward', -1.0, 20.0))


def window(ivs):
    """nsprof_summarize intervals summed: counters (max of maxes), events, slow lines."""
    w = {'counters': {}, 'events': {}, 'slow': []}
    for iv in ivs:
        w['slow'] += iv['slow']
        for k, v in iv['counters'].items():
            t = w['counters'].setdefault(k, {'ev': 0, 'total_ms': 0.0, 'max_us': 0.0})
            t['ev'] += v['ev']
            t['total_ms'] += v['total_ms']
            t['max_us'] = max(t['max_us'], v['max_us'])
        for k, v in iv['events'].items():
            w['events'][k] = w['events'].get(k, 0) + v
    return w


def fmt(c):
    return f"{c['ev']}x {c['total_ms']:.1f} ms (max {c['max_us'] / 1e3:.1f})" if c else '-'


def summary(w):
    c, e = w['counters'], w['events']
    parts = ', '.join(f'{k} {c[k]["total_ms"]:.1f}' for k in PARTS if k in c)
    asy = ([f'build {fmt(c["async_build"])}'] if 'async_build' in c else []) + \
        [f'{k} {e["async_" + k]}' for k in ASYNC if e.get('async_' + k)]
    return (f'frame gap max {c.get("flip_gap", {}).get("max_us", 0) / 1e3:.1f} ms; '
            f'shader_gen {fmt(c.get("shader_gen"))}; pipeline_gen {fmt(c.get("pipeline_gen"))}; '
            f'tex_upload {c.get("tex_upload", {}).get("total_ms", 0):.1f} ms'
            + (f'; [{parts} ms]' if parts else '')
            + (f'; evict {e.get("shader_evict", 0)}/{e.get("pipeline_evict", 0)}'
               if e.get('shader_evict') or e.get('pipeline_evict') else '')
            + (f'; async {", ".join(asy)}' if asy else ''))


def nap(secs, proc):
    """Sleep, failing as soon as xemu exits."""
    end = time.time() + secs
    while True:
        if proc.poll() is not None:
            raise RuntimeError(f'xemu exited ({proc.returncode})')
        left = end - time.time()
        if left <= 0:
            return
        time.sleep(min(0.2, left))


def tb_counts(q):
    """(TB flush count, TB invalidate count) from HMP info jit, None where missing."""
    r = q.cmd('human-monitor-command', **{'command-line': 'info jit'}) or ''
    return tuple(int(m.group(1)) if m else None for m in
                 (re.search(rf'TB {k} count\s+(\d+)', r) for k in ('flush', 'invalidate')))


def delta(a, b):
    return f'+{b - a}' if a is not None and b is not None else '?'


def casts(a, q, proc, log):
    """Cast a.casts times; the log offset at each press."""
    offs = []
    nap(a.settle, proc)
    for _ in range(a.casts):
        offs.append(os.path.getsize(log))
        q.cmd('xemu-input-queue', lines=cast_lines())
        nap(a.every, proc)
    return offs


def walk(a, q, proc, log, shot):
    """Walk the phases; per boundary (phase, seconds, log offset, flushes, invalidations)."""
    marks = []
    for name, ly, secs in walk_phases(a.settle):
        marks.append((name, secs, os.path.getsize(log)) + tb_counts(q))
        if ly is not None:
            q.cmd('xemu-input-queue', lines=[f'axis ly {ly:.6f}'])
        if name != 'forward':
            nap(secs, proc)
            continue
        nap(SHOT_AT, proc)
        try:
            q.cmd('screendump', filename=shot, format='png')
        except RuntimeError as x:   # an error reply is not fatal (score_shot says SKIP)
            if isinstance(x, bd.RunFailed):   # no reply: its late answer would desync QMP
                raise
            print(f'cast-probe: screendump: {x}', file=sys.stderr)
        nap(secs - SHOT_AT, proc)
    marks.append(('end', 0.0, os.path.getsize(log)) + tb_counts(q))
    q.cmd('xemu-input-queue', lines=['clear'])
    return marks


def score(png):
    """score_shot.py's verdict line and exit code (3: an artifact)."""
    if not os.path.isfile(png):
        return 'screenshot: SKIP (no screendump)', 0
    r = subprocess.run([sys.executable, '-I', os.path.join(REPO, 'scripts/score_shot.py'), png],
                       stdout=subprocess.PIPE, text=True)
    return r.stdout.strip(), r.returncode


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--app', required=True, help='a bundle clone, never dist/xemu.app')
    p.add_argument('--walk', action='store_true', help='the streaming walk instead of the casts')
    p.add_argument('--config', default='', help='source config (default: see the docstring)')
    p.add_argument('--hdd', default='', help="a drive to clone instead of the config's")
    p.add_argument('--state', default='', help='vanilla-rz-w1-dark; --walk: azurik-stream-e2')
    p.add_argument('--casts', type=int, default=5)
    p.add_argument('--every', type=float, default=6.0)
    p.add_argument('--window', type=int, default=3, help='nsprof intervals summed per cast')
    p.add_argument('--settle', type=float, default=None, help='seconds before the casts (10) or walk (15)')
    p.add_argument('--base', default='', help='a base path to clone into the run (warm caches)')
    p.add_argument('--keep', default='', help="keep the log, the run's base path and the shot here")
    p.add_argument('--windowed', action='store_true')
    p.add_argument('--set', action='append', default=[], metavar='TABLE.KEY=VALUE',
                   help='a scratch-config key, through harness-config.py --set (repeatable)')
    p.add_argument('env', nargs='*', help='K=V for xemu')
    a = p.parse_args()
    if os.path.realpath(a.app) == os.path.realpath(os.path.join(REPO, 'dist/xemu.app')):
        sys.exit('cast-probe: --app must be a clone, never dist/xemu.app')
    binary = os.path.join(a.app, 'Contents/MacOS/xemu')
    if not os.access(binary, os.X_OK):
        sys.exit(f'cast-probe: no xemu bundle at {a.app}')
    if not bd.lease.knows_base_path(binary):
        sys.exit(f"cast-probe: {a.app} predates XEMU_BASE_PATH (its caches would go to the owner's pref dir)")
    if any('=' not in kv for kv in a.env):
        p.error('env arguments are K=V')
    env = dict(kv.split('=', 1) for kv in a.env)
    state = a.state or ('azurik-stream-e2' if a.walk else 'vanilla-rz-w1-dark')
    a.settle = a.settle if a.settle is not None else 15.0 if a.walk else 10.0
    fx = os.environ.get('AZURIK_FX', '')
    if a.config:
        cfg = a.config
    elif fx and not a.walk:
        cfg = os.path.join(fx, 'xemu.toml')
    elif a.walk or a.state:
        cfg = bd.CFG
    else:
        sys.exit('cast-probe: the casts need AZURIK_FX (the D98 fixtures folder) or --config')
    if not os.path.isfile(cfg):
        sys.exit(f'cast-probe: no source config at {cfg}')
    # The D98 disc is the folder's vanilla.iso, as for bench-det --check d98.
    d98 = bool(fx) and os.path.realpath(cfg) == os.path.realpath(os.path.join(fx, 'xemu.toml'))
    disc = os.path.join(fx, 'vanilla.iso') if d98 else bd.cfg_path('dvd_path', cfg) or ''
    if d98 and not os.path.isfile(disc):
        sys.exit(f"cast-probe: AZURIK_FX={fx} is not the Modkit's D98 fixtures folder "
                 '(xemu.toml, the drive, eeprom.bin, vanilla.iso): no vanilla.iso')
    if a.hdd and not os.path.isfile(a.hdd):
        sys.exit(f'cast-probe: no drive at {a.hdd}')
    bd.sweep_scratch()
    d = bd.scratch_dir()
    try:
        dvd = ''
        if os.path.isfile(disc):   # a clone, as for the drive: no fixture is opened in place
            dvd = f'{d}/dvd.iso'
            subprocess.run(['cp', '-c', disc, dvd], check=True)
        src = cfg
        if a.hdd:   # a copy naming --hdd, so scratch() clones it and never the config's drive
            src = f'{d}/src.toml'
            shutil.copyfile(cfg, src)
            bd.harness_config(src, d, os.path.abspath(a.hdd), bd.cfg_path('eeprom_path', cfg)
                              or os.path.join(os.path.dirname(cfg), 'eeprom.bin'))
        toml, pads, _, sock, log = bd.scratch(d, src, dvd, a.set)
        app = f'{d}/xemu.app'
        subprocess.run(['cp', '-Rc', a.app, app], check=True)
        base = f'{d}/base'
        if a.base:
            subprocess.run(['cp', '-Rc', a.base, base], check=True)
        os.makedirs(base, exist_ok=True)
        e = dict(os.environ, XEMU_BASE_PATH=base + '/',
                 XEMU_NV2A_NSPROF='5000' if a.walk else '1000')
        if not a.windowed:
            e['XEMU_HEADLESS'] = '1'
        e.update(env)
        cmd = [f'{app}/Contents/MacOS/xemu', '-config_path', toml, '-qmp', f'unix:{sock},server,nowait',
               '-S'] + pads
        what = f'{state} walk' if a.walk else f'{state} x{a.casts}'
        secs = 60 + a.settle + (45 if a.walk else a.casts * a.every)
        shot = f'{d}/shot-forward.png'
        marks, fail, rc, fp = [], '', None, None
        with bd.lease.popen(cmd, f'cast-probe {what}', secs, keep=env, env=e,
                            stdout=subprocess.DEVNULL, stderr=open(log, 'w')) as proc:
            try:
                bd.wait_log(log, 'xemu: ready', 1, proc)
                q = bd.QMP(sock, proc, 60)   # a wedged xemu: RunFailed, not a hang
                q.cmd('stop')
                r = q.cmd('human-monitor-command', **{'command-line': f'loadvm {state}'})
                if r and 'rror' in r:
                    raise RuntimeError(f'loadvm {state} on {cfg}: {r.strip()}')
                bd.wait_log(log, 'xemu: state loaded', 1, proc)
                q.cmd('cont')
                marks = walk(a, q, proc, log, shot) if a.walk else casts(a, q, proc, log)
                ru = bd._RusageV4()
                if bd._libproc.proc_pid_rusage(proc.pid, 4, bd.ctypes.byref(ru)) == 0:
                    fp = ru.phys_footprint / 2**20
                try:
                    q.cmd('quit')
                except bd.RunFailed:   # no reply in 60 s: wedged
                    raise
                except (RuntimeError, OSError):   # it can exit before replying
                    pass
                rc = proc.wait(timeout=60)
            except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as x:
                fail, rc = str(x) or type(x).__name__, proc.poll()
        if a.keep:
            os.makedirs(a.keep, exist_ok=True)
            shutil.copy(log, os.path.join(a.keep, 'log'))
            if os.path.isdir(os.path.join(a.keep, 'base')):
                shutil.rmtree(os.path.join(a.keep, 'base'))
            subprocess.run(['cp', '-Rc', base, os.path.join(a.keep, 'base')], check=True)
            if os.path.isfile(shot):
                shot = shutil.copy(shot, os.path.join(a.keep, 'shot-forward.png'))
        raw = open(log, 'rb').read()   # offsets are bytes
        if fail:
            tail = [ln for ln in raw.decode(errors='replace').splitlines() if not ln.startswith('nsprof:')]
            how = 'still running' if rc is None else f'exit {rc}'
            print(f'cast-probe: {fail} (xemu {how}); log tail:', *tail[-8:], sep='\n  ')
            sys.exit(1)

        def ivs(start, end=None):
            return nsprof.parse_lines(raw[start:end].decode(errors='replace').splitlines())
        load = os.getloadavg()
        drv = bd.vk_driver(log)
        print(f'cast-probe: {state}, ' + ('walk' if a.walk else f'{a.casts} casts {a.every:g} s apart')
              + f', {"warm" if a.base else "cold"} caches' + (f', {drv}' if drv else '')
              + (f', {" ".join(a.set + a.env)}' if a.set or a.env else '')
              + f', load {load[0]:.1f}' + (f', footprint {fp:.0f} MiB at the end' if fp else ''))
        status = 0
        if not a.walk:
            for i, off in enumerate(marks):
                w = window(ivs(off)[:a.window])
                print(f'  cast {i + 1}: {summary(w)}')
                for s in w['slow'][:6]:
                    print(f'      {s}')
        else:
            # One parse, cut at each boundary by the summaries printed before
            # it: an interval (and the slow lines printed during it) counts in
            # the phase its summary printed in, even across a boundary.
            every = ivs(0, marks[-1][2])
            cut = [len(ivs(0, m[2])) for m in marks]
            for (name, secs, _, fl, inv), nxt, i, j in zip(marks, marks[1:], cut, cut[1:]):
                w = window(every[i:j])
                print(f'  {name:8s} {secs:2.0f} s: TB flush {delta(fl, nxt[3])}, '
                      f'invalidate {delta(inv, nxt[4])}; {summary(w)}')
                for s in w['slow'][:6]:
                    print(f'      {s}')
            post = every[cut[0]:cut[-1]]
            gaps = [iv['counters'].get('flip_gap', {}).get('max_us', 0) / 1e3 for iv in post]
            if gaps:
                n = sorted(iv['secs'] for iv in post)[len(post) // 2]
                print(f'  C2: {sum(g > 50 for g in gaps[1:])} of {len(gaps) - 1} post-load {n:.0f} s '
                      f'intervals with a frame over 50 ms (worst {max(gaps[1:], default=0):.0f} ms; '
                      f'the load\'s interval, left out: {gaps[0]:.0f} ms)')
            verdict, src_rc = score(shot)
            print(f'  forward shot: {verdict}')
            status = 3 if src_rc == 3 else 0
        if rc != 0:
            print(f'cast-probe: xemu exited with {rc}')
            status = 1
        sys.exit(status)
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    main()
