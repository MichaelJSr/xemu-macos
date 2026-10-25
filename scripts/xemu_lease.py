#!/usr/bin/env python3
"""How the fork's automated xemu launches identify themselves.

Every xemu a macOS launcher in scripts/ starts (the Azurik harness
relies on this; PROMPT-7 A; the Windows bench carries only the marker):

  - carries the argv marker `-name xemu-fork-auto` right after the
    binary (MARK), so `ps -axo command` tells it from the owner's
    interactive xemu, which never has it;
  - runs under `nice -n 10` (NICE), as the harness's sandboxes do;
  - holds one of XEMU_FORK_SLOTS (default 4) machine-wide slots for as
    long as it runs: flock(2) on <lease dir>/slots/<i>.lock, through a
    descriptor the xemu inherits. A flock lock belongs to the open file,
    not the process, so it stays with the xemu when its launcher dies
    (SIGKILL included) and goes when the xemu exits. On macOS flock and
    fcntl locks exclude each other, so older launchers' lockf slots
    count;
  - has a lease <lease dir>/<xemu pid>.json {who, purpose, started,
    expected_end, ...}, removed when it exits. A lease is live only while
    its pid runs with the marker in its argv; anything else is stale and
    the next launcher sweeps it;
  - keeps its data (shader and pipeline caches) in the bench cache,
    XEMU_BASE_PATH=$XEMU_BENCH_CACHE (default
    ~/Library/Caches/xemu-fork-bench), never in the owner's pref dir;
  - never inherits AUTOMATION_ENV: the variables that name something
    another xemu or the harness may own (the owner's XEMU_RAM_SHM name,
    a pipe, a record or log file, an XEMU_QMP_WATCH socket), that bind
    host pads, that pick the
    Vulkan driver (an exported XEMU_VK_DRIVER would silently take every
    deterministic and offscreen run off its MoltenVK pin), that change
    what the guest reads (XEMU_LANGUAGE), or that opt a run into a
    harness interface that changes it (XEMU_RENDER_NULL_RAM,
    XEMU_LOADVM_SHRINK_RAM, XEMU_AGP_FULL_DECODE, XEMU_CSTATUS_FULL_RAM,
    XEMU_SOCKET_BACKLOG, XEMU_STRICT_CPUID, XEMU_GUEST_BUGCHECK: an
    inherited one would change both arms of a bench). The normal-mode
    switches an A/B sets per arm are not in it. A launcher
    drops them from its own environment; a value it sets itself stays;
  - never starts while an owner demo runs: while /tmp/xemu-leases/
    owner-demo.json (or, under a moved lease dir, its owner-demo.json)
    names a running pid, a launcher waits before taking its slot,
    saying so on stderr every minute (XEMU_OWNER_DEMO_WAIT=0: it exits
    75 instead). The harness writes that file for each owner sitting: a
    JSON object with at least "pid" (int: a process that lives as long
    as the sitting); "who", "purpose" and "expected_end" (strings) are
    shown if present. A dead pid or a file that doesn't parse counts as
    no demo, so write it atomically (a temp file in the lease dir, then
    rename).

The lease dir is $XEMU_LEASE_DIR, default /tmp/xemu-leases (the harness
reads it; tests point the variable elsewhere). A moved lease dir has its
own leases and slots, but /tmp/xemu-leases/owner-demo.json is honoured
either way: the override never turns the demo guard off.

zsh twin: scripts/xemu-lease.zsh. `python3 xemu_lease.py` lists the live
leases and sweeps the stale ones. A launch by hand takes a slot too:

    python3 scripts/xemu_lease.py run [--env K=V]... [--purpose TEXT] \
        [--secs N] -- <xemu> -config_path <scratch toml> [args...]

waits for a slot, writes the lease, and execs `nice`d `<xemu> -name
xemu-fork-auto args` in its own process (same pid), with the inherited
AUTOMATION_ENV dropped (--env sets any variable for this run) and
XEMU_BASE_PATH set to the bench cache unless already set; the xemu
keeps the slot until it exits. It refuses a binary without
XEMU_BASE_PATH (no such string in it: any build from a tree without
0babe23fbc, whatever its version) unless its clone is portable (an
xemu.toml in Contents/Resources): it would keep its caches in the
owner's pref dir.
"""
import contextlib, json, os, subprocess, sys, time

try:
    import fcntl
except ImportError:  # Windows: no slots or leases, the marker still applies
    fcntl = None

