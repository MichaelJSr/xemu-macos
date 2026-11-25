#!/usr/bin/env python3
"""Azurik pre-release pack: tests/azurik's checks on the Modkit's D98 fixtures, one command.

    AZURIK_FX=<D98 folder> azurik-pack.py [--cases ID[,ID]] [--all]
        [--renderer null|offscreen] [--dsp ENGINE[,ENGINE]] [-j N]
        [--timeout-scale X] [--bundle APP] [--out FILE] [--list]
        NAME=APP[:K=V,TABLE.KEY=V,...] ...

Each case is one fresh xemu set up as bench-det.py's runs are (imported:
scratch(), run_env(), the lease launch, QMP, the gdbstub): headless null,
-S -icount shift=2,align=off,sleep=off, paused at its state (or at
power-on), then driven over QMP: hooks, run-until, the input pipe,
checkpoints, the block and voice logs, the WAV. Inputs come from the
packs' own JSON, tick reads are judged by the packs' replays.py and
physics_check.py, and the oracles that exist only in the packs' README
prose are the tables below. The case's sandbox (HDD and EEPROM clones,
logs, records, the WAV) goes when it ends; a failed case keeps its xemu
log as /tmp/xbd-fail.<pid>.<arm>-<case>.log. One cp -c clone of
$AZURIK_FX/vanilla.iso per batch is every case's disc. Packs 04 and 10
get the 232-256 keyboard map they were recorded with, written into each
run's scratch config (harness-config.py --set).

Cases (--list prints the selection with each case's expected seconds).
The core set is the default; --all adds the on-demand cases. --cases
takes ids, or a pack prefix (02, 04, 10) for that pack's core cases (all
of them with --all).
  01-w2        pack 01's w2 replay: gdbstub writes at run-until halts, three hooks
               (pad and camera written, CORE read), one advance to the empty pad queue
  02-<case>    the 11 D98 physics cases: run-until rows, @F+1 pipe lines
  03-e2, 03-e7 pack 03's scene checks by collect hooks, the +300/+600 RAM unmoved
  04-<level>   the 22 level warps: RAM, block-log totals, blockstats deltas
  05           death.bik cut at each 0x53750 (F1, F2, blockstats), then F2 + 900
  10-fork      e6's song on the fork's DSP JIT: two replays in one process
  10-upstream  one replay on dsp56300's JIT (the default where the fork's isn't built)
  ckpt         checkpoint save and restore (incremental, verified), xemu-run, a
               second loadvm: all on e6's f300 golden
  on demand: 01-w1 (hooks plus a mid-advance pipe press), 02-cold-jump
  (power-on, the menu route, .text), 05-straight (one advance), 10-interp.
--renderer offscreen runs pack 05 through Vulkan (a portable clone with
cold caches; MoltenVK's RAM oracle); the other packs stay null. --dsp
picks pack 10's engines (fork, upstream, interp; default fork,upstream).

An arm is a bundle clone or a bare binary, as for bench-det (a bare
binary runs in a refresh-clone.sh clone of --bundle; dist/ is refused);
K=V goes to xemu's environment, TABLE.KEY=V into its config (a key
genconfig refuses fails the case, the packs' own keys too). Up to -j
(default 3) cases run at once, longest first. One line per case and arm,
then exit 0 (all OK), 2 (arms disagree), 4 (an oracle moved) or 1 (a
case failed), as bench-det --check. A case's xemu is killed at 10x its
expected seconds plus 60 s, times --timeout-scale (default 1; 4 for a
debug build).
"""
import argparse, collections, concurrent.futures, contextlib, csv, hashlib, importlib.util
import json, os, re, shutil, signal, struct, subprocess, sys, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AZ = os.path.join(REPO, 'tests/azurik')


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def _pack(path):
    with open(os.path.join(AZ, path), encoding='utf-8') as f:
        return json.load(f)


bd = _module('bench_det', os.path.join(REPO, 'scripts/bench-det.py'))
pc = _module('physics_check', os.path.join(AZ, '02-physics/physics_check.py'))
REPLAYS = _pack('01-replays/replays.json')['replays']
PHYS = {c['id']: c for c in _pack('02-physics/physics.json')['cases']}
SCENES = _pack('03-heavy-scenes/scenes.json')
TOUR = _pack('04-level-tour/results-v0.15.json')
MOVIE = _pack('05-bink-movie/movie.json')
SONG, SONG15 = _pack('10-audio-e6-song/song.json'), _pack('10-audio-e6-song/results-v0.15.json')
with open(os.path.join(REPO, 'scripts/det-goldens.json')) as _f:
    GOLD = json.load(_f)

# The oracles that live only in README prose (tests/azurik/README.md and the
# packs' own): v0.15's, all matched again on v0.16.6 (2026-11-24). An
# intentional move edits them here, with the reason, in the same commit.
START12 = dict({s['tag']: s['start12'] for s in SCENES['scenes']}, **{MOVIE['tag']: MOVIE['start12']},
               **{'vanilla-flat': 'f576b2d7a6c6', 'vanilla-wall': '3338c3020d4e',   # 02's starts
                  'vanilla-ocean': '8260ec3afeff', 'vanilla-enemy': '75b9730025dd'})
RAM01 = {'w2-walk-pickup': ('aad6cc27fbb1', '7e4e74cc76a6'),   # at the start halt, after the last tick
         'w1-seg0': ('58df07e54b9d', None)}                    # w1's end follows its A press
POPUP01 = [415]   # w1: entry records when the popup stalls the ticks; one A press clears it
END02 = {'walk': '132499b74127', 'roll': 'c2ed673c127e', 'jump': '73bc237f6888',
         'swim': 'e577eeb299db', 'beach-jump': 'e8f55d06b95d', 'beach-flaps': '551b98fbd735',
         'enemy-hits': '75b7170edc8f', 'ledge-j14': 'ff2d63cd224a', 'ledge-j16': '81a3f3917563',
         'ledge-j28': 'a105d33c0908', 'ledge-j30': '47dd2d11f4c5'}
COLD02 = {'level': 'levels/training_room', 'preview': False,   # the D98 EEPROM: no save preview
          'text': [979408, 0]}           # default.xbe's .text in guest RAM: bytes read, bytes differing
CHECK03 = {'vanilla-rz-e2-cart': {'paths': ['+245 levels/earth/e5']},
           'vanilla-rz-e7-keylord': {'damage': {'player': 0, 'others': 12, 'first': 207}}}
