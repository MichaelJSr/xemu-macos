#!/usr/bin/env python3
"""bench-savestate-ab-win.py - reproducible in-game perf A/B for xemu on Windows.

Windows port of the protocol in scripts/bench-savestate-ab.sh (which stays
macOS-only: zsh, dist/xemu.app, APFS clones, AF_UNIX monitor socket,
screencapture). Same protocol, same gates, same receipt engine
(scripts/bench-receipt.py) and the same nsprof summarizer, so a Windows
receipt is structurally comparable to a macOS one - but ONLY within its own
platform: never compare absolute fps across hosts.

  bench-savestate-ab-win.py <snapshot> <ENV_VAR> [pairs] [secs] [outdir] [flags]
  bench-savestate-ab-win.py --menu - <ENV_VAR> [pairs] [secs] [outdir] [flags]
  bench-savestate-ab-win.py --dry-run [outdir]

Runs interleaved pairs (B: ENV_VAR=0, E: ENV_VAR=<--e-value>) of one binary,
loading <snapshot> through the QEMU monitor after boot (CLI -loadvm loads
before xemu attaches USB controllers and fails with a topology mismatch).
Per-run nsprof logs, CPU samples, a screenshot and a gate verdict land in
<outdir>; the batch ends with meta.json + receipt.json + the printed receipt
block.

What it enforces (the Windows equivalents of the macOS disciplines):

  isolation      never runs from the user's live files: dist\\ is copied to a
                 work dir (--no-copy-exe opts out), the qcow2 and eeprom are
                 copied, and a scratch xemu.toml is passed with -config_path.
                 The user's config/hdd are only ever COPY SOURCES.
  config parity  xemu REWRITES its toml on exit (it drops default-valued keys
                 and adds display.vulkan.preferred_physical_device), so the
                 scratch config is regenerated from the template BEFORE EVERY
                 RUN and patched by [section] + key, never by key alone.
  quiet machine  scripts/win/quiet_check.py runs before the batch: leftover
                 xemu processes, build/harness processes matched by COMMAND
                 LINE (the leftover-harness-LOOP class owns no xemu process
                 between cycles), CPU load and nvidia-smi GPU utilization.
                 --no-quiet-check to override, recorded in the receipt.
  renderer pin   display.renderer is read from the template, recorded in
                 meta/receipt and pinned into every scratch config. The batch
                 REFUSES to start on anything but VULKAN (--allow-renderer
                 overrides): nsprof_flip_tick() is called only from the
                 Vulkan renderer, so an OPENGL run produces a log with zero
                 nsprof intervals and every run dies at the gate.
  scene gate     every run's nsprof log is gated by scripts/bench-receipt.py:
                 draws/flip inside --draws-band, draws/flip CV <= --cv-max,
                 scene-anchored trailing tail, one post-load transient
                 dropped. Cross-arm draws/flip identity is checked in the
                 receipt (>15% refuses the fps delta).
  identity       exe sha256 (hashlib), git describe, host CPU/GPU, and the
                 full env delta of each arm are written into meta.json.
  robustness     a failed launch, a failed loadvm or a mid-dwell exit is
                 recorded as a DEAD run and the batch continues. Unique work
                 dir and monitor port per invocation.
  visual check   one screenshot mid-dwell (scripts/win/capture_screen.ps1),
                 SCORED AFTER the run ends (scripts/win/score_shot_win.py) so
                 the scorer's CPU never lands inside a timed window. Any
                 ARTIFACT sets exit code 3.

--menu mode. Until a golden savestate exists on this machine the harness can
still exercise every mechanism except loadvm: --menu boots and dwells in the
title/menu scene. Menu fps is vsync-adjacent and swings ~18-30 fps between
intervals, so --menu numbers are a PLUMBING CHECK, NOT A CITABLE RESULT; the
receipt's meta records mode=menu for exactly that reason. Its default band is
0:10 (the menu scene runs ~2 draws/flip; the boot interval runs 70-130 and is
excluded by the band, which is how the gate anchors on the scene).

Cross-binary / multi-variable arms (the abx.sh shapes): --b-exe/--e-exe give
each arm its own binary, --b-env/--e-env give each arm an env SET
("XEMU_A=1 XEMU_B=2"). Pass "-" for <ENV_VAR> when using --b-env/--e-env.

Escape hatches / knobs are xemu's own XEMU_* env vars; this script only sets
XEMU_NV2A_NSPROF=1 (the measurement channel) plus the arm's variables.

Requirements: Python >= 3.10, stdlib only, Windows. PowerShell is used for
the screenshot and the quiet check; both degrade to SKIP/override rather than
failing a batch.
"""

import argparse
import ctypes
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECEIPT_PY = os.path.join(ROOT, "scripts", "bench-receipt.py")
SUMMARIZER = os.path.join(ROOT, ".claude", "skills",
                          "xemu-diagnostics-and-tooling", "scripts",
                          "nsprof_summarize.py")
LIST_SNAPSHOTS = os.path.join(ROOT, ".claude", "skills",
                              "xemu-diagnostics-and-tooling", "scripts",
                              "list_snapshots.py")
WIN_DIR = os.path.join(ROOT, "scripts", "win")
CAPTURE_PS1 = os.path.join(WIN_DIR, "capture_screen.ps1")
SCORER = os.path.join(WIN_DIR, "score_shot_win.py")
QUIET_CHECK = os.path.join(WIN_DIR, "quiet_check.py")