MARKER = 'xemu-fork-auto'
MARK = ['-name', MARKER]
NICE = ['nice', '-n', '10']
DEFAULT_LEASE_DIR = '/tmp/xemu-leases'
LEASE_DIR = os.environ.get('XEMU_LEASE_DIR') or DEFAULT_LEASE_DIR
AUTOMATION_ENV = ('XEMU_RAM_SHM', 'XEMU_INPUT_PIPE', 'XEMU_INPUT_RECORD',
                  'XEMU_INPUT_RECORD_PORT', 'XEMU_TRACE_RECORD', 'XEMU_TRACE_REGIONS',
                  'XEMU_BLOCK_LOG', 'XEMU_VOICE_LOG', 'XEMU_AUDIO_WAV',
                  'XEMU_ICOUNT_LIVE_PADS', 'XEMU_VK_DRIVER', 'XEMU_QMP_WATCH',
                  'XEMU_LANGUAGE', 'XEMU_RENDER_NULL_RAM', 'XEMU_LOADVM_SHRINK_RAM',
                  'XEMU_AGP_FULL_DECODE', 'XEMU_CSTATUS_FULL_RAM', 'XEMU_SOCKET_BACKLOG',
                  'XEMU_STRICT_CPUID', 'XEMU_GUEST_BUGCHECK')
OWNER_DEMO = 'owner-demo.json'
_dropped = set()
_warned = set()


def environ(env=None, keep=()):
    """env (default os.environ) for an automated xemu: without the
    AUTOMATION_ENV values it inherited from this process's environment.
    A value the caller set itself stays: a different one, or any key in
    keep (the caller's own, even when it equals the inherited value)."""
    e = dict(os.environ if env is None else env)
    for k in AUTOMATION_ENV:
        if k in os.environ and k not in keep and e.get(k) == os.environ[k]:
            del e[k]
            if k not in _dropped:
                _dropped.add(k)
                print(f'lease: not passing the inherited {k} to xemu (set it per run)',
                      file=sys.stderr, flush=True)
    return e


def bench_cache():
    """The benches' own data base path (created on first use). Warns once
    when it holds a KosmicKrisp strike marker (runs whose config says
    driver = 'kosmickrisp' then use MoltenVK for that build;
    docs/kosmickrisp.md)."""
    d = os.environ.get('XEMU_BENCH_CACHE') or os.path.expanduser(
        '~/Library/Caches/xemu-fork-bench')
    os.makedirs(d, exist_ok=True)
    strike = os.path.join(d, 'kosmickrisp-strike.txt')
    if strike not in _warned and os.path.exists(strike):
        _warned.add(strike)
        with contextlib.suppress(OSError):
            build = open(strike, errors='replace').readline().strip()
            print(f"lease: {strike} exists: runs with driver = 'kosmickrisp' use MoltenVK for {build}; "
                  'remove it if a killed run left it', file=sys.stderr, flush=True)
    return d


def _is_ours(pid):
    try:
        r = subprocess.run(['ps', '-o', 'command=', '-p', str(pid)],
                           capture_output=True, text=True)
    except OSError:
        return True  # can't tell: keep the lease
    return r.returncode > 1 or f'-name {MARKER}' in r.stdout


def sweep():
    """Remove leases whose xemu is gone (older than 10 s: a launcher writes
    its lease before the fork has exec'd the marked argv)."""
    try:
        names = os.listdir(LEASE_DIR)
    except OSError:
        return
    now = time.time()
    for n in names:
        if not (n.endswith('.json') and n[:-5].isdigit()):
            continue
        f = os.path.join(LEASE_DIR, n)
        try:
            if now - os.stat(f).st_mtime > 10 and not _is_ours(int(n[:-5])):
                os.remove(f)
        except OSError:
            pass


def owner_demo_files():
    """<lease dir>/owner-demo.json and, always, the harness's
    /tmp/xemu-leases/owner-demo.json: XEMU_LEASE_DIR moves the leases and
    slots, never the demo guard."""
    return list(dict.fromkeys(os.path.normpath(os.path.join(d, OWNER_DEMO))
                              for d in (LEASE_DIR, DEFAULT_LEASE_DIR)))


def owner_demo():
    """(file, record) for the first owner-demo.json whose pid runs, else
    None."""
    for f in owner_demo_files():
        rec = _owner_demo_record(f)
        if rec is not None:
            return f, rec
    return None


