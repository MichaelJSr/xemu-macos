#!/usr/bin/env python3
"""Deterministic-mode A/B with load-robust host counters.

    bench-det.py [-n PAIRS] [-f FRAMES] [-t TAG] [-i LINE]... [--hash-at F,F]
                 [--icount ARGS] [--renderer null|offscreen] [--out FILE]
                 [--shot DIR] [--dvd ISO] [--timeout-scale X] [--bundle APP]
                 NAME=APP[:K=V,K=V] ...

Each run clones the source config ($XEMU_TEST_CONFIG, else
../fixtures/xemu.toml) and the HDD and EEPROM it names into a fresh temp
dir (muted, pad stubs via harness-config.py), starts APP headless with the
null renderer under `-S -icount shift=2,align=off,sleep=off`, loads TAG
(`-` boots cold), writes the -i pipe lines, advances FRAMES frames and
hashes guest RAM (sha256[:16]: QMP xemu-ram-digest, or xemu-dump-ram's
file on builds without it). Arms run interleaved, PAIRS times.

APP is a bundle clone, or a bare binary (new=build/qemu-system-i386),
which runs in a temp clone of --bundle (default dist/xemu.app, only ever
cloned) made by scripts/refresh-clone.sh. An APP inside dist/ is refused.
Each arm's identity prints first: the version, commit and build date
embedded in the binary (`= HEAD` when it is the checkout's), its
sha256[:12], arch, debug or release, and the PGO state where it shows
(an instrumented binary, or a bare binary's build.ninja).

A run fails, and keeps its log as /tmp/xbd-fail.<pid>.<label>.log,
when xemu exits nonzero after quit, or a QMP reply takes over
30 + FRAMES/20 s (the failure line names the xemu's priority, its CPU
against runnable seconds and the load). --timeout-scale X multiplies
that and the start-up waits (default 1; 4 for an arm whose binary is a
debug or PGO-instrumented build, pgo-train's runs included). A run's
violations=/VIOLATION/assert/refute log lines (a refuter arm's counts,
an assert) print under its line; a VIOLATION event, a nonzero
violations= count, a refute line's nonzero misses= or unset=, or a
tlb-wclean miss fails the run, so a refuter arm beside new=
(ref=build/qemu-system-i386:XEMU_X87_REFUTE=1) is pass/fail. So does
xemu's complaint about an arm's own config key (unrecognized, wrong
type, invalid value: it ran on the default).

--renderer offscreen runs Vulkan instead (XEMU_HEADLESS=1) on a portable
clone of APP whose shader and pipeline caches stay in the run dir (cold
every run). Each line (and --timeline's) then names the driver from the
run's `- Driver:` line, and bench-det stops when it isn't the one the arm
asked for: MoltenVK (the deterministic pin) without XEMU_VK_DRIVER,
KosmicKrisp or MoltenVK with XEMU_VK_DRIVER=kosmickrisp|moltenvk. Surface
downloads write rendered pixels into guest RAM, so offscreen hashes are
per driver.

An arm's K=V list splits at a comma only where NAME= follows it, so a
value may hold commas as long as none is followed by NAME=:
kk=/tmp/e.app:XEMU_VK_DRIVER=kosmickrisp,MESA_KK_DISABLE_WORKAROUNDS=2,6,17
A K with a dot is a config key, written into each run's scratch toml by
`harness-config.py --set` (TABLE.KEY=VALUE, the last dot splits them;
checked against config_spec.yml), e.g.
off=/tmp/e.app:perf.async_pipeline_compile=false

--shot DIR saves each run's picture at its final stop (a rendered one
needs --renderer offscreen: the null renderer's is the VGA scanout, mostly
blank): after the last
xemu-frame-advance and the RAM hash, a QMP screendump (PNG) to
DIR/<arm>-<pair>-f<N>.png (--check: DIR/<arm>-<run>-f<N>.png), then
quit. Never at an earlier stop: a screendump can finish the frame
(pgraph_vk_finish) and add a split, which moves later report sums and
pixels. Its sha256[:12] prints as png=, beside the RAM hash, and never
counts as a RAM difference. Compare pictures only from the same
-f/--hash-at schedule, by pixel-difference counts rather than sha (an
f900 picture of a 900,1800 schedule is a separate `-f 900 --shot` run,
without --hash-at, whose last stop would set the picture). At the end,
each arm's pictures are diffed against the first arm's of the same run
(scripts/score_shot.py's diff: differing pixels, those off by more than
32, PSNR and the KK bar).
--dvd ISO passes -dvd_path ISO to every run (no clone: pass a clone).

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

Each xemu runs as scripts/xemu_lease.py says: `nice -n 10` (taken out
of PRIO_DARWIN_BG, named on stderr if PRI 4 stays), the argv marker `-name xemu-fork-auto`, one of
XEMU_FORK_SLOTS (4) slots, a lease in /tmp/xemu-leases, XEMU_BASE_PATH
set to the bench cache, and none of the automation variables this
process inherited (XEMU_RAM_SHM, XEMU_VK_DRIVER, ...; an arm's own K=V
stays, even when it equals the exported value).

Example: bench-det.py -n 4 base=/tmp/b.app new=/tmp/e.app
(an arm can add env: new=/tmp/e.app:XEMU_GUEST_MO=1)

--check goldens NAME=APP ...: runs every golden of scripts/det-goldens.json
(heavy-f1 to 300 and 1200, stream-e2 600 walking, cold boot 900 and 1800),
each in a fresh process, up to -j (default XEMU_FORK_SLOTS, 4) at once,
and asserts the hashes for the binary's arch and the config's mem_limit
(an arm's sys.mem_limit key wins); arms with no golden key (a refuter in
their env, another sys.* config key) only have to agree.
--check tags:FILE runs each tag in FILE (one a line) for -f frames
(default 300) and asserts the arms agree. --check d98 runs tests/azurik
pack 03 (the four scenes of tests/azurik/03-heavy-scenes/scenes.json, RAM
at +300 and +600, the fight scene on its first candidate's 70 lines) on
the Modkit's D98 fixtures: AZURIK_FX names their folder (never a
committed path; unset, d98 prints `skipped`); its xemu.toml is each run's
source config, and one cp -c clone of its vanilla.iso per batch is every
run's -dvd_path. Its hashes are asserted from det-goldens.json's "d98"
table. A comma list runs its sets in one batch; the per-change gate is
one arm, `--check goldens,d98 new=build/qemu-system-i386`. One line per
run, then exit 0 (all OK), 2 (arms disagree), 4 (a golden moved) or 1 (a
run failed, or the sets gave no runs: d98 alone without AZURIK_FX, an
empty tags file). Wall and cycle numbers from parallel runs contend:
--check is a gate, not an A/B.

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
--from-state TAG boots the harness's boot-build path instead of power-on:
launch on the config's disc, loadvm TAG (paused), put --dvd in the tray
(blockdev-change-medium, as the Azurik harness's reset_into does),
run --hdd-io, system_reset, then the same phases from the reset; a
"load" row (no frames) times the loadvm, swap and reset.
Example: bench-det.py --timeline -n 2 base=/tmp/a.app new=/tmp/b.app
         bench-det.py --timeline --from-state vanilla-ig-w1 --dvd grown.iso -n 20 base=/tmp/a.app
"""
import argparse, bisect, contextlib, csv, ctypes, hashlib, importlib.util, json, math
import os, re, shutil
import socket, statistics, subprocess, sys, tempfile, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The source config, as for every harness: $XEMU_TEST_CONFIG, else
# ../fixtures/xemu.toml beside the checkout. Its hdd_path / eeprom_path /
# dvd_path name the files to clone (else the same names beside it).
CFG = os.environ.get('XEMU_TEST_CONFIG') or os.path.join(
    os.path.dirname(REPO), 'fixtures', 'xemu.toml')
