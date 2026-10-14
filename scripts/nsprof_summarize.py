#!/usr/bin/env python3
"""nsprof_summarize.py — parse XEMU_NV2A_NSPROF=1 stderr logs into per-interval
tables, plus an optional two-log A/B comparison.

The nsprof profiler (hw/xbox/nv2a/nsprof.c) prints a summary to stderr every
~5 s of guest flips, e.g.:

    nsprof: 5.0s interval, 193 flips (38.6/s)
    nsprof:   fence_wait   ev=386     total= 443.90ms per_flip= 2300.0us max= 8123.0us
    nsprof:   draws               ev=78088   per_flip=  404.6

Counter lines carry total=/per_flip=/max= (nanosecond accumulators, printed in
ms/us); event lines carry only ev=/per_flip= (plain counts). This tool groups
them by interval, derives fps (= flips / interval seconds), draws/flip,
passes/flip and fence ms/flip, and prints one row per interval plus a mean
summary. By default the FIRST interval is dropped: right after a savestate
load it mixes load-transient work (shader/pipeline gen, texture uploads) into
the numbers and is not comparable.

Usage:
    nsprof_summarize.py LOG                    # per-interval table + summary
    nsprof_summarize.py LOG_A LOG_B            # A/B comparison of means
    nsprof_summarize.py LOG --all              # also dump every counter/event mean
    nsprof_summarize.py LOG --keep-first       # keep the load-transient interval
    nsprof_summarize.py LOG --drop-first 2     # drop the first 2 intervals
    nsprof_summarize.py LOG --json             # machine-readable output

A/B mode compares means over the kept intervals and WARNS when mean
draws/flip differs by more than 15% between logs — that means the two runs
were not on the same scene and the comparison is invalid (anchor scene
identity on draws/flip before comparing anything).

stdlib only; feed it the raw stderr of an xemu run (other stderr lines are
ignored).
"""

import argparse
import json
import math
import re
import sys

HEADER_RE = re.compile(
    r"^nsprof: ([0-9.]+)s interval, (\d+) flips \(([0-9.]+)/s\)")
# Counter line: "nsprof:   name ev=N total=X.XXms per_flip=Y.Yus max=Z.Zus"
COUNTER_RE = re.compile(
    r"^nsprof:\s+(\S+)\s+ev=(\d+)\s+total=\s*([0-9.]+)ms"
    r"\s+per_flip=\s*([0-9.]+)us\s+max=\s*([0-9.]+)us\s*$")
# Event line: "nsprof:   name ev=N per_flip=Y.Y"  (no total=/max=)
EVENT_RE = re.compile(
    r"^nsprof:\s+(\S+)\s+ev=(\d+)\s+per_flip=\s*([0-9.]+)\s*$")

FINISH_PREFIX = "finish_"


def parse_log(path):
    """Return a list of interval dicts:
    {secs, flips, counters: {name: {ev, total_ms, per_flip_us, max_us}},
     events: {name: count}}"""
    intervals = []
    cur = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = HEADER_RE.match(line)
            if m:
                cur = {
                    "secs": float(m.group(1)),
                    "flips": int(m.group(2)),
                    "counters": {},
                    "events": {},
                }
                intervals.append(cur)
                continue
            if cur is None:
                continue
            m = COUNTER_RE.match(line)
            if m:
                cur["counters"][m.group(1)] = {
                    "ev": int(m.group(2)),
                    "total_ms": float(m.group(3)),
                    "per_flip_us": float(m.group(4)),
                    "max_us": float(m.group(5)),
                }
                continue
            m = EVENT_RE.match(line)
            if m:
                cur["events"][m.group(1)] = int(m.group(2))
    return intervals