EXIT_ARTIFACT = 3


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def log(msg):
    print("bench: " + msg, flush=True)


def die(msg, code=2):
    print("bench: error: " + msg, file=sys.stderr, flush=True)
    sys.exit(code)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_describe():
    try:
        p = subprocess.run(["git", "-C", ROOT, "describe", "--tags",
                            "--match", "v*", "--always", "--dirty"],
                           capture_output=True, text=True, timeout=30)
        if p.returncode == 0:
            return p.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "unknown"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def parse_env_set(text):
    """'A=1 B=2' -> {'A': '1', 'B': '2'}

    Windows-appropriate splitting. shlex's POSIX default treats a backslash
    as an escape and silently eats it (--e-env "XEMU_X=C:\\tools\\x" would set
    "C:toolsx" and the receipt would record an env delta that was never
    applied), while posix=False keeps backslashes but stops honouring quotes
    once a token has started ('X="C:\\Program Files\\x"' splits in two). So:
    POSIX quote handling with escapes disabled - backslashes survive
    verbatim, and quoting a value that contains spaces still works.
    """
    lex = shlex.shlex(text or "", posix=True)
    lex.whitespace_split = True
    lex.commenters = ""        # '#' is a legal character inside a value
    lex.escape = ""            # a backslash is a path separator, not an escape
    try:
        toks = list(lex)
    except ValueError as exc:  # unbalanced quote: a message, never a traceback
        die("cannot parse env set %r: %s" % (text, exc))
    out = {}
    for tok in toks:
        if "=" not in tok:
            die("bad env assignment %r (want NAME=VALUE)" % tok)
        k, _, v = tok.partition("=")
        out[k] = v
    return out


# ---------------------------------------------------------------------------
# section-aware TOML patcher
#
# xemu rewrites its config on exit (default-valued keys disappear,
# display.vulkan.preferred_physical_device appears), so the scratch config is
# rebuilt from the template for every run. Keys must be matched under their
# [section] header: hdd_path lives in [sys.files], and a key-only match would
# happily rewrite a same-named key in another table.
# ---------------------------------------------------------------------------

SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")


class TomlEdit:
    def __init__(self, text):
        self.lines = text.replace("\r\n", "\n").split("\n")

    def _key_re(self, key):
        return re.compile(r"^\s*" + re.escape(key) + r"\s*=")

    def get(self, section, key):
        kre = self._key_re(key)
        cur = ""
        for line in self.lines:
            m = SECTION_RE.match(line)
            if m:
                cur = m.group(1).strip()
                continue
            if cur == section and kre.match(line):
                val = line.split("=", 1)[1].strip()
                if len(val) >= 2 and val[0] in "'\"" and val[-1] == val[0]:
                    val = val[1:-1]
                return val
        return None

    def set(self, section, key, literal):
        kre = self._key_re(key)
        cur = ""
        sect_start = sect_end = None
        for i, line in enumerate(self.lines):
            m = SECTION_RE.match(line)
            if m:
                if cur == section and sect_end is None:
                    sect_end = i
                cur = m.group(1).strip()
                if cur == section:
                    sect_start = i
                continue
            if cur == section and kre.match(line):
                self.lines[i] = "%s = %s" % (key, literal)
                return
        if sect_start is None:
            self.lines += ["", "[%s]" % section, "%s = %s" % (key, literal)]
            return
        end = sect_end if sect_end is not None else len(self.lines)
        while end > sect_start + 1 and not self.lines[end - 1].strip():
            end -= 1
        self.lines.insert(end, "%s = %s" % (key, literal))

    def shortcuts(self):
        """[general.snapshots.shortcuts] as {name: tag}."""
        out = {}
        cur = ""
        for line in self.lines:
            m = SECTION_RE.match(line)
            if m:
                cur = m.group(1).strip()
                continue
            if cur == "general.snapshots.shortcuts" and "=" in line:
                k, _, v = line.partition("=")
                v = v.strip()
                if len(v) >= 2 and v[0] in "'\"" and v[-1] == v[0]:
                    v = v[1:-1]
                out[k.strip()] = v
        return out

    def render(self):
        return "\n".join(self.lines).rstrip("\n") + "\n"


def toml_path_literal(path):
    """TOML literal string: no escape processing, so Windows backslashes
    survive verbatim."""
    if "'" in path:
        die("path contains a single quote, which a TOML literal string "
            "cannot hold: %s" % path)
    return "'%s'" % path


# ---------------------------------------------------------------------------
# process CPU sampling (no psutil: GetProcessTimes through ctypes)
# ---------------------------------------------------------------------------

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class _FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", ctypes.c_uint32),
                ("dwHighDateTime", ctypes.c_uint32)]


def _ft_to_100ns(ft):
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


class CpuSampler:
    """%CPU of a process, ps(1)-style: 100 = one core saturated."""

    def __init__(self, pid):
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.h = self.k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                      False, pid)
        self.prev = None
        self.prev_t = None

    def sample(self):
        if not self.h:
            return None
        c, e, k, u = _FILETIME(), _FILETIME(), _FILETIME(), _FILETIME()
        ok = self.k32.GetProcessTimes(self.h, ctypes.byref(c), ctypes.byref(e),
                                      ctypes.byref(k), ctypes.byref(u))
        if not ok:
            return None
        busy = (_ft_to_100ns(k) + _ft_to_100ns(u)) / 1e7  # seconds
        now = time.monotonic()
        out = None
        if self.prev is not None and now > self.prev_t:
            out = (busy - self.prev) / (now - self.prev_t) * 100.0
        self.prev, self.prev_t = busy, now
        return out

    def close(self):
        if self.h:
            self.k32.CloseHandle(self.h)
            self.h = None


