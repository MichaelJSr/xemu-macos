#!/usr/bin/env python3
"""First-use hitch probe: casts in real time, compile and frame-gap counters per cast.

    cast-probe.py [--app APP] [--config TOML] [--hdd QCOW2] [--state TAG]
                  [--casts N] [--every S] [--base DIR] [--windowed]
                  [--keep DIR] [K=V ...]

Loads STATE (default vanilla-rz-w1-dark, an Azurik D98 state at a dark
cave's entrance) in a clone of APP, offscreen (XEMU_HEADLESS=1) unless
--windowed, real time, muted, with XEMU_NV2A_NSPROF=1000, then casts N
times, S seconds apart: left trigger held, B pressed (Azurik's fire
armor: a light, a glow and particles, so its first casts build new
shaders and pipelines). Per cast it prints the nsprof seconds after the
press: shader_gen and pipeline_gen (events / total / max) and their
parts, flip_gap max (the longest frame), the slow-creation lines, LRU
evictions, and the run's load average.

The run's base path (shader and pipeline caches) is empty unless --base
names a folder to clone into it (cp -c), e.g. a base an earlier probe
kept with --keep: cold and warm first casts are different questions.
The config (default $XEMU_TEST_CONFIG, else ../fixtures/xemu.toml) and
HDD (default the config's) are cloned per run, as bench-det does; K=V
pairs go to xemu's environment. Launches go through xemu_lease.py
(marker, nice, slot, lease).
"""
import argparse, importlib.util, os, re, shutil, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_spec = importlib.util.spec_from_file_location('bench_det', os.path.join(REPO, 'scripts/bench-det.py'))
bd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bd)

B_KEY = 79   # the D98 keyboard pad's B


def cast_lines():
    return ['+0 trigger lt 1.000000', '+12 down %d' % B_KEY, '+18 up %d' % B_KEY,
            '+40 trigger lt 0.000000']


def scratch(cfg_src, hdd_src, d):
    eeprom = re.search(r"(?m)^eeprom_path = '(.*)'", open(cfg_src).read()).group(1)
    subprocess.run(['cp', '-c', hdd_src, f'{d}/hdd.qcow2'], check=True)
    subprocess.run(['cp', '-c', eeprom, f'{d}/eeprom.bin'], check=True)
    toml = f'{d}/xemu.toml'
    shutil.copyfile(cfg_src, toml)
    # The clones, screenshots in d, fullscreen off, muted, pads stubbed.
    pads = subprocess.run([sys.executable, '-I', os.path.join(REPO, 'scripts/harness-config.py'), toml,
                           '--workdir', d, '--hdd', f'{d}/hdd.qcow2', '--eeprom', f'{d}/eeprom.bin'],
                          stdout=subprocess.PIPE, text=True, check=True).stdout.split()
    return toml, pads


def intervals(text):
    """nsprof intervals in text: {'fps': f, name: {'ev', 'ms', 'max_ms'} | count, 'slow': [...]}."""
    out, cur = [], None
    for line in text.splitlines():
        m = re.match(r'nsprof: ([\d.]+)s interval, (\d+) flips \(([\d.]+)/s\)', line)
        if m:
            cur = {'fps': float(m.group(3)), 'slow': []}
            out.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r'nsprof:\s+(\S+)\s+ev=(\d+)\s+total=\s*([\d.]+)ms .*max=\s*([\d.]+)us', line)
        if m:
            cur[m.group(1)] = {'ev': int(m.group(2)), 'ms': float(m.group(3)),
                               'max_ms': float(m.group(4)) / 1e3}
            continue
        m = re.match(r'nsprof:\s+(\S+)\s+ev=(\d+)\s+per_flip', line)
        if m:
            cur[m.group(1)] = int(m.group(2))
            continue
        if line.startswith('nsprof: slow '):
            cur['slow'].append(line[len('nsprof: '):])
    return out


def window(ivs, n):
    """Sum the first n intervals: totals, events, maxes, slow lines."""
    s = {'slow': []}
    for iv in ivs[:n]:
        for k, v in iv.items():
            if k == 'fps':
                continue
            if k == 'slow':
                s['slow'] += v
            elif isinstance(v, dict):
                t = s.setdefault(k, {'ev': 0, 'ms': 0.0, 'max_ms': 0.0})
                t['ev'] += v['ev']
                t['ms'] += v['ms']
                t['max_ms'] = max(t['max_ms'], v['max_ms'])
            else:
                s[k] = s.get(k, 0) + v
    return s