_spec = importlib.util.spec_from_file_location(
    'xemu_lease', os.path.join(REPO, 'scripts/xemu_lease.py'))
lease = importlib.util.module_from_spec(_spec)  # marker, nice, slot, lease, bench cache, rusage
_spec.loader.exec_module(lease)
_RusageV4, _libproc = lease.RusageV4, lease._libproc   # cast-probe reads these
_TICK_S, _Timebase, _tb = lease.TICK_S, lease._Timebase, getattr(lease, '_tb', None)   # importers


class RunFailed(RuntimeError):
    """A run that failed: its message names why (and the kept log)."""


def counters(pid, times=False):
    """(instructions, cycles) of the whole process; with times, also CPU and
    runnable seconds (runnable = on a core or waiting for one, all threads)."""
    r = lease.rusage(pid)
    if r is None:
        raise OSError('proc_pid_rusage failed')
    if times:
        return (r.instructions, r.cycles, (r.user_time + r.system_time) * lease.TICK_S,
                r.runnable_time * lease.TICK_S)
    return r.instructions, r.cycles


def stalled(pid, what):
    """what, plus why a run may stall: the xemu's priority (4: PRIO_DARWIN_BG),
    its CPU against runnable seconds, and the load."""
    pri = subprocess.run(['ps', '-o', 'pri=', '-p', str(pid)], capture_output=True,
                         text=True).stdout.strip() or '?'
    t = lease.cpu_runnable(pid)
    load = os.getloadavg()
    return (f'{what} (xemu pid {pid} pri {pri}'
            + (f', {t[0]:.1f} s CPU, {t[1]:.1f} s runnable' if t else '')
            + f'; load {load[0]:.1f}; --timeout-scale raises the limit)')


class QMP:
    def __init__(self, path, proc, timeout=None):
        """timeout: seconds any one reply may take (None: no limit), then
        RunFailed."""
        self.s = socket.socket(socket.AF_UNIX)
        self.proc, self.timeout = proc, timeout
        for _ in range(1200):
            try:
                self.s.connect(path)
                break
            except OSError:
                if proc.poll() is not None:
                    raise RuntimeError('xemu exited before QMP came up')
                time.sleep(0.05)
        self.f = self.s.makefile('rw')
        self.events = []
        self._line('the QMP greeting')
        self.cmd('qmp_capabilities')

    def _line(self, what):
        self.s.settimeout(self.timeout)
        try:
            return self.f.readline()
        except socket.timeout:
            raise RunFailed(stalled(self.proc.pid, f'{what}: no reply in {self.timeout:g} s')) \
                from None

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self._line(name) or '{"error": "EOF"}')
            if 'event' in r:
                self.events.append(r['event'])
                continue
            if 'error' in r:
                raise RuntimeError(f'{name}: {r["error"]}')
            return r.get('return')

    def wait_event(self, name, timeout=30):
        """Until event name arrives (or came during an earlier command)."""
        end = time.time() + timeout
        self.s.settimeout(timeout)
        try:
            while name not in self.events:
                if time.time() > end:
                    raise RuntimeError(f'no {name} event')
                r = json.loads(self.f.readline() or '{"error": "EOF"}')
                if 'error' in r:
                    raise RuntimeError(f'waiting for {name}: {r["error"]}')
                if 'event' in r:
                    self.events.append(r['event'])
        finally:
            self.s.settimeout(None)
        self.events.remove(name)


def wait_log(log, text, count, proc, timeout=120):
    end = time.time() + timeout
    while open(log, errors='replace').read().count(text) < count:
        if proc.poll() is not None:
            raise RunFailed(f'xemu exited {proc.returncode} before "{text}"')
        if time.time() > end:
            raise RunFailed(stalled(proc.pid, f'no "{text}" in {timeout:g} s'))
        time.sleep(0.01)


def cfg_path(key, cfg=None):
    """A path the source config (default CFG) names (hdd_path, eeprom_path,
    dvd_path), or None."""
    m = re.search(rf"""(?m)^\s*{key}\s*=\s*['"](.*)['"]\s*$""", open(cfg or CFG).read())
    return m and m.group(1)