# ---------------------------------------------------------------------------
# QEMU monitor over TCP
# ---------------------------------------------------------------------------

def monitor_cmd(port, cmd, settle=3.0, timeout=5.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        time.sleep(0.5)
        s.settimeout(1.0)
        try:
            s.recv(65536)          # banner
        except socket.timeout:
            pass
        s.sendall((cmd + "\n").encode())
        time.sleep(settle)
        data = b""
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        except (socket.timeout, OSError):
            pass
        return data.decode("utf-8", "replace")
    finally:
        try:
            s.close()
        except OSError:
            pass


def monitor_up(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1.0)
        s.close()
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# the harness
# ---------------------------------------------------------------------------

class Bench:
    def __init__(self, args):
        self.a = args
        self.out = os.path.abspath(args.outdir)
        self.work = os.path.join(self.out, "work.%d" % os.getpid())
        os.makedirs(self.out, exist_ok=True)
        os.makedirs(self.work, exist_ok=True)
        self.run_jsons = []
        self.interleave = []
        self.artifact = False
        self.template_text = None
        self.hdd = None
        self.eeprom = None
        self.exe = {}          # label prefix -> exe path
        self.arm_env = {}      # 'B'/'E' -> dict
        self.staged = {}       # source dir -> work-dir copy

    # -- setup ------------------------------------------------------------
    def prepare(self):
        a = self.a
        src_cfg = a.config_template or os.path.join(
            os.environ.get("APPDATA", ""), "xemu", "xemu", "xemu.toml")
        if not os.path.isfile(src_cfg):
            die("no config template at %s (--config-template)" % src_cfg)
        self.config_source = os.path.abspath(src_cfg)
        with open(self.config_source, encoding="utf-8", errors="replace") as f:
            self.template_text = f.read()
        tmpl = TomlEdit(self.template_text)

        # Renderer. xemu drops default-valued keys when it rewrites its
        # config, so an ABSENT display.renderer means the spec default
        # (VULKAN, config_spec.yml). It matters because the measurement
        # channel is renderer-specific: nsprof_flip_tick() is called only
        # from hw/xbox/nv2a/pgraph/vk/renderer.c, so under OPENGL the log
        # carries zero nsprof intervals and every run dies at the gate with
        # no visible explanation. Recorded in meta and pinned per run.
        self.renderer = (tmpl.get("display", "renderer") or "VULKAN").upper()
        if self.renderer not in ("VULKAN", "OPENGL", "NULL"):
            die("config template %s has display.renderer = %r, which is not "
                "one of NULL/OPENGL/VULKAN (config_spec.yml)"
                % (self.config_source, self.renderer))
        if self.renderer != "VULKAN":
            if not a.allow_renderer:
                die("config template %s selects display.renderer = %s, but "
                    "nsprof (the only measurement channel this harness reads) "
                    "ticks solely on the Vulkan renderer: every run would "
                    "produce a log with no nsprof intervals and be recorded "
                    "DEAD. Set renderer = 'VULKAN' in the template, or pass "
                    "--allow-renderer to run anyway (plumbing only - there "
                    "will be no fps numbers)."
                    % (self.config_source, self.renderer))
            log("warning: renderer %s with --allow-renderer - nsprof never "
                "ticks outside VULKAN, expect DEAD runs" % self.renderer)
        log("renderer %s (pinned into every scratch config)" % self.renderer)

        # snapshot tag: the monitor only knows the qcow2 vm-* tag; the
        # [general.snapshots.shortcuts] names are UI keybindings and
        # `loadvm f8` fails silently into a socket nobody reads.
        self.snapshot = None
        if not a.menu:
            snap = a.snapshot
            shortcuts = tmpl.shortcuts()
            if snap in shortcuts:
                log("snapshot shortcut %s -> tag %s" % (snap, shortcuts[snap]))
                snap = shortcuts[snap]
            elif not snap.startswith("vm-"):
                log("warning: '%s' is not a vm-* tag and no shortcut maps it; "
                    "the monitor needs the qcow2 tag" % snap)
            self.snapshot = snap

        # hdd / eeprom sources
        hdd_src = a.hdd or tmpl.get("sys.files", "hdd_path")
        if not hdd_src or not os.path.isfile(hdd_src):
            die("cannot resolve hdd qcow2 (config had %r); pass --hdd"
                % hdd_src)
        self.hdd_source = os.path.abspath(hdd_src)
        eeprom_src = a.eeprom or tmpl.get("sys.files", "eeprom_path")
        self.eeprom_source = (os.path.abspath(eeprom_src)
                              if eeprom_src and os.path.isfile(eeprom_src)
                              else None)

        if self.snapshot:
            self.verify_snapshot()

        # binaries
        default_exe = a.exe or os.path.join(ROOT, "dist", "xemu.exe")
        b_exe = a.b_exe or default_exe
        e_exe = a.e_exe or default_exe
        for tag, src in (("B", b_exe), ("E", e_exe)):
            if not os.path.isfile(src):
                die("no xemu binary at %s (build first, or --exe)" % src)
            self.exe[tag] = self.stage_exe(tag, os.path.abspath(src))
        self.exe_source = {"B": os.path.abspath(b_exe),
                           "E": os.path.abspath(e_exe)}

        # arm environments
        if a.b_env or a.e_env:
            self.arm_env = {"B": parse_env_set(a.b_env),
                            "E": parse_env_set(a.e_env)}
            self.var_value = {"B": a.b_env or "", "E": a.e_env or ""}
        else:
            if a.env_var in (None, "-", ""):
                die("need <ENV_VAR> (or --b-env/--e-env)")
            self.arm_env = {"B": {a.env_var: "0"},
                            "E": {a.env_var: str(a.e_value)}}
            self.var_value = {"B": "0", "E": str(a.e_value)}

        # data copies: the live files are never opened by xemu here
        self.hdd = os.path.join(self.work, "hdd.qcow2")
        log("copying hdd %s -> %s (%.0f MiB)" %
            (self.hdd_source, self.hdd,
             os.path.getsize(self.hdd_source) / (1 << 20)))
        shutil.copyfile(self.hdd_source, self.hdd)
        if self.eeprom_source:
            self.eeprom = os.path.join(self.work, "eeprom.bin")
            shutil.copyfile(self.eeprom_source, self.eeprom)

    def stage_exe(self, tag, src):
        """Copy the binary's whole directory into the work dir. A rebuild
        mid-batch would otherwise swap the binary under a running batch, and
        with --isolate-caches the copy is also where the portable-mode
        marker gets planted (never the user's dist\\)."""
        if self.a.no_copy_exe:
            if self.a.isolate_caches:
                die("--isolate-caches needs a copied binary (it plants a "
                    "portable xemu.toml next to the exe); drop --no-copy-exe")
            return src
        srcdir = os.path.dirname(src)
        # One copy per distinct source: the common single-binary A/B must not
        # pay for two ~200 MiB copies, and both arms then run byte-identical
        # files (with --isolate-caches they also share one warm cache dir,
        # exactly as the macOS harness does).
        if srcdir in self.staged:
            return os.path.join(self.staged[srcdir], os.path.basename(src))
        dst_dir = os.path.join(self.work, "bin-%s" % tag)
        log("copying %s -> %s" % (srcdir, dst_dir))
        shutil.copytree(srcdir, dst_dir, dirs_exist_ok=True)
        self.staged[srcdir] = dst_dir
        return os.path.join(dst_dir, os.path.basename(src))

    def verify_snapshot(self):
        """Fail before the first 25 s boot instead of after N dead runs."""
        try:
            p = subprocess.run([sys.executable, LIST_SNAPSHOTS,
                                self.hdd_source, "--json"],
                               capture_output=True, text=True, timeout=120)
            names = [s["name"] for s in json.loads(p.stdout)["snapshots"]]
        except Exception as e:                       # noqa: BLE001
            log("warning: could not read the qcow2 snapshot table (%s); "
                "skipping the pre-flight check" % e)
            return
        if self.snapshot not in names:
            die("snapshot %r is not in %s (has: %s). Save one in xemu first "
                "(F7/F8), or use --menu." %
                (self.snapshot, self.hdd_source, ", ".join(names) or "none"))
        log("snapshot %s found in the image" % self.snapshot)

    def write_config(self, label, exe_path):
        """Regenerate the scratch config for ONE run."""
        t = TomlEdit(self.template_text)
        t.set("sys.files", "hdd_path", toml_path_literal(self.hdd))
        if self.eeprom:
            t.set("sys.files", "eeprom_path", toml_path_literal(self.eeprom))
        if self.a.dvd:
            t.set("sys.files", "dvd_path",
                  toml_path_literal(os.path.abspath(self.a.dvd)))
        # Pin the renderer: it is a default-valued key xemu drops on
        # rewrite, and the whole measurement channel depends on it.
        t.set("display", "renderer", "'%s'" % self.renderer)
        # fullscreen skews present pacing; the welcome/update dialogs steal
        # the window and the network on a first launch of a scratch config.
        t.set("display.window", "fullscreen_on_startup", "false")
        t.set("general", "show_welcome", "false")
        t.set("general.updates", "check", "false")
        if self.a.surface_scale:
            t.set("display.quality", "surface_scale", str(self.a.surface_scale))
        if self.a.isolate_caches:
            # Portable-mode marker: SDL_GetBasePath() is the exe's directory
            # on Windows, so planting the config there moves the whole data
            # base path (spirv cache, pipeline_cache.bin) into the COPY.
            path = os.path.join(os.path.dirname(exe_path), "xemu.toml")
        else:
            path = os.path.join(self.work, "xemu.%s.toml" % label)
        with open(path, "w", encoding="utf-8") as f:
            f.write(t.render())
        return path

    # -- one run ----------------------------------------------------------
    def run_one(self, label, arm, secs, warmup=False):
        a = self.a
        logpath = os.path.join(self.out, "ab_%s.log" % label)
        cpupath = os.path.join(self.out, "ab_%s.cpu" % label)
        runjson = os.path.join(self.out, "run_%s.json" % label)
        self.run_jsons.append(runjson)
        self.interleave.append(label)

        exe = self.exe[arm]
        cfg = self.write_config(label, exe)
        port = free_port()

        env = dict(os.environ)
        env["XEMU_NV2A_NSPROF"] = "1"
        env.update(self.arm_env[arm])
        # nsprof writes to stdout, monitor/QEMU noise to stderr: capture both
        # into one file (archaeology 9.x - a half-captured log gates INVALID).
        logf = open(logpath, "w", encoding="utf-8", errors="replace")
        cmd = [exe, "-config_path", cfg,
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port]
        proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT,
                                cwd=os.path.dirname(exe), env=env)

        try:
            up = False
            for _ in range(40):
                if proc.poll() is not None:
                    break
                if monitor_up(port):
                    up = True
                    break
                time.sleep(1.0)
            if not up:
                self.kill(proc)
                logf.close()
                self.record_dead(label, logpath, runjson,
                                 "launch failed (monitor port %d never "
                                 "opened)" % port, arm, warmup)
                return

            time.sleep(a.boot_wait)

            # A xemu that exits during boot (the 6-10 s Windows failure this
            # harness exists to chase) must become a recorded DEAD run, not
            # monitor traffic into a socket nobody holds any more.
            if proc.poll() is not None:
                logf.close()
                self.record_dead(label, logpath, runjson,
                                 "xemu exited during boot, before loadvm "
                                 "(exit %s)" % proc.returncode, arm, warmup)
                return

            if self.snapshot:
                try:
                    resp = monitor_cmd(port, "loadvm %s" % self.snapshot)
                except (OSError, socket.timeout) as exc:
                    # socket errors and timeouts are all OSError subclasses;
                    # unguarded, a xemu that died or wedged between the
                    # monitor opening and the end of --boot-wait killed the
                    # whole batch with a traceback instead of costing one run.
                    rc = proc.poll()
                    self.kill(proc)
                    logf.close()
                    self.record_dead(
                        label, logpath, runjson,
                        "monitor loadvm failed (%s: %s); xemu was %s"
                        % (type(exc).__name__, exc,
                           "still running" if rc is None
                           else "already gone (exit %s)" % rc),
                        arm, warmup)
                    return
                # Sidecar, NEVER the run log: xemu holds that file open with
                # its own offset, and a parent write at offset 0 would
                # overwrite the nsprof header lines (archaeology 9.8).
                with open(os.path.join(self.out, "mon_%s.log" % label), "w",
                          encoding="utf-8") as mf:
                    mf.write("loadvm %s\n" % self.snapshot)
                    mf.write(resp + "\n")
                if re.search(r"error|Error|does not exist|unknown command",
                             resp):
                    self.kill(proc)
                    logf.close()
                    self.record_dead(label, logpath, runjson,
                                     "loadvm failed (see mon_%s.log)" % label,
                                     arm, warmup)
                    return

            # dwell: 5 s ticks, per-tick %CPU, one screenshot mid-run
            sampler = CpuSampler(proc.pid)
            sampler.sample()                     # prime the delta
            ticks = max(1, secs // 5)
            shot_tick = (ticks + 1) // 2
            shot = None
            died = None
            with open(cpupath, "w", encoding="utf-8") as cf:
                for j in range(1, ticks + 1):
                    time.sleep(5.0)
                    if proc.poll() is not None:
                        died = proc.returncode
                        break
                    pct = sampler.sample()
                    if pct is not None:
                        cf.write("%.1f\n" % pct)
                        cf.flush()
                    if a.screenshot and j == shot_tick:
                        shot = self.capture(label, proc.pid)
            sampler.close()

            if died is not None:
                logf.close()
                self.record_dead(label, logpath, runjson,
                                 "xemu exited during the dwell (exit %s)"
                                 % died, arm, warmup, check_nsprof=True)
                return

            try:
                monitor_cmd(port, "quit", settle=0.5, timeout=5.0)
            except OSError:
                pass
        finally:
            for _ in range(15):
                if proc.poll() is not None:
                    break
                time.sleep(1.0)
            self.kill(proc)
            if not logf.closed:
                logf.close()

        # Scoring runs AFTER the timed window on purpose: a pure-Python PNG
        # decode inside the dwell would show up in the numbers it is meant to
        # validate.
        shotline = self.score(label, shot) if shot else ""
        self.gate(label, logpath, runjson, arm, warmup)
        asserts = 0
        try:
            with open(logpath, encoding="utf-8", errors="replace") as f:
                asserts = sum(1 for l in f if "Assertion" in l)
        except OSError:
            pass
        log("%s done (%s, asserts=%d)%s" %
            (label, self.describe_arm(arm), asserts,
             (" [%s]" % shotline) if shotline else ""))

    def describe_arm(self, arm):
        return " ".join("%s=%s" % kv for kv in self.arm_env[arm].items()) \
            or "(no env delta)"

    def kill(self, proc):
        if proc.poll() is not None:
            return
        # /T (tree): TerminateProcess on the direct child alone would orphan
        # anything it launched, and an orphan holds the qcow2 write lock and
        # burns CPU into the NEXT run - the leftover-process class the quiet
        # check exists to catch.
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           capture_output=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
            except OSError:
                pass

    def capture(self, label, pid):
        png = os.path.join(self.out, "shot_%s.png" % label)
        thumb = os.path.join(self.out, "shot_%s.small.png" % label)
        try:
            p = subprocess.run(
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                 "-File", CAPTURE_PS1, "-Out", png, "-ProcessId", str(pid),
                 "-Thumb", thumb], capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as e:
            self.write_verdict(label, "screenshot: SKIP (capture failed: %s)"
                               % e)
            return None
        if p.returncode != 0 or not os.path.isfile(png):
            self.write_verdict(label, "screenshot: SKIP (capture failed: %s)"
                               % (p.stderr or "").strip()[:160])
            return None
        return (png, thumb if os.path.isfile(thumb) else None)

    def score(self, label, shot):
        png, thumb = shot
        cmd = [sys.executable, SCORER, png]
        if thumb:
            cmd += ["--thumb", thumb]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=300)
        except (OSError, subprocess.TimeoutExpired) as e:
            self.write_verdict(label, "screenshot: SKIP (scoring failed: %s)"
                               % e)
            return ""
        line = (p.stdout or "").strip().splitlines()
        line = line[-1] if line else "screenshot: SKIP (no verdict)"
        self.write_verdict(label, line)
        if line.startswith("screenshot: ARTIFACT"):
            self.artifact = True
        return line

    def write_verdict(self, label, line):
        # Sidecar file, never the run log: xemu's non-append stdout redirect
        # overwrites bytes appended to the shared log (archaeology 9.8).
        with open(os.path.join(self.out, "shot_%s.verdict" % label), "w",
                  encoding="utf-8") as f:
            f.write(line + "\n")

    def dead_reason(self, base, logpath, arm, check_nsprof):
        """Name the two Windows failure shapes a bare exit code hides.

        empty capture   xemu only inherits this harness's stdout when
                        AttachConsole(ATTACH_PARENT_PROCESS) succeeds; with no
                        console attached it freopens BOTH streams to xemu.log
                        in its cwd (ui/xemu.c), leaving the redirected file
                        here at 0 bytes with the real output in the sidecar.
        no nsprof       a log with no interval headers at all is the renderer
                        signature (nsprof only ticks on VULKAN), not a
                        mysterious gate failure.
        """
        notes = []
        try:
            size = os.path.getsize(logpath)
        except OSError:
            size = -1
        if size == 0:
            exe = self.exe.get(arm)
            alt = (os.path.join(os.path.dirname(exe), "xemu.log")
                   if exe else "xemu.log next to the exe")
            notes.append("captured log is EMPTY - with no console attached "
                         "xemu redirects stdout/stderr to xemu.log in its "
                         "cwd; look at %s%s"
                         % (alt, "" if os.path.isfile(alt)
                            else " (not present)"))
        elif size > 0 and check_nsprof:
            heads = -1
            try:
                with open(logpath, encoding="utf-8", errors="replace") as f:
                    heads = sum(1 for l in f
                                if l.startswith("nsprof: ")
                                and " interval," in l)
            except OSError:
                pass
            if heads == 0:
                notes.append("log has 0 nsprof interval headers (renderer=%s; "
                             "nsprof_flip_tick only runs on VULKAN)"
                             % getattr(self, "renderer", "?"))
        return base + ("; " + "; ".join(notes) if notes else "")

    def record_dead(self, label, logpath, runjson, reason, arm, warmup,
                    check_nsprof=False):
        reason = self.dead_reason(reason, logpath, arm, check_nsprof)
        cmd = [sys.executable, RECEIPT_PY, "dead", "--label", label,
               "--log", logpath, "--out", runjson, "--reason", reason,
               "--var-value", self.var_value[arm]]
        if warmup:
            cmd.append("--warmup")
        subprocess.run(cmd)

    def gate(self, label, logpath, runjson, arm, warmup):
        cmd = [sys.executable, RECEIPT_PY, "gate", "--label", label,
               "--log", logpath, "--out", runjson, "--summarizer", SUMMARIZER,
               "--band", self.a.draws_band, "--cv-max", str(self.a.cv_max),
               "--var-value", self.var_value[arm]]
        if warmup:
            cmd.append("--warmup")
        p = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        if p.returncode not in (0, 3) or not os.path.isfile(runjson):
            # The gate itself failed (e.g. a log with no nsprof headers at
            # all): record the run as DEAD so the batch still produces a
            # receipt instead of crashing in the aggregator.
            sys.stdout.write(p.stderr)
            self.record_dead(label, logpath, runjson,
                             "gate failed: %s" %
                             ((p.stderr or "").strip().splitlines() or
                              ["unknown"])[-1][:160], arm, warmup,
                             check_nsprof=True)

    # -- batch ------------------------------------------------------------
    def quiet_check(self):
        if self.a.no_quiet_check:
            log("quiet-machine check SKIPPED (--no-quiet-check)")
            return {"skipped": True}
        try:
            p = subprocess.run([sys.executable, QUIET_CHECK, "--json",
                                "--cpu-max", str(self.a.cpu_max),
                                "--gpu-max", str(self.a.gpu_max),
                                "--allow-pid", str(os.getpid())],
                               capture_output=True, text=True, timeout=180)
            res = json.loads(p.stdout)
        except Exception as e:                       # noqa: BLE001
            die("quiet-machine check could not run (%s); --no-quiet-check to "
                "proceed anyway" % e)
        if not res.get("quiet"):
            for prob in res.get("problems", []):
                print("bench:   - %s" % prob, file=sys.stderr)
            die("machine is not quiet - a timed run against a loaded machine "
                "is an environment failure, not a result. Kill the listed "
                "processes (harness LOOPS first, they respawn xemu) or pass "
                "--no-quiet-check.")
        log("quiet-machine check OK (cpu=%s%% gpu=%s%%)" %
            (res.get("cpu_load"), res.get("gpu_util")))
        return res

    def host_info(self):
        try:
            p = subprocess.run([sys.executable, QUIET_CHECK, "--info"],
                               capture_output=True, text=True, timeout=180)
            return json.loads(p.stdout)
        except Exception:                            # noqa: BLE001
            return {}

    def run(self):
        a = self.a
        quiet = self.quiet_check()
        self.prepare()

        desc = git_describe()
        sha = {tag: sha256_file(path) for tag, path in self.exe.items()}
        log("binary %s sha256 %s..." % (desc, sha["B"][:16]))
        if sha["B"] != sha["E"]:
            log("cross-binary A/B: E sha256 %s..." % sha["E"][:16])
        log("work dir %s" % self.work)
        log("hdd copy %s (source %s untouched)" % (self.hdd, self.hdd_source))
        log("cache isolation %s" % ("ON" if a.isolate_caches else "OFF"))
        if a.menu:
            log("MENU MODE: no loadvm. Menu fps swings between intervals - "
                "this is a plumbing check, not a citable number.")

        if not a.no_warmup:
            self.run_one("warmup", "E", a.warmup_secs, warmup=True)
        for k in range(1, a.pairs + 1):
            self.run_one("B%d" % k, "B", a.secs)
            self.run_one("E%d" % k, "E", a.secs)

        meta = {
            "config": {
                "snapshot": self.snapshot or "(menu mode, no loadvm)",
                "env_var": a.env_var if a.env_var not in (None, "-") else
                           "B:[%s] E:[%s]" % (a.b_env or "", a.e_env or ""),
                "pairs": a.pairs, "secs": a.secs,
                "draws_band": a.draws_band, "cv_max": a.cv_max,
                "boot_wait": a.boot_wait,
                "cache_isolation": "on" if a.isolate_caches else "off",
                "renderer": self.renderer,
                "interleave": self.interleave,
                "mode": "menu" if a.menu else "savestate",
                "arm_env": {k: v for k, v in self.arm_env.items()},
                "harness": "bench-savestate-ab-win.py",
            },
            "binary": {
                "git_describe": desc,
                "binary_sha256": sha["B"],
                "binary_sha256_E": sha["E"],
                "moltenvk_version": "n/a (windows)",
                "app_clone": self.exe["B"],
                "app_clone_E": self.exe["E"],
                "app_source": self.exe_source["B"],
                "config_path": os.path.join(self.work, "xemu.<label>.toml"),
                "config_source": self.config_source,
                "hdd_clone": self.hdd, "hdd_source": self.hdd_source,
            },
            "session": {
                "host": platform.node(),
                "os": platform.platform(),
                "python": sys.version.split()[0],
                "hardware": self.host_info(),
                "quiet_check": quiet,
                # A stray XEMU_*/VK_* knob in the launching shell applies to
                # BOTH arms and silently moves the baseline; record what was
                # actually inherited so a receipt can be re-read later.
                "inherited_env": {k: v for k, v in os.environ.items()
                                  if k.startswith(("XEMU_", "MVK_", "VK_"))},
            },
            "run_jsons": self.run_jsons,
        }
        metapath = os.path.join(self.out, "meta.json")
        with open(metapath, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2)

        rc = subprocess.run([sys.executable, RECEIPT_PY, "receipt",
                             "--meta", metapath,
                             "--out", os.path.join(self.out, "receipt.json")]
                            ).returncode
        log("receipt at %s" % os.path.join(self.out, "receipt.json"))

        if a.screenshot:
            log("visual checks:")
            for label in self.interleave:
                vp = os.path.join(self.out, "shot_%s.verdict" % label)
                if os.path.isfile(vp):
                    with open(vp, encoding="utf-8") as f:
                        print("  " + f.read().strip())
            if self.artifact:
                print("bench: VISUAL ARTIFACT flagged - review the shot_*.png",
                      file=sys.stderr)
                if rc == 0:
                    rc = EXIT_ARTIFACT

        if not a.keep_work:
            shutil.rmtree(self.work, ignore_errors=True)
        else:
            log("work dir kept at %s" % self.work)
        return rc