def derive(iv):
    """Per-interval derived metrics (per-flip normalization)."""
    flips = max(iv["flips"], 1)
    c = iv["counters"]
    e = iv["events"]

    def cms(name):  # counter total ms / flip
        return c[name]["total_ms"] / flips if name in c else 0.0

    def epf(name):  # event count / flip
        return e.get(name, 0) / flips

    finish_total = sum(v for k, v in e.items() if k.startswith(FINISH_PREFIX))
    return {
        "secs": iv["secs"],
        "flips": iv["flips"],
        "fps": iv["flips"] / iv["secs"] if iv["secs"] > 0 else 0.0,
        "draws_pf": epf("draws"),
        "passes_pf": epf("renderpass"),
        "pipebind_pf": epf("pipeline_bind"),
        "fence_ms_pf": cms("fence_wait"),
        "aux_fence_ms_pf": cms("aux_fence"),
        "flip_idle_ms_pf": cms("flip_idle"),
        "shader_gen_ev": c.get("shader_gen", {}).get("ev", 0),
        "pipeline_gen_ev": c.get("pipeline_gen", {}).get("ev", 0),
        "finish_pf": finish_total / flips,
        "counters": c,
        "events": e,
    }


def mean_sd(vals):
    if not vals:
        return 0.0, 0.0
    m = sum(vals) / len(vals)
    if len(vals) < 2:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return m, math.sqrt(var)


def summarize(rows):
    """Mean metrics over derived rows, plus mean per-flip value for every
    counter (ms/flip) and event (count/flip) seen in any kept interval."""
    out = {}
    for key in ("fps", "draws_pf", "passes_pf", "pipebind_pf", "fence_ms_pf",
                "aux_fence_ms_pf", "flip_idle_ms_pf", "finish_pf"):
        out[key] = mean_sd([r[key] for r in rows])
    cnames = sorted({n for r in rows for n in r["counters"]})
    enames = sorted({n for r in rows for n in r["events"]})
    out["counter_ms_pf"] = {
        n: mean_sd([r["counters"].get(n, {}).get("total_ms", 0.0) /
                    max(r["flips"], 1) for r in rows]) for n in cnames}
    out["event_pf"] = {
        n: mean_sd([r["events"].get(n, 0) / max(r["flips"], 1) for r in rows])
        for n in enames}
    out["n_intervals"] = len(rows)
    return out


def print_table(rows, dropped, label):
    print(f"== {label}: {len(rows)} interval(s) kept"
          f" ({dropped} dropped as load-transient) ==")
    hdr = (f"{'iv':>3} {'secs':>5} {'flips':>6} {'fps':>6} {'draws/f':>8} "
           f"{'passes/f':>8} {'fence ms/f':>10} {'idle ms/f':>9} "
           f"{'finish/f':>8} {'shgen':>5} {'pipgen':>6}")
    print(hdr)
    for i, r in enumerate(rows):
        print(f"{i:>3} {r['secs']:>5.1f} {r['flips']:>6} {r['fps']:>6.1f} "
              f"{r['draws_pf']:>8.1f} {r['passes_pf']:>8.1f} "
              f"{r['fence_ms_pf']:>10.2f} {r['flip_idle_ms_pf']:>9.2f} "
              f"{r['finish_pf']:>8.2f} {r['shader_gen_ev']:>5} "
              f"{r['pipeline_gen_ev']:>6}")


def print_summary(s, show_all=False):
    def fmt(key, unit=""):
        m, sd = s[key]
        return f"{m:.2f} ±{sd:.2f}{unit}"
    print(f"  mean fps          : {fmt('fps')}")
    print(f"  mean draws/flip   : {fmt('draws_pf')}")
    print(f"  mean passes/flip  : {fmt('passes_pf')}")
    print(f"  mean fence ms/flip: {fmt('fence_ms_pf')}")
    print(f"  mean idle ms/flip : {fmt('flip_idle_ms_pf')}")
    print(f"  mean finishes/flip: {fmt('finish_pf')}")
    if show_all:
        print("  -- all counters (mean ms/flip) --")
        for n, (m, sd) in sorted(s["counter_ms_pf"].items()):
            print(f"    {n:<16} {m:>9.3f} ±{sd:.3f}")
        print("  -- all events (mean count/flip) --")
        for n, (m, sd) in sorted(s["event_pf"].items()):
            print(f"    {n:<20} {m:>9.2f} ±{sd:.2f}")