O05 = {'loads': [[4, 'bink:death.bik', None], [6046, 'levels/death/d2', 'D1_D2_Fall']],
       'dvd': 50307072, 'hdd_wr': 65024, 'frames': 6946}
CUT05 = {'dvd_to_F1': 8192, 'dvd_F1_F2': 50298880, 'hdd_wr_F1': 65024}   # HDD writes all before F1
RAM05 = {'null': '9ab9c0158f03', 'offscreen': '10b5a35aa66b'}   # offscreen: MoltenVK's
STOPS10 = {'end': 51, 'guest': 72}   # the voice log's stop rows by why

ICOUNT = 'shift=2,align=off,sleep=off'
TICK, ENTRY, CAM, RET1, RET2, LOAD = 0x8CCC0, 0x8C230, 0x85276, 0x8C3F1, 0x8C401, 0x53750
BUTTONS01 = 'a b x y black white lt rt lstick rstick start back'.split()
# Packs 04 and 10 were recorded with xemu's 25 keyboard-pad inputs, in
# config_spec.yml order, on scancodes 232-256 (tests/azurik/README.md).
KEYMAP232 = [f'input.keyboard_controller_scancode_map.{k}={232 + i}' for i, k in enumerate(
    'a b x y back guide start lstick_btn rstick_btn white black dpad_up dpad_down dpad_left '
    'dpad_right lstick_up lstick_left lstick_right lstick_down ltrigger rstick_up rstick_left '
    'rstick_right rstick_down rtrigger'.split())]
DSP = {'fork': ['audio.dsp_jit.enabled=true'],
       'upstream': ['audio.dsp_jit.enabled=false', 'audio.use_dsp_jit=true'],
       'interp': ['audio.dsp_jit.enabled=false', 'audio.use_dsp_jit=false']}
MISSING = object()


def state_is(v):
    return {'addr': '[esp+4]+0x94', 'size': 4, 'type': 'u', 'op': 'eq', 'value': v}


def cstr(h):
    """A NUL-terminated string from a hook's hex, or None (unreadable: NULL)."""
    return bytes.fromhex(h).split(b'\0')[0].decode('latin-1') if h else None


def tagged(lines, base):
    """A pack's '+N body' lines as '@<base+N> body' (a pipe +N counts from the
    frame xemu reads it at, not from the load)."""
    return [f'@{base + int(n[1:])} {body}' for n, body in (ln.split(' ', 1) for ln in lines)]


