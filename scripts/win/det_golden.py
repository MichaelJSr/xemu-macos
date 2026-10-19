#!/usr/bin/env python3
"""det_golden.py - one deterministic run of xemu.exe on Windows.

The Windows stand-in for scripts/bench-det.py, which is macOS-only (libproc
counters, AF_UNIX QMP, .app bundles). Same recipe as its run_once: copy the
template config's HDD and EEPROM into a scratch dir, write a scratch config
(muted, pad stubs from scripts/harness-config.py), start xemu.exe with
XEMU_HEADLESS=null under -S -icount shift=2,align=off,sleep=off, loadvm TAG
('-' boots cold), write the -i lines to the input pipe while paused, advance
to each --hash-at frame and hash guest RAM (QMP xemu-ram-digest; the first
16 hex digits of the sha256, as bench-det prints them).

  det_golden.py --exe X --tag azurik-heavy-f1 --hash-at 300,1200
  det_golden.py --exe X --tag azurik-stream-e2 --hash-at 600 -i "axis ly 1.0"
  det_golden.py --exe X --tag - --hash-at 900,1800

--set section.key=LITERAL edits the scratch config (audio.use_dsp_jit=false,
perf.hard_fpu=false); --env K=V adds a variable for xemu; --renderer
offscreen renders with Vulkan, hidden (XEMU_HEADLESS=1), and --shots saves a
PNG at each stop; --pages saves per-page hashes at each stop; --wav records
the APU output (XEMU_AUDIO_WAV) and hashes it. Artifacts go to --save. One
JSON line per run on stdout, appended to --out when given. Mcyc/frame comes
from QueryProcessCycleTime (whole process, TSC-rate cycles: CPU time, not
core clocks as on macOS, so it rises when hyperthread siblings are busy).
--pmc adds instructions and core cycles per frame over the advances, from a
kernel context-switch trace with hardware counters (elevated shell, Windows
Performance Toolkit's xperf): Minsns/frame doesn't move with host load.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
KIT = os.environ.get('XEMU_KIT') or next(   # as kit.ps1: the kit beside the repo, else C:\xemu-kit
    (d for d in (os.path.join(os.path.dirname(REPO), 'xemu-kit'), r'C:\xemu-kit')
     if os.path.isfile(os.path.join(d, 'SHA256SUMS.txt'))), os.path.join(os.path.dirname(REPO), 'xemu-kit'))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bw = _load('bench_win', os.path.join(REPO, 'scripts', 'bench-savestate-ab-win.py'))  # TomlEdit, lease
ctl = _load('xemu_ctl', os.path.join(HERE, 'xemu_ctl.py'))

_k32 = ctypes.WinDLL('kernel32', use_last_error=True)
_k32.OpenProcess.restype = ctypes.c_void_p
_k32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
_k32.QueryProcessCycleTime.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64))
_k32.CloseHandle.argtypes = (ctypes.c_void_p,)
_k32.SetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.c_size_t)


def cycles(h):
    c = ctypes.c_uint64(0)
    return c.value if h and _k32.QueryProcessCycleTime(h, ctypes.byref(c)) else 0


XPERF = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                     'Windows Kits', '10', 'Windows Performance Toolkit', 'xperf.exe')


def pmc_start(etl):
    """Kernel trace of context switches with the instructions-retired and core-cycle counters
    (needs an elevated shell and the Windows Performance Toolkit)."""
    subprocess.run([XPERF, '-on', 'PROC_THREAD+CSWITCH', '-pmc', 'InstructionRetired,UnhaltedCoreCycles',
                    'CSWITCH', 'strict', '-BufferSize', '1024', '-MinBuffers', '64', '-MaxBuffers', '1024',
                    '-f', etl], check=True, capture_output=True)


def pmc_stop(etl, pid):
    """Stop the trace and sum the counters over pid's threads: each Pmc event holds its CPU's
    running counts at a switch, so the delta to the CPU's previous switch belongs to the thread
    switched out. Returns (instructions, core cycles, events lost)."""
    subprocess.run([XPERF, '-stop'], check=True, capture_output=True)
    txt = etl + '.txt'
    subprocess.run([XPERF, '-i', etl, '-o', txt, '-a', 'dumper'], check=True, capture_output=True)
    last, pend, ins, cyc, lost = {}, None, 0, 0, None
    tag = '(%d)' % pid
    with open(txt, errors='replace') as f:
        for line in f:
            s = line.lstrip()
            if s.startswith('Pmc,'):
                p = s.split(',')
                pend = (int(p[3]), int(p[4])) if p[3].strip().isdigit() else None
            elif s.startswith('CSwitch,') and pend:
                p = s.split(',')
                cpu = p[16].strip()
                if cpu in last and tag in p[8].replace(' ', ''):
                    ins += pend[0] - last[cpu][0]
                    cyc += pend[1] - last[cpu][1]
                last[cpu] = pend
                pend = None
            elif lost is None and 'Events Lost:' in s:
                lost = int(s.split('Events Lost:')[1].split(',')[0])
    for p in (etl, txt):
        try:
            os.remove(p)
        except OSError:
            pass
    return ins, cyc, lost


free_port = bw.free_port


def run(a):
    stops = [int(x) for x in a.hash_at.split(',')]
    work = os.path.join(KIT, 'work')
    os.makedirs(work, exist_ok=True)
    os.makedirs(a.save, exist_ok=True)
    d = tempfile.mkdtemp(prefix='det.', dir=work)
    tag = os.path.basename(d)
    res = {'label': a.label, 'exe': a.exe, 'tag': a.tag, 'input': a.input,
           'set': a.set, 'env': a.env, 'renderer': a.renderer}
    proc = h = log = hdd = None
    rc, tracing = 1, False
    try:
        t = bw.TomlEdit(open(a.config, encoding='utf-8', errors='replace').read())
        hdd, eep = os.path.join(d, 'hdd.qcow2'), os.path.join(d, 'eeprom.bin')
        shutil.copyfile(t.get('sys.files', 'hdd_path'), hdd)
        shutil.copyfile(t.get('sys.files', 'eeprom_path'), eep)
        t.set('sys.files', 'hdd_path', bw.toml_path_literal(hdd))
        t.set('sys.files', 'eeprom_path', bw.toml_path_literal(eep))
        t.set('general', 'screenshot_dir', bw.toml_path_literal(d))
        t.set('display.window', 'fullscreen_on_startup', 'false')
        for s in a.set:
            key, _, lit = s.partition('=')
            section, _, name = key.rpartition('.')
            t.set(section, name, lit)
        toml = os.path.join(d, 'xemu.toml')
        with open(toml, 'w', encoding='utf-8') as f:
            f.write(t.render())
        pads = subprocess.run([sys.executable, '-I', os.path.join(REPO, 'scripts', 'harness-config.py'), toml],
                              capture_output=True, text=True, check=True).stdout.split()
        port, pipe = free_port(), 'xdet-%d' % os.getpid()
        env = bw.lease.environ()
        env['XEMU_HEADLESS'] = 'null' if a.renderer == 'null' else '1'
        env['XEMU_INPUT_PIPE'] = pipe
        env['XEMU_BASE_PATH'] = (os.environ.get('XEMU_BENCH_CACHE') or
                                 os.path.join(os.environ['LOCALAPPDATA'], 'xemu-fork-bench'))
        os.makedirs(env['XEMU_BASE_PATH'], exist_ok=True)
        wav = os.path.join(a.save, tag + '.wav')
        if a.wav:
            env['XEMU_AUDIO_WAV'] = wav
        for kv in a.env:
            k, _, v = kv.partition('=')
            env[k] = v
        cmd = [a.exe, '-name', 'xemu-fork-auto', '-config_path', toml,
               '-qmp', 'tcp:127.0.0.1:%d,server=on,wait=off' % port,
               '-S', '-icount', a.icount] + pads
        log = open(os.path.join(d, 'stderr.log'), 'w')
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log, cwd=d, env=env)
        h = _k32.OpenProcess(0x1000 | 0x0200, False, proc.pid)  # QUERY_LIMITED_INFORMATION | SET_INFORMATION
        if a.affinity:
            _k32.SetProcessAffinityMask(h, ctypes.c_size_t(int(a.affinity, 0)))
        q = ctl.QMP(port, 120)
        ctl.wait_ready(q, 180, proc)
        q.cmd('stop')
        if a.tag != '-':
            ctl.loadvm(q, a.tag, 120)
        if a.input:
            q.cmd('query-xemu-input')  # polls the pipe, so xemu creates it now
            name = '\\\\.\\pipe\\' + pipe
            for _ in range(100):
                try:
                    with open(name, 'wb', buffering=0) as f:
                        f.write(('\n'.join(a.input) + '\n').encode('ascii'))
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError('cannot open ' + name)
            time.sleep(0.3)
            res['queued'] = len(q.cmd('query-xemu-input').get('queued') or [])
        done, wall, cyc, hashes, dig = 0, 0.0, 0, [], {}
        etl = os.path.join(d, 'pmc.etl')
        if a.pmc:
            pmc_start(etl)
            tracing = True
        for s in stops:
            c0, t0 = cycles(h), time.time()
            adv = q.cmd('xemu-frame-advance', frames=s - done)
            wall += time.time() - t0
            cyc += cycles(h) - c0
            done = s
            if isinstance(adv, dict) and adv.get('stopped'):
                raise RuntimeError('advance to %d ended early: %r' % (s, adv))
            dig = q.cmd('xemu-ram-digest', pages=True) if a.pages else q.cmd('xemu-ram-digest')
            hashes.append('f%d=%s' % (s, dig['sha256'][:16]))
            res['hashes'] = ' '.join(hashes)  # kept if a later stop or the quit fails
            if a.pages:
                with open(os.path.join(a.save, '%s-f%d.pages' % (tag, s)), 'w') as f:
                    f.write(dig['page-hashes'])
            if a.shots:
                q.cmd('screendump', filename=os.path.join(a.save, '%s-f%d.png' % (tag, s)), format='png')
        res.update(hashes=' '.join(hashes), fps=round(done / wall, 1) if wall else None,
                   mcyc_per_frame=round(cyc / done / 1e6, 2) if done else None,
                   wall_s=round(wall, 1), ram_size=dig.get('ram-size'), run=tag)
        if a.pmc:
            tracing = False
            ins, core, lost = pmc_stop(etl, proc.pid)
            res.update(minsns_per_frame=round(ins / done / 1e6, 3), mcore_per_frame=round(core / done / 1e6, 2),
                       pmc_events_lost=lost)
        try:
            q.cmd('quit')
        except (EOFError, OSError):
            pass
        # A crash or a debug-build assert during teardown must not pass as a clean run: a
        # nonzero exit takes the failure path, which keeps the logs.
        code = proc.wait(timeout=180)
        res['exit_code'] = code
        if code:
            raise RuntimeError('xemu exited %#x after quit' % (code & 0xffffffff))
        if a.wav and os.path.exists(wav):
            with open(wav, 'rb') as f:
                res['wav_sha256'] = hashlib.sha256(f.read()).hexdigest()[:16]
        rc = 0
    except Exception as e:  # one JSON line either way
        res['error'] = '%s: %s' % (type(e).__name__, e)
    finally:
        if tracing:
            subprocess.run([XPERF, '-stop'], capture_output=True)
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=30)
        if h:
            _k32.CloseHandle(h)
        if log:
            log.close()
        if rc:
            for n in ('stderr.log', 'xemu.log'):
                p = os.path.join(d, n)
                if os.path.exists(p):
                    with open(p, errors='replace') as f:
                        res['log_tail_' + n] = f.read()[-2000:]
        if rc and hdd and not a.keep:
            try:  # keep the failed run's logs and config, drop its 2.2 GB HDD copy
                os.remove(hdd)
            except OSError:
                pass
        if a.keep or rc:
            res['dir'] = d
        else:
            shutil.rmtree(d, ignore_errors=True)
    line = json.dumps(res)
    print(line, flush=True)
    if a.out:
        with open(a.out, 'a', encoding='utf-8') as f:
            f.write(line + '\n')
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='xemu.exe to run')
    ap.add_argument('--config', default=os.environ.get('XEMU_TEST_CONFIG') or
                    os.path.join(KIT, 'work', 'xemu-fixtures.toml'), help='template config')
    ap.add_argument('-t', '--tag', default='azurik-heavy-f1', help="savestate tag; '-' boots cold")
    ap.add_argument('--hash-at', default='300', help='comma-separated frames to hash at')
    ap.add_argument('-i', '--input', action='append', default=[], help='pipe line written while paused')
    ap.add_argument('--icount', default='shift=2,align=off,sleep=off')
    ap.add_argument('--set', action='append', default=[], help='section.key=TOML literal')
    ap.add_argument('--env', action='append', default=[], help='K=V for xemu')
    ap.add_argument('--renderer', choices=('null', 'offscreen'), default='null')
    ap.add_argument('--shots', action='store_true', help='PNG screendump at each stop (use offscreen)')
    ap.add_argument('--pages', action='store_true', help='save per-page hashes at each stop')
    ap.add_argument('--wav', action='store_true', help='record the APU output and hash it')
    ap.add_argument('--save', default=os.path.join(KIT, 'results', 'det'), help='artifact folder')
    ap.add_argument('--pmc', action='store_true',
                    help='instructions and core cycles per frame from a kernel trace (elevated, Windows Performance Toolkit)')
    ap.add_argument('--affinity', default='',
                    help='CPU mask for xemu (e.g. 0x0f), to keep it off the cores a parallel build uses')
    ap.add_argument('--label', default='')
    ap.add_argument('--out', default='', help='append the JSON line to this file')
    ap.add_argument('--keep', action='store_true',
                    help='keep the scratch dir with its HDD copy (about 2.2 GB); a failed run keeps its logs and config only')
    return run(ap.parse_args())


if __name__ == '__main__':
    sys.exit(main())
