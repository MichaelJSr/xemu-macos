#!/usr/bin/env python3
"""score_frames.py — magenta-artifact scorer for xemu window captures.

Requires Pillow (pip3 install pillow). Not part of the Python standard
library; every other script in this skill's scripts/ directory is
stdlib-only, but this one needs image decoding.

Detects the "pink-tile" corruption pattern (see the pink-tile saga: giant
magenta blocks under a MoltenVK prefill misconfiguration; smaller magenta
squares from unswizzle/texture-path bugs) in captured PNGs:

    magenta pixel := R>170 and B>140 and G < 0.55*min(R,B)

Rather than testing every full-resolution pixel, the frame is first
downscaled with a box filter to a coarse grid (default cell size 8px, i.e.
one grid cell ~= one 8x8 source block), the magenta test runs on that small
grid, and hot cells are clustered (8-connected flood fill). This is cheap
(a few thousand cells even for a 1080p capture) and, because real artifacts
are far larger than one grid cell, distinguishes two morphologies that map
to different subsystems (per this fork's debugging history):

  - "morton-square": small (<=64px per side) blocky cluster(s), often
    several scattered across the frame -> texture / unswizzle path.
  - "screen-rect": a cluster spanning >=12% of frame width or height ->
    compositor / present / driver (this was the MVK_CONFIG_PREFILL_METAL_
    COMMAND_BUFFERS=2 bug: whole-frame magenta blocks from immediate
    command-buffer encoding racing streamed texture uploads).
  - "irregular": doesn't fit either bucket cleanly; eyeball it.

Usage:
    score_frames.py capture1.png [capture2.png ...]
    score_frames.py captures/*.png --cell-size 16
    score_frames.py captures/*.png --json

Exit code: 0 if no file is flagged, 1 if any file is flagged (so it can
gate a test script), 2 on a usage/decode error.
"""

import argparse
import json
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit(
        "error: score_frames.py requires Pillow, which is not installed.\n"
        "       Install with: pip3 install pillow\n"
        "       (every other script in this skill's scripts/ dir is "
        "stdlib-only; this is the one exception.)")


def is_magenta(r, g, b):
    return r > 170 and b > 140 and g < 0.55 * min(r, b)


def coarse_grid(im, cell_size):
    """Return (cols, rows, hot[row][col] bool grid, px_w, px_h) where
    px_w/px_h are the actual pixel size of one cell (image dims need not
    be an exact multiple of cell_size)."""
    w, h = im.size
    cols = max(1, round(w / cell_size))
    rows = max(1, round(h / cell_size))
    thumb = im.convert("RGB").resize((cols, rows), Image.BOX)
    data = thumb.load()
    hot = [[is_magenta(*data[x, y]) for x in range(cols)] for y in range(rows)]
    return cols, rows, hot, w / cols, h / rows


def cluster(hot, cols, rows):
    """8-connected flood fill over the boolean grid. Returns a list of
    clusters, each a list of (col, row) cell coordinates."""
    seen = [[False] * cols for _ in range(rows)]
    clusters = []
    for sy in range(rows):
        for sx in range(cols):
            if not hot[sy][sx] or seen[sy][sx]:
                continue
            stack = [(sx, sy)]
            seen[sy][sx] = True
            cells = []
            while stack:
                cx, cy = stack.pop()
                cells.append((cx, cy))
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        nx, ny = cx + dx, cy + dy
                        if (0 <= nx < cols and 0 <= ny < rows and
                                hot[ny][nx] and not seen[ny][nx]):
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            clusters.append(cells)
    return clusters


