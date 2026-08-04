#!/usr/bin/env python3
"""bench-receipt.py — gate + receipt engine for scripts/bench-savestate-ab.sh.

Encodes the validity disciplines the interleaved savestate A/B method
demands (see .claude/skills/xemu-testing and the 9.x benchmarking
incident classes in xemu-failure-archaeology) so they are ENFORCED by
the harness instead of left to operator discipline:

  gate      per-run scene-identity gate: parse one nsprof log (via
            nsprof_summarize.py --json), assert every kept interval's
            draws/flip lies inside the required band (incident 9.1:
            menu-vs-gameplay), and flag within-run bimodality via the
            draws/flip coefficient of variation (incident 9.2:
            attract-reel regime straddling). Writes a per-run JSON.

  receipt   aggregate per-run JSONs into the full protocol receipt:
            per-arm mean +/- sd, per-pair deltas and sign tally,
            cross-arm draws/flip identity check (>15% = INVALID,
            >5% = composition warning + draws/s throughput, the 1.18
            correction), binary identity, and a machine-readable JSON
            receipt. Exit code carries the verdict.

  selftest  --dry-run backend: builds synthetic nsprof fixtures (a
            clean 3-pair A/B, a bimodal run, an out-of-band menu run),
            proves the gate PASSES the clean runs and FAILS the bad
            ones (a net must fire before it is trusted), then builds
            a receipt from the clean runs and checks its verdict and
            sign tally. Touches nothing outside --outdir.

stdlib only. Python >= 3.8.
"""

import argparse
import json
import math
import os
import platform
import subprocess
import sys
import time

GATE_EXIT_INVALID = 3
RECEIPT_EXIT_INVALID = 4
SELFTEST_EXIT_FAIL = 5


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


def mean_sd(vals):
    if not vals:
        return 0.0, 0.0
    m = sum(vals) / len(vals)
    if len(vals) < 2:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return m, math.sqrt(var)


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
    # incidents 9.1/9.2 still fire.
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
        "band": [band_lo, band_hi],
        "band_ok": band_ok,
        "out_of_band_intervals": out_of_band,
        "bimodal": bimodal,
        "cv_max": args.cv_max,
        "valid": valid,
    }
    with open(args.out, "w") as f:
        json.dump(run, f, indent=2)

    tag = "OK" if valid else "INVALID"
    detail = []
    if not band_ok:
        detail.append(f"no usable in-band tail (intervals {out_of_band} "
                      f"outside draws/flip band [{band_lo:g},{band_hi:g}]; "
                      f"incident-9.1 class)")
    if bimodal:
        detail.append(f"bimodal: draws/flip CV {cv:.3f} > {args.cv_max:g} "
                      f"(incident-9.2 class)")
    anchored = (f" pre-scene={boot_excluded}" if boot_excluded else "")
    print(f"gate[{args.label}]: {tag}  fps={fps_mean:.2f}±{fps_sd:.2f} "
          f"draws/flip={d_mean:.1f}±{d_sd:.1f} n={len(intervals)}{anchored}"
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
        return {
            "n_runs": len(arm),
            "fps_mean": fm, "fps_sd": fs,
            "draws_pf_mean": dm, "draws_pf_sd": ds,
            "draws_per_s_mean": mean_sd(
                [r["draws_per_s_mean"] for r in arm])[0],
            "all_gates_ok": all(r["valid"] for r in arm),
        }

    sb, se = arm_stats(arm_b), arm_stats(arm_e)

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
            pairs.append({"pair": k, "B_fps": b["fps_mean"],
                          "E_fps": e["fps_mean"], "delta": d, "sign": sign})
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
                             "(incident-9.1/9.2 class)")
        elif ratio > CROSS_ARM_WARN:
            scene["verdict"] = "warn-composition"
            scene["note"] = (f"arm draws/flip differ by {ratio*100:.1f}% "
                             "(>5%): frame-rate feedback moved scene "
                             "composition; compare draws/s throughput, "
                             "not raw fps alone (the 1.18 correction)")
        else:
            scene["verdict"] = "ok"

    gates_ok = bool(live) and all(r["valid"] for r in live)
    if dead or not live:
        verdict = "dead-runs"
    elif not gates_ok or scene["verdict"] == "invalid":
        verdict = "invalid"
    else:
        verdict = "valid"

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
        "protocol": "bench-savestate-ab/2",
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
        "throughput": throughput,
        "fps_delta": fps_delta,
        "dead_runs": [r["label"] for r in dead],
        "verdict": verdict,
    }
    with open(args.out, "w") as f:
        json.dump(receipt, f, indent=2)

    print_receipt_block(receipt)
    return 0 if verdict == "valid" else RECEIPT_EXIT_INVALID


