#!/usr/bin/env python3
"""Run the PGO training corpus against a profile-generate build.

    XEMU_PGO=generate ./build.sh
    rm -f pgo/*.profraw          # keep pgo/default.profdata until the merge
    cp -Rc dist/xemu.app /tmp/xemu-pgo.app   # never run dist/xemu.app itself
    scripts/pgo-train.py /tmp/xemu-pgo.app
    XEMU_PGO=use ./build.sh      # merges pgo/*.profraw into default.profdata

The corpus (each session its own process, ending in a clean QMP quit so
the counters are written and merged online into pgo/*.profraw):
  - boot to Azurik's intro, 90 s, windowed
  - azurik-heavy-f1 x2, azurik-heavy-w2 x2, F5 vm-20260704032357, 80 s
    each after loadvm, windowed; the second heavy-f1 on KosmicKrisp
    (XEMU_VK_DRIVER=kosmickrisp: an arm64 bundle on macOS 26), the rest
    on Auto (MoltenVK), so the opt-in driver's paths get profile data
    too
  - an azurik-stream-e2 walk, 60 s, windowed
  - deterministic (icount, null renderer): azurik-heavy-f1 3000 frames
    and a 1200-frame azurik-stream-e2 walk
Windowed sessions open a muted, unfocused window on a fixture clone. Pass
--only det|windowed to run half of it.
"""
import argparse, importlib.util, os, subprocess, sys, tempfile, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('bench_det', os.path.join(HERE, 'bench-det.py'))
bd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bd)

KK = {'XEMU_VK_DRIVER': 'kosmickrisp'}
WINDOWED = [('-', 90, None, {}), ('azurik-heavy-f1', 80, None, {}),
            ('azurik-heavy-f1', 80, None, KK)] + [(t, 80, None, {}) for t in (
    'azurik-heavy-w2', 'azurik-heavy-w2', 'vm-20260704032357')] + [
    ('azurik-stream-e2', 60, 'axis ly 1.0', {})]
DET = [('azurik-heavy-f1', 3000, []), ('azurik-stream-e2', 1200, ['axis ly 1.0'])]


def windowed(app, tag, secs, walk, extra):
    d = tempfile.mkdtemp(prefix='xpgo.', dir='/tmp')
    try:
        toml, pads, fifo, sock, log = bd.scratch(d)
        cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,
               '-qmp', f'unix:{sock},server,nowait'] + pads
        env = dict(os.environ, XEMU_INPUT_PIPE=fifo, XEMU_BASE_PATH=bd.lease.bench_cache(),
                   **extra)
        env.pop('XEMU_HEADLESS', None)
        with bd.lease.popen(cmd, f'pgo-train {tag}', secs + 60, keep=extra, env=env,
                            stdout=subprocess.DEVNULL, stderr=open(log, 'w')) as proc:
            bd.wait_log(log, 'xemu: ready', 1, proc)
            q = bd.QMP(sock, proc)
            if tag != '-':
                time.sleep(5)
                q.cmd('human-monitor-command', **{'command-line': f'loadvm {tag}'})
                bd.wait_log(log, 'xemu: state loaded', 1, proc)
            if walk:
                fd = os.open(fifo, os.O_WRONLY)
                os.write(fd, (walk + '\n').encode())
                os.close(fd)
            time.sleep(secs)
            try:
                q.cmd('quit')
            except Exception:
                pass
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                print(f'  {tag}: killed (profile for this session lost)', flush=True)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('app', help='bundle built with XEMU_PGO=generate')
    p.add_argument('--only', choices=('det', 'windowed'))
    a = p.parse_args()
    if a.only != 'det':
        for tag, secs, walk, extra in WINDOWED:
            print(f'windowed {tag} {secs}s' + ''.join(f' {k}={v}' for k, v in extra.items()),
                  flush=True)
            windowed(a.app, tag, secs, walk, extra)
    if a.only != 'windowed':
        for tag, frames, inp in DET:
            print(f'det {tag} {frames} frames', flush=True)
            r = bd.run_once(a.app, {}, argparse.Namespace(
                tag=tag, frames=frames, input=inp, hash_at='', renderer='null',
                icount='shift=2,align=off,sleep=off'))
            print(f'  {r["fps"]:.1f} fps {r["ram"]}', flush=True)


if __name__ == '__main__':
    main()
