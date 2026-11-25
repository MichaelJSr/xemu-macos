#!/usr/bin/env python3
"""bench-receipt.py — gate + receipt engine for scripts/bench-abx.sh and
the Windows bench (scripts/bench-savestate-ab-win.py).

Encodes the validity disciplines the interleaved savestate A/B method
demands (see the xemu-operate and xemu-history skills) so they are ENFORCED by
the harness instead of left to operator discipline:

  gate      per-run scene-identity gate: parse one nsprof log (via
            nsprof_summarize.py --json), assert every kept interval's
            draws/flip lies inside the required band (menu/out-of-band
            scene: menu-vs-gameplay), and flag within-run bimodality via
            the draws/flip coefficient of variation (bimodal attract
            reel: regime straddling). Also records, over the kept
            intervals, the headroom numbers a capped scene is judged
            by: flip_idle ms/flip (the wait for vblank), the worst
            frame (nsprof flip_gap max), process CPU (the per-tick
            samples in --cpu, default the log's ab_<label>.cpu) and,
            when the run had XEMU_APU_PROF=1, the APU thread's
            `apu: utilization`. Writes a per-run JSON.

  receipt   aggregate per-run JSONs into the full protocol receipt:
            per-arm mean +/- sd and headroom, per-pair deltas and sign
            tally, cross-arm draws/flip identity check (>15% = INVALID,
            >5% = composition warning + draws/s throughput, the 1.18
            correction), the Vulkan driver of every run (an arm whose
            runs name more than one `- Driver:` is INVALID: with
            driver = 'kosmickrisp' a KosmicKrisp strike marker in the
            shared data folder switches later runs to MoltenVK), binary
            identity, and a machine-readable JSON receipt. CAPPED when
            every run sits within 0.5 fps of the frame cap (--cap-fps,
            else the meta's config.cap_fps, else 60), or every run
            within 0.5 fps of half of it (a 30 fps lock), with a small
            sd: fps can't separate the arms, more pairs won't either,
            so judge flip_idle, process CPU and the worst frames
            instead. An arm whose APU thread peaks at 90% or more gets
            a WARNING (it is close to falling behind real time) but no
            verdict change. Exit code carries the verdict (0: valid or
            capped).

  selftest  --dry-run backend: builds synthetic nsprof fixtures (a
            clean 3-pair A/B, a bimodal run, an out-of-band menu run,
            a capped A/B), proves the gate PASSES the clean runs and
            FAILS the bad ones (a net must fire before it is trusted),
            then builds receipts from them and checks verdict, sign
            tally and headroom. Touches nothing outside --outdir.

stdlib only. Python >= 3.8.
"""

import argparse
import json
import math
import os
import platform
import re
import subprocess
import sys
import time

GATE_EXIT_INVALID = 3
RECEIPT_EXIT_INVALID = 4
SELFTEST_EXIT_FAIL = 5

DEFAULT_SUMMARIZER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "nsprof_summarize.py")
CAP_FPS = 60.0         # the guest's vblank cap (config.cap_fps, --cap-fps)
CAP_FPS_TOL = 0.5      # a run within this of the cap sits at it ...
CAP_RUN_SD_MAX = 1.0   # ... when its intervals' fps sd is this small
APU_UTIL_WARN = 90.0   # an APU thread peaking here nears real-time loss

# The interval header and the flip_gap counter as hw/xbox/nv2a/nsprof.c
# prints them (flip_gap max: the interval's longest frame), and
# hw/xbox/mcpx/apu/apu.c's XEMU_APU_PROF=1 line.
NSPROF_HEADER_RE = re.compile(
    r"^nsprof: ([0-9.]+)s interval, (\d+) flips \(([0-9.]+)/s\)")
FLIP_GAP_RE = re.compile(
    r"^nsprof:\s+flip_gap\s+ev=\d+\s+total=\s*[0-9.]+ms"
    r"\s+per_flip=\s*[0-9.]+us\s+max=\s*([0-9.]+)us")
APU_RE = re.compile(r"^apu: utilization\s+([0-9.]+)%")


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def run_summarizer(summarizer, log_path):
    """Run nsprof_summarize.py --json on one log; return its per-log dict."""
    # --drop-first 0 is load-bearing: the summarizer's default is 1, but the
    # gate does its own scene-anchored transient dropping.
    cmd = [sys.executable, summarizer, log_path, "--json",
           "--drop-first", "0"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"summarizer failed on {log_path}: {proc.stderr.strip() or proc.stdout.strip()}")
    data = json.loads(proc.stdout)
    if not isinstance(data, list) or not data:
        raise RuntimeError(f"summarizer returned no results for {log_path}")
    return data[0]


def log_vk_driver(log_path):
    """The run's `- Driver: <name>, <version> (id N)`, or '' (none logged)."""
    try:
        text = open(log_path, errors="replace").read()
    except OSError:
        return ""
    m = re.search(r"(?m)^- Driver: (.*?) \(id \d+\)$", text)
    return m.group(1) if m else ""


def log_extras(log_path):
    """(flip_gap max in ms per nsprof interval, None where the interval
    printed none; [(interval index, APU utilization %)]). An `apu:` line
    printed before interval k's summary ran during interval k."""
    gaps, apu = [], []
    try:
        f = open(log_path, errors="replace")
    except OSError:
        return gaps, apu
    with f:
        for line in f:
            if NSPROF_HEADER_RE.match(line):
                gaps.append(None)
                continue
            m = FLIP_GAP_RE.match(line)
            if m and gaps:
                gaps[-1] = float(m.group(1)) / 1000.0
                continue
            m = APU_RE.match(line)
            if m:
                apu.append((len(gaps), float(m.group(1))))
    return gaps, apu


