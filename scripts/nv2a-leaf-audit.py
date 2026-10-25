#!/usr/bin/env python3
"""Check the threaded pusher's leaf kelvin methods against the leaf rule.

usage: python3 -I scripts/nv2a-leaf-audit.py [--build DIR] [-v]

pgraph.c's pgraph_leaf_always / pgraph_leaf_short_inline lists (plus
ARRAY_ELEMENT16/32 and DRAW_ARRAYS, kinds set in pgraph_kelvin_leaf_init)
name the handlers pfifo_threaded_command runs holding pfifo.lock. This
compiles hw/xbox/nv2a's non-renderer sources to LLVM IR (-O0, the build's
own flags from DIR/compile_commands.json; default build/), follows every
function each listed handler calls or references, and fails if one reaches
what its kind doesn't allow: a lock, the BQL, a cond, event or thread call,
a PFIFO kick, an interrupt, a renderer function or an indirect call (a
renderer op). Run it after an upstream merge that touches the handlers.
-v also lists unlisted handlers that pass (candidates). Exit 0 clean, 1 a
violation, 2 a usage or build problem.
"""
import argparse, json, os, re, shlex, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORBID = re.compile(
    r'^(qemu_mutex_\w+|qemu_rec_mutex_\w+|bql_\w+|qemu_cond_\w+|qemu_event_\w+'
    r'|qemu_sem_\w+|pthread_\w+|qemu_thread_\w+|pfifo_kick|pfifo_guest_kick'
    r'|nv2a_update_irq|pgraph_(?:vk|gl|null)_\w+|nv2a_gc_flip'
    r'|pgraph_context_switch|nv2a_vsh_\w+|vsh_\w+)$')
# What a kind's condition (pgraph_leaf_kelvin_method) keeps from running
ALLOWED = {
    'always': set(),
    'short_inline': {'pgraph_allocate_inline_buffer_vertices'},
    'one_draw_array': {'pgraph_expand_draw_arrays'},
    'no_gc': {'nv2a_gc_draw_arrays', 'nv2a_gc_end', 'nv2a_gc_begin'},
}


def emit_ir(build, out):
    cc = json.load(open(os.path.join(build, 'compile_commands.json')))
    ents = [e for e in cc if '/hw/xbox/nv2a/' in e['file'] and e['file'].endswith('.c')
            and not re.search(r'/(vk|gl|null|thirdparty)/', e['file'])]
    for e in ents:
        args, keep, skip = shlex.split(e['command']), [], False
        for a in args:
            if skip:
                skip = False
            elif a in ('-o', '-MF', '-MQ'):
                skip = True
            elif not (a in ('-c', '-MD') or a.startswith(('-fprofile', '-flto', '-O'))):
                keep.append(a)
        dst = os.path.join(out, e['file'].split('/hw/xbox/nv2a/')[1].replace('/', '_') + '.ll')
        r = subprocess.run(keep + ['-O0', '-S', '-emit-llvm', '-o', dst], cwd=e['directory'],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'nv2a-leaf-audit: {e["file"]}: {r.stderr[:400]}')
    return len(ents)


def load_defs(out):
    defs = {}
    for f in os.listdir(out):
        cur, buf = None, []
        for line in open(os.path.join(out, f), errors='replace'):
            m = re.match(r'define [^@]*@"?([\w.$]+)"?\(', line)
            if m:
                cur, buf = m.group(1), [line]
            elif cur:
                buf.append(line)
                if line.startswith('}'):
                    defs.setdefault(cur, ''.join(buf))
                    cur = None
    return defs


def reach(defs, fn, allowed):
    """{offending callee: path} over everything fn calls or references."""
    hits, seen, todo = {}, {fn}, [(fn, [fn])]
    while todo:
        f, path = todo.pop()
        body = defs[f]
        for m in re.finditer(r'call [^@%]*(@"?[\w.$]+"?|%[\w.]+)\(', body):
            t = m.group(1)
            if t.startswith('%'):
                # pgraph_method_inc/_non_inc call the _int handler they're given
                if f not in ('pgraph_method_inc', 'pgraph_method_non_inc'):
                    hits.setdefault('an indirect call', path)
                continue
            name = t[1:].strip('"')
            if FORBID.match(name):
                hits.setdefault(name, path)
        for m in re.finditer(r'@"?([\w.$]+)"?', body):
            name = m.group(1)
            if name in defs and name not in seen and name not in allowed:
                seen.add(name)
                todo.append((name, path + [name]))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build', default=os.path.join(ROOT, 'build'))
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    if not os.path.exists(os.path.join(a.build, 'compile_commands.json')):
        print(f'nv2a-leaf-audit: no compile_commands.json in {a.build}', file=sys.stderr)
        return 2

    src = open(os.path.join(ROOT, 'hw/xbox/nv2a/pgraph/pgraph.c')).read()
    kinds = {}
    for kind, var in (('always', 'pgraph_leaf_always'), ('short_inline', 'pgraph_leaf_short_inline')):
        m = re.search(r'static const MethodFunc %s\[\] = \{(.*?)\};' % var, src, re.S)
        if not m:
            print(f'nv2a-leaf-audit: {var} not found in pgraph.c', file=sys.stderr)
            return 2
        for n in re.findall(r'LEAF\((\w+)\)', m.group(1)):
            kinds[n] = kind
    kinds.update(ARRAY_ELEMENT16='one_draw_array', ARRAY_ELEMENT32='one_draw_array',
                 DRAW_ARRAYS='no_gc')

    with tempfile.TemporaryDirectory(prefix='leafaudit.') as out:
        n = emit_ir(a.build, out)
        defs = load_defs(out)
    handlers = sorted(m.group(1) for m in (re.match(r'pgraph_NV097_(\w+)_handler$', d) for d in defs)
                      if m and not m.group(1).endswith('_int'))
    bad = 0
    for h in sorted(kinds):
        if h not in handlers:
            print(f'{h}: listed, but no such handler')
            bad += 1
            continue
        hits = reach(defs, f'pgraph_NV097_{h}_handler', ALLOWED[kinds[h]])
        for what, path in hits.items():
            print(f'{h} ({kinds[h]}): reaches {what} via {" > ".join(path[1:]) or "itself"}')
            bad += 1
    if a.v:
        for h in handlers:
            if h not in kinds and not reach(defs, f'pgraph_NV097_{h}_handler', set()):
                print(f'{h}: unlisted, passes the rule')
    print(f'nv2a-leaf-audit: {len(kinds)} listed of {len(handlers)} handlers, {n} sources: '
          + ('clean' if not bad else f'{bad} violation(s)'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