def fmt(c):
    return f"{c['ev']}x {c['ms']:.1f} ms (max {c['max_ms']:.1f})" if c else '-'


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--app', default=os.path.join(REPO, 'dist/xemu.app'))
    p.add_argument('--config', default=os.environ.get('XEMU_TEST_CONFIG') or
                   os.path.join(os.path.dirname(REPO), 'fixtures', 'xemu.toml'))
    p.add_argument('--hdd', default='')
    p.add_argument('--state', default='vanilla-rz-w1-dark')
    p.add_argument('--casts', type=int, default=5)
    p.add_argument('--every', type=float, default=6.0)
    p.add_argument('--window', type=int, default=3, help='nsprof seconds summed per cast')
    p.add_argument('--settle', type=float, default=10.0)
    p.add_argument('--base', default='', help='a base path to clone into the run (warm caches)')
    p.add_argument('--keep', default='', help='keep the log and the run\'s base path here')
    p.add_argument('--windowed', action='store_true')
    p.add_argument('env', nargs='*', help='K=V for xemu')
    a = p.parse_args()
    if os.path.realpath(a.app) == os.path.realpath(os.path.join(REPO, 'dist/xemu.app')):
        sys.exit('cast-probe: --app must be a clone, never dist/xemu.app')
    hdd = a.hdd or re.search(r"(?m)^hdd_path = '(.*)'", open(a.config).read()).group(1)
    env = dict(kv.split('=', 1) for kv in a.env)
    d = tempfile.mkdtemp(prefix='fcast.', dir='/tmp')
    try:
        toml, pads = scratch(a.config, hdd, d)
        app = f'{d}/xemu.app'
        subprocess.run(['cp', '-Rc', a.app, app], check=True)
        base = f'{d}/base'
        if a.base:
            subprocess.run(['cp', '-Rc', a.base, base], check=True)
        os.makedirs(base, exist_ok=True)
        sock, log = f'{d}/qmp.sock', f'{d}/log'
        e = dict(os.environ, XEMU_BASE_PATH=base + '/', XEMU_NV2A_NSPROF='1000')
        if not a.windowed:
            e['XEMU_HEADLESS'] = '1'
        e.update(env)
        cmd = [f'{app}/Contents/MacOS/xemu', '-config_path', toml, '-qmp', f'unix:{sock},server,nowait',
               '-S'] + pads
        offs = []
        with bd.lease.popen(cmd, f'cast-probe {a.state} x{a.casts}', 60 + a.settle + a.casts * a.every,
                            keep=env, env=e, stdout=subprocess.DEVNULL, stderr=open(log, 'w')) as proc:
            bd.wait_log(log, 'xemu: ready', 1, proc)
            q = bd.QMP(sock, proc)
            q.cmd('stop')
            r = q.cmd('human-monitor-command', **{'command-line': f'loadvm {a.state}'})
            if r and 'rror' in r:
                sys.exit(f'cast-probe: loadvm {a.state}: {r.strip()}')
            bd.wait_log(log, 'xemu: state loaded', 1, proc)
            q.cmd('cont')
            time.sleep(a.settle)
            for _ in range(a.casts):
                offs.append(os.path.getsize(log))
                q.cmd('xemu-input-queue', lines=cast_lines())
                time.sleep(a.every)
            ru = bd._RusageV4()
            fp = (ru.phys_footprint / 2**20
                  if bd._libproc.proc_pid_rusage(proc.pid, 4, bd.ctypes.byref(ru)) == 0 else None)
            try:
                q.cmd('quit')
            except Exception:
                pass
            proc.wait(timeout=60)
        text = open(log, errors='replace').read()
        load = os.getloadavg()
        print(f'cast-probe: {a.state}, {a.casts} casts {a.every:g} s apart, '
              f'{"warm" if a.base else "cold"} caches, load {load[0]:.1f}'
              + (f', footprint {fp:.0f} MiB at the end' if fp else ''))
        for i, off in enumerate(offs):
            w = window(intervals(text[off:]), a.window)
            parts = ', '.join(f'{k} {w[k]["ms"]:.1f}' for k in
                              ('glsl_gen', 'spv_load', 'spv_reflect', 'glslang', 'vk_module', 'drv_pipeline')
                              if k in w)
            print(f'  cast {i + 1}: frame gap max {w.get("flip_gap", {}).get("max_ms", 0):.1f} ms; '
                  f'shader_gen {fmt(w.get("shader_gen"))}; pipeline_gen {fmt(w.get("pipeline_gen"))}; '
                  f'tex_upload {w.get("tex_upload", {}).get("ms", 0):.1f} ms'
                  + (f'; [{parts} ms]' if parts else '')
                  + (f'; evict {w.get("shader_evict", 0)}/{w.get("pipeline_evict", 0)}'
                     if w.get('shader_evict') or w.get('pipeline_evict') else ''))
            for s in w['slow'][:6]:
                print(f'      {s}')
        if a.keep:
            os.makedirs(a.keep, exist_ok=True)
            shutil.copy(log, os.path.join(a.keep, 'log'))
            if os.path.isdir(os.path.join(a.keep, 'base')):
                shutil.rmtree(os.path.join(a.keep, 'base'))
            subprocess.run(['cp', '-Rc', base, os.path.join(a.keep, 'base')], check=True)
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    main()
