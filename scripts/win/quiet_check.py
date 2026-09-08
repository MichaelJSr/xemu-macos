#!/usr/bin/env python3
"""quiet_check.py - Windows quiet-machine gate for timed xemu runs.

The macOS protocol's mandatory pre-run check is `pgrep -fl "ninja|clang"`
plus `pgrep -fl 'soak|probe-run|movement-probe|bench-savestate|abx'`;
neither pgrep nor those shells exist here, so the whole class of
"benchmark ran against a compiling machine" and the leftover-harness-LOOP
incident (a stale soak that owns no xemu process between cycles and
relaunches xemu mid-batch) had no Windows detector at all.

What it checks:
  1. leftover xemu processes that are not ours (a previous run that never
     died still holds the qcow2 write lock and burns CPU/GPU);
  2. build and harness processes by COMMAND LINE, not just image name -
     ninja/meson/cc1/gcc/clang/lto/link plus soak|probe|bench|abx loops,
     which run as python.exe/bash.exe and are invisible to a name match.
     Our own process subtree is excluded (this script's own command line
     matches the harness pattern);
  3. host CPU load (Win32_Processor LoadPercentage);
  4. NVIDIA GPU utilization via nvidia-smi, when present.

Usage:
  quiet_check.py [--json] [--cpu-max 15] [--gpu-max 15] [--info]
Exit 0 = quiet, 1 = not quiet (the harness refuses to time a run), 2 =
the check itself could not run (treated as not quiet by the harness).

stdlib only. Also used by scripts/bench-savestate-ab-win.py to collect the
CPU/GPU identity recorded in the receipt (--info).
"""

import argparse
import json
import os
import re
import subprocess
import sys

# Deliberately NOT matching bare "cl"/"ld"/"link"/"make": on Windows those
# tokens occur inside ordinary paths and the gate would cry wolf.
BUILD_RE = re.compile(
    r"(?:^|[\\/\s\"])(ninja|meson|cc1plus|cc1|lto1|collect2|gcc|g\+\+|"
    r"clang\+\+|clang|msbuild|cmake|build\.sh|rebuild-quick\.sh)"
    r"(?:\.exe)?(?:\s|\"|$)", re.I)
HARNESS_RE = re.compile(
    r"(soak|probe-run|movement-probe|bench-savestate|bench-receipt|abx|"
    r"run-test|soak-cycles)", re.I)

PS = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command"]