def classify(cells, px_w, px_h, img_w, img_h):
    xs = [c[0] for c in cells]
    ys = [c[1] for c in cells]
    min_cx, max_cx = min(xs), max(xs)
    min_cy, max_cy = min(ys), max(ys)
    bbox_w_cells = max_cx - min_cx + 1
    bbox_h_cells = max_cy - min_cy + 1
    bbox_w_px = bbox_w_cells * px_w
    bbox_h_px = bbox_h_cells * px_h
    fill_ratio = len(cells) / (bbox_w_cells * bbox_h_cells)
    frac_w = bbox_w_px / img_w
    frac_h = bbox_h_px / img_h

    if frac_w >= 0.12 or frac_h >= 0.12:
        morph = "screen-rect"
    elif bbox_w_px <= 64 and bbox_h_px <= 64:
        morph = "morton-square"
    else:
        morph = "irregular"

    return {
        "cells": len(cells),
        "bbox_px": [round(min_cx * px_w), round(min_cy * px_h),
                    round((max_cx + 1) * px_w), round((max_cy + 1) * px_h)],
        "bbox_w_px": round(bbox_w_px),
        "bbox_h_px": round(bbox_h_px),
        "frac_w": round(frac_w, 3),
        "frac_h": round(frac_h, 3),
        "fill_ratio": round(fill_ratio, 2),
        "morphology": morph,
    }


def score_file(path, cell_size):
    try:
        im = Image.open(path)
        im.load()
    except Exception as e:
        return {"file": path, "error": str(e)}
    w, h = im.size
    cols, rows, hot, px_w, px_h = coarse_grid(im, cell_size)
    raw_clusters = cluster(hot, cols, rows)
    clusters = [classify(c, px_w, px_h, w, h) for c in raw_clusters]
    clusters.sort(key=lambda c: c["cells"], reverse=True)
    return {
        "file": path, "width": w, "height": h,
        "grid": [cols, rows], "cell_px": [round(px_w, 1), round(px_h, 1)],
        "flagged": len(clusters) > 0,
        "clusters": clusters,
    }


def print_result(r):
    if "error" in r:
        print(f"{r['file']}: ERROR: {r['error']}")
        return
    if not r["flagged"]:
        print(f"{r['file']}: CLEAN  ({r['width']}x{r['height']}, "
              f"grid {r['grid'][0]}x{r['grid'][1]} @ {r['cell_px'][0]}px)")
        return
    counts = {}
    for c in r["clusters"]:
        counts[c["morphology"]] = counts.get(c["morphology"], 0) + 1
    tally = ", ".join(f"{v} {k}" for k, v in counts.items())
    print(f"{r['file']}: FLAGGED  {len(r['clusters'])} cluster(s)  ({tally})")
    for i, c in enumerate(r["clusters"]):
        print(f"    [{i}] {c['morphology']:<13} bbox_px={c['bbox_px']} "
              f"({c['bbox_w_px']}x{c['bbox_h_px']}, "
              f"{c['frac_w']*100:.0f}%W x {c['frac_h']*100:.0f}%H) "
              f"cells={c['cells']} fill={c['fill_ratio']}")


def main():
    ap = argparse.ArgumentParser(
        description="Score captured PNGs for magenta ('pink-tile') "
                    "artifacts using a coarse-grid clustering heuristic.")
    ap.add_argument("files", nargs="+", help="image file(s) to score")
    ap.add_argument("--cell-size", type=int, default=8,
                    help="coarse-grid cell size in source pixels "
                         "(default: 8)")
    ap.add_argument("--json", action="store_true", help="JSON output")
    args = ap.parse_args()

    results = [score_file(f, args.cell_size) for f in args.files]

    if args.json:
        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        for r in results:
            print_result(r)
        n_flagged = sum(1 for r in results if r.get("flagged"))
        n_err = sum(1 for r in results if "error" in r)
        print(f"\n{len(results)} file(s): {n_flagged} flagged, "
              f"{len(results) - n_flagged - n_err} clean, {n_err} error(s)")

    if any("error" in r for r in results):
        sys.exit(2)
    sys.exit(1 if any(r.get("flagged") for r in results) else 0)


if __name__ == "__main__":
    main()