def _owner_demo_record(f):
    try:
        with open(f) as fh:
            rec = json.load(fh)
        if isinstance(rec['pid'], bool):
            raise TypeError
        pid = int(rec['pid'])
    except FileNotFoundError:
        return None
    except (OSError, ValueError, KeyError, TypeError):
        if f not in _dropped:
            _dropped.add(f)
            print(f'lease: ignoring {f}: not a JSON object with an integer "pid"',
                  file=sys.stderr, flush=True)
        return None
    if pid <= 0:
        return None
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return None
    except PermissionError:
        pass  # runs, as another user
    return rec


def wait_owner_demo(quiet=False):
    """Return once no owner demo runs (XEMU_OWNER_DEMO_WAIT=0: exit 75)."""
    t0 = time.time()
    said = None
    while (found := owner_demo()) is not None:
        where, rec = found
        what = f'pid {rec["pid"]}'
        for k in ('who', 'purpose', 'expected_end'):
            if rec.get(k):
                what += f', {k} {rec[k]}'
        if os.environ.get('XEMU_OWNER_DEMO_WAIT') == '0':
            print(f'lease: owner demo running ({what}; {where}): not starting xemu '
                  '(XEMU_OWNER_DEMO_WAIT=0)', file=sys.stderr, flush=True)
            raise SystemExit(75)
        w = int(time.time() - t0)
        if not quiet and (said is None or w - said >= 60):
            print(f'lease: owner demo running ({what}; {where}): no new xemu until it '
                  'ends; waiting', file=sys.stderr, flush=True)
            said = w
        time.sleep(2)
    if said is not None and not quiet:
        print(f'lease: owner demo over after {time.time() - t0:.0f} s',
              file=sys.stderr, flush=True)


class Slot:
    """One of XEMU_FORK_SLOTS machine-wide slots: an flock'd descriptor
    (fd). The xemu inherits it (popen's pass_fds, run's exec) and holds
    the slot until it exits; handoff() then drops the launcher's copy.
    Until then, or without a child, release() frees it. Taken only while
    no owner demo runs."""

    def __init__(self, quiet=False):
        self.index, self.fd = None, None
        if fcntl is None:
            return
        try:
            n = max(1, int(os.environ.get('XEMU_FORK_SLOTS', '4')))
        except ValueError:
            n = 4
        d = os.path.join(LEASE_DIR, 'slots')
        os.makedirs(d, exist_ok=True)
        sweep()
        t0 = time.time()
        said = None
        while True:
            wait_owner_demo(quiet)
            for i in range(n):
                fd = os.open(os.path.join(d, f'{i}.lock'), os.O_RDWR | os.O_CREAT, 0o666)
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError:
                    os.close(fd)
                    continue
                if owner_demo() is not None:  # one began meanwhile: wait for it
                    os.close(fd)
                    break
                self.index, self.fd = i, fd
                if said is not None and not quiet:
                    print(f'lease: slot {i} free after {time.time() - t0:.0f} s',
                          file=sys.stderr, flush=True)
                return
            if owner_demo() is not None:
                continue
            w = int(time.time() - t0)
            if not quiet and (said is None or w - said >= 60):
                print(f'lease: all {n} fork-xemu slots busy (XEMU_FORK_SLOTS); waiting',
                      file=sys.stderr, flush=True)
                said = w
            time.sleep(2)

    def handoff(self):
        """The xemu inherited fd: drop the launcher's copy, so the slot
        lasts exactly as long as that xemu (index stays for the lease)."""
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def release(self):
        if self.fd is not None:
            os.close(self.fd)  # the last descriptor of the open file drops the lock
        self.index, self.fd = None, None


def _stamp(t):
    return time.strftime('%Y-%m-%dT%H:%M:%S%z', time.localtime(t))


def write_lease(pid, purpose, expected_secs, slot=None, who=None):
    now = time.time()
    rec = {'pid': pid, 'who': who or os.path.basename(sys.argv[0]),
           'purpose': purpose, 'started': _stamp(now),
           'expected_end': _stamp(now + expected_secs),
           'started_epoch': int(now), 'expected_end_epoch': int(now + expected_secs),
           'slot': slot, 'launcher_pid': os.getpid(), 'marker': MARKER}
    os.makedirs(LEASE_DIR, exist_ok=True)
    f = os.path.join(LEASE_DIR, f'{pid}.json')
    with open(f'{f}.{os.getpid()}.tmp', 'w') as out:
        json.dump(rec, out)
    os.replace(f'{f}.{os.getpid()}.tmp', f)