def print_receipt_block(r):
    c, b = r["config"], r["binary"]
    print("\n================ PROTOCOL RECEIPT (machine copy: receipt.json) ================")
    print(f"protocol      : {r['protocol']}   generated {r['generated_utc']}")
    print(f"experiment    : {c.get('env_var', '?')}=0 (B) vs =1 (E), "
          f"snapshot '{c.get('snapshot', '?')}', {c.get('pairs', '?')} pairs x "
          f"{c.get('secs', '?')} s, interleaved {c.get('interleave', [])}")
    print(f"binary        : {b.get('git_describe', '?')}  "
          f"sha256 {str(b.get('binary_sha256', '?'))[:16]}...  "
          f"MoltenVK {b.get('moltenvk_version', '?')}")
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
    for p in r["pairs"]:
        print(f"pair {p['pair']}        : B {p['B_fps']:.2f} -> E {p['E_fps']:.2f}  "
              f"delta {p['delta']:+.2f} ({p['sign']})")
    t = r["sign_tally"]
    print(f"sign tally    : +{t['pos']} / -{t['neg']} / 0:{t['zero']} of "
          f"{t['n_pairs']} pairs"
          + ("  (UNANIMOUS)" if t["unanimous"] else "  (not unanimous — "
             "underpowered for a marginal claim; escalate to 6/6)"))
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
    if r["dead_runs"]:
        print(f"dead runs     : {', '.join(r['dead_runs'])}")
    print(f"VERDICT       : {r['verdict'].upper()}")
    print("===============================================================================")


# ---------------------------------------------------------------------------
# selftest fixtures (--dry-run)
# ---------------------------------------------------------------------------

def write_fixture(path, interval_specs):
    """Write a synthetic nsprof stderr log. interval_specs: list of
    (fps, draws_pf). Matches hw/xbox/nv2a/nsprof.c line formats that
    nsprof_summarize.py parses."""
    with open(path, "w") as f:
        f.write("synthetic fixture: non-nsprof noise line\n")
        for fps, draws_pf in interval_specs:
            secs = 5.0
            flips = int(round(fps * secs))
            draws = int(round(draws_pf * flips))
            f.write(f"nsprof: {secs:.1f}s interval, {flips} flips "
                    f"({flips/secs:.1f}/s)\n")
            f.write(f"nsprof:   fence_wait   ev={flips*2}     "
                    f"total= 443.90ms per_flip= 2300.0us max= 8123.0us\n")
            f.write(f"nsprof:   draws               ev={draws}   "
                    f"per_flip=  {draws_pf:.1f}\n")
            f.write(f"nsprof:   renderpass          ev={flips*14}   "
                    f"per_flip=  14.0\n")


def gate_fixture(outdir, summarizer, label, log, band, cv_max,
                 var_value, warmup=False):
    ns = argparse.Namespace(
             label=label, log=log, out=os.path.join(outdir, f"run_{label}.json"),
             summarizer=summarizer, band=band, cv_max=cv_max,
             warmup=warmup, var_value=var_value)
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

    # 2. the gate must FIRE on an incident-9.1 fixture (menu scene,
    #    draws/flip ~3, steady) ...
    menu_log = os.path.join(outdir, "ab_menu-negative.log")
    write_fixture(menu_log, [(60.0, 3.0)] * 5)
    rc, menu_json = gate_fixture(outdir, args.summarizer, "menu-negative",
                                 menu_log, band, cv_max, var_value="0")
    with open(menu_json) as f:
        menu_run = json.load(f)
    expect(rc == GATE_EXIT_INVALID and not menu_run["band_ok"],
           "gate FIRES on out-of-band menu fixture (incident 9.1)")

    # 3. ... and on an incident-9.2 fixture (bimodal attract reel:
    #    in-band regime mix, high CV)
    bimodal_log = os.path.join(outdir, "ab_bimodal-negative.log")
    write_fixture(bimodal_log,
                  [(48.0, 350.0), (20.0, 390.0), (58.0, 150.0),
                   (21.0, 385.0), (55.0, 160.0), (20.5, 380.0)])
    rc, bim_json = gate_fixture(outdir, args.summarizer, "bimodal-negative",
                                bimodal_log, band, cv_max, var_value="0")
    with open(bim_json) as f:
        bim_run = json.load(f)
    expect(rc == GATE_EXIT_INVALID and bim_run["bimodal"],
           "gate FIRES on bimodal fixture (incident 9.2)")

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
    g.add_argument("--summarizer", required=True)
    g.add_argument("--band", type=parse_band, default=(100.0, 1e9),
                   metavar="LO[:HI]")
    g.add_argument("--cv-max", type=float, default=0.10)
    g.add_argument("--warmup", action="store_true")
    g.add_argument("--var-value", default="")

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

    s = sub.add_parser("selftest", help="--dry-run plumbing check")
    s.add_argument("--outdir", required=True)
    s.add_argument("--summarizer", required=True)

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