def read_cpu(path):
    """Mean of a run's per-tick process CPU % samples (one number a line;
    bench-abx and the Windows bench write them), the first dropped as
    the post-load transient when there are more; (None, 0) without."""
    vals = []
    try:
        with open(path, errors="replace") as f:
            for line in f:
                try:
                    vals.append(float(line.strip()))
                except ValueError:
                    pass
    except (OSError, TypeError):
        return None, 0
    if len(vals) > 1:
        vals = vals[1:]
    return (sum(vals) / len(vals), len(vals)) if vals else (None, 0)


def mean_sd(vals):
    if not vals:
        return 0.0, 0.0
    m = sum(vals) / len(vals)
    if len(vals) < 2:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return m, math.sqrt(var)


def mean_or_none(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else None


# ---------------------------------------------------------------------------
# gate
# ---------------------------------------------------------------------------

def cmd_gate(args):
    band_lo, band_hi = args.band
    summary = run_summarizer(args.summarizer, args.log)

    all_iv = summary["intervals"]

    # Anchor at the scene: warm launches print boot-menu nsprof intervals
    # before loadvm (a cold first launch boots flip-silent, so its log
    # starts in-game — the asymmetry that motivated this). Keep only the
    # TRAILING contiguous run of in-band intervals, then drop that run's
    # first interval as the post-load transient. A run that never enters
    # (or falls out of) the scene keeps nothing and gates INVALID —
    # the menu/out-of-band and bimodal attract-reel gates still fire.
    def in_band(iv):
        return band_lo <= iv["draws_pf"] <= band_hi
    start = len(all_iv)
    while start > 0 and in_band(all_iv[start - 1]):
        start -= 1
    boot_excluded = start
    intervals = all_iv[start:]
    transient_dropped = 0
    if intervals:
        intervals = intervals[1:]
        transient_dropped = 1

    draws = [iv["draws_pf"] for iv in intervals]
    fpss = [iv["fps"] for iv in intervals]

    d_mean, d_sd = mean_sd(draws)
    cv = (d_sd / d_mean) if d_mean > 0 else 0.0
    bimodal = cv > args.cv_max

    band_ok = len(intervals) >= 2
    out_of_band = [] if band_ok else [
        i for i, iv in enumerate(all_iv) if not in_band(iv)]
    valid = band_ok and not bimodal

    fps_mean, fps_sd = mean_sd(fpss)
    # Headroom over the same kept intervals (index start + transient on).
    kept_from = boot_excluded + transient_dropped
    gaps, apu = log_extras(args.log)
    if len(gaps) == len(all_iv) and intervals:
        kept_gaps = [g for g in gaps[kept_from:] if g is not None]
    else:   # the two parses disagree: report no frame gaps
        kept_gaps = []
    apu_kept = [u for k, u in apu if intervals and k >= kept_from]
    cpu_path = args.cpu
    if cpu_path is None and args.log.endswith(".log"):
        cpu_path = args.log[:-4] + ".cpu"
    cpu_mean, cpu_n = read_cpu(cpu_path)
    run = {
        "label": args.label,
        "log": args.log,
        "warmup": args.warmup,
        "dead": False,
        "var_value": args.var_value,
        "n_intervals": len(intervals),
        "dropped_intervals": boot_excluded + transient_dropped,
        "boot_excluded_intervals": boot_excluded,
        "fps_mean": fps_mean,
        "fps_sd": fps_sd,
        "draws_pf_mean": d_mean,
        "draws_pf_sd": d_sd,
        "draws_pf_cv": cv,
        "draws_per_s_mean": mean_sd(
            [iv["draws_pf"] * iv["fps"] for iv in intervals])[0],
        "flip_idle_ms_pf": mean_or_none(
            [iv.get("flip_idle_ms_pf") for iv in intervals]),
        "flip_gap_max_ms": max(kept_gaps) if kept_gaps else None,
        "cpu_pct_mean": cpu_mean,
        "cpu_samples": cpu_n,
        "apu_util_pct_mean": mean_or_none(apu_kept),
        "apu_util_pct_max": max(apu_kept) if apu_kept else None,
        "band": [band_lo, band_hi],
        "band_ok": band_ok,
        "out_of_band_intervals": out_of_band,
        "bimodal": bimodal,
        "cv_max": args.cv_max,
        "valid": valid,
        "vk_driver": log_vk_driver(args.log),
    }
    with open(args.out, "w") as f:
        json.dump(run, f, indent=2)

    tag = "OK" if valid else "INVALID"
    detail = []
    if not band_ok:
        detail.append(f"no usable in-band tail (intervals {out_of_band} "
                      f"outside draws/flip band [{band_lo:g},{band_hi:g}]; "
                      f"menu/out-of-band scene)")
    if bimodal:
        detail.append(f"bimodal: draws/flip CV {cv:.3f} > {args.cv_max:g} "
                      f"(bimodal attract reel)")
    anchored = (f" pre-scene={boot_excluded}" if boot_excluded else "")
    extra = ""
    if run["flip_idle_ms_pf"] is not None and intervals:
        extra += f" idle={run['flip_idle_ms_pf']:.2f}ms/f"
    if cpu_mean is not None:
        extra += f" cpu={cpu_mean:.0f}%"
    if run["apu_util_pct_mean"] is not None:
        extra += f" apu={run['apu_util_pct_mean']:.1f}%"
    print(f"gate[{args.label}]: {tag}  fps={fps_mean:.2f}±{fps_sd:.2f} "
          f"draws/flip={d_mean:.1f}±{d_sd:.1f} n={len(intervals)}{anchored}"
          f"{extra}"
          + ("  -- " + "; ".join(detail) if detail else ""))
    return 0 if valid else GATE_EXIT_INVALID


def write_dead_run(args):
    run = {
        "label": args.label,
        "log": args.log,
        "warmup": args.warmup,
        "dead": True,
        "var_value": args.var_value,
        "reason": args.reason,
        "valid": False,
    }
    with open(args.out, "w") as f:
        json.dump(run, f, indent=2)
    print(f"gate[{args.label}]: DEAD ({args.reason})")
    return 0  # recorded, batch continues (harness-hardening spec item 7)


# ---------------------------------------------------------------------------
# receipt
# ---------------------------------------------------------------------------

CROSS_ARM_INVALID = 0.15   # parser's scene-identity threshold
CROSS_ARM_WARN = 0.05      # composition-feedback: also emit draws/s


def cmd_receipt(args):
    with open(args.meta) as f:
        meta = json.load(f)

    runs = []
    for rj in meta["run_jsons"]:
        with open(rj) as f:
            runs.append(json.load(f))

    bench_runs = [r for r in runs if not r.get("warmup")]
    dead = [r for r in bench_runs if r.get("dead")]
    live = [r for r in bench_runs if not r.get("dead")]
    arm_b = [r for r in live if r["label"].startswith("B")]
    arm_e = [r for r in live if r["label"].startswith("E")]

    def arm_stats(arm):
        if not arm:
            return None
        fm, fs = mean_sd([r["fps_mean"] for r in arm])
        dm, ds = mean_sd([r["draws_pf_mean"] for r in arm])
        gaps = [r.get("flip_gap_max_ms") for r in arm
                if r.get("flip_gap_max_ms") is not None]
        apu_max = [r.get("apu_util_pct_max") for r in arm
                   if r.get("apu_util_pct_max") is not None]
        return {
            "n_runs": len(arm),
            "fps_mean": fm, "fps_sd": fs,
            "draws_pf_mean": dm, "draws_pf_sd": ds,
            "draws_per_s_mean": mean_sd(
                [r["draws_per_s_mean"] for r in arm])[0],
            # headroom (None where no run recorded it)
            "flip_idle_ms_pf": mean_or_none(
                [r.get("flip_idle_ms_pf") for r in arm]),
            "cpu_pct_mean": mean_or_none([r.get("cpu_pct_mean") for r in arm]),
            "flip_gap_max_ms_mean": mean_or_none(gaps),
            "flip_gap_max_ms": max(gaps) if gaps else None,
            "apu_util_pct_mean": mean_or_none(
                [r.get("apu_util_pct_mean") for r in arm]),
            "apu_util_pct_max": max(apu_max) if apu_max else None,
            "all_gates_ok": all(r["valid"] for r in arm),
        }

    sb, se = arm_stats(arm_b), arm_stats(arm_e)
    cap_fps = float(args.cap_fps if getattr(args, "cap_fps", None)
                    else meta.get("config", {}).get("cap_fps") or CAP_FPS)

    # per-pair deltas + sign tally (B_k vs E_k). Pair indices come from
    # the labels actually present so a fully-dead pair can't hide the
    # pairs after it.
    pairs, tally = [], {"pos": 0, "neg": 0, "zero": 0}
    by_label = {r["label"]: r for r in live}
    ks = sorted({int(l[1:]) for l in by_label
                 if l[:1] in "BE" and l[1:].isdigit()})
    for k in ks:
        b, e = by_label.get(f"B{k}"), by_label.get(f"E{k}")
        if b and e:
            d = e["fps_mean"] - b["fps_mean"]
            sign = "+" if d > 0 else ("-" if d < 0 else "0")
            tally["pos" if d > 0 else ("neg" if d < 0 else "zero")] += 1
            pair = {"pair": k, "B_fps": b["fps_mean"],
                    "E_fps": e["fps_mean"], "delta": d, "sign": sign}
            for key in ("cpu_pct_mean", "flip_idle_ms_pf"):
                if b.get(key) is not None and e.get(key) is not None:
                    pair["B_" + key], pair["E_" + key] = b[key], e[key]
            pairs.append(pair)
    unanimous = (len(pairs) > 0 and
                 (tally["pos"] == len(pairs) or tally["neg"] == len(pairs)))

    # cross-arm scene identity
    scene = {"verdict": "no-data", "cross_arm_draws_pf_ratio": None,
             "note": ""}
    if sb and se and sb["draws_pf_mean"] > 0 and se["draws_pf_mean"] > 0:
        hi = max(sb["draws_pf_mean"], se["draws_pf_mean"])
        ratio = abs(sb["draws_pf_mean"] - se["draws_pf_mean"]) / hi
        scene["cross_arm_draws_pf_ratio"] = ratio
        if ratio > CROSS_ARM_INVALID:
            scene["verdict"] = "invalid"
            scene["note"] = (f"arm draws/flip differ by {ratio*100:.0f}% "
                             "(>15%): different scenes, fps delta REFUSED "
                             "(menu/out-of-band scene or bimodal attract reel)")
        elif ratio > CROSS_ARM_WARN:
            scene["verdict"] = "warn-composition"
            scene["note"] = (f"arm draws/flip differ by {ratio*100:.1f}% "
                             "(>5%): frame-rate feedback moved scene "
                             "composition; compare draws/s throughput, "
                             "not raw fps alone (the 1.18 correction)")
        else:
            scene["verdict"] = "ok"

    # One driver per arm: a strike marker can switch runs whose setting is
    # kosmickrisp to MoltenVK mid-session.
    drivers = {arm: sorted({r.get("vk_driver") for r in runs_
                            if r.get("vk_driver")})
               for arm, runs_ in (("B", arm_b), ("E", arm_e))}
    driver_mixed = [arm for arm, d in drivers.items() if len(d) > 1]

    gates_ok = bool(live) and all(r["valid"] for r in live)
    # At the cap fps can't separate the arms and more pairs won't help;
    # nor at half of it, the 30 fps lock of a game that can't make every
    # vblank. Every run must sit at the same one.
    capped_at = None
    for level in (cap_fps, cap_fps / 2):
        if sb and se and all(
                abs(r["fps_mean"] - level) <= CAP_FPS_TOL and
                r.get("fps_sd", 0.0) <= CAP_RUN_SD_MAX for r in arm_b + arm_e):
            capped_at = level
            break
    capped = capped_at is not None
    # An APU thread near 100% falls behind real time (audio stutters)
    # whatever the fps says: warn, no verdict change.
    apu_warn = [arm for arm, s in (("B", sb), ("E", se))
                if s and s.get("apu_util_pct_max") is not None and
                s["apu_util_pct_max"] >= APU_UTIL_WARN]
    if dead or not live:
        verdict = "dead-runs"
    elif not gates_ok or scene["verdict"] == "invalid" or driver_mixed:
        verdict = "invalid"
    elif capped:
        verdict = "capped"
    else:
        verdict = "valid"

    headroom = None
    if sb and se:
        headroom = {"cap_fps": cap_fps, "capped_at": capped_at}
        for key in ("flip_idle_ms_pf", "cpu_pct_mean",
                    "flip_gap_max_ms_mean", "apu_util_pct_mean",
                    "draws_per_s_mean"):
            if sb.get(key) is not None and se.get(key) is not None:
                headroom[key] = {"B": sb[key], "E": se[key],
                                 "delta": se[key] - sb[key]}

    fps_delta = None
    if sb and se and verdict == "valid":
        d = se["fps_mean"] - sb["fps_mean"]
        fps_delta = {"abs": d,
                     "pct": (d / sb["fps_mean"] * 100.0)
                            if sb["fps_mean"] else None}

    throughput = None
    if sb and se:
        tb, te = sb["draws_per_s_mean"], se["draws_per_s_mean"]
        throughput = {"B_draws_per_s": tb, "E_draws_per_s": te,
                      "delta_pct": ((te - tb) / tb * 100.0) if tb else None}

    receipt = {
        "protocol": "bench-receipt/3",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "config": meta.get("config", {}),
        "binary": meta.get("binary", {}),
        "session": meta.get("session", {}),
        "runs": runs,
        "arms": {"B": sb, "E": se},
        "pairs": pairs,
        "sign_tally": {**tally, "n_pairs": len(pairs),
                       "unanimous": unanimous},
        "scene_identity": scene,
        "drivers": drivers,
        "driver_mixed_arms": driver_mixed,
        "apu_warn_arms": apu_warn,
        "throughput": throughput,
        "headroom": headroom,
        "fps_delta": fps_delta,
        "dead_runs": [r["label"] for r in dead],
        "verdict": verdict,
    }
    with open(args.out, "w") as f:
        json.dump(receipt, f, indent=2)

    print_receipt_block(receipt)
    return 0 if verdict in ("valid", "capped") else RECEIPT_EXIT_INVALID


def print_receipt_block(r):
    c, b = r["config"], r["binary"]
    print("\n================ PROTOCOL RECEIPT (machine copy: receipt.json) ================")
    print(f"protocol      : {r['protocol']}   generated {r['generated_utc']}")
    experiment = (c.get('experiment')
                  or f"{c.get('env_var', '?')}=0 (B) vs =1 (E)")
    print(f"experiment    : {experiment}, "
          f"snapshot '{c.get('snapshot', '?')}', {c.get('pairs', '?')} pairs x "
          f"{c.get('secs', '?')} s, interleaved {c.get('interleave', [])}")
    # bench-abx.sh: binary.arms, each arm's own identity (one bundle for
    # both arms repeats it); the Windows bench writes one flat binary
    # block instead.
    for arm, a in sorted(b.get('arms', {}).items()):
        print(f"binary {arm}      : {a.get('git_describe', '?')}  "
              f"sha256 {str(a.get('binary_sha256', '?'))[:16]}...  "
              f"MoltenVK {a.get('moltenvk_version', '?')}  "
              f"env '{a.get('env', '')}'"
              + (f"  set '{a['set']}'" if a.get('set') else ""))
        if a.get('vk_driver'):
            print(f"driver {arm}      : {a['vk_driver']}")
    if not b.get('arms'):
        print(f"binary        : {b.get('git_describe', '?')}  "
              f"sha256 {str(b.get('binary_sha256', '?'))[:16]}...  "
              f"MoltenVK {b.get('moltenvk_version', '?')}")
        if b.get('vk_driver'):
            print(f"driver        : {b['vk_driver']}")
    print(f"isolation     : app clone {b.get('app_clone', '?')}")
    print(f"                config {b.get('config_path', '?')}")
    print(f"                hdd clone {b.get('hdd_clone', '?')}")
    print(f"scene gate    : draws/flip band {c.get('draws_band', '?')}, "
          f"CV max {c.get('cv_max', '?')}, scene-anchored tail, "
          f"first in-scene interval dropped")
    for arm in ("B", "E"):
        s = r["arms"][arm]
        if s:
            print(f"arm {arm}         : fps {s['fps_mean']:.2f} ±{s['fps_sd']:.2f}  "
                  f"draws/flip {s['draws_pf_mean']:.1f} ±{s['draws_pf_sd']:.1f}  "
                  f"({s['n_runs']} runs, gates "
                  f"{'ok' if s['all_gates_ok'] else 'FAILED'})")
    for arm in ("B", "E"):
        s = r["arms"][arm]
        if not s:
            continue
        h = [f"draws/s {s['draws_per_s_mean']:.0f}"]
        if s.get("flip_idle_ms_pf") is not None:
            h.append(f"flip_idle {s['flip_idle_ms_pf']:.2f} ms/flip")
        if s.get("cpu_pct_mean") is not None:
            h.append(f"CPU {s['cpu_pct_mean']:.0f}%")
        if s.get("flip_gap_max_ms") is not None:
            h.append(f"worst frame {s['flip_gap_max_ms_mean']:.1f} ms "
                     f"(max {s['flip_gap_max_ms']:.1f})")
        print(f"headroom {arm}    : " + ", ".join(h))
        if s.get("apu_util_pct_mean") is not None:
            print(f"apu {arm}         : utilization {s['apu_util_pct_mean']:.1f}% "
                  f"mean, {s['apu_util_pct_max']:.1f}% max (XEMU_APU_PROF)")
        if arm in r.get("apu_warn_arms", []):
            print(f"WARNING       : arm {arm}'s APU thread peaked at "
                  f"{s['apu_util_pct_max']:.1f}% (>= {APU_UTIL_WARN:g}%): "
                  "near falling behind real time, audio may stutter "
                  "whatever the fps")
    for p in r["pairs"]:
        cpu = ""
        if p.get("B_cpu_pct_mean") is not None:
            cpu = (f"  CPU {p['B_cpu_pct_mean']:.0f}% -> {p['E_cpu_pct_mean']:.0f}% "
                   f"({p['E_cpu_pct_mean'] - p['B_cpu_pct_mean']:+.0f})")
        print(f"pair {p['pair']}        : B {p['B_fps']:.2f} -> E {p['E_fps']:.2f}  "
              f"delta {p['delta']:+.2f} ({p['sign']}){cpu}")
    t = r["sign_tally"]
    capped = r["verdict"] == "capped"
    print(f"sign tally    : +{t['pos']} / -{t['neg']} / 0:{t['zero']} of "
          f"{t['n_pairs']} pairs"
          + ("  (both arms at the cap: fps can't separate them)" if capped
             else "  (UNANIMOUS)" if t["unanimous"] else "  (not unanimous)"))
    si = r["scene_identity"]
    print(f"scene identity: {si['verdict']}"
          + (f" — {si['note']}" if si["note"] else ""))
    if r["throughput"] and si["verdict"] in ("warn-composition", "invalid"):
        tp = r["throughput"]
        print(f"throughput    : draws/s B {tp['B_draws_per_s']:.0f} -> "
              f"E {tp['E_draws_per_s']:.0f} ({tp['delta_pct']:+.1f}%)")
    if r["fps_delta"]:
        print(f"fps delta     : {r['fps_delta']['abs']:+.2f} fps "
              f"({r['fps_delta']['pct']:+.2f}%)")
    hr = r.get("headroom") or {}
    if capped:
        d = []
        for key, what, fmt in (("flip_idle_ms_pf", "flip_idle", "{:+.2f} ms/flip"),
                               ("cpu_pct_mean", "CPU", "{:+.1f} points"),
                               ("flip_gap_max_ms_mean", "worst frame", "{:+.1f} ms"),
                               ("apu_util_pct_mean", "APU", "{:+.1f} points")):
            if key in hr:
                d.append(what + " " + fmt.format(hr[key]["delta"]))
        cap = hr.get("cap_fps", CAP_FPS)
        at = hr.get("capped_at") or cap
        print(f"capped        : every run within {CAP_FPS_TOL:g} fps of "
              f"{at:g}" + (f" (half the {cap:g} cap)" if at != cap else "")
              + "; E - B: "
              + (", ".join(d) if d else "no headroom counters recorded")
              + " (more flip_idle, less CPU, shorter worst frames = more "
              "headroom; add no pairs)")
    if r["dead_runs"]:
        print(f"dead runs     : {', '.join(r['dead_runs'])}")
    for arm in r.get("driver_mixed_arms", []):
        print(f"driver mix    : arm {arm} ran {' and '.join(r['drivers'][arm])} "
              "(a KosmicKrisp strike marker?): verdict REFUSED")
    print(f"VERDICT       : {r['verdict'].upper()}")
    print("===============================================================================")


# ---------------------------------------------------------------------------
# selftest fixtures (--dry-run)
# ---------------------------------------------------------------------------

def write_fixture(path, interval_specs, idle_ms=None, gap_ms=None, apu=None,
                  cpu=None):
    """Write a synthetic nsprof stderr log. interval_specs: list of
    (fps, draws_pf). Matches hw/xbox/nv2a/nsprof.c line formats that
    nsprof_summarize.py parses. idle_ms / gap_ms add flip_idle (per flip)
    and flip_gap (max) counters, apu an `apu: utilization` line before
    each interval's summary, cpu an ab_<label>.cpu sibling (one sample
    per interval)."""
    with open(path, "w") as f:
        f.write("synthetic fixture: non-nsprof noise line\n")
        for fps, draws_pf in interval_specs:
            secs = 5.0
            flips = int(round(fps * secs))
            draws = int(round(draws_pf * flips))
            if apu is not None:
                f.write(f"apu: utilization {apu:5.1f}% (375 frames/s)\n")
            f.write(f"nsprof: {secs:.1f}s interval, {flips} flips "
                    f"({flips/secs:.1f}/s)\n")
            f.write(f"nsprof:   fence_wait   ev={flips*2}     "
                    f"total= 443.90ms per_flip= 2300.0us max= 8123.0us\n")
            if idle_ms is not None:
                f.write(f"nsprof:   flip_idle    ev={flips}     "
                        f"total={idle_ms * flips:7.2f}ms "
                        f"per_flip={idle_ms * 1000:7.1f}us max=20000.0us\n")
            if gap_ms is not None:
                f.write(f"nsprof:   flip_gap     ev={flips}     "
                        f"total={1000 * secs:7.2f}ms "
                        f"per_flip={1e6 / fps:7.1f}us "
                        f"max={gap_ms * 1000:7.1f}us\n")
            f.write(f"nsprof:   draws               ev={draws}   "
                    f"per_flip=  {draws_pf:.1f}\n")
            f.write(f"nsprof:   renderpass          ev={flips*14}   "
                    f"per_flip=  14.0\n")
    if cpu is not None:
        with open(path[:-4] + ".cpu", "w") as f:
            f.write("".join(f"{cpu + (40 if i == 0 else 0):.1f}\n"
                            for i in range(len(interval_specs))))


def gate_fixture(outdir, summarizer, label, log, band, cv_max,
                 var_value, warmup=False):
    ns = argparse.Namespace(
             label=label, log=log, out=os.path.join(outdir, f"run_{label}.json"),
             summarizer=summarizer, band=band, cv_max=cv_max,
             warmup=warmup, var_value=var_value, cpu=None)
    return cmd_gate(ns), ns.out


def cmd_selftest(args):
    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)
    band = (100.0, 1e9)
    cv_max = 0.10
    failures = []

    def expect(cond, what):
        print(("  ok    " if cond else "  FAIL  ") + what)
        if not cond:
            failures.append(what)

    print("== bench --dry-run selftest: plumbing check without xemu ==")

    # 1. clean interleaved 3-pair experiment; E arm ~ +0.5 fps
    run_jsons = []
    interleave = ["warmup"]
    fixtures = {"warmup": [(38.0, 440.0)] * 3}
    for k in (1, 2, 3):
        fixtures[f"B{k}"] = [(38.4 + 0.05 * k, 452.0 + 2 * k)] * 5
        fixtures[f"E{k}"] = [(38.9 + 0.05 * k, 455.0 + 2 * k)] * 5
        interleave += [f"B{k}", f"E{k}"]
    for label, spec in fixtures.items():
        log = os.path.join(outdir, f"ab_{label}.log")
        write_fixture(log, spec)
        rc, rj = gate_fixture(outdir, args.summarizer, label, log, band,
                              cv_max, var_value="0" if label.startswith("B")
                              else "1", warmup=(label == "warmup"))
        expect(rc == 0, f"gate passes clean fixture {label}")
        run_jsons.append(rj)

    # 2. the gate must FIRE on a menu/out-of-band scene fixture
    #    (draws/flip ~3, steady) ...
    menu_log = os.path.join(outdir, "ab_menu-negative.log")
    write_fixture(menu_log, [(60.0, 3.0)] * 5)
    rc, menu_json = gate_fixture(outdir, args.summarizer, "menu-negative",
                                 menu_log, band, cv_max, var_value="0")
    with open(menu_json) as f:
        menu_run = json.load(f)
    expect(rc == GATE_EXIT_INVALID and not menu_run["band_ok"],
           "gate FIRES on out-of-band menu fixture")

    # 3. ... and on a bimodal attract-reel fixture (in-band regime mix,
    #    high CV)
    bimodal_log = os.path.join(outdir, "ab_bimodal-negative.log")
    write_fixture(bimodal_log,
                  [(48.0, 350.0), (20.0, 390.0), (58.0, 150.0),
                   (21.0, 385.0), (55.0, 160.0), (20.5, 380.0)])
    rc, bim_json = gate_fixture(outdir, args.summarizer, "bimodal-negative",
                                bimodal_log, band, cv_max, var_value="0")
    with open(bim_json) as f:
        bim_run = json.load(f)
    expect(rc == GATE_EXIT_INVALID and bim_run["bimodal"],
           "gate FIRES on bimodal fixture")

    # 4. receipt over the clean runs: expect verdict valid, 3/3 sign +
    meta = {
        "config": {"snapshot": "dry-run-fixture", "env_var": "XEMU_SELFTEST",
                   "pairs": 3, "secs": 25, "draws_band": list(band),
                   "cv_max": cv_max, "interleave": interleave,
                   "dry_run": True},
        "binary": {"git_describe": "dry-run (xemu not launched)",
                   "binary_sha256": "-", "moltenvk_version": "-",
                   "app_clone": "-", "config_path": "-", "hdd_clone": "-"},
        "session": {"host": platform.node(), "os": platform.platform()},
        "run_jsons": run_jsons,
    }
    meta_path = os.path.join(outdir, "meta.json")
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)
    ns = argparse.Namespace(meta=meta_path,
                            out=os.path.join(outdir, "receipt.json"))
    rc = cmd_receipt(ns)
    with open(ns.out) as f:
        receipt = json.load(f)
    expect(rc == 0 and receipt["verdict"] == "valid",
           "receipt verdict valid on clean fixtures")
    expect(receipt["sign_tally"]["pos"] == 3 and
           receipt["sign_tally"]["unanimous"],
           "sign tally 3/3 positive, unanimous")
    expect(receipt["scene_identity"]["verdict"] == "ok",
           "cross-arm scene identity ok on clean fixtures")

    # 5. receipt must REFUSE an fps delta when the arms are different
    #    scenes (cross-arm draws/flip > 15%)
    mism_log = os.path.join(outdir, "ab_E9.log")
    write_fixture(mism_log, [(55.0, 220.0)] * 5)  # in-band, different scene
    _, mism_json = gate_fixture(outdir, args.summarizer, "E9", mism_log,
                                band, cv_max, var_value="1")
    b9_log = os.path.join(outdir, "ab_B9.log")
    write_fixture(b9_log, [(38.5, 452.0)] * 5)
    _, b9_json = gate_fixture(outdir, args.summarizer, "B9", b9_log,
                              band, cv_max, var_value="0")
    meta2 = dict(meta)
    meta2["run_jsons"] = [b9_json, mism_json]
    meta2_path = os.path.join(outdir, "meta-mismatch.json")
    with open(meta2_path, "w") as f:
        json.dump(meta2, f, indent=2)
    ns2 = argparse.Namespace(meta=meta2_path,
                             out=os.path.join(outdir, "receipt-mismatch.json"))
    rc2 = cmd_receipt(ns2)
    with open(ns2.out) as f:
        receipt2 = json.load(f)
    expect(rc2 == RECEIPT_EXIT_INVALID and
           receipt2["scene_identity"]["verdict"] == "invalid" and
           receipt2["fps_delta"] is None,
           "receipt REFUSES fps delta across mismatched scenes (>15%)")

    # 6. ... and when an arm's runs name more than one Vulkan driver
    #    (a strike marker switched driver = 'kosmickrisp' mid-session)
    mixed = []
    for i, rj in enumerate(run_jsons):
        with open(rj) as f:
            run = json.load(f)
        first_b = run["label"] == "B1"
        run["vk_driver"] = ("KosmicKrisp, Mesa 26.3.0" if first_b
                            else "MoltenVK, 1.4.3")
        path = os.path.join(outdir, f"run_mixed_{i}.json")
        with open(path, "w") as f:
            json.dump(run, f, indent=2)
        mixed.append(path)
    meta3 = dict(meta)
    meta3["run_jsons"] = mixed
    meta3_path = os.path.join(outdir, "meta-mixed.json")
    with open(meta3_path, "w") as f:
        json.dump(meta3, f, indent=2)
    ns3 = argparse.Namespace(meta=meta3_path,
                             out=os.path.join(outdir, "receipt-mixed.json"))
    rc3 = cmd_receipt(ns3)
    with open(ns3.out) as f:
        receipt3 = json.load(f)
    expect(rc3 == RECEIPT_EXIT_INVALID and
           receipt3["driver_mixed_arms"] == ["B"],
           "receipt REFUSES an arm whose runs ran two drivers")

    # 7. both arms at the 60 fps cap: CAPPED (exit 0, no fps delta), and
    #    the headroom it is judged by instead: E idles 2 ms a flip longer
    #    on 10 points less CPU, its worst frame 5 ms shorter, APU 20% vs 25%
    capped_jsons = []
    for k in (1, 2):
        for arm, idle, cpu, gap, apu in (("B", 6.0, 230.0, 30.0, 25.0),
                                         ("E", 8.0, 220.0, 25.0, 20.0)):
            label = f"{arm}{k}"
            log = os.path.join(outdir, f"ab_cap{label}.log")
            write_fixture(log, [(60.0, 576.0 + k)] * 5, idle_ms=idle,
                          gap_ms=gap, apu=apu, cpu=cpu)
            rc, rj = gate_fixture(outdir, args.summarizer, label, log, band,
                                  cv_max, var_value=arm)
            renamed = os.path.join(outdir, f"run_cap{label}.json")
            os.replace(rj, renamed)
            expect(rc == 0, f"gate passes capped fixture {label}")
            capped_jsons.append(renamed)
    meta4 = dict(meta)
    meta4["run_jsons"] = capped_jsons
    meta4_path = os.path.join(outdir, "meta-capped.json")
    with open(meta4_path, "w") as f:
        json.dump(meta4, f, indent=2)
    ns4 = argparse.Namespace(meta=meta4_path,
                             out=os.path.join(outdir, "receipt-capped.json"))
    rc4 = cmd_receipt(ns4)
    with open(ns4.out) as f:
        receipt4 = json.load(f)
    hr = receipt4.get("headroom") or {}
    expect(rc4 == 0 and receipt4["verdict"] == "capped" and
           receipt4["fps_delta"] is None,
           "receipt says CAPPED (exit 0, no fps delta) with both arms at 60")
    expect(abs(hr.get("flip_idle_ms_pf", {}).get("delta", 0) - 2.0) < 0.01 and
           abs(hr.get("cpu_pct_mean", {}).get("delta", 0) + 10.0) < 0.01 and
           abs(hr.get("flip_gap_max_ms_mean", {}).get("delta", 0) + 5.0) < 0.01 and
           abs(hr.get("apu_util_pct_mean", {}).get("delta", 0) + 5.0) < 0.01,
           "capped headroom: flip_idle +2 ms, CPU -10 (transient sample "
           "dropped), worst frame -5 ms, APU -5")
    expect(receipt["verdict"] == "valid" and
           receipt["headroom"].get("cpu_pct_mean") is None,
           "38 fps fixtures are not capped; no CPU without .cpu samples")
    expect(receipt4["apu_warn_arms"] == [],
           "no APU warning with the APU thread at 20-25%")

    # 8. the cap's other shapes: a 30 fps lock (half the default 60 cap)
    #    is CAPPED, B at 30 vs E at 60 is a real fps win, a meta cap_fps
    #    of 50 (PAL) caps at 50; an E APU thread peaking at 95% warns
    #    without changing the verdict.
    def cap_receipt(name, fps_of, cap_fps=None):
        jsons = []
        for k in (1, 2):
            for arm in ("B", "E"):
                label = f"{arm}{k}"
                log = os.path.join(outdir, f"ab_{name}{label}.log")
                write_fixture(log, [(fps_of[arm], 576.0 + k)] * 5,
                              idle_ms=6.0, apu=95.0 if arm == "E" else 40.0)
                _, rj = gate_fixture(outdir, args.summarizer, label, log,
                                     band, cv_max, var_value=arm)
                renamed = os.path.join(outdir, f"run_{name}{label}.json")
                os.replace(rj, renamed)
                jsons.append(renamed)
        m = dict(meta, run_jsons=jsons,
                 config=dict(meta["config"], **({"cap_fps": cap_fps}
                                                if cap_fps else {})))
        mpath = os.path.join(outdir, f"meta-{name}.json")
        with open(mpath, "w") as f:
            json.dump(m, f, indent=2)
        nsx = argparse.Namespace(meta=mpath,
                                 out=os.path.join(outdir, f"receipt-{name}.json"))
        rcx = cmd_receipt(nsx)
        with open(nsx.out) as f:
            return rcx, json.load(f)

    rc5, half = cap_receipt("half", {"B": 30.0, "E": 30.0})
    expect(rc5 == 0 and half["verdict"] == "capped" and
           half["headroom"]["capped_at"] == 30.0,
           "receipt says CAPPED at 30 for a 30 fps lock (half the 60 cap)")
    expect(half["apu_warn_arms"] == ["E"],
           "APU WARNING on the arm whose thread peaks at 95%, verdict kept")
    rc6, win = cap_receipt("halfwin", {"B": 30.0, "E": 60.0})
    expect(rc6 == 0 and win["verdict"] == "valid" and
           win["fps_delta"] is not None,
           "B at 30 vs E at 60 is not capped: the fps delta stands")
    rc7, pal = cap_receipt("pal", {"B": 50.0, "E": 50.0}, cap_fps=50)
    expect(rc7 == 0 and pal["verdict"] == "capped" and
           pal["headroom"]["capped_at"] == 50.0,
           "meta config.cap_fps 50 (bench-abx --cap-fps): CAPPED at 50")

    print()
    if failures:
        print(f"selftest: {len(failures)} FAILURE(S)")
        return SELFTEST_EXIT_FAIL
    print("selftest: all checks passed — harness plumbing is sound "
          "(no xemu launched, no user files touched)")
    return 0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_band(s):
    lo, _, hi = s.partition(":")
    return (float(lo), float(hi) if hi else 1e9)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gate", help="per-run scene-identity gate")
    g.add_argument("--label", required=True)
    g.add_argument("--log", required=True)
    g.add_argument("--out", required=True)
    g.add_argument("--summarizer", default=DEFAULT_SUMMARIZER)
    g.add_argument("--band", type=parse_band, default=(100.0, 1e9),
                   metavar="LO[:HI]")
    g.add_argument("--cv-max", type=float, default=0.10)
    g.add_argument("--warmup", action="store_true")
    g.add_argument("--var-value", default="")
    g.add_argument("--cpu", default=None, metavar="FILE",
                   help="per-tick process CPU %% samples, one a line "
                        "(default: the log's .cpu sibling, if any)")

    d = sub.add_parser("dead", help="record a run that failed to launch")
    d.add_argument("--label", required=True)
    d.add_argument("--log", required=True)
    d.add_argument("--out", required=True)
    d.add_argument("--reason", required=True)
    d.add_argument("--warmup", action="store_true")
    d.add_argument("--var-value", default="")

    r = sub.add_parser("receipt", help="aggregate runs into the receipt")
    r.add_argument("--meta", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--cap-fps", type=float, default=None,
                   help="frame cap for the CAPPED verdict (default: the "
                        "meta's config.cap_fps, else %g)" % CAP_FPS)

    s = sub.add_parser("selftest", help="--dry-run plumbing check")
    s.add_argument("--outdir", required=True)
    s.add_argument("--summarizer", default=DEFAULT_SUMMARIZER)

    args = ap.parse_args()
    if args.cmd == "gate":
        return cmd_gate(args)
    if args.cmd == "dead":
        return write_dead_run(args)
    if args.cmd == "receipt":
        return cmd_receipt(args)
    if args.cmd == "selftest":
        return cmd_selftest(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