def harness_config(toml, d, hdd, eeprom, dvd='', sets=()):
    """scripts/harness-config.py --workdir d on the scratch toml (the clones,
    screenshots in d, fullscreen off, muted, pads stubbed; sets: config
    keys, a {TABLE.KEY: VALUE} dict or TABLE.KEY=VALUE strings): its pad
    args."""
    sets = [f'{k}={v}' for k, v in sets.items()] if isinstance(sets, dict) else list(sets)
    return subprocess.run([sys.executable, '-I', os.path.join(REPO, 'scripts/harness-config.py'),
                           toml, '--workdir', d, '--hdd', hdd, '--eeprom', eeprom]
                          + (['--dvd', os.path.abspath(dvd)] if dvd else [])
                          + [x for kv in sets for x in ('--set', kv)],
                          stdout=subprocess.PIPE, text=True, check=True).stdout.split()


def scratch(d, cfg=None, dvd='', sets=()):
    """Clone the source config's (default CFG) HDD, EEPROM and config (muted,
    pads stubbed; dvd_path = dvd when given; sets as harness_config's) into
    d."""
    cfg = cfg or CFG
    for key, name, dst in (('hdd_path', 'xbox_hdd.qcow2', 'hdd.qcow2'),
                           ('eeprom_path', 'eeprom.bin', 'eeprom.bin')):
        src = cfg_path(key, cfg) or os.path.join(os.path.dirname(cfg), name)
        subprocess.run(['cp', '-c', src, os.path.join(d, dst)], check=True)
    toml = os.path.join(d, 'xemu.toml')
    shutil.copyfile(cfg, toml)
    pads = harness_config(toml, d, f'{d}/hdd.qcow2', f'{d}/eeprom.bin', dvd, sets)
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


def renderer_target(app, a, d):
    """(app, base path, XEMU_HEADLESS) for a run in d. Offscreen Vulkan
    writes shader and pipeline caches to the base path: keep them in d
    (cold every run). The portable clone covers binaries that predate
    XEMU_BASE_PATH."""
    if a.renderer != 'offscreen':
        return app, lease.bench_cache(), 'null'
    clone = os.path.join(d, 'xemu.app')
    subprocess.run(['cp', '-Rc', app, clone], check=True)
    open(os.path.join(clone, 'Contents/Resources/xemu.toml'), 'w').close()
    return clone, d, '1'


def run_env(env, headless, fifo, base):
    """The run's environment: the arm's K=V win over bench-det's own."""
    e = dict(os.environ, XEMU_HEADLESS=headless, XEMU_INPUT_PIPE=fifo, XEMU_BASE_PATH=base)
    e.update(env)
    return e


def vk_driver(log):
    """The `- Driver: <name>, <info> (id N)` of the run, or '' (null renderer)."""
    m = re.search(r'(?m)^- Driver: (.*?) \(id \d+\)$', open(log, errors='replace').read())
    return m.group(1) if m else ''


def check_driver(name, env, driver):
    """An offscreen run's driver must be the one its arm asked for: MoltenVK
    (the deterministic pin) without XEMU_VK_DRIVER, KosmicKrisp or MoltenVK
    for those names (a dylib path is taken as is)."""
    want = env.get('XEMU_VK_DRIVER') or 'moltenvk'
    expect = {'kosmickrisp': 'KosmicKrisp', 'moltenvk': 'MoltenVK'}.get(want)
    if driver and expect and not driver.startswith(expect):
        sys.exit(f'bench-det: {name} ran {driver}, not {expect} '
                 f'(XEMU_VK_DRIVER={env.get("XEMU_VK_DRIVER", "")!r}; '
                 'deterministic runs without it must pin MoltenVK)')


LIVE = set()   # running xemus (--check kills them on an interrupt)
STOP = threading.Event()   # set on an interrupt: the killed runs keep no logs
# Log lines a run prints under its line: violation counts, asserts, the
# refuters' reports (XEMU_TLB_WCLEAN_REFUTE's `refute ... misses=N unset=N`
# and its `tlb-wclean miss` lines, XEMU_TLB_REVAL_REFUTE's last words
# before its abort). BAD fails the run: a VIOLATION event, a nonzero
# violations=, misses= or unset= count on a refute line, a tlb-wclean miss.
NOTE = re.compile(r'(?i)violation|assert|refute|tlb-wclean miss')
BAD = re.compile(r'VIOLATION(?!S)|(?i:violations)=[1-9]|refute.*\b(?:misses|unset)=[1-9]'
                 r'|tlb-wclean miss|tlb-reval refute: (?:kept|a cross-page)')
# genconfig's complaints about a key (subprojects/genconfig/cnode.h): one
# about an arm's own config key fails the run (xemu ran on the default).
GENCONFIG = re.compile(r"(?:unrecognized|incorrect type for|invalid value for) key '([^']*)'")


def split_env(env):
    """An arm's K=V as (environment, config keys): a K with a dot is a
    TABLE.KEY for harness-config.py --set."""
    return ({k: v for k, v in env.items() if '.' not in k},
            {k: v for k, v in env.items() if '.' in k})


def log_notes(log, cap=12, keys=()):
    """(the log's NOTE lines, and genconfig's complaints about a config key
    in keys, at most cap; True if one fails the run: BAD, or a key in keys
    xemu didn't take)."""
    try:
        text = open(log, errors='replace').read().splitlines()
    except OSError:
        return [], False
    lines, bad = [], False
    for x in text:
        g = GENCONFIG.search(x)
        if g and g.group(1) in keys:
            lines.append(x)
            bad = True
        elif NOTE.search(x):
            lines.append(x)
            bad = bad or bool(BAD.search(x))
    if len(lines) > cap:
        lines = lines[:cap] + [f'... {len(lines) - cap} more']
    return lines, bad