def print_ab(sa, sb, name_a, name_b):
    print(f"\n== A/B comparison: A={name_a}  B={name_b} ==")
    da, db = sa["draws_pf"][0], sb["draws_pf"][0]
    if da > 0 and db > 0:
        ratio = abs(da - db) / max(da, db)
        if ratio > 0.15:
            print(f"  *** WARNING: draws/flip differ by {ratio*100:.0f}% "
                  f"(A={da:.1f} B={db:.1f}). Different scenes — this A/B is "
                  f"INVALID. Re-run from the same savestate. ***")
    rows = [("fps", "fps", False),
            ("draws/flip", "draws_pf", False),
            ("passes/flip", "passes_pf", True),
            ("fence ms/flip", "fence_ms_pf", True),
            ("idle ms/flip", "flip_idle_ms_pf", True),
            ("finishes/flip", "finish_pf", True)]
    print(f"  {'metric':<14} {'A':>10} {'B':>10} {'delta':>10} {'%':>8}")
    for label, key, lower_better in rows:
        a, b = sa[key][0], sb[key][0]
        d = b - a
        pct = (d / a * 100.0) if a else float("inf")
        print(f"  {label:<14} {a:>10.2f} {b:>10.2f} {d:>+10.2f} {pct:>+7.1f}%")
    # union of counters, as ms/flip
    names = sorted(set(sa["counter_ms_pf"]) | set(sb["counter_ms_pf"]))
    print(f"  -- counters, mean ms/flip --")
    for n in names:
        a = sa["counter_ms_pf"].get(n, (0.0, 0.0))[0]
        b = sb["counter_ms_pf"].get(n, (0.0, 0.0))[0]
        print(f"  {n:<14} {a:>10.3f} {b:>10.3f} {b-a:>+10.3f}")


def main():
    ap = argparse.ArgumentParser(
        description="Summarize XEMU_NV2A_NSPROF=1 stderr logs "
                    "(one log: per-interval table; two logs: A/B).")
    ap.add_argument("logs", nargs="+", help="1 or 2 nsprof stderr log files")
    ap.add_argument("--drop-first", type=int, default=1, metavar="N",
                    help="drop first N intervals as load-transient "
                         "(default 1; ignored when too few intervals)")
    ap.add_argument("--keep-first", action="store_true",
                    help="shorthand for --drop-first 0")
    ap.add_argument("--all", action="store_true",
                    help="print every counter/event mean, not just headline")
    ap.add_argument("--json", action="store_true",
                    help="emit JSON instead of tables")
    args = ap.parse_args()
    if len(args.logs) > 2:
        ap.error("pass one log (summary) or two logs (A/B)")
    drop = 0 if args.keep_first else args.drop_first

    results = []
    for path in args.logs:
        ivs = parse_log(path)
        if not ivs:
            sys.exit(f"error: no 'nsprof:' interval headers found in {path} "
                     f"(was the run launched with XEMU_NV2A_NSPROF=1 and "
                     f"stderr captured?)")
        rows = [derive(iv) for iv in ivs]
        d = min(drop, max(len(rows) - 1, 0))  # always keep >= 1 interval
        kept = rows[d:]
        results.append((path, kept, d))

    if args.json:
        out = []
        for path, kept, d in results:
            s = summarize(kept)
            out.append({
                "log": path, "dropped_intervals": d,
                "intervals": [{k: v for k, v in r.items()
                               if k not in ("counters", "events")}
                              for r in kept],
                "mean": {k: (v if not isinstance(v, tuple) else
                             {"mean": v[0], "sd": v[1]})
                         for k, v in s.items()
                         if k not in ("counter_ms_pf", "event_pf")},
                "counter_ms_per_flip": {n: {"mean": m, "sd": sd}
                                        for n, (m, sd)
                                        in s["counter_ms_pf"].items()},
                "event_per_flip": {n: {"mean": m, "sd": sd}
                                   for n, (m, sd) in s["event_pf"].items()},
            })
        json.dump(out, sys.stdout, indent=2)
        print()
        return

    summaries = []
    for path, kept, d in results:
        print_table(kept, d, path)
        s = summarize(kept)
        summaries.append(s)
        print_summary(s, show_all=args.all)
        print()
    if len(summaries) == 2:
        print_ab(summaries[0], summaries[1],
                 results[0][0], results[1][0])


if __name__ == "__main__":
    main()