def drop_lease(pid):
    with contextlib.suppress(OSError):
        os.remove(os.path.join(LEASE_DIR, f'{pid}.json'))


def auto_cmd(cmd):
    """NICE + the binary + MARK + its arguments."""
    return NICE + [cmd[0]] + MARK + list(cmd[1:])


@contextlib.contextmanager
def popen(cmd, purpose, expected_secs, keep=(), **kw):
    """subprocess.Popen(auto_cmd(cmd), **kw) inside a slot and a lease,
    with environ(kw's env, keep). nice execs xemu, so proc.pid is xemu's
    pid, and xemu inherits the slot's descriptor. On exit a still-running
    xemu is killed, then the lease goes."""
    kw['env'] = environ(kw.get('env'), keep)
    slot = Slot()
    proc = None
    try:
        if slot.fd is not None:
            kw['pass_fds'] = tuple(kw.get('pass_fds', ())) + (slot.fd,)
        proc = subprocess.Popen(auto_cmd(cmd), **kw)
        slot.handoff()
        if fcntl is not None:
            write_lease(proc.pid, purpose, expected_secs, slot.index)
        yield proc
    finally:
        if proc is not None:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            drop_lease(proc.pid)
        slot.release()


def knows_base_path(binary):
    """True if xemu binary honours XEMU_BASE_PATH, or runs portable (an
    xemu.toml in SDL's base path: the bundle's Contents/Resources, or a
    bare binary's folder)."""
    d = os.path.dirname(os.path.abspath(binary))
    if any(os.path.exists(os.path.join(p, 'xemu.toml'))
           for p in (os.path.join(d, '..', 'Resources'), d)):
        return True
    import mmap
    with open(binary, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        return m.find(b'XEMU_BASE_PATH') >= 0


def run(argv):
    """`run [--env K=V]... [--purpose TEXT] [--secs N] -- <xemu> args`: a
    launch by hand, as the launchers do it (module docstring). Never
    returns."""
    import argparse
    ap = argparse.ArgumentParser(prog='xemu_lease.py run')
    ap.add_argument('--env', action='append', default=[], metavar='K=V',
                    help='set for xemu (AUTOMATION_ENV is never inherited)')
    ap.add_argument('--purpose', default='hand launch')
    ap.add_argument('--secs', type=int, default=3600, help='expected run time (lease)')
    ap.add_argument('cmd', nargs=argparse.REMAINDER, help='-- <xemu> args...')
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ['--'] else a.cmd
    if not cmd or not os.access(cmd[0], os.X_OK):
        ap.error('needs -- <xemu binary> [args]')
    if '-config_path' not in cmd:
        ap.error('needs -config_path <scratch toml>: xemu saves its config at exit')
    if not knows_base_path(cmd[0]):
        ap.error(f'{cmd[0]} predates XEMU_BASE_PATH and would keep its caches in the '
                 "owner's pref dir: put a scratch xemu.toml in its clone's "
                 'Contents/Resources (portable mode) or use a newer build')
    env = environ()
    for kv in a.env:
        k, eq, v = kv.partition('=')
        if not k or not eq:
            ap.error(f'--env {kv!r}: want K=V')
        env[k] = v
    env.setdefault('XEMU_BASE_PATH', bench_cache())
    slot = Slot()
    if slot.fd is not None:
        os.set_inheritable(slot.fd, True)   # exec keeps it: the xemu holds the slot
        write_lease(os.getpid(), a.purpose, a.secs, slot.index, who='xemu_lease.py run')
    if hasattr(os, 'nice'):
        os.nice(10)
    os.execve(cmd[0], [cmd[0]] + MARK + cmd[1:], env)


def main():
    if sys.argv[1:2] == ['run']:
        run(sys.argv[2:])
    sweep()
    found = owner_demo()
    if found is not None:
        print(f'owner demo running ({found[0]}): {json.dumps(found[1])}')
    try:
        names = sorted(os.listdir(LEASE_DIR))
    except OSError:
        names = []
    for n in names:
        if n.endswith('.json') and n[:-5].isdigit():
            with contextlib.suppress(OSError, ValueError, KeyError):
                r = json.load(open(os.path.join(LEASE_DIR, n)))
                print(f'{r["pid"]:>6} slot {r.get("slot")} {r["who"]}: {r["purpose"]} '
                      f'(started {r["started"]}, expected end {r["expected_end"]})')


if __name__ == '__main__':
    main()
