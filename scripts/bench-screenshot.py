#!/usr/bin/env python3
"""Mid-run visual artifact check for the savestate bench harness.

Usage: bench-screenshot.py <xemu-pid> <out.png>

Finds the xemu window belonging to <pid> (Quartz window list — works
unfocused and across Spaces), captures it with `screencapture -l`, and
scores the frame for the two artifact classes the fork's history says
matter (plus a stuck-frame catch-all):

  WHITE_SCREEN   near-uniform bright frame (>=90% of pixels near-white)
  MAGENTA_TILES  screen-aligned magenta blocks / Morton-tile corruption
                 (the fork's texture/present corruption signature)
  UNIFORM_FRAME  flat frame of any color (luma stddev ~0) — a stuck or
                 black presentation mid-scene

Prints exactly one line to stdout:
  screenshot: PASS <stats> <path>
  screenshot: ARTIFACT(<classes>) <stats> <path>
  screenshot: SKIP (<reason>)

Exit code: 0 on PASS/SKIP, 3 on ARTIFACT (callers may treat 3 as soft).
A SKIP is never an error: benches must not die because a window wasn't
capturable (e.g. headless CI). Known benign class to keep in mind when
reviewing a flagged PNG: the Azurik death flash is a single-frame
full-screen magenta wash with the HUD intact (validation skill §1.3).
"""
import subprocess
import sys


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
    from PIL import Image
    im = Image.open(path).convert('RGB')
    w, h = im.size
    if w > 320:
        im = im.resize((320, max(1, h * 320 // w)))
        w, h = im.size
    px = list(im.getdata())
    n = len(px)

    lumas = [(r * 299 + g * 587 + b * 114) // 1000 for r, g, b in px]
    mean = sum(lumas) / n
    var = sum((l - mean) ** 2 for l in lumas) / n
    std = var ** 0.5

    white = sum(1 for r, g, b in px if min(r, g, b) > 235) / n

    # Magenta test + coarse-grid clustering (screen-aligned blocks).
    gx, gy = 16, 12
    cells = [0] * (gx * gy)
    counts = [0] * (gx * gy)
    magenta = 0
    for i, (r, g, b) in enumerate(px):
        cx = (i % w) * gx // w
        cy = (i // w) * gy // h
        counts[cy * gx + cx] += 1
        if r > 170 and b > 140 and g < 0.55 * min(r, b):
            magenta += 1
            cells[cy * gx + cx] += 1
    mag_frac = magenta / n
    hot_cells = sum(1 for c, t in zip(cells, counts) if t and c / t > 0.35)

    classes = []
    if white >= 0.90:
        classes.append('WHITE_SCREEN')
    if std < 4.0 and white < 0.90:
        classes.append('UNIFORM_FRAME')
    if hot_cells >= 1 or mag_frac > 0.02:
        classes.append('MAGENTA_TILES')
    stats = ('luma_mean=%.0f luma_std=%.1f white=%.2f magenta=%.4f '
             'hot_cells=%d' % (mean, std, white, mag_frac, hot_cells))
    return classes, stats


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
