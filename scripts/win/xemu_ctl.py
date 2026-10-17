#!/usr/bin/env python3
"""xemu_ctl.py - one-shot QMP, HMP and gdbstub calls to a running xemu.

    xemu_ctl.py wait     QMP_PORT [TIMEOUT]         until query-xemu-status says ready
    xemu_ctl.py qmp      QMP_PORT CMD [key=val ...]  one QMP command; prints its return as JSON
    xemu_ctl.py hmp      QMP_PORT WORDS ...          one HMP command line; prints its output
    xemu_ctl.py loadvm   QMP_PORT TAG [TIMEOUT]      HMP loadvm, then wait until the load counts
    xemu_ctl.py gdbcheck GDB_PORT PC_HEX [TIMEOUT]   attach, break at PC, continue, check, detach
    xemu_ctl.py shmhash  NAME [SIZE]                 sha256 of shared memory NAME (first SIZE bytes)

xemu must listen on -qmp tcp:127.0.0.1:PORT,server=on,wait=off (and -gdb
tcp:127.0.0.1:PORT for gdbcheck). key=val values: true/false become booleans,
integers become ints, anything else stays a string, so no JSON quoting has to
survive PowerShell 5.1. QMP serves one client at a time; every call connects,
runs and disconnects. Exit status: 0 ok, 1 failed, 2 usage.
"""
import hashlib
import json
import socket
import sys
import time

LONG = 3600  # seconds one QMP command may take (a long xemu-frame-advance)


def die(msg, code=1):
    print('xemu_ctl: ' + msg, file=sys.stderr, flush=True)
    sys.exit(code)


class QMP:
    def __init__(self, port, timeout=60.0):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
                break
            except OSError:
                if time.time() > end:
                    raise RuntimeError('no QMP listener on 127.0.0.1:%d' % port)
                time.sleep(0.25)
        self.s.settimeout(LONG)
        self.f = self.s.makefile('rw', encoding='utf-8', newline='\n')
        if not self.f.readline():
            raise RuntimeError('QMP closed before its greeting')
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        msg = {'execute': name}
        if args:
            msg['arguments'] = args
        self.f.write(json.dumps(msg) + '\n')
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError('QMP closed during ' + name)
            r = json.loads(line)
            if 'event' in r:
                continue
            if 'error' in r:
                err = r['error']
                raise RuntimeError('%s: %s %s' % (name, err.get('class', ''), err.get('desc', err)))
            return r.get('return')

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def wait_ready(q, timeout, proc=None):
    end = time.time() + timeout
    while not q.cmd('query-xemu-status').get('ready'):
        if proc is not None and proc.poll() is not None:
            raise RuntimeError('xemu exited (%s) before it was ready' % proc.returncode)
        if time.time() > end:
            raise RuntimeError('xemu not ready after %ds' % timeout)
        time.sleep(0.1)


def loadvm(q, tag, timeout):
    before = q.cmd('query-xemu-status')['loads']
    out = q.hmp('loadvm ' + tag)
    if 'rror' in out:
        raise RuntimeError('loadvm %s: %s' % (tag, out.strip()))
    end = time.time() + timeout
    while q.cmd('query-xemu-status')['loads'] <= before:
        if time.time() > end:
            raise RuntimeError('loadvm %s: no completed load after %ds' % (tag, timeout))
        time.sleep(0.1)


class RSP:
    """The few gdb remote-protocol packets gdbcheck needs."""

    def __init__(self, port, timeout):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
                break
            except OSError:
                if time.time() > end:
                    raise RuntimeError('no gdbstub on 127.0.0.1:%d' % port)
                time.sleep(0.25)
        self.s.settimeout(timeout)
        self.buf, self.ack = b'', True
        self.ack = self.cmd('QStartNoAckMode') != 'OK'

    def send(self, p):
        b = p.encode('latin-1')
        self.s.sendall(b'$%s#%02x' % (b, sum(b) & 0xff))

    def reply(self):
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

    def cmd(self, p):
        self.send(p)
        return self.reply()

    def eip(self):  # i386 'g' order: eax ecx edx ebx esp ebp esi edi eip
        return int.from_bytes(bytes.fromhex(self.cmd('g')[64:72]), 'little')


def gdbcheck(port, pc, timeout):
    r = RSP(port, timeout)
    print('attach: ' + r.cmd('?'))
    if r.cmd('Z0,%x,1' % pc) != 'OK':
        raise RuntimeError('the stub refused Z0 at %#x' % pc)
    t0 = time.time()
    r.send('c')
    stop = r.reply()
    took = time.time() - t0
    eip = r.eip()
    mem = r.cmd('m%x,10' % pc)
    r.cmd('z0,%x,1' % pc)
    r.send('D')
    try:
        r.reply()
    except (EOFError, OSError):
        pass
    ok = stop[:1] in ('T', 'S') and eip == pc
    print('stop=%s after %.2fs eip=%#x bytes@pc=%s -> %s' % (stop[:3], took, eip, mem, 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def value(v):
    if v in ('true', 'false'):
        return v == 'true'
    try:
        return int(v)
    except ValueError:
        return v


def main(argv):
    if len(argv) < 3 or argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0 if len(argv) > 1 and argv[1] in ('-h', '--help') else 2
    op, rest = argv[1], argv[3:]
    try:
        if op == 'shmhash':
            from multiprocessing import shared_memory
            m = shared_memory.SharedMemory(name=argv[2])
            try:
                n = int(rest[0]) if rest else m.size
                print(hashlib.sha256(bytes(m.buf[:n])).hexdigest())
            finally:
                m.close()
            return 0
        port = int(argv[2])
        if op == 'gdbcheck':
            return gdbcheck(port, int(rest[0], 16), float(rest[1]) if len(rest) > 1 else 30.0)
        q = QMP(port)
        rc = 0
        if op == 'wait':
            wait_ready(q, float(rest[0]) if rest else 120.0)
            print(json.dumps(q.cmd('query-xemu-status')))
        elif op == 'qmp':
            if not rest:
                die('qmp needs a command name', 2)
            args = {}
            for kv in rest[1:]:
                k, sep, v = kv.partition('=')
                if not sep:
                    die('bad argument %r (want key=value)' % kv, 2)
                args[k] = value(v)
            try:
                print(json.dumps(q.cmd(rest[0], **args)))
            except EOFError:
                if rest[0] != 'quit':
                    raise
        elif op == 'hmp':
            out = q.hmp(' '.join(rest))
            print(out, end='' if out.endswith('\n') else '\n')
            rc = 1 if 'rror' in out else 0
        elif op == 'loadvm':
            loadvm(q, rest[0], float(rest[1]) if len(rest) > 1 else 90.0)
            print('loaded ' + rest[0])
        else:
            die('unknown op %r' % op, 2)
        q.close()
        return rc
    except (RuntimeError, EOFError, OSError, ValueError, IndexError) as e:
        die('%s: %s' % (type(e).__name__, e))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