def keep_log(e, log, label, keys=()):
    """e's message; the log, unless an interrupt killed the run, moves to
    /tmp/xbd-fail.<pid>.<label>.log, named with its log_notes lines (keys
    as there) and, when there are none or xemu exited or hung up, its last
    lines."""
    msg = str(e) or type(e).__name__
    if STOP.is_set() or not os.path.exists(log):
        return msg
    notes, _ = log_notes(log, keys=keys)
    if not notes or re.search(r'EOF|exited', msg):
        with contextlib.suppress(OSError):
            tail = [x for x in open(log, errors='replace').read().splitlines()
                    if x.strip() and x not in notes][-3:]
            notes += [f'(log tail) {x}' for x in tail]
    kept = f'/tmp/xbd-fail.{os.getpid()}.{re.sub(r"[^A-Za-z0-9_.-]", "_", label)}.log'
    try:
        shutil.move(log, kept)
    except OSError:
        return msg
    return msg + f'; log {kept}' + ''.join(f'\n    {x}' for x in notes)


_scales = {}


def auto_scale(binary):
    """The timeout scale a binary needs unasked: 4 for a debug or
    PGO-instrumented (generate) build, else 1 (read once per file)."""
    try:
        st = os.stat(binary)
    except OSError:
        return 1
    k = (os.path.realpath(binary), st.st_size, st.st_mtime_ns)
    if k not in _scales:
        i = identity(binary)
        _scales[k] = 4 if i['debug'] or i['pgo'] == 'generate' else 1
    return _scales[k]


def run_once(app, env, a, purpose='', label='run', scale=None):
    """One run (the module docstring); scale multiplies its timeouts (default
    a.timeout_scale, else auto_scale of APP's binary: pgo-train comes here
    directly). Raises RunFailed, keeping the log."""
    shot, dvd = getattr(a, 'shot', ''), getattr(a, 'dvd', '')   # pgo-train's Namespace has neither
    scale = scale or getattr(a, 'timeout_scale', None) or auto_scale(
        os.path.join(app, 'Contents/MacOS/xemu'))
    env, sets = split_env(env)
    d = scratch_dir()
    log = os.path.join(d, 'log')
    try:
        toml, pads, fifo, sock, log = scratch(d, getattr(a, 'config', '') or None, dvd, sets)
        app, base, headless = renderer_target(app, a, d)
        e = run_env(env, headless, fifo, base)
        cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,  # + MARK: lease.popen
               '-qmp', f'unix:{sock},server,nowait', '-S', '-icount', a.icount] + pads
        if dvd:
            cmd += ['-dvd_path', os.path.abspath(dvd)]
        stops = [int(x) for x in a.hash_at.split(',')] if a.hash_at else [a.frames]
        limit = (30 + stops[-1] / 20) * scale   # any one QMP reply
        with lease.popen(cmd, purpose or f'bench-det {a.tag} {stops[-1]} frames',
                         int(limit), keep=env, env=e, stdout=subprocess.DEVNULL,
                         stderr=open(log, 'w')) as proc:
            LIVE.add(proc)
            wait_log(log, 'xemu: ready', 1, proc, 120 * scale)
            q = QMP(sock, proc, limit)
            q.cmd('stop')
            if a.tag != '-':
                q.cmd('human-monitor-command', **{'command-line': f'loadvm {a.tag}'})
                wait_log(log, 'xemu: state loaded', 1, proc, 120 * scale)
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
            png = f = ''
            if shot:   # the final stop only, after its RAM hash (see the docstring)
                os.makedirs(shot, exist_ok=True)
                f = os.path.join(os.path.abspath(shot), f'{label}-f{done}.png')
                q.cmd('screendump', filename=f, format='png')
                png = hashlib.sha256(open(f, 'rb').read()).hexdigest()[:12]
            try:
                q.cmd('quit')
            except Exception:
                pass
            try:
                rc = proc.wait(timeout=30 * scale)
            except subprocess.TimeoutExpired:
                raise RunFailed(stalled(proc.pid, f'xemu still runs {30 * scale:.0f} s after quit'))
            LIVE.discard(proc)
        if rc != 0:
            raise RunFailed(f'xemu exited {rc} after quit')
        notes, violated = log_notes(log, keys=sets)
        if violated:
            raise RunFailed(('xemu ran without a config key it was given'
                             if any(GENCONFIG.search(x) for x in notes)
                             else 'VIOLATION or refuter miss')
                            + f' ({" ".join(hashes)})')
        return {'fps': done / wall, 'mcyc': cyc / done / 1e6, 'mins': ins / done / 1e6,
                'cpu_s': cpu, 'runnable_s': run, 'ram': ' '.join(hashes), 'png': png,
                'driver': vk_driver(log), 'png_path': f, 'notes': notes}
    except Exception as x:
        raise RunFailed(keep_log(x, log, label, sets)) from x
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
    (marker, nice, slot, lease); stack.close() ends it. With --from-state,
    on the config's disc, then loadvm, the swap to --dvd, --hdd-io and a
    reset, all paused; the returned row times them (else None)."""
    env, sets = split_env(env)
    toml, pads, fifo, sock, log = scratch(d, sets=sets)
    app, base, headless = renderer_target(app, a, d)
    e = run_env(env, headless, fifo, base)
    cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,  # + MARK: lease.popen
           '-qmp', f'unix:{sock},server,nowait', '-S', '-icount', a.icount] + pads
    if a.dvd and not a.from_state:
        cmd += ['-dvd_path', a.dvd]
    if gdb:
        cmd += ['-gdb', f'unix:{os.path.join(d, "gdb.sock")},server=on,wait=off']
    proc = stack.enter_context(lease.popen(
        cmd, f'bench-det --timeline {"markers" if gdb else "timing"} ({a.renderer})', 180,
        keep=env, env=e, stdout=subprocess.DEVNULL, stderr=open(log, 'w')))
    wait_log(log, 'xemu: ready', 1, proc)
    q = QMP(sock, proc)
    q.cmd('stop')
    load = None
    if a.from_state:
        hdd = os.path.join(d, 'hdd.qcow2')
        s0 = tl_snap(q, proc.pid, hdd)
        r = q.cmd('human-monitor-command', **{'command-line': f'loadvm {a.from_state}'})
        if r and 'rror' in r:
            raise RuntimeError(f'loadvm {a.from_state}: {r.strip()}')
        wait_log(log, 'xemu: state loaded', 1, proc)
        if a.dvd:   # the build into the tray (Azurik harness: Sandbox.insert)
            q.cmd('blockdev-change-medium', device='ide0-cd1', filename=os.path.abspath(a.dvd),
                  format='raw')
    for c in a.hdd_io:   # on the scratch clone, before power-on (after the load's disk revert)
        q.cmd('human-monitor-command', **{'command-line': f'qemu-io ide0-hd0 "{c}"'})
    if a.from_state:
        q.cmd('system_reset')
        q.wait_event('RESET')
        load = tl_row(s0, tl_snap(q, proc.pid, hdd), 0)
        load['start'] = 0
    return proc, q, fifo, log, load


def tl_ram(q, d):
    return ram_sha256(q, d)[:16]


def tl_markers(app, env, a, va):
    """Marker pass: each event's frame, the input schedule, the end RAM."""
    d = scratch_dir()
    stack = contextlib.ExitStack()
    try:
        proc, q, fifo, log, _ = tl_launch(app, env, a, d, True, stack)
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
        info['driver'] = vk_driver(log)
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
        proc, q, fifo, log, load = tl_launch(app, env, a, d, False, stack)
        fd = os.open(fifo, os.O_RDWR)
        os.write(fd, ('\n'.join(schedule) + '\n').encode())   # after the load, which drops queued lines
        time.sleep(0.2)
        rows, frame, phases, names, start = {}, 0, [], [], 'reset'
        if load:
            rows['load'] = load
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
        return rows, ram, vk_driver(log)
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
        if not frames and p != 'load':
            print(f'  {p:12s} {start:6d} {0:6d}       -')
            continue
        def io(dev, op):
            return (f"{r[f'{dev}.{op}_operations']:.0f}/{r[f'{dev}.{op}_bytes'] / 2**20:.1f}/"
                    f"{r[f'{dev}.{op}_total_time_ns'] / 1e6:.0f}")
        hp = lambda k: r.get(f'hp.{k}', 0)
        xlate, apu = hp('translate-ns') / 1e6, hp('apu-ns') / 1e6
        grow = r['qcow2_grow'] / 2**20
        msf = f'{r["ms"] / frames:6.2f}' if frames else f'{"-":>6s}'
        idle = f'{hp("idle-ns") / (frames * 1e9 / 60) * 100:5.1f}' if frames else f'{"-":>5s}'
        line = (f'  {p:12s} {start:6d} {frames:6.0f} {r["ms"]:7.0f} {msf} '
                f'{r["vcpu_ms"]:6.0f} {r["vcpu_ms"] - xlate - apu:6.0f} {xlate:6.0f} {apu:5.0f} '
                f'{r["other_ms"]:6.0f} {idle}  '
                f'{io("hdd", "rd"):>16s} {io("hdd", "wr"):>16s} {grow:5.1f} '
                f'{r["hdd.flush_operations"]:.0f}/{r["hdd.flush_total_time_ns"] / 1e6:<4.0f} '
                f'{io("dvd", "rd"):>16s}')
        if base and base[1].get(p, {}).get('ms'):
            line += f'  {(r["ms"] / base[1][p]["ms"] - 1) * 100:+7.1f}%'
        print(line)
    return means