def sha12(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()[:12]


class Batch:
    """The batch: its dir (the disc clone), the D98 config and keyboard map."""

    def __init__(self, a, fx):
        self.a, self.fx, self.d98 = a, fx, os.path.join(fx, 'xemu.toml')
        self.dir = bd.scratch_dir()   # sweep_scratch removes it if we die
        try:
            self.iso = os.path.join(self.dir, 'dvd.iso')
            subprocess.run(['cp', '-c', os.path.join(fx, 'vanilla.iso'), self.iso], check=True)
            with open(self.d98) as f:
                sec = f.read().partition('[input.keyboard_controller_scancode_map]')[2].split('\n[', 1)[0]
            self.keys = {k: int(v) for k, v in re.findall(r'(?m)^\s*(\w+)\s*=\s*(\d+)', sec)}
            if not self.keys:
                raise RuntimeError(f'{self.d98}: no [input.keyboard_controller_scancode_map]')
            self.inputs = {r[1]: r[4] for r in bd.d98_runs(fx, self.iso)}   # pack 03's, fight built
        except BaseException:
            shutil.rmtree(self.dir, True)
            raise

    def golden(self, arm, key):
        """det-goldens.json's d98 hashes for key on arm's arch and mem_limit
        (its sys.mem_limit, else the D98 config's) ({'f300': ..., 'f600':
        ...}), else {} (agreement only: any other sys.* key, a refuter)."""
        gk = bd.golden_key(arm[1], arm[2], self.d98)
        return GOLD['d98'].get(gk[0], {}).get(gk[1], {}).get(key, {}) if gk else {}


class Ctx:
    """One case on one arm: its exit stack (the xemu, the sandbox) and its limit."""

    def __init__(self, b, case, arm):
        self.b, self.case, self.arm = b, case, arm
        self.scale = b.a.scales.get(arm[0]) or 1
        self.limit = (10 * case.est + 60) * self.scale
        self.stack, self.vm, self.expired = contextlib.ExitStack(), None, ''

    def launch(self, **kw):
        return VM(self, **kw)

    def expire(self, proc):
        self.expired = bd.stalled(proc.pid, f'over its {self.limit:.0f} s limit; killed')
        proc.kill()


class VM:
    """A case's xemu in its own sandbox: paused, under icount, headless."""

    def __init__(self, ctx, sets=(), gdb=False, renderer='null'):
        name, app, env = ctx.arm
        env, arm_sets = bd.split_env(env)
        # the config keys genconfig must take (log_notes fails the case if it refused one)
        self.keys = [s.split('=', 1)[0] for s in sets] + list(arm_sets)
        ctx.vm, self.ctx, self.d, self.cpu = self, ctx, bd.scratch_dir(), None   # a failed start: its log
        ctx.stack.callback(shutil.rmtree, self.d, True)
        self.log = os.path.join(self.d, 'log')
        toml, pads, fifo, sock, _ = bd.scratch(self.d, ctx.b.d98, ctx.b.iso, list(sets) + [
            f'{k}={v}' for k, v in arm_sets.items()])
        app, base, headless = bd.renderer_target(app, argparse.Namespace(renderer=renderer), self.d)
        cmd = [os.path.join(app, 'Contents/MacOS/xemu'), '-config_path', toml,
               '-qmp', f'unix:{sock},server,nowait', '-S', '-icount', ICOUNT] + pads + [
            '-dvd_path', ctx.b.iso]
        self.gdb = os.path.join(self.d, 'gdb.sock') if gdb else None
        if gdb:
            cmd += ['-gdb', f'unix:{self.gdb},server=on,wait=off']
        err = open(self.log, 'w')
        ctx.stack.callback(err.close)
        self.proc = ctx.stack.enter_context(bd.lease.popen(
            cmd, f'azurik-pack {ctx.case.id} {name}', int(ctx.limit), keep=env,
            env=bd.run_env(env, headless, fifo, base), stdout=subprocess.DEVNULL, stderr=err))
        bd.LIVE.add(self.proc)
        ctx.stack.callback(bd.LIVE.discard, self.proc)
        timer = threading.Timer(ctx.limit, ctx.expire, (self.proc,))   # from launch, not the slot wait
        timer.daemon = True
        timer.start()
        ctx.stack.callback(timer.cancel)
        bd.wait_log(self.log, 'xemu: ready', 1, self.proc, ctx.limit)
        self.q = bd.QMP(sock, self.proc)
        self.q.cmd('stop')
        self.fd = os.open(fifo, os.O_RDWR)   # held open: the pipe never sees EOF
        ctx.stack.callback(os.close, self.fd)
        self.recs, self.dropped, self.stop, self.rsp = [], 0, None, None

    def cmd(self, execute, /, **args):
        if 'name' not in args:
            return self.q.cmd(execute, **args)
        # QMP.cmd's own first parameter is called name (the checkpoint commands' argument)
        self.q.f.write(json.dumps({'execute': execute, 'arguments': args}) + '\n')
        self.q.f.flush()
        while True:
            r = json.loads(self.q.f.readline() or '{"error": "EOF"}')
            if 'error' in r:
                raise RuntimeError(f'{execute}: {r["error"]}')
            if 'event' not in r:
                return r.get('return')
            self.q.events.append(r['event'])

    def take(self, r):
        """Keep the hook records, drops and stop a reply carries."""
        h = (r or {}).get('hooks') or {}
        self.recs += h.get('records', [])
        self.dropped += h.get('dropped', 0)
        self.stop = h.get('stop') or self.stop
        return r

    def hits(self, hook):
        return [x for x in self.recs if x['hook'] == hook]

    def hooks(self, specs):
        return [h['id'] for h in self.cmd('xemu-hooks-set', hooks=specs)]

    def loadvm(self, tag):
        """loadvm tag (the VM stays paused); its frame counter."""
        with open(self.log, errors='replace') as f:
            n = f.read().count('xemu: state loaded') + 1   # checkpoint restores print it too
        r = self.cmd('human-monitor-command', **{'command-line': f'loadvm {tag}'})
        if r and 'rror' in r:
            raise RuntimeError(f'loadvm {tag}: {r.strip()}')
        bd.wait_log(self.log, 'xemu: state loaded', n, self.proc, self.ctx.limit)
        return self.frame()

    def frame(self):
        return self.cmd('query-xemu-frame')['frame']

    def ram(self, n=12):
        return bd.ram_sha256(self.q, self.d)[:n]

    def advance(self, frames, **kw):
        r = self.take(self.cmd('xemu-frame-advance', frames=frames, **kw))
        if r.get('timed-out') or r.get('stopped') or r.get('debug-stop'):
            raise RuntimeError(f'advance {frames}: {r}')
        return r

    def until(self, until, hits=1, frames=600, need=True):
        r = self.take(self.cmd('xemu-run-until', until=until, hits=hits, frames=frames))
        if need and r['reason'] != 'hit':
            raise RuntimeError(f'run-until {until[0]["pc"]:#x} x{hits}: {r["reason"]} '
                               f'at frame {r["frame"]}')
        return r

    def send(self, lines):
        """Write lines to XEMU_INPUT_PIPE; return once xemu has queued them all."""
        n = len(self.cmd('query-xemu-input')['queued']) + len(lines)
        os.write(self.fd, ''.join(x + '\n' for x in lines).encode())
        for _ in range(100):
            q = self.cmd('query-xemu-input')['queued']   # reads the pipe first
            if len(q) >= n:
                return
            time.sleep(0.02)
        raise RuntimeError(f'pipe: {len(q)} of {n} lines queued')

    def attach(self):
        """Connect the gdbstub (pack 01 does it after its load, as its driver did)."""
        self.rsp = bd.RSP(self.gdb, self.proc)

    def mem(self, va, n):
        """Guest memory over the gdbstub, at a halt."""
        out = b''
        while len(out) < n:
            k = min(n - len(out), 0x400)
            h = self.rsp.cmd('m%x,%x' % (va + len(out), k))
            if len(h) != 2 * k:
                raise RuntimeError(f'gdb read {va + len(out):#x}: {h[:16]!r}')
            out += bytes.fromhex(h)
        return out

    def u32(self, va):
        return int.from_bytes(self.mem(va, 4), 'little')

    def poke(self, va, data):
        if self.rsp.cmd('M%x,%x:%s' % (va, len(data), data.hex())) != 'OK' or \
                self.mem(va, len(data)) != data:
            raise RuntimeError(f'gdb write {va:#x} failed')

    def quit(self):
        """Quit; a nonzero exit, a VIOLATION or a config key xemu didn't
        take fails the case (bench-det's rules). Returns the log's notes."""
        with contextlib.suppress(OSError):   # it already died: the exit code says why
            self.cpu = [round(x, 2) for x in bd.counters(self.proc.pid, True)[2:]]
        with contextlib.suppress(Exception):
            self.cmd('quit')
        rc = self.proc.wait(timeout=60 * self.ctx.scale)
        notes, violated = bd.log_notes(self.log, keys=self.keys)
        if rc or violated:
            raise RuntimeError(f'xemu exited {rc} after quit' if rc else
                               'xemu ran without a config key it was given'
                               if any(bd.GENCONFIG.search(x) for x in notes) else 'VIOLATION')
        return notes


# --- the cases: each returns (got, want); want's keys are the oracle -------

def case_01(ctx, replay):
    """Pack 01: the start recipe (gdbstub writes at run-until halts), the
    three hooks, the advance; replays.py checks the per-tick CORE reads."""
    spec, w2, keys = REPLAYS[replay], replay == 'w2-walk-pickup', ctx.b.keys
    vm = ctx.launch(gdb=True)
    vm.loadvm(spec['start']['loadvm'])
    got = {'start12': vm.ram()}
    vm.attach()
    vm.until([{'pc': TICK}], frames=6000)
    if w2:   # the game's own level request (R7), its load, then the 20th steady tick
        for va, s in ((0x38D320, b'\0'), (0x38D424, b'levels/water/w2\0'), (0x38D528, b'W1_W2_Fall\0')):
            vm.poke(va, s)
        vm.poke(vm.u32(vm.u32(vm.u32(0x1BF63C) + 0x2C) + 0xD8) + 0x10, struct.pack('<I', 0x38D320))
        r = vm.until([{'pc': LOAD}], frames=6000)
        got['load_path'] = cstr(vm.mem(vm.u32(r['regs']['esp'] + 8), 64).hex())
        vm.until([{'pc': TICK, 'cond': [state_is(v)]} for v in (0, 3, 6)], hits=20, frames=6000)
    vm.poke(vm.u32(vm.u32(vm.u32(0x1BF63C) + 0x2C) + 0xE0) + 0x14, b'\xff' * 4)   # popup bits
    pace = vm.u32(0x38C058) + 4                                                  # slow-motion cvar
    if vm.u32(pace) != 1:
        vm.poke(pace, struct.pack('<I', 1))
    E = vm.u32(vm.until([{'pc': TICK}], frames=6000)['regs']['esp'] + 4)
    vm.until([{'pc': ENTRY, 'cond': [{'reg': 'ecx', 'op': 'eq', 'value': E}]}], frames=6000)
    got['start_check'] = hashlib.sha256(vm.mem(E, 0x1C0) + vm.mem(vm.u32(E + 0x20), 0x240)).hexdigest()[:12]
    got['ram_start_halt'] = vm.ram()
    idx = vm.u32(0x1A7AE4)
    pad = 0x37BE98 + 0x54 * (idx if idx < 4 else 0)
    pads, cams = [], []
    for s in _pack(os.path.join('01-replays', spec['segments'])):
        held = set(s.get('buttons', []))
        pads += [struct.pack('<18f', *s['stick'], 0, 0, 0, 0,
                             *[float(b in held) for b in BUTTONS01]).hex()] * s['n']
        cams += [struct.pack('<2f', *s['cam']).hex()] * s['n']
    key = {'addr': hex(E + 0x18), 'size': 8}
    ent, _, col = vm.hooks([
        {'pcs': [ENTRY], 'cond': [{'reg': 'ecx', 'op': 'eq', 'value': E}], 'key': key,
         'write': [{'addr': hex(pad), 'size': 72}], 'set': pads, 'stop-empty': True},
        {'pcs': [CAM], 'key': key, 'write': [{'addr': 'esp+0x14', 'size': 8}], 'set': cams},
        {'pcs': [RET1, RET2], 'key': key, 'collect': [
            {'addr': f'[{E + 0x20:#x}]+0x15c', 'size': 12}, {'addr': hex(E + 0x94), 'size': 4},
            {'addr': f'[{E + 0x20:#x}]+0x210', 'size': 4}]}])
    if w2:
        vm.advance(6000)
    else:   # tick 415's popup stalls the updates: A after 120 vblanks with no entry clears it
        f0, last, got['popup'] = vm.frame(), None, []
        while not vm.stop:
            n0 = len(vm.recs)
            r = vm.advance(30)
            for x in vm.recs[n0:]:
                if x['hook'] == ent and not x.get('empty'):
                    last = x['frame']
            if vm.stop:
                break
            F = r['frame']
            if F - f0 > 9000 or len(got['popup']) > 10:
                raise RuntimeError(f'w1: no end after {len(got["popup"])} A presses')
            if F - (f0 if last is None else last) >= 120:
                got['popup'].append(sum(1 for x in vm.hits(ent) if not x.get('empty')))
                vm.send([f'@{F + 1} down {keys["a"]}', f'@{F + 10} up {keys["a"]}'])
                last = F
    ticks = os.path.join(vm.d, 'ticks.json')
    with open(ticks, 'w') as f:
        json.dump([x['data'] for x in vm.hits(col)], f)
    c = subprocess.run([sys.executable, '-I', os.path.join(AZ, '01-replays/replays.py'), 'check', replay,
                        ticks], capture_output=True, text=True)
    if not c.stdout.strip():
        raise RuntimeError(f'replays.py check: {c.stderr.strip()[-300:]}')
    c = json.loads(c.stdout)
    stop = vm.stop or {}
    got.update(ticks=c['ticks'], first_diff=c['first_diff'], records=len(vm.recs), dropped=vm.dropped,
               stop=[stop.get('hit'), stop.get('pc')],
               unwritten=sum(1 for x in vm.recs if x.get('unwritten')))
    n = spec['ticks']
    want = {'start12': START12[spec['start']['loadvm']], 'start_check': spec['start_check']['start_sha12'],
            'ram_start_halt': RAM01[replay][0], 'ticks': n, 'first_diff': None,
            'records': 3 * n + 1, 'dropped': 0, 'stop': [n, ENTRY], 'unwritten': 0}
    if w2:
        got['ram'] = vm.ram()
        want.update(load_path='levels/water/w2', ram=RAM01[replay][1])
    else:
        want['popup'] = POPUP01
    return got, want


def phys_rows(ctx, vm, hid, sched, row, ticks):
    """From a halt at the player tick (row), on to row `ticks`, writing each
    schedule row's lines to the pipe at its halt, tagged @F+1."""
    for k in sorted(sched):
        if k > row:
            vm.until([{'pc': TICK}], k - row, (k - row) * 8 + 300)
            row = k
        if len(vm.hits(hid)) != row:
            raise RuntimeError(f'row {row}: {len(vm.hits(hid))} records')
        F = vm.frame()
        vm.send([f'@{F + 1} {verb} {ctx.b.keys[name]}' for verb, name in (x.split() for x in sched[k])])
    vm.until([{'pc': TICK}], ticks - row, (ticks - row) * 8 + 300)


def cold_route(ctx, vm, got):
    """02 cold-jump's boot, bench-det --timeline's route by run-until: START
    at prophecy.bik and at the title, DPAD_LEFT, A (Start New Game), A on a
    stall until the arena player's event clock has run 20 s. Then the
    physics hook: (its id, the first row from 4 on in state 0)."""
    va, keys = bd.TL_VAS, ctx.b.keys

    def press(key, at):
        vm.send([f'@{at} down {keys[key]}', f'@{at + 6} up {keys[key]}'])

    def until_pressing(name, frames, key, tries):
        for _ in range(tries):
            r = vm.until([{'pc': va[name]}], 1, frames, need=False)
            if r['reason'] == 'hit':
                return r
            press(key, r['frame'] + 1)
        raise RuntimeError(f'cold boot: no {name} after {tries} {key} presses')

    press('start', vm.until([{'pc': va['prophecy']}], 1, 6000)['frame'] + 2)
    until_pressing('prophecy_end', 90, 'start', 40)
    trace = [{'address': va['boot_state'], 'size': 4, 'virtual': True}]
    for _ in range(200):
        r = vm.advance(10, trace=trace)
        if any(s['data'][0] and int.from_bytes(bytes.fromhex(s['data'][0]), 'little') == bd.TL_TITLE
               for s in r.get('trace', [])):
            break
    else:
        raise RuntimeError('cold boot: no title')
    press('start', r['frame'] + 1)
    r = until_pressing('inv_reset', 30, 'start', 60)   # the save list
    at = r['frame'] + 30
    press('dpad_left', at)
    r = vm.until([{'pc': va['inv_reset']}], 1, 30 + bd.TL_PREVIEW_WAIT, need=False)
    got['preview'] = r['reason'] == 'hit'
    press('a', r['frame'] + 30 if got['preview'] else max(at + bd.TL_PREVIEW_WAIT, r['frame'] + 1))
    lvl = vm.hooks([{'pcs': [va['load_level']], 'collect': [{'addr': '[esp+8]', 'size': 64}]}])[0]
    f_load = until_pressing('load_level', 1500, 'a', 4)['frame']
    clk = vm.hooks([{'pcs': [TICK], 'key': {'addr': '[esp+4]+0x18', 'size': 8},
                     'collect': [{'addr': '[esp+4]+0x18', 'size': 8}]}])[0]
    c0 = c1 = None
    last_a = -999
    while c1 is None or c1 - c0 < 20.0:
        n0 = len(vm.recs)
        r = vm.advance(30)
        clock = [struct.unpack('<d', bytes.fromhex(x['data'][0]))[0]
                 for x in vm.recs[n0:] if x['hook'] == clk]
        if clock:
            c0 = clock[0] if c0 is None else c0
            c1 = clock[-1]
        elif (c0 is not None or r['frame'] - f_load >= 900) and r['frame'] - last_a >= 30:
            last_a = r['frame']
            press('a', r['frame'] + 1)   # a dialog stalls the tick
        if r['frame'] - f_load > 9000:
            raise RuntimeError('cold boot: the arena clock did not run 20 s in 9000 frames')
    loads = vm.hits(lvl)
    got['level'] = cstr(loads[0]['data'][0]) if loads else None
    vm.cmd('xemu-hooks-clear')
    vm.advance(0)
    hid = vm.hooks([pc.hook()])[0]
    vm.until([{'pc': TICK}], 1, 600)
    vm.until([{'pc': TICK}], 3, 300)
    vm.until([{'pc': TICK, 'cond': [state_is(0)]}], 1, 600)
    return hid, len(vm.hits(hid))


def xbe_text(iso):
    """(VA, raw bytes) of default.xbe's .text in an XISO image."""
    with open(iso, 'rb') as f:
        for base in (0, 0x18300000, 0x2080000, 0x0FD90000):   # xiso, redump game partitions
            f.seek(base + 0x10000)
            hdr = f.read(28)
            if hdr[:20] == b'MICROSOFT*XBOX*MEDIA':
                break
        else:
            raise RuntimeError('no XDVDFS volume')
        sec, size = struct.unpack('<II', hdr[20:28])
        f.seek(base + sec * 2048)
        d, todo = f.read(size), [0]
        while todo:
            o = todo.pop() * 4
            if o + 14 > len(d) or d[o:o + 2] == b'\xff\xff':
                continue
            left, right, start, fsize = struct.unpack('<HHII', d[o:o + 12])
            todo += [x for x in (left, right) if x]
            if d[o + 14:o + 14 + d[o + 13]].lower() == b'default.xbe':
                f.seek(base + start * 2048)
                x = f.read(fsize)
                break
        else:
            raise RuntimeError('no default.xbe')
    xb = struct.unpack_from('<I', x, 0x104)[0]
    n, sh = struct.unpack_from('<II', x, 0x11C)
    for i in range(n):
        _, va, _, raddr, rsize, naddr = struct.unpack_from('<6I', x, sh - xb + i * 56)
        if x[naddr - xb:naddr - xb + 16].split(b'\0')[0] == b'.text':
            return va, x[raddr:raddr + rsize]
    raise RuntimeError('no .text section')


def case_02(ctx, cid):
    """Pack 02: physics_check.py's numbers from the per-tick hook; D98 cases
    from their loadvm, cold-jump from power-on (plus the .text check)."""
    case = PHYS[cid]
    tag = case['start'].get('loadvm')
    vm = ctx.launch()
    got, want = {}, {'physics': 'pass', 'records': case['ticks'], 'dropped': 0}
    if tag:
        vm.loadvm(tag)
        got['start12'] = vm.ram()
        want.update(start12=START12[tag], ram=END02[cid])
        poke = [{'pcs': [TICK], 'write': [{'addr': '[[esp+4]+0x20]+0x210', 'size': 4}],
                 'set': [struct.pack('<i', case['start']['hp']).hex()]}] if 'hp' in case['start'] else []
        hid = vm.hooks(poke + [pc.hook()])[-1]   # the poke lands at row 0, before its read
        vm.until([{'pc': TICK}], 1, 600)
        sched, row = {int(k): v for k, v in case['schedule'].items()}, 0
    else:
        hid, row = cold_route(ctx, vm, got)
        want.update(COLD02)
        sched = {row: ['down y'], row + 6: ['up y']}
    phys_rows(ctx, vm, hid, sched, row, case['ticks'])
    vm.take({'hooks': vm.cmd('xemu-hooks-drain')})
    recs = vm.hits(hid)
    try:
        got['physics'] = pc.judge(case, pc.numbers(case, pc.decode(recs)))['bad'] or 'pass'
    except Exception as e:   # rows that don't fit the case
        got['physics'] = f'{type(e).__name__}: {e}'
    got.update(records=len(recs), dropped=vm.dropped, ram=vm.ram())
    if not tag:
        va, raw = xbe_text(ctx.b.iso)
        f = os.path.join(vm.d, 'text.bin')
        vm.cmd('memsave', val=va, size=len(raw), filename=f)
        with open(f, 'rb') as fh:
            g = fh.read()
        got['text'] = [len(g), sum(x != y for x, y in zip(g, raw)) + abs(len(g) - len(raw))]
    return got, want


def case_03(ctx, tag):
    """Pack 03: a scene's check by collect hooks (scenes.json's check_hooks),
    with its +300/+600 RAM still det-goldens.json's d98 values."""
    vm = ctx.launch()
    base = vm.loadvm(tag)
    got = {'start12': vm.ram()}
    hid = vm.hooks([SCENES['check_hooks'][tag]])[0]
    vm.send(ctx.b.inputs[tag])
    for f in SCENES['stops']:
        vm.advance(f - (vm.frame() - base))
        got[f'f{f}'] = vm.ram(16)
    recs = vm.hits(hid)
    if tag == 'vanilla-rz-e2-cart':   # load_level's path
        got['paths'] = [f'+{x["frame"] - base} {cstr(x["data"][0])}' for x in recs]
    else:   # apply_damage(critter, new HP, flag) against the player's critter
        mine = [x['data'][0][:8] == x['data'][1] for x in recs]
        got['damage'] = {'player': sum(mine), 'others': len(mine) - sum(mine),
                         'first': recs[0]['frame'] - base if recs else None}
    return got, dict(CHECK03[tag], start12=START12[tag], **ctx.b.golden(ctx.arm, tag))


def blockstats(vm):
    return {b['device']: {'rd': b['stats']['rd_bytes'], 'wr': b['stats']['wr_bytes']}
            for b in vm.cmd('query-blockstats') if b.get('device') in ('ide0-hd0', 'ide0-cd1')}


def case_04(ctx, level):
    """Pack 04: one level's recorded warp from vanilla-beach-shield5 under the
    block log: RAM, block-log totals, blockstats deltas."""
    exp = TOUR['levels'][level]
    vm = ctx.launch(sets=KEYMAP232)
    base = vm.loadvm(TOUR['tag'])
    got = {'start12': vm.ram()}
    blog = os.path.join(vm.d, 'block.tsv')
    vm.cmd('xemu-block-log-start', filename=blog)
    vm.send(tagged(exp['lines'], base))
    bs0 = blockstats(vm)
    vm.advance(exp['frames'])
    bs1 = blockstats(vm)
    vm.cmd('xemu-block-log-stop')
    tot, status = {}, collections.Counter()
    with open(blog) as f:
        for row in csv.DictReader(f, delimiter='\t'):
            t = tot.setdefault(f'{row["drive"]} {row["op"]}', {'rows': 0, 'bytes': 0})
            t['rows'] += 1
            t['bytes'] += int(row['bytes'])
            status[row['status']] += 1
    got.update(ram=vm.ram(), block=tot, block_rows=sum(status.values()), statuses=sorted(status),
               blockstats={k: {'rd': bs1[k]['rd'] - bs0[k]['rd'], 'wr': bs1[k]['wr'] - bs0[k]['wr']}
                           for k in bs1})
    return got, {'start12': exp['start12'], 'ram': exp['ram12'], 'block': exp['block'],
                 'block_rows': exp['block_rows'], 'statuses': ['ok'], 'blockstats': exp['blockstats']}


def case_05(ctx, mode):
    """Pack 05: movie.json's request and hooks, then cut (run-until 0x53750
    twice, blockstats at each, F2 + 900) or one straight advance."""
    rend = ctx.b.a.renderer
    vm = ctx.launch(renderer=rend)
    base = vm.loadvm(MOVIE['tag'])
    got = {'start12': vm.ram()}
    bs0 = blockstats(vm)
    hid = vm.hooks(MOVIE['hooks'])[1]

    def io(bs, dev, k):
        return bs[dev][k] - bs0[dev][k]
    want = dict(O05, start12=MOVIE['start12'], dropped=0, ram=RAM05[rend])
    if mode == 'cut':   # under icount a run cut at hits is the uncut run
        at = []
        for _ in range(2):
            vm.until([{'pc': LOAD}], 1, 12000)
            at.append(blockstats(vm))
        got.update(dvd_to_F1=io(at[0], 'ide0-cd1', 'rd'), hdd_wr_F1=io(at[0], 'ide0-hd0', 'wr'),
                   dvd_F1_F2=io(at[1], 'ide0-cd1', 'rd') - io(at[0], 'ide0-cd1', 'rd'))
        vm.advance(MOVIE['tail'])
        want.update(CUT05)
    else:
        vm.advance(O05['frames'])
    bs = blockstats(vm)
    got.update(loads=[[x['frame'] - base, cstr(x['data'][0]), cstr(x['data'][1])] for x in vm.hits(hid)],
               dvd=io(bs, 'ide0-cd1', 'rd'), hdd_wr=io(bs, 'ide0-hd0', 'wr'), frames=vm.frame() - base,
               dropped=vm.dropped, ram=vm.ram())
    if rend == 'offscreen':
        got['driver'] = bd.vk_driver(vm.log).split(',')[0]
        if not bd.split_env(ctx.arm[2])[0].get('XEMU_VK_DRIVER'):
            want['driver'] = 'MoltenVK'   # the deterministic pin
    return got, want


def voice_summary(path, f0):
    """The voice log's rows, events, stop reasons, distinct start rates and the
    sha256[:12] of its rows with frames counted from the load (audio_song.py's)."""
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    cols = lines[0].lstrip('#').split()
    i_f, i_e, i_hz, i_w = (cols.index(c) for c in ('frame', 'event', 'hz', 'why'))
    ev, stops, rates, h, n = collections.Counter(), collections.Counter(), set(), hashlib.sha256(), 0
    for ln in lines[1:]:
        f = ln.split('\t')
        if len(f) < len(cols):
            continue
        n += 1
        ev[f[i_e]] += 1
        if f[i_e] == 'stop':
            stops[f[i_w]] += 1
        elif f[i_e] == 'start':
            rates.add(f[i_hz])
        f[i_f] = str(int(f[i_f]) - f0)
        h.update(('\t'.join(f) + '\n').encode())
    return {'rows': n, 'events': dict(ev), 'stops': dict(stops), 'rates': len(rates),
            'sha': h.hexdigest()[:12]}


def case_10(ctx, engine, replays):
    """Pack 10: e6's song (song.json's hook, results-v0.15.json's lines) under
    the voice log and the WAV, `replays` loads in one process."""
    vm = ctx.launch(sets=KEYMAP232 + DSP[engine])
    rep = SONG15['replays'][0]
    got, want = {}, {}
    for k in range(1, replays + 1):
        p = f'r{k}.'
        base = vm.loadvm(SONG['tag'])
        vm.recs = []
        got[p + 'start12'] = vm.ram()
        vm.cmd('xemu-hooks-clear')
        hid = vm.hooks(SONG['hooks'])[0]
        voice, wav = os.path.join(vm.d, f'voice-{k}.tsv'), os.path.join(vm.d, f'audio-{k}.wav')
        vm.cmd('xemu-voice-log-start', filename=voice)
        vm.cmd('xemu-audio-wav-start', filename=wav)
        vm.send(tagged(SONG15['lines'], base))
        vm.advance(SONG15['a']['frames'])
        vm.cmd('xemu-voice-log-stop')
        w = vm.cmd('xemu-audio-wav-stop')
        got[p + 'ram'] = vm.ram()
        got[p + 'wav'] = [sha12(wav), w.get('samples')]
        os.remove(wav)   # game audio: never kept
        got[p + 'voice'] = voice_summary(voice, base)
        recs = vm.hits(hid)
        got[p + 'setup'] = [sum('set' in x for x in recs), sum(1 for x in recs if x.get('unwritten'))]
        want.update({p + 'start12': SONG['start12'], p + 'ram': SONG15['a']['ram12'],
                     p + 'wav': [rep['wav']['sha12'], rep['wav']['samples']], p + 'setup': [1, 0],
                     p + 'voice': {'rows': rep['voice']['rows'], 'events': rep['voice']['events'],
                                   'stops': STOPS10, 'rates': rep['voice']['start_rates'],
                                   'sha': rep['voice']['sha12_from_load']}})
    with open(vm.log, errors='replace') as f:
        log = f.read()
    got['engine'] = ('upstream' if 'xemu: DSP JIT enabled (dsp56300 backend)' in log else
                     'fork' if 'xemu: DSP JIT enabled' in log else 'interp')
    want['engine'] = engine
    return got, want


def case_ckpt(ctx):
    """Checkpoints on e6 with the x24 lines: save c0, 300 frames (e6's f300
    golden), save c1 against c0, incremental restores (the second one by
    the dirty log, verified), xemu-run from c0, then a second loadvm into a
    warm TB cache. Every 300-frame RAM must be the same."""
    tag, lines = 'vanilla-rz-e6-song', SCENES['inputs']['x24']
    vm = ctx.launch()
    vm.loadvm(tag)
    got = {'start12': vm.ram()}

    def run():
        vm.send(lines)   # a restore drops queued lines: queue them for each run
        vm.advance(300)
        return vm.ram(16)

    def missed(execute, /, **kw):
        return vm.cmd(execute, verify=True, **kw).get('pages-missed')
    vm.cmd('xemu-checkpoint-save', name='c0')
    got['first'] = run()
    got['save_missed'] = missed('xemu-checkpoint-save', name='c1', base='c0')
    vm.cmd('xemu-checkpoint-restore', name='c0', incremental=True)   # the first compares every page
    got['restored'] = run()
    got['restore_missed'] = missed('xemu-checkpoint-restore', name='c0', incremental=True)
    got['verified'] = run()
    got['c1_missed'] = missed('xemu-checkpoint-restore', name='c1', incremental=True)
    got['c1'] = vm.ram(16)
    r = vm.cmd('xemu-run', checkpoint='c0', input=lines, frames=300)
    got['xemu_run'] = [r['reason'], r['frames'], vm.ram(16)]
    vm.loadvm(tag)
    got['reload_start12'] = vm.ram()
    got['reload'] = run()
    h = ctx.b.golden(ctx.arm, tag).get('f300') or got['first']
    return got, {'start12': START12[tag], 'first': h, 'save_missed': 0, 'restored': h, 'restore_missed': 0,
                 'verified': h, 'c1_missed': 0, 'c1': h, 'xemu_run': ['budget', 300, h],
                 'reload_start12': START12[tag], 'reload': h}


# --- the table, the runner ------------------------------------------------

Case = collections.namedtuple('Case', 'id core est fn args')   # est: seconds on a free box


def table():
    t = [Case('01-w2', True, 12, case_01, ('w2-walk-pickup',)),
         Case('01-w1', False, 10, case_01, ('w1-seg0',))]
    t += [Case('02-' + cid, 'loadvm' in c['start'], 3 if 'loadvm' in c['start'] else 15, case_02, (cid,))
          for cid, c in PHYS.items()]
    t += [Case('03-e2', True, 5, case_03, ('vanilla-rz-e2-cart',)),
          Case('03-e7', True, 5, case_03, ('vanilla-rz-e7-keylord',))]
    t += [Case('04-' + lv.rsplit('/', 1)[-1], True, 3, case_04, (lv,)) for lv in TOUR['levels']]
    t += [Case('05', True, 18, case_05, ('cut',)), Case('05-straight', False, 18, case_05, ('straight',))]
    t += [Case('10-' + e, e != 'interp', 18 if e == 'fork' else 10, case_10, (e, 2 if e == 'fork' else 1))
          for e in DSP]
    return t + [Case('ckpt', True, 8, case_ckpt, ())]


def select(a):
    t = table()
    picks = [x for x in a.cases.split(',') if x]
    for x in picks:
        if not any(c.id == x or c.id.startswith(x + '-') for c in t):
            sys.exit(f'azurik-pack: no case {x!r} (--list --all shows them)')
    out = [c for c in t if any(c.id == x or (c.id.startswith(x + '-') and (c.core or a.all))
                               for x in picks)] if picks else [c for c in t if c.core or a.all]
    if a.dsp:
        eng = a.dsp.split(',')
        if set(eng) - set(DSP):
            sys.exit(f'azurik-pack: --dsp takes {", ".join(DSP)}')
        if any(c.fn is case_10 for c in out):
            out = [c for c in out if c.fn is not case_10] + [c for c in t if c.id in ['10-' + e for e in eng]]
    if a.renderer == 'offscreen':
        out = [c._replace(est=c.est * 2) if c.fn is case_05 else c for c in out]
    return out


def run_case(b, case, arm):
    ctx, t0 = Ctx(b, case, arm), time.time()
    r = {'case': case.id, 'arm': arm[0]}
    try:
        got, want = case.fn(ctx, *case.args)
        r['notes'] = ctx.vm.quit()
        r['got'], r['want'] = got, want
        r['moved'] = [k for k in want if got.get(k, MISSING) != want[k]]
    except (Exception, SystemExit) as e:   # SystemExit: a bench-det helper's, in this thread
        msg = (ctx.expired + ': ' if ctx.expired else '') + (str(e) or type(e).__name__)
        r['error'] = (bd.keep_log(RuntimeError(msg), ctx.vm.log, f'{arm[0]}-{case.id}', keys=ctx.vm.keys)
                      if ctx.vm else msg)
    finally:
        r['cpu'] = ctx.vm and ctx.vm.cpu
        ctx.stack.close()   # kills a xemu still running, drops its lease, removes the sandbox
    r['wall'] = round(time.time() - t0, 1)
    return r


def short(v, n=160):
    s = v if isinstance(v, str) else json.dumps(v)
    return s if len(s) <= n else s[:n] + '...'


def report(r, width):
    if 'error' in r:
        verdict, text = 'FAIL', r['error']
    elif r['moved']:
        verdict = 'MOVED'
        text = '; '.join(f'{k} {short(r["got"].get(k))} (want {short(r["want"][k])})' for k in r['moved'])
    else:
        verdict = 'OK'
        text = ' '.join(f'{k} {r["got"][k]}' for k in ('ram', 'f600', 'r2.ram', 'reload', 'ticks', 'engine')
                        if k in r['got'])
    cpu = r.get('cpu')
    print(f'{r["case"]:{width}s} {r["arm"]:10s} {verdict:5s}  {text}  ({r["wall"]:.1f} s'
          + (f', {cpu[0]:.1f} s CPU, {cpu[1]:.1f} s runnable' if cpu else '') + ')'
          + ''.join(f'\n    {x}' for x in r.get('notes') or []), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--cases', default='', metavar='ID[,ID]',
                   help='case ids or pack prefixes (02, 04, 10); default: the core set')
    p.add_argument('--all', action='store_true', help='add the on-demand cases')
    p.add_argument('--renderer', choices=('null', 'offscreen'), default='null',
                   help='pack 05 through Vulkan (offscreen); the other packs stay null')
    p.add_argument('--dsp', default='', metavar='E[,E]',
                   help='pack 10 engines: fork, upstream, interp (default fork,upstream)')
    p.add_argument('-j', '--jobs', type=int, default=3, help='cases at once (default 3)')
    p.add_argument('--timeout-scale', type=float, default=None, metavar='X',
                   help='multiply the case limits (default 1; 4 for a debug build)')
    p.add_argument('--bundle', default=os.path.join(REPO, 'dist/xemu.app'), metavar='APP',
                   help='the bundle a bare-binary arm runs in a clone of (default dist/xemu.app)')
    p.add_argument('--out', default='', metavar='FILE', help='append one JSON line per case and arm')
    p.add_argument('--list', action='store_true', help='print the selected cases and exit')
    p.add_argument('arms', nargs='*', help='NAME=APP[:K=V,TABLE.KEY=V,...]; APP may be a bare binary')
    a = p.parse_args()
    sel = select(a)
    if a.list:
        for c in sel:
            print(f'{c.id:16s} {"core" if c.core else "on demand":9s} ~{c.est:g} s')
        print(f'{len(sel)} cases, ~{sum(c.est for c in sel):g} s of xemu time')
        return
    if not a.arms:
        p.error('needs at least one NAME=APP arm')
    fx = os.environ.get('AZURIK_FX', '')
    if not fx or not all(os.path.isfile(os.path.join(fx, f)) for f in ('xemu.toml', 'vanilla.iso')):
        sys.exit("azurik-pack: AZURIK_FX must name the Modkit's D98 fixtures folder "
                 '(xemu.toml, the drive, eeprom.bin, vanilla.iso)')
    bd.sweep_scratch()
    arms = []
    for spec in a.arms:   # as bench-det's: a comma splits only before K=
        name, eq, rest = spec.partition('=')
        app, _, envs = rest.partition(':')
        kvs = [kv for kv in re.split(r',(?=[A-Za-z_][A-Za-z0-9_.]*=)', envs) if kv]
        if not name or not eq or any('=' not in kv for kv in kvs):
            p.error(f'arm {spec!r}: want NAME=APP[:K=V,...]')
        arms.append((name, app, dict(kv.split('=', 1) for kv in kvs)))
    arms, tmp = bd.prepare_arms(a, arms)   # prints each arm's identity
    b = None
    try:
        b = Batch(a, fx)
        jobs = sorted(((c, arm) for c in sel for arm in arms), key=lambda j: -j[0].est)
        width = max(len(c.id) for c in sel)
        load = os.getloadavg()
        print(f'azurik-pack: {len(sel)} cases x {len(arms)} arm(s), -j {a.jobs}, load {load[0]:.1f}',
              flush=True)

        def stop(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGINT, stop)   # also when started in the background
        signal.signal(signal.SIGTERM, stop)
        out = open(a.out, 'a') if a.out else None
        t0, results = time.time(), []
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs))
        try:
            for fut in concurrent.futures.as_completed([pool.submit(run_case, b, c, arm) for c, arm in jobs]):
                r = fut.result()
                results.append(r)
                report(r, width)
                if out:
                    out.write(json.dumps(r) + '\n')
                    out.flush()
        except KeyboardInterrupt:
            bd.STOP.set()
            for proc in list(bd.LIVE):
                proc.kill()
            pool.shutdown(wait=False, cancel_futures=True)
            print('azurik-pack: interrupted; runs killed')
            sys.exit(1)
        pool.shutdown()
        by = collections.defaultdict(dict)
        for r in results:
            by[r['case']][r['arm']] = r
        disagree = []
        for cid, rs in by.items():
            gots = [r['got'] for r in rs.values() if 'got' in r]
            keys = sorted(k for k in set().union(*gots)
                          if len({json.dumps(g.get(k), sort_keys=True) for g in gots}) > 1) if gots else []
            if keys:
                disagree.append(f'{cid} ({", ".join(keys)})')
        moved = [r['case'] for r in results if r.get('moved')]
        failed = [r['case'] for r in results if 'error' in r]
        load = os.getloadavg()
        print(f'{len(results) - len(moved) - len(failed)} OK, {len(moved)} MOVED, {len(failed)} FAIL '
              f'in {time.time() - t0:.0f} s; load {load[0]:.1f}'
              + (f'; moved: {" ".join(sorted(set(moved)))}' if moved else '')
              + (f'; failed: {" ".join(sorted(set(failed)))}' if failed else ''), flush=True)
        if disagree:
            print('arms disagree: ' + '; '.join(disagree))
            sys.exit(2)
        sys.exit(4 if moved else 1 if failed else 0)
    finally:
        if b:
            shutil.rmtree(b.dir, ignore_errors=True)
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