# ---------------------------------------------------------------------------
# entry
# ---------------------------------------------------------------------------

def dry_run(outdir):
    """Plumbing self-test: no xemu, no user files. Proves the receipt gates
    fire on the incident fixtures AND that the Windows artifact oracle fires
    on synthetic corruption."""
    os.makedirs(outdir, exist_ok=True)
    rc = subprocess.run([sys.executable, RECEIPT_PY, "selftest",
                         "--outdir", outdir,
                         "--summarizer", SUMMARIZER]).returncode
    rc2 = subprocess.run([sys.executable, SCORER, "--selftest",
                          os.path.join(outdir, "shots")]).returncode
    print("\n== toml patcher check (section-scoped, survives xemu's rewrite) ==")
    t = TomlEdit("[general]\nshow_welcome = true\n\n[sys.files]\n"
                 "hdd_path = 'C:\\old.qcow2'\n")
    t.set("sys.files", "hdd_path", "'C:\\new.qcow2'")
    t.set("display.window", "fullscreen_on_startup", "false")
    t.set("general", "show_welcome", "false")
    ok = ("hdd_path = 'C:\\new.qcow2'" in t.render() and
          "[display.window]" in t.render() and
          "show_welcome = false" in t.render() and
          t.get("sys.files", "hdd_path") == "C:\\new.qcow2")
    print(("  ok    " if ok else "  FAIL  ") +
          "patch by [section]+key, insert missing section")

    print("\n== renderer read (xemu drops the key when it is the default) ==")
    r_absent = TomlEdit("[general]\nshow_welcome = false\n").get(
        "display", "renderer")
    r_gl = TomlEdit("[display]\nrenderer = 'OPENGL'\n").get(
        "display", "renderer")
    ok_r = r_absent is None and r_gl == "OPENGL"
    print(("  ok    " if ok_r else "  FAIL  ") +
          "absent -> %r (harness reads VULKAN), explicit -> %r"
          % (r_absent, r_gl))

    print("\n== env-set split (Windows backslashes survive) ==")
    got = parse_env_set(
        'XEMU_A=C:\\tools\\x XEMU_B="C:\\Program Files\\y" XEMU_C=1')
    ok_e = got == {"XEMU_A": "C:\\tools\\x",
                   "XEMU_B": "C:\\Program Files\\y", "XEMU_C": "1"}
    print(("  ok    " if ok_e else "  FAIL  ") + "--b-env/--e-env -> %r" % got)

    return rc or rc2 or (0 if ok and ok_r and ok_e else 1)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("snapshot", nargs="?")
    ap.add_argument("env_var", nargs="?")
    ap.add_argument("pairs", nargs="?", type=int, default=3)
    ap.add_argument("secs", nargs="?", type=int, default=75)
    ap.add_argument("outdir", nargs="?", default="./bench-out")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--menu", action="store_true",
                    help="skip loadvm; bench the boot/menu scene")
    ap.add_argument("--draws-band", default=None,
                    help="scene band LO[:HI] (default 100:inf, menu 0:10)")
    ap.add_argument("--cv-max", type=float, default=0.10)
    ap.add_argument("--exe", default=None, help="xemu.exe (default dist/)")
    ap.add_argument("--b-exe", default=None)
    ap.add_argument("--e-exe", default=None)
    ap.add_argument("--b-env", default=None, help='env SET, "A=1 B=2"')
    ap.add_argument("--e-env", default=None)
    ap.add_argument("--e-value", default="1")
    ap.add_argument("--config-template", default=None)
    ap.add_argument("--hdd", default=None)
    ap.add_argument("--eeprom", default=None)
    ap.add_argument("--dvd", default=None)
    ap.add_argument("--surface-scale", type=int, default=None)
    ap.add_argument("--boot-wait", type=int, default=None,
                    help="seconds from launch to loadvm (default 25, menu 45)")
    ap.add_argument("--warmup-secs", type=int, default=40)
    ap.add_argument("--no-warmup", action="store_true")
    ap.add_argument("--no-copy-exe", action="store_true")
    ap.add_argument("--isolate-caches", action="store_true",
                    default=os.environ.get("XEMU_BENCH_ISOLATE_CACHES") == "1")
    ap.add_argument("--keep-work", action="store_true")
    ap.add_argument("--no-screenshot", dest="screenshot", action="store_false",
                    default=True)
    ap.add_argument("--no-quiet-check", action="store_true")
    ap.add_argument("--allow-renderer", action="store_true",
                    help="run even if display.renderer is not VULKAN "
                         "(nsprof never ticks there: DEAD runs, no numbers)")
    ap.add_argument("--cpu-max", type=float, default=15.0)
    ap.add_argument("--gpu-max", type=float, default=15.0)
    args = ap.parse_args()

    if args.dry_run:
        return dry_run(args.snapshot or "./bench-out-dryrun")

    if os.name != "nt":
        # MSYS2's /usr/bin/python is a POSIX build (os.name == 'posix') even
        # though it runs on Windows: it has no ctypes.WinDLL for the
        # GetProcessTimes sampler and it hands xemu MSYS-style paths.
        hint = ("on macOS/Linux use scripts/bench-savestate-ab.sh instead"
                if not (os.environ.get("MSYSTEM") or
                        sys.platform.startswith(("msys", "cygwin")))
                else "you are running MSYS2's POSIX python (MSYSTEM=%s); "
                     "install the native one (`pacman -S "
                     "mingw-w64-x86_64-python`) and invoke "
                     "/mingw64/bin/python, or use python.org's Windows python"
                     % os.environ.get("MSYSTEM", "?"))
        die("this harness needs a native Windows python (os.name == 'nt'); "
            "this interpreter reports os.name == %r - %s." % (os.name, hint))
    if not args.snapshot:
        die("usage: bench-savestate-ab-win.py <snapshot> <ENV_VAR> "
            "[pairs] [secs] [outdir]   (or --menu - <ENV_VAR>, or --dry-run)")
    if not args.menu and args.snapshot in ("-", "menu"):
        die("snapshot '-' only makes sense with --menu")
    if args.draws_band is None:
        args.draws_band = "0:10" if args.menu else "100"
    if args.boot_wait is None:
        args.boot_wait = 45 if args.menu else 25
    for path in (RECEIPT_PY, SUMMARIZER, SCORER, CAPTURE_PS1, QUIET_CHECK):
        if not os.path.isfile(path):
            die("missing %s" % path)

    bench = Bench(args)
    try:
        return bench.run()
    except KeyboardInterrupt:
        die("interrupted", 130)


if __name__ == "__main__":
    sys.exit(main())