def ps_json(cmd, timeout=30):
    """Run a PowerShell snippet that emits JSON; return the parsed value."""
    try:
        p = subprocess.run(PS + [cmd], capture_output=True, text=True,
                           timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise RuntimeError("powershell failed: %s" % e)
    if p.returncode != 0:
        raise RuntimeError("powershell rc=%d: %s" %
                           (p.returncode, (p.stderr or "").strip()[:200]))
    out = (p.stdout or "").strip()
    if not out:
        return None
    return json.loads(out)


def process_table():
    rows = ps_json(
        "@(Get-CimInstance Win32_Process | Select-Object ProcessId,"
        "ParentProcessId,Name,CommandLine) | ConvertTo-Json -Compress -Depth 3")
    if rows is None:
        return []
    if isinstance(rows, dict):
        rows = [rows]
    return rows


def own_subtree(rows, pid):
    """PIDs of this process, its ancestors and its descendants - all of
    which legitimately match the harness pattern."""
    by_pid = {r["ProcessId"]: r for r in rows}
    children = {}
    for r in rows:
        children.setdefault(r.get("ParentProcessId"), []).append(r["ProcessId"])
    own = set()
    # ancestors (bash/ssh/agent wrapper that launched us)
    cur = pid
    while cur in by_pid and cur not in own:
        own.add(cur)
        cur = by_pid[cur].get("ParentProcessId")
    # descendants
    stack = [pid]
    while stack:
        p = stack.pop()
        for c in children.get(p, []):
            if c not in own:
                own.add(c)
                stack.append(c)
    own.add(pid)
    return own


def cpu_load():
    v = ps_json("(Get-CimInstance Win32_Processor | "
                "Measure-Object -Property LoadPercentage -Average)"
                ".Average | ConvertTo-Json -Compress")
    return float(v) if v is not None else None


def cpu_info():
    v = ps_json("@(Get-CimInstance Win32_Processor | Select-Object Name,"
                "NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed) | "
                "ConvertTo-Json -Compress -Depth 3")
    if isinstance(v, dict):
        v = [v]
    return v or []


def nvidia_smi(query):
    try:
        p = subprocess.run(
            ["nvidia-smi", "--query-gpu=" + query, "--format=csv,noheader"],
            capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    return [l.strip() for l in p.stdout.splitlines() if l.strip()]


def gpu_info():
    lines = nvidia_smi("name,driver_version,memory.total")
    if lines is not None:
        return {"source": "nvidia-smi", "gpus": lines}
    try:
        v = ps_json("@(Get-CimInstance Win32_VideoController | "
                    "Select-Object -ExpandProperty Name) | "
                    "ConvertTo-Json -Compress")
    except RuntimeError:
        return {"source": "unknown", "gpus": []}
    if isinstance(v, str):
        v = [v]
    return {"source": "Win32_VideoController", "gpus": v or []}


def gpu_util():
    lines = nvidia_smi("utilization.gpu")
    if not lines:
        return None
    vals = []
    for l in lines:
        m = re.match(r"(\d+)", l)
        if m:
            vals.append(int(m.group(1)))
    return max(vals) if vals else None


def check(cpu_max, gpu_max, allow_pids=()):
    result = {"quiet": True, "problems": [], "cpu_load": None,
              "gpu_util": None, "xemu_processes": [], "busy_processes": []}
    rows = process_table()
    own = own_subtree(rows, os.getpid()) | set(allow_pids)

    for r in rows:
        pid = r.get("ProcessId")
        if pid in own:
            continue
        name = (r.get("Name") or "")
        cmd = (r.get("CommandLine") or name)
        if name.lower().startswith("xemu"):
            result["xemu_processes"].append({"pid": pid, "cmd": cmd[:200]})
            continue
        m = BUILD_RE.search(cmd) or HARNESS_RE.search(cmd)
        if m:
            result["busy_processes"].append(
                {"pid": pid, "match": m.group(1), "cmd": cmd[:200]})

    if result["xemu_processes"]:
        result["problems"].append(
            "leftover xemu process(es): %s" %
            ", ".join(str(x["pid"]) for x in result["xemu_processes"]))
    if result["busy_processes"]:
        result["problems"].append(
            "build/harness process(es) running: %s" %
            "; ".join("%d [%s] %s" % (x["pid"], x["match"], x["cmd"][:70])
                      for x in result["busy_processes"]))

    try:
        result["cpu_load"] = cpu_load()
    except RuntimeError as e:
        result["problems"].append("cpu load unavailable: %s" % e)
    if result["cpu_load"] is not None and result["cpu_load"] > cpu_max:
        result["problems"].append("CPU load %.0f%% > %.0f%%" %
                                  (result["cpu_load"], cpu_max))

    result["gpu_util"] = gpu_util()
    if result["gpu_util"] is not None and result["gpu_util"] > gpu_max:
        result["problems"].append("GPU utilization %d%% > %d%%" %
                                  (result["gpu_util"], gpu_max))

    result["quiet"] = not result["problems"]
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--cpu-max", type=float, default=15.0)
    ap.add_argument("--gpu-max", type=float, default=15.0)
    ap.add_argument("--allow-pid", type=int, action="append", default=[],
                    help="PID (and subtree) to ignore, e.g. the caller")
    ap.add_argument("--info", action="store_true",
                    help="print host CPU/GPU identity as JSON and exit 0")
    args = ap.parse_args()

    if args.info:
        try:
            info = {"cpu": cpu_info(), "gpu": gpu_info()}
        except RuntimeError as e:
            info = {"error": str(e)}
        json.dump(info, sys.stdout, indent=2)
        print()
        return 0

    try:
        res = check(args.cpu_max, args.gpu_max, args.allow_pid)
    except RuntimeError as e:
        if args.json:
            json.dump({"quiet": False, "problems": ["check failed: %s" % e]},
                      sys.stdout, indent=2)
            print()
        else:
            print("quiet-check: ERROR %s" % e)
        return 2

    if args.json:
        json.dump(res, sys.stdout, indent=2)
        print()
    else:
        print("quiet-check: %s  cpu=%s%% gpu=%s%%" %
              ("QUIET" if res["quiet"] else "NOT QUIET",
               "?" if res["cpu_load"] is None else "%.0f" % res["cpu_load"],
               "?" if res["gpu_util"] is None else res["gpu_util"]))
        for p in res["problems"]:
            print("  - %s" % p)
    return 0 if res["quiet"] else 1


if __name__ == "__main__":
    sys.exit(main())
