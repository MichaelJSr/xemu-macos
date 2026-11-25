#!/usr/bin/env python3
"""Mid-run visual artifact check for the savestate bench harness.

Usage: bench-screenshot.py <xemu-pid> <out.png>

Finds the xemu window belonging to <pid> (Quartz window list — works
unfocused and across Spaces), captures it with `screencapture -l`, and
scores a 320-wide `sips` thumbnail of it with scripts/score_shot.py
(WHITE_SCREEN, MAGENTA_TILES, UNIFORM_FRAME; the same scorer and
thresholds as every other harness). The thumbnail keeps the pure-Python
decode to ~0.1 s, since callers score in the middle of a timed dwell.

Prints exactly one line to stdout:
  screenshot: PASS <stats> <path>
  screenshot: ARTIFACT(<classes>) <stats> <path>
  screenshot: SKIP (<reason>)

Exit code: 0 on PASS/SKIP, 3 on ARTIFACT (callers may treat 3 as soft).
A SKIP is never an error: benches must not die because a window wasn't
capturable (e.g. headless CI). Known benign class to keep in mind when
reviewing a flagged PNG: the Azurik death flash is a single-frame
full-screen magenta wash with the HUD intact.
"""
import importlib.util
import os
import subprocess
import sys

# By path, so `python3 -I` (no script dir on sys.path) still finds it.
_spec = importlib.util.spec_from_file_location(
    'score_shot', os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               'score_shot.py'))
score_shot = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(score_shot)


def find_window(pid):
    import Quartz
    wl = Quartz.CGWindowListCopyWindowInfo(
        Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID)
    best = None
    for w in wl or []:
        if w.get('kCGWindowOwnerPID') != pid:
            continue
        if w.get('kCGWindowLayer', 1) != 0:
            continue
        b = w.get('kCGWindowBounds', {})
        area = b.get('Width', 0) * b.get('Height', 0)
        if area < 64 * 64:
            continue
        if best is None or area > best[1]:
            best = (w['kCGWindowNumber'], area)
    return best[0] if best else None


def score(path):
    """(classes, stats) for a PNG, scored on a sips thumbnail when sips
    can make one (a full Retina capture decodes in seconds)."""
    thumb = os.path.splitext(path)[0] + '.small.png'
    try:
        made = subprocess.run(
            ['sips', '--resampleWidth', str(score_shot.TARGET_W), path,
             '--out', thumb], capture_output=True).returncode == 0
    except OSError:
        made = False
    try:
        return score_shot.score_file(thumb if made else path)
    finally:
        if made:
            os.unlink(thumb)


def main():
    if len(sys.argv) != 3:
        print('screenshot: SKIP (usage: bench-screenshot.py <pid> <out.png>)')
        return 0
    pid, out = int(sys.argv[1]), sys.argv[2]
    try:
        wid = find_window(pid)
    except Exception as e:
        print('screenshot: SKIP (window list failed: %s)' % e)
        return 0
    if wid is None:
        print('screenshot: SKIP (no window for pid %d)' % pid)
        return 0
    r = subprocess.run(['screencapture', '-x', '-o', '-l', str(wid), out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('screenshot: SKIP (capture failed: %s)' %
              (r.stderr.strip() or r.returncode))
        return 0
    try:
        classes, stats = score(out)
    except Exception as e:
        print('screenshot: SKIP (scoring failed: %s)' % e)
        return 0
    if classes:
        print('screenshot: ARTIFACT(%s) %s %s' %
              ('+'.join(classes), stats, out))
        return 3
    print('screenshot: PASS %s %s' % (stats, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
