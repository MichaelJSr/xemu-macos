#!/usr/bin/env python3
"""How the fork's automated xemu launches identify themselves.

Every xemu a macOS launcher in scripts/ starts (the Azurik harness
relies on this; PROMPT-7 A; the Windows bench carries only the marker):

  - carries the argv marker `-name xemu-fork-auto` right after the
    binary (MARK), so `ps -axo command` tells it from the owner's
    interactive xemu, which never has it;
  - runs under `nice -n 10` (NICE), as the harness's sandboxes do;
  - holds one of XEMU_FORK_SLOTS (default 4) machine-wide slots while it
    runs: fcntl locks on /tmp/xemu-leases/slots/<i>.lock, so a launcher
    that dies frees its slot;
  - has a lease /tmp/xemu-leases/<xemu pid>.json {who, purpose, started,
    expected_end, ...}, removed when it exits. A lease is live only while
    its pid runs with the marker in its argv; anything else is stale and
    the next launcher sweeps it;
  - keeps its data (shader and pipeline caches) in the bench cache,
    XEMU_BASE_PATH=$XEMU_BENCH_CACHE (default
    ~/Library/Caches/xemu-fork-bench), never in the owner's pref dir;
  - never inherits AUTOMATION_ENV: the variables that name something
    another xemu or the harness may own (the owner's XEMU_RAM_SHM name,
    a pipe, a record or log file) or that bind host pads. A launcher
    drops them from its own environment; a value it sets itself stays.

zsh twin: scripts/xemu-lease.zsh. `python3 xemu_lease.py` lists the live
leases and sweeps the stale ones.
"""
import contextlib, json, os, subprocess, sys, time

try:
    import fcntl
except ImportError:  # Windows: no slots or leases, the marker still applies
    fcntl = None

MARKER = 'xemu-fork-auto'
MARK = ['-name', MARKER]
NICE = ['nice', '-n', '10']
LEASE_DIR = '/tmp/xemu-leases'
AUTOMATION_ENV = ('XEMU_RAM_SHM', 'XEMU_INPUT_PIPE', 'XEMU_INPUT_RECORD',
                  'XEMU_INPUT_RECORD_PORT', 'XEMU_TRACE_RECORD', 'XEMU_TRACE_REGIONS',
                  'XEMU_BLOCK_LOG', 'XEMU_VOICE_LOG', 'XEMU_AUDIO_WAV',
                  'XEMU_ICOUNT_LIVE_PADS')
_held = set()
_dropped = set()


def environ(env=None):
    """env (default os.environ) for an automated xemu: without the
    AUTOMATION_ENV values it inherited from this process's environment.
    A value the caller set itself (not the inherited one) stays."""
    e = dict(os.environ if env is None else env)
    for k in AUTOMATION_ENV:
        if k in os.environ and e.get(k) == os.environ[k]:
            del e[k]
            if k not in _dropped:
                _dropped.add(k)
                print(f'lease: not passing the inherited {k} to xemu (set it per run)',
                      file=sys.stderr, flush=True)
    return e


def bench_cache():
    """The benches' own data base path (created on first use)."""
    d = os.environ.get('XEMU_BENCH_CACHE') or os.path.expanduser(
        '~/Library/Caches/xemu-fork-bench')
    os.makedirs(d, exist_ok=True)
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


class Slot:
    """One of XEMU_FORK_SLOTS machine-wide slots, held until release()."""

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
            for i in range(n):
                if i in _held:
                    continue
                fd = os.open(os.path.join(d, f'{i}.lock'), os.O_RDWR | os.O_CREAT, 0o666)
                try:
                    fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError:
                    os.close(fd)
                    continue
                self.index, self.fd = i, fd
                _held.add(i)
                if said is not None and not quiet:
                    print(f'lease: slot {i} free after {time.time() - t0:.0f} s',
                          file=sys.stderr, flush=True)
                return
            w = int(time.time() - t0)
            if not quiet and (said is None or w - said >= 60):
                print(f'lease: all {n} fork-xemu slots busy (XEMU_FORK_SLOTS); waiting',
                      file=sys.stderr, flush=True)
                said = w
            time.sleep(2)

    def release(self):
        if self.fd is not None:
            os.close(self.fd)  # closing the descriptor drops the lock
            _held.discard(self.index)
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
def popen(cmd, purpose, expected_secs, **kw):
    """subprocess.Popen(auto_cmd(cmd), **kw) inside a slot and a lease,
    with environ(kw's env). nice execs xemu, so proc.pid is xemu's pid.
    On exit a still-running xemu is killed, then the lease and the slot
    go."""
    kw['env'] = environ(kw.get('env'))
    slot = Slot()
    proc = None
    try:
        proc = subprocess.Popen(auto_cmd(cmd), **kw)
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


def main():
    sweep()
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