def timeline(a, arms):
    if a.tag != '-':
        sys.exit('--timeline boots cold: leave -t at its default or pass -t - '
                 '(--from-state TAG for the loadvm, swap and reset path)')
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
    if a.from_state:
        print(f'timeline: from state {a.from_state}: loadvm, '
              + (f'{iso} into the tray, ' if a.dvd else '') + 'system_reset', flush=True)
    print(f'timeline: markers on {name} ({a.renderer}); disc {iso}; XBE entry '
          f'{va["xbe_entry"] and hex(va["xbe_entry"])} ({release} VAs)', flush=True)
    t = time.time()
    marks, info, schedule = tl_markers(app, env, a, va)
    check_driver(name, env, info['driver'])
    print(f'  level {info.get("level")}; end frame {info["end"]} ram {info["ram"]}; '
          f'{len(schedule) // 2} presses; {time.time() - t:.1f} s'
          + (f' ({info["driver"]})' if info['driver'] else ''), flush=True)
    res = {n: [] for n, _, _ in arms}
    drivers = {}
    out = open(a.out, 'a') if a.out else None
    first_log = None
    if a.block_log:
        os.makedirs(a.block_log, exist_ok=True)
    for i in range(a.pairs):
        for n, app, env in arms:
            if a.block_log:
                blog = os.path.join(a.block_log, f'{n}-{i + 1}.tsv')
                env = dict(env, XEMU_BLOCK_LOG=blog)
            rows, ram, drv = tl_timing(app, env, a, marks, info, schedule)
            check_driver(n, env, drv)
            drivers[n] = drv
            res[n].append(rows)
            tot = sum(r['ms'] for p, r in rows.items() if p != 'tail')
            same = 'same RAM' if ram == info['ram'] else f'RAM {ram} DIFFERS from the marker pass'
            print(f'{i + 1} {n}: boot to steady {tot / 1e3:.2f} s host, {same}'
                  + (f' ({drv})' if drv else ''), flush=True)
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
        means = tl_print(n + (f' ({drivers[n]})' if drivers.get(n) else ''), res[n], base)
        base = base or (n, means)


D98_SCENES = os.path.join(REPO, 'tests/azurik/03-heavy-scenes/scenes.json')


