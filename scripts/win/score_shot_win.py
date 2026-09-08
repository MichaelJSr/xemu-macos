#!/usr/bin/env python3
"""score_shot_win.py - visual artifact oracle for the Windows bench harness.

Windows port of the scoring half of scripts/bench-screenshot.py (the
capture half lives in scripts/win/capture_screen.ps1, because Quartz and
`screencapture -l` do not exist here). Same three artifact classes, same
thresholds, same one-line verdict format, so a Windows verdict line means
exactly what a macOS one means:

  WHITE_SCREEN   >=90% of pixels near-white
  MAGENTA_TILES  screen-aligned magenta blocks (the fork's texture /
                 present corruption signature - see the pink-tile saga)
  UNIFORM_FRAME  luma stddev ~0: stuck or black presentation mid-scene

Usage:
  score_shot_win.py <shot.png> [--thumb <shot.small.png>] [--label L]
  score_shot_win.py --selftest

Prints exactly one line:
  screenshot: PASS <stats> <path>
  screenshot: ARTIFACT(<classes>) <stats> <path>
  screenshot: SKIP (<reason>)
Exit code 0 on PASS/SKIP, 3 on ARTIFACT. A SKIP is never an error: a
bench must not die because a frame was not capturable.

stdlib only, by design. The macOS scorer needs Pillow; this one carries a
minimal PNG reader (8-bit, non-interlaced - what System.Drawing writes)
so the artifact gate cannot silently degrade to "skipped, no Pillow" on a
fresh machine, which is exactly how the ARTIFACT exit-3 net went missing
on Windows in the first place. Decode the small thumbnail written by
capture_screen.ps1 rather than the full 1080p PNG: pure-Python unfiltering
of 2M pixels would cost seconds of CPU, and the harness scores AFTER the
timed dwell precisely so scoring can never perturb a measurement.

Known benign class when reviewing a flagged PNG: the Azurik death flash is
a single-frame full-screen magenta wash with the HUD intact.
"""

import argparse
import struct
import sys
import zlib


# ---------------------------------------------------------------------------
# minimal PNG reader (8-bit, non-interlaced)
# ---------------------------------------------------------------------------

class PngError(Exception):
    pass


def read_png(path):
    """Return (width, height, pixels) where pixels is a flat bytearray of
    RGB triples."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise PngError("not a PNG")

    pos = 8
    idat = []
    plte = None
    ihdr = None
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + body + crc
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body[:13])
        elif ctype == b"PLTE":
            plte = body
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    if ihdr is None:
        raise PngError("no IHDR")
    w, h, depth, color, comp, filt, interlace = ihdr
    if depth != 8:
        raise PngError("unsupported bit depth %d" % depth)
    if interlace != 0:
        raise PngError("interlaced PNG unsupported")
    if comp != 0 or filt != 0:
        raise PngError("unsupported compression/filter method")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise PngError("unsupported color type %d" % color)
    if color == 3 and plte is None:
        raise PngError("palette image without PLTE")

    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    if len(raw) < h * (stride + 1):
        raise PngError("short IDAT stream")

    # Per-scanline defiltering (PNG spec 9.2); each byte references the
    # same byte of the pixel to its left (a), the row above (b) and the
    # up-left pixel (c).
    out = bytearray(h * stride)
    prev = bytearray(stride)
    src = 0
    for y in range(h):
        ft = raw[src]
        src += 1
        line = bytearray(raw[src:src + stride])
        src += stride
        bpp = channels
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise PngError("bad filter type %d on row %d" % (ft, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line

    rgb = bytearray(w * h * 3)
    if color == 2:
        rgb[:] = out
    elif color == 6:
        rgb[0::3] = out[0::4]
        rgb[1::3] = out[1::4]
        rgb[2::3] = out[2::4]
    elif color == 0:
        rgb[0::3] = out
        rgb[1::3] = out
        rgb[2::3] = out
    elif color == 4:
        g = out[0::2]
        rgb[0::3] = g
        rgb[1::3] = g
        rgb[2::3] = g
    else:  # palette
        for i, idx in enumerate(out):
            rgb[3 * i:3 * i + 3] = plte[3 * idx:3 * idx + 3]
    return w, h, rgb


# ---------------------------------------------------------------------------
# scoring (thresholds mirror scripts/bench-screenshot.py score())
# ---------------------------------------------------------------------------

TARGET_W = 320
GRID_X, GRID_Y = 16, 12
HOT_CELL_FRAC = 0.35
WHITE_FRAC = 0.90
UNIFORM_STD = 4.0
MAGENTA_FRAC = 0.02


def downsample(w, h, rgb, target_w=TARGET_W):
    """Box-average down to <= target_w wide (matches the BOX filter the
    macOS scorer's Pillow resize uses; averaging, not sampling, is what
    makes a small artifact block survive the shrink)."""
    if w <= target_w:
        return w, h, rgb
    nw = target_w
    nh = max(1, h * target_w // w)
    out = bytearray(nw * nh * 3)
    for ny in range(nh):
        y0 = ny * h // nh
        y1 = max(y0 + 1, (ny + 1) * h // nh)
        for nx in range(nw):
            x0 = nx * w // nw
            x1 = max(x0 + 1, (nx + 1) * w // nw)
            r = g = b = 0
            n = 0
            for y in range(y0, y1):
                base = (y * w + x0) * 3
                for x in range(x0, x1):
                    r += rgb[base]
                    g += rgb[base + 1]
                    b += rgb[base + 2]
                    base += 3
                    n += 1
            i = (ny * nw + nx) * 3
            out[i] = r // n
            out[i + 1] = g // n
            out[i + 2] = b // n
    return nw, nh, out


def score(w, h, rgb):
    n = w * h
    lum_sum = 0
    lum_sq = 0
    white = 0
    magenta = 0
    cells = [0] * (GRID_X * GRID_Y)
    counts = [0] * (GRID_X * GRID_Y)
    for i in range(n):
        r = rgb[3 * i]
        g = rgb[3 * i + 1]
        b = rgb[3 * i + 2]
        l = (r * 299 + g * 587 + b * 114) // 1000
        lum_sum += l
        lum_sq += l * l
        if r > 235 and g > 235 and b > 235:
            white += 1
        cx = (i % w) * GRID_X // w
        cy = (i // w) * GRID_Y // h
        counts[cy * GRID_X + cx] += 1
        if r > 170 and b > 140 and g < 0.55 * min(r, b):
            magenta += 1
            cells[cy * GRID_X + cx] += 1
    mean = lum_sum / n
    std = max(0.0, lum_sq / n - mean * mean) ** 0.5
    white_frac = white / n
    mag_frac = magenta / n
    hot_cells = sum(1 for c, t in zip(cells, counts) if t and c / t > HOT_CELL_FRAC)

    classes = []
    if white_frac >= WHITE_FRAC:
        classes.append("WHITE_SCREEN")
    if std < UNIFORM_STD and white_frac < WHITE_FRAC:
        classes.append("UNIFORM_FRAME")
    if hot_cells >= 1 or mag_frac > MAGENTA_FRAC:
        classes.append("MAGENTA_TILES")
    stats = ("luma_mean=%.0f luma_std=%.1f white=%.2f magenta=%.4f "
             "hot_cells=%d" % (mean, std, white_frac, mag_frac, hot_cells))
    return classes, stats


def score_file(path):
    w, h, rgb = read_png(path)
    w, h, rgb = downsample(w, h, rgb)
    return score(w, h, rgb)


# ---------------------------------------------------------------------------
# selftest: the net must fire before it is trusted
# ---------------------------------------------------------------------------

def write_png(path, w, h, pixel_fn):
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type None
        for x in range(w):
            raw += bytes(pixel_fn(x, y))
    idat = zlib.compress(bytes(raw), 6)

    def chunk(ctype, body):
        return (struct.pack(">I", len(body)) + ctype + body +
                struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def selftest(outdir):
    import os
    import random
    os.makedirs(outdir, exist_ok=True)
    rnd = random.Random(1234)
    cases = []

    p = os.path.join(outdir, "sel_white.png")
    write_png(p, 160, 120, lambda x, y: (250, 250, 250))
    cases.append((p, "WHITE_SCREEN"))

    p = os.path.join(outdir, "sel_black.png")
    write_png(p, 160, 120, lambda x, y: (0, 0, 0))
    cases.append((p, "UNIFORM_FRAME"))

    def magenta_tiles(x, y):
        if 40 <= x < 120 and 30 <= y < 90:
            return (255, 0, 255)
        return (60 + (x % 40), 90 + (y % 30), 70)
    p = os.path.join(outdir, "sel_magenta.png")
    write_png(p, 160, 120, magenta_tiles)
    cases.append((p, "MAGENTA_TILES"))

    def scene(x, y):
        # A plausible frame: structured, high-contrast, green-rich. Channels
        # stay correlated (r,b <= 200 with g >= 120), so no pixel can satisfy
        # the magenta test - a per-channel uniform-random fixture would, and
        # would make this net look like it false-positives on any noise.
        return (40 + (x * 3 + rnd.randrange(0, 20)) % 160,
                120 + (y * 5) % 100,
                50 + (x + y * 2) % 150)
    p = os.path.join(outdir, "sel_scene.png")
    write_png(p, 160, 120, scene)
    cases.append((p, None))

    failures = []
    for path, expect in cases:
        classes, stats = score_file(path)
        ok = (expect in classes) if expect else (not classes)
        print("  %s  %-14s -> %s  [%s]" %
              ("ok   " if ok else "FAIL ", expect or "clean",
               "+".join(classes) or "none", stats))
        if not ok:
            failures.append(path)
    # A 1024-wide magenta frame must survive the downsample path too.
    p = os.path.join(outdir, "sel_magenta_big.png")
    write_png(p, 1024, 256, lambda x, y: (255, 0, 255) if
              (200 <= x < 500 and 40 <= y < 200) else (30, 120, 60))
    classes, stats = score_file(p)
    ok = "MAGENTA_TILES" in classes
    print("  %s  %-14s -> %s  [%s]" %
          ("ok   " if ok else "FAIL ", "downsampled", "+".join(classes) or "none",
           stats))
    if not ok:
        failures.append(p)
    return 0 if not failures else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?")
    ap.add_argument("--thumb", default=None,
                    help="decode this downscaled copy instead (cheaper)")
    ap.add_argument("--report-path", default=None,
                    help="path to print in the verdict (default: image)")
    ap.add_argument("--selftest", metavar="OUTDIR", default=None)
    args = ap.parse_args()

    if args.selftest:
        print("== score_shot_win selftest: the artifact net must FIRE ==")
        return selftest(args.selftest)

    if not args.image:
        print("screenshot: SKIP (usage: score_shot_win.py <shot.png>)")
        return 0
    src = args.thumb or args.image
    shown = args.report_path or args.image
    try:
        classes, stats = score_file(src)
    except Exception as e:  # decode failure must never kill a bench
        print("screenshot: SKIP (scoring failed: %s)" % e)
        return 0
    if classes:
        print("screenshot: ARTIFACT(%s) %s %s" %
              ("+".join(classes), stats, shown))
        return 3
    print("screenshot: PASS %s %s" % (stats, shown))
    return 0


if __name__ == "__main__":
    sys.exit(main())