def d98_runs(fx, iso):
    """tests/azurik pack 03 as --check runs: (key, tag, frames, hash_at, input,
    golden table name, config, disc). scenes.json is read, never written;
    its fight input is null, so the first candidate of fight_candidates
    (heading (0, 1), W 60: 70 lines) is built by its rule, as
    tests/azurik/README.md does."""
    sj = json.load(open(D98_SCENES))
    stops = sj['stops']
    fc = sj['fight_candidates']
    (lx, ly), w = fc['headings_lx_ly'][0], fc['walks_W'][0]
    axes = [(n, v) for n, v in (('lx', lx), ('ly', ly)) if v != 0]
    fight = ([f'+2 axis {n} {v:.6f}' for n, v in axes] +
             [f'+{2 + w} axis {n} 0.000000' for n, _ in axes])
    for k in range(4 + w, stops[-1], 16):     # A (81) down 4 vblanks every 16
        if k + 4 >= stops[-1]:
            break
        fight += [f'+{k} down 81', f'+{k + 4} up 81']
    inputs = dict(sj['inputs'], fight=fight)
    return [(s['tag'], s['tag'], stops[-1], ','.join(map(str, stops)), inputs[s['input']],
             'd98', os.path.join(fx, 'xemu.toml'), iso) for s in sj['scenes']]


def golden_key(app, env, cfg=None, sets=None):
    """(arch, mem_limit) a golden is kept for, or None (a refuter arm, or
    one whose config keys set a sys.* key other than sys.mem_limit). The
    arm's config keys are sets plus env's dotted keys; their
    sys.mem_limit wins over the source config's (cfg, default CFG)."""
    env, own = split_env(env)
    sets = {**own, **(sets or {})}
    if any('REFUTE' in k for k in env) or any(
            k.startswith('sys.') and k != 'sys.mem_limit' for k in sets):
        return None
    archs = subprocess.run(['lipo', '-archs', os.path.join(app, 'Contents/MacOS/xemu')],
                           capture_output=True, text=True).stdout.split()
    if 'sys.mem_limit' in sets:
        m = re.match(r"""\s*['"]?(\d+)""", str(sets['sys.mem_limit']))
    else:
        m = re.search(r"""(?m)^\s*mem_limit\s*=\s*['"]?(\d+)""", open(cfg or CFG).read())
    return ('arm64' if 'arm64' in archs else 'x86_64', m.group(1) if m else '64')


# xemu_version, xemu_commit and xemu_date (xemu-version.c) sit together in
# the binary's strings: git describe without the v, HEAD, `date -u`.
_VERSION = re.compile(rb'([0-9][0-9A-Za-z.+~-]{0,63})\x00+([0-9a-f]{40})\x00+'
                      rb'([A-Z][a-z]{2} [A-Z][a-z]{2} [ 0-9][0-9] [0-9:]{8} UTC [0-9]{4})\x00')


def identity(binary, build_dir=''):
    """What a binary is, read from its bytes (never run): {version, commit,
    date, sha, archs, debug, pgo}. pgo is 'generate' (instrumented), else
    'use' or 'off' from build_dir's build.ninja, else ''."""
    data = open(binary, 'rb').read()
    m = _VERSION.search(data)
    pgo = 'generate' if b'__llvm_prf_cnts' in data else ''
    ninja = os.path.join(build_dir, 'build.ninja') if build_dir else ''
    if not pgo and ninja and os.path.isfile(ninja):
        pgo = 'use' if b'-fprofile-use' in open(ninja, 'rb').read() else 'off'
    archs = subprocess.run(['lipo', '-archs', binary], capture_output=True, text=True).stdout.split()
    return {'version': m and m.group(1).decode(), 'commit': m and m.group(2).decode(),
            'date': m and m.group(3).decode(), 'sha': hashlib.sha256(data).hexdigest()[:12],
            'archs': archs, 'debug': b'xemu | v%s Debug' in data, 'pgo': pgo}


def describe(i, head=''):
    s = (f'{i["version"]} ({i["commit"][:10]}{" = HEAD" if i["commit"] == head else ""}, '
         f'built {i["date"]})' if i['commit'] else 'no embedded version')
    s += f', sha256 {i["sha"]}, {"+".join(i["archs"]) or "?"}, {"debug" if i["debug"] else "release"}'
    return s + (f', pgo {i["pgo"]}' if i['pgo'] else '')


def prepare_arms(a, arms):
    """Refuse arms in dist/, run a bare binary in a clone of a.bundle
    (scripts/refresh-clone.sh) inside a temp dir, print each arm's identity
    and set a.scales (each arm's timeout scale). Returns (arms, temp dir or
    None)."""
    head = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True,
                          text=True).stdout.strip()
    out, tmp, a.scales, clones = [], None, {}, {}
    arms_given = {name: app for name, app, _ in arms}
    try:
        for name, app, env in arms:
            if lease.in_dist(app):
                sys.exit(f"bench-det: {name}={app} is in dist/, the owner's bundle: pass a clone "
                         '(cp -Rc dist/xemu.app /tmp/x.app) or the bare build/qemu-system-i386')
            src = ''
            if os.path.isfile(app) and os.path.abspath(app) in clones:   # a refuter beside new=
                src = os.path.abspath(app)
                app = clones[src]
            elif os.path.isfile(app):
                src, tmp = os.path.abspath(app), tmp or scratch_dir()
                app = clones[src] = os.path.join(tmp, f'{name}.app')
                if not os.path.isdir(os.path.join(a.bundle, 'Contents')):
                    sys.exit(f'bench-det: {name}: a bare binary runs in a clone of --bundle, '
                             f'and {a.bundle} is not a bundle')
                r = subprocess.run([os.path.join(REPO, 'scripts/refresh-clone.sh'), app, src,
                                    '--from', a.bundle], capture_output=True, text=True)
                if r.returncode:
                    sys.exit(f'bench-det: {name}: {(r.stderr or r.stdout).strip()}')
            elif not os.path.isfile(os.path.join(app, 'Contents/MacOS/xemu')):
                sys.exit(f'bench-det: {name}={app} is neither a bundle nor an xemu binary')
            i = identity(src or os.path.join(app, 'Contents/MacOS/xemu'), os.path.dirname(src))
            a.scales[name] = a.timeout_scale or (4 if i['debug'] or i['pgo'] == 'generate' else 1)
            print(f'{name}: {describe(i, head)}' + (f'; from {arms_given[name]}' if src else '')
                  + (f'; timeouts x{a.scales[name]:g}' if a.scales[name] != 1 else ''), flush=True)
            out.append((name, app, env))
    except BaseException:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
        raise
    return out, tmp


def check(a, arms):
    """--check: run the sets' runs in parallel, assert goldens and agreement."""
    gold = json.load(open(os.path.join(REPO, 'scripts/det-goldens.json')))
    runs, batch = [], None
    try:
        for name in a.check.split(','):
            if name == 'goldens':
                runs += [(k, v['tag'], v['frames'], v['hash_at'], v['input'], '', '', a.dvd)
                         for k, v in gold['runs'].items()]
            elif name.startswith('tags:'):
                tags = [t.strip() for t in open(name[5:]) if t.strip() and not t.startswith('#')]
                f = a.frames if a.frames != 1200 else 300
                runs += [(t, t, f, '', a.input, None, '', a.dvd) for t in tags]
            elif name == 'd98':
                fx = os.environ.get('AZURIK_FX', '')
                if not fx:
                    print('d98: skipped (AZURIK_FX unset)', flush=True)
                    continue
                if not os.path.isfile(os.path.join(fx, 'xemu.toml')):
                    sys.exit(f'bench-det: AZURIK_FX={fx} is not the Modkit\'s D98 fixtures folder '
                             '(xemu.toml, the drive, eeprom.bin, vanilla.iso)')
                batch = batch or scratch_dir()   # the ISO clone: removed with the batch
                iso = os.path.join(batch, 'dvd.iso')
                subprocess.run(['cp', '-c', os.path.join(fx, 'vanilla.iso'), iso], check=True)
                runs += d98_runs(fx, iso)
            else:
                sys.exit(f'bench-det: unknown --check set {name}')
        if not runs:   # a gate that ran nothing (d98 alone without AZURIK_FX, an empty tags file)
            sys.exit(f'bench-det: --check {a.check} ran no runs')
        check_runs(a, arms, gold, runs)
    finally:
        if batch:
            shutil.rmtree(batch, ignore_errors=True)


def fork_slots():
    """XEMU_FORK_SLOTS (default 4): the lease's machine-wide xemu slots."""
    try:
        return max(1, int(os.environ.get('XEMU_FORK_SLOTS') or 4))
    except ValueError:
        return 4


def shot_diffs(rows, base):
    """rows: (label, the first arm's result, this arm's) of the same run.
    Prints each picture's diff against the first arm's (score_shot.diff)."""
    rows = [(l, b, r) for l, b, r in rows if b and r and b.get('png_path') and r.get('png_path')]
    if not rows:
        return
    try:
        spec = importlib.util.spec_from_file_location('score_shot',
                                                      os.path.join(REPO, 'scripts/score_shot.py'))
        ss = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ss)
    except (OSError, ImportError, SyntaxError) as e:
        print(f'shot diff: no scripts/score_shot.py ({e})')
        return
    print(f'shot diff against {base} (KK bar: PSNR >= 45 dB, <= 0.01% of pixels off by > 32):')
    for label, b, r in rows:
        if b['png'] == r['png']:
            print(f'  {label}: 0 px differ (the same picture)', flush=True)
            continue
        try:
            x = ss.diff(b['png_path'], r['png_path'])
        except Exception as e:   # sizes differ, or a PNG doesn't decode
            print(f'  {label}: {e}', flush=True)
            continue
        psnr = 'inf' if math.isinf(x['psnr']) else f'{x["psnr"]:.1f}'
        print(f'  {label}: {x["pixels"]} px differ, {x["off_gt_32"]} by > 32 (max {x["max_delta"]}), '
              f'PSNR {psnr} dB, bbox {x["bbox"]}: {"within" if x["pass_kk_bar"] else "OUTSIDE"} '
              'the KK bar', flush=True)


def check_runs(a, arms, gold, runs):
    import concurrent.futures

    def want_of(key, table, cfg, app, env):
        """The golden hashes for key ({stop: hash}), or None (agreement only)."""
        gk = golden_key(app, env, cfg or None) if table is not None else None
        root = gold if table == '' else gold.get(table, {}) if table else {}
        return root.get(gk[0], {}).get(gk[1], {}).get(key) if gk else None

    width = max([12] + [len(r[0]) for r in runs])
    jobs = [(r, arm) for r in runs for arm in arms]
    results, full, failed = {}, {}, False

    def stop(signum, frame):
        raise KeyboardInterrupt
    import signal
    signal.signal(signal.SIGINT, stop)     # also when started in the background
    signal.signal(signal.SIGTERM, stop)

    def one(job):
        (key, tag, frames, hash_at, inp, _, cfg, dvd), (name, app, env) = job
        ns = argparse.Namespace(**vars(a))
        ns.tag, ns.frames, ns.hash_at, ns.input = tag, frames, hash_at, list(inp)
        ns.config, ns.dvd = cfg, dvd
        return run_once(app, env, ns, f'bench-det --check {key} {name}', f'{name}-{key}',
                        getattr(a, 'scales', {}).get(name))

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs or min(len(jobs), fork_slots()))
    try:
        futs = {pool.submit(one, j): j for j in jobs}
        for fut in concurrent.futures.as_completed(futs):
            (key, _, _, _, _, table, cfg, _), (name, app, env) = futs[fut]
            try:
                r = fut.result()
            except Exception as e:
                print(f'{key:{width}s} {name:10s} FAIL  {e}', flush=True)
                failed = True
                continue
            results[(key, name)], full[(key, name)] = r['ram'], r
            want = want_of(key, table, cfg, app, env)
            got = dict(h.split('=') for h in r['ram'].split())
            if want is None:
                verdict = 'n-a'
            elif all(got.get(s) == v for s, v in want.items()):
                verdict = 'OK'
            else:
                verdict = 'MOVED (want ' + ' '.join(f'{s}={v}' for s, v in want.items()) + ')'
            print(f'{key:{width}s} {name:10s} {r["ram"]}  {verdict}  {r["mins"]:.3f} Mins/f'
                  + (f'  png={r["png"]}' if r['png'] else '')
                  + ''.join(f'\n    {x}' for x in r['notes']), flush=True)
    except KeyboardInterrupt:
        STOP.set()
        for p in list(LIVE):
            p.kill()
        pool.shutdown(wait=False, cancel_futures=True)
        print('bench-det: interrupted; runs killed')
        sys.exit(1)
    pool.shutdown()
    if a.shot:
        shot_diffs([(f'{key} {n}', full.get((key, arms[0][0])), full.get((key, n)))
                    for (key, *_) in runs for n, _, _ in arms[1:]], arms[0][0])
    for (key, *_) in runs:
        hs = {results[(key, n)] for n, _, _ in arms if (key, n) in results}
        if len(hs) > 1:
            print(f'{key}: arms disagree: {", ".join(sorted(hs))}')
            sys.exit(2)
    for (key, _, _, _, _, table, cfg, _) in runs:
        for name, app, env in arms:
            want = want_of(key, table, cfg, app, env)
            got = dict(h.split('=') for h in results.get((key, name), '').split() if '=' in h)
            if want and (key, name) in results and any(got.get(s) != v for s, v in want.items()):
                sys.exit(4)
    sys.exit(1 if failed else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('-n', '--pairs', type=int, default=4)
    p.add_argument('-f', '--frames', type=int, default=1200)
    p.add_argument('-t', '--tag', default='azurik-heavy-f1')
    p.add_argument('-i', '--input', action='append', default=[])
    p.add_argument('--hash-at', default='')
    p.add_argument('--icount', default='shift=2,align=off,sleep=off')
    p.add_argument('--out', default='')
    p.add_argument('--renderer', choices=('null', 'offscreen'), default='null',
                   help='offscreen: Vulkan on a portable clone with cold caches')
    p.add_argument('--shot', default='', metavar='DIR',
                   help='QMP screendump at the final stop to DIR/<arm>-<pair>-f<N>.png '
                        '(rendered with --renderer offscreen)')
    t = p.add_argument_group('boot timeline (--timeline)')
    t.add_argument('--timeline', action='store_true',
                   help='cold boot to steady play, phase by phase (-n timing runs per arm)')
    t.add_argument('--dvd', default='', help='disc to boot (default: the fixture config\'s); '
                   'with --from-state, the disc put in the tray after the load; plain and '
                   '--check runs get -dvd_path ISO (no clone)')
    t.add_argument('--from-state', default='', metavar='TAG',
                   help='boot from a loaded state instead of power-on: loadvm TAG, --dvd into the '
                        'tray, --hdd-io, system_reset (the Azurik harness\'s boot-build path)')
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
    p.add_argument('--check', default='', metavar='SET[,SET]',
                   help='goldens, tags:FILE, d98 (AZURIK_FX): run in parallel and assert '
                        '(see the docstring)')
    p.add_argument('-j', '--jobs', type=int, default=0,
                   help='--check runs at once (default XEMU_FORK_SLOTS, 4)')
    p.add_argument('--timeout-scale', type=float, default=None, metavar='X',
                   help='multiply the run timeouts (default 1; 4 for a debug or '
                        'PGO-instrumented build)')
    p.add_argument('--bundle', default=os.path.join(REPO, 'dist/xemu.app'), metavar='APP',
                   help='the bundle a bare-binary arm runs in a clone of (default dist/xemu.app)')
    p.add_argument('arms', nargs='+', help='NAME=APP[:K=V,TABLE.KEY=V,...]; APP may be a bare binary')
    a = p.parse_args()
    sweep_scratch()
    arms = []
    for spec in a.arms:
        name, eq, rest = spec.partition('=')
        if not name or not eq:
            p.error(f'arm {spec!r}: want NAME=APP[:K=V,...]')
        app, _, envs = rest.partition(':')
        kvs = re.split(r',(?=[A-Za-z_][A-Za-z0-9_.]*=)', envs)   # see the docstring
        if any('=' not in kv for kv in kvs if kv):
            p.error(f'arm {spec!r}: want K=V after the colon')
        arms.append((name, app, dict(kv.split('=', 1) for kv in kvs if kv)))
    arms, tmp = prepare_arms(a, arms)
    try:
        if a.check:
            return check(a, arms)
        if a.timeline:
            a.route = a.route.split(',')
            if a.tag == 'azurik-heavy-f1':
                a.tag = '-'
            return timeline(a, arms)
        return ab(a, arms)
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


def ab(a, arms):
    """The plain A/B: -n interleaved pairs, then each arm against the first."""
    res = {n: [] for n, _, _ in arms}
    out = open(a.out, 'a') if a.out else None
    for i in range(a.pairs):
        for name, app, env in arms:
            try:
                r = run_once(app, env, a, f'bench-det {name} pair {i + 1}/{a.pairs} '
                                          f'{a.tag} {a.frames if not a.hash_at else a.hash_at} frames',
                             f'{name}-{i + 1}', a.scales.get(name))
            except RunFailed as e:
                print(f'{i + 1} {name}: FAIL  {e}', flush=True)
                sys.exit(1)
            res[name].append(r)
            line = (f'{i + 1} {name}: {r["fps"]:.1f} fps  {r["mcyc"]:.3f} Mcyc/f  '
                    f'{r["mins"]:.3f} Mins/f  {r["cpu_s"]:.2f}/{r["runnable_s"]:.2f} cpu/runnable s  '
                    f'{r["ram"]}' + (f'  png={r["png"]}' if r['png'] else '')
                    + (f'  ({r["driver"]})' if r['driver'] else '')
                    + ''.join(f'\n    {x}' for x in r['notes']))
            print(line, flush=True)
            if out:
                out.write(json.dumps({'pair': i + 1, 'arm': name, **r}) + '\n')
            check_driver(name, env, r['driver'])
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
    if a.shot:
        shot_diffs([(f'{n}-{i + 1}', res[base][i], res[n][i])
                    for i in range(a.pairs) for n, _, _ in arms[1:]], base)
    hashes = {r['ram'] for rs in res.values() for r in rs}
    if len(hashes) > 1:
        print('RAM hashes differ:', ', '.join(sorted(hashes)))
        sys.exit(2)


if __name__ == '__main__':
    main()
