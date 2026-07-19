#!/bin/zsh
# Reproducible in-game perf A/B for xemu — hardened harness.
#
#   scripts/bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir] [flags]
#   scripts/bench-savestate-ab.sh --dry-run [outdir]
#
# Runs interleaved pairs (B: ENV_VAR=0, E: ENV_VAR=1) of a single
# binary, loading <snapshot> through the QEMU monitor after boot (CLI
# -loadvm loads before xemu attaches USB controllers and fails with a
# topology mismatch). Emits per-run nsprof logs + 5 s CPU samples to
# <outdir> (default ./bench-out), then a full protocol receipt
# (receipt.json + printed block).
#
# What this version ENFORCES (previously operator discipline; see the
# 9.x incident classes in .claude/skills/xemu-failure-archaeology and
# the testing-audit hardening spec):
#   isolation      never launches dist/xemu.app or touches the user's
#                  live config/hdd: the bundle is APFS-cloned into a
#                  work dir, the qcow2 and eeprom are cloned (cp -c;
#                  snapshots ride along), and a scratch xemu.toml is
#                  passed via -config_path (fullscreen forced off —
#                  fullscreen skews present pacing). User paths are
#                  only ever CLONE SOURCES.
#   launch parity  canonical MVK_CONFIG_* exported explicitly.
#   scene gate     every run's nsprof log is parsed; each kept
#                  interval's draws/flip must lie in --draws-band
#                  (default 100:inf, the in-game threshold) and the
#                  run must not be bimodal (draws/flip CV <= --cv-max,
#                  default 0.10). First interval dropped as load
#                  transient. Cross-arm draws/flip identity is checked
#                  in the receipt: >15% refuses the fps delta, >5%
#                  emits draws/s throughput (composition feedback).
#   binary identity  git describe + binary sha256 + bundled MoltenVK
#                  version of what ACTUALLY ran, recorded in the
#                  receipt.
#   robustness     a failed launch/loadvm is recorded as a DEAD run;
#                  the batch continues. Unique work dir + monitor
#                  socket per invocation.
#   visual check   one window screenshot is captured partway through
#                  each run and scored for white-screen / stuck-frame /
#                  magenta-tile artifacts (scripts/bench-screenshot.py);
#                  verdicts land in the run logs and the batch summary,
#                  and any ARTIFACT sets exit code 3. Skipped gracefully
#                  where no window is capturable (--no-screenshot to
#                  disable).
#
# --dry-run validates all gate/receipt plumbing against synthetic
# nsprof fixtures WITHOUT launching xemu and without reading the
# user's config/hdd (see scripts/bench-receipt.py selftest — it also
# proves the gates FIRE on incident-9.1/9.2 fixtures).
#
# Flags (all optional, may appear anywhere):
#   --dry-run            plumbing self-test, no xemu, no user files
#   --draws-band LO[:HI] scene-identity band for draws/flip (def 100:inf)
#   --cv-max X           bimodality threshold on draws/flip CV (def 0.10)
#   --app PATH           bundle to CLONE (default $ROOT/dist/xemu.app)
#   --config-src PATH    config to CLONE (default the user's xemu.toml)
#   --hdd PATH           qcow2 to CLONE (default: hdd_path from config-src)
#   --boot-wait N        seconds from launch to loadvm (default 25)
#   --keep-work          keep the work dir (app/hdd clones) afterwards
#   --no-screenshot      skip the mid-run visual artifact check
#   --e-value V          value of ENV_VAR in the E arm (default 1; B stays 0)
#
# Analysis note: baseline reproducibility of this protocol measured at
# +/-0.02 fps on static scenes. Default 3 pairs is a smoke bar; a
# marginal (<2%) claim needs 6 pairs and a unanimous sign tally — the
# receipt prints the tally and says when it is underpowered.
set -e

ROOT=${0:a:h:h}
RECEIPT_PY=$ROOT/scripts/bench-receipt.py
SHOT_PY=$ROOT/scripts/bench-screenshot.py
SUMMARIZER=$ROOT/.claude/skills/xemu-diagnostics-and-tooling/scripts/nsprof_summarize.py

# ---- argument parsing (positionals unchanged; flags additive) -------------
typeset -a pos
DRY_RUN=0
DRAWS_BAND=100
CV_MAX=0.10
APP_SRC=""
CONFIG_SRC=""
HDD_SRC=""
BOOT_WAIT=25
KEEP_WORK=0
SCREENSHOT=1
E_VALUE=1
while (( $# > 0 )); do
  case "$1" in
    --dry-run)     DRY_RUN=1 ;;
    --draws-band)  DRAWS_BAND=$2; shift ;;
    --cv-max)      CV_MAX=$2; shift ;;
    --app)         APP_SRC=$2; shift ;;
    --config-src)  CONFIG_SRC=$2; shift ;;
    --hdd)         HDD_SRC=$2; shift ;;
    --boot-wait)   BOOT_WAIT=$2; shift ;;
    --keep-work)   KEEP_WORK=1 ;;
    --no-screenshot) SCREENSHOT=0 ;;
    --e-value)     E_VALUE=$2; shift ;;
    -h|--help)     sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --*)           echo "unknown flag: $1" >&2; exit 2 ;;
    *)             pos+=("$1") ;;
  esac
  shift
done

if (( DRY_RUN )); then
  OUT=${pos[1]:-./bench-out-dryrun}
  mkdir -p $OUT
  [[ -f $SUMMARIZER ]] || { echo "missing $SUMMARIZER" >&2; exit 1; }
  exec python3 $RECEIPT_PY selftest --outdir $OUT --summarizer $SUMMARIZER
fi

SNAP=${pos[1]:?usage: bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir] | --dry-run}
VAR=${pos[2]:?need env var name}
PAIRS=${pos[3]:-3}
SECS=${pos[4]:-75}
OUT=${pos[5]:-./bench-out}

: ${APP_SRC:=$ROOT/dist/xemu.app}
: ${CONFIG_SRC:=$HOME/Library/Application Support/xemu/xemu/xemu.toml}

[[ -f $SUMMARIZER ]] || { echo "missing $SUMMARIZER (scene gate is mandatory)" >&2; exit 1; }
[[ -d $APP_SRC ]] || { echo "no app bundle at $APP_SRC (build first, or --app)" >&2; exit 1; }
[[ -f $CONFIG_SRC ]] || { echo "no config at $CONFIG_SRC (--config-src)" >&2; exit 1; }

mkdir -p $OUT
WORK=$OUT/work.$$
mkdir -p $WORK

cleanup() {
  if (( ! KEEP_WORK )); then rm -rf $WORK; fi
}
trap cleanup EXIT

# ---- isolation: clone bundle, hdd, eeprom; scratch config -----------------
# Never execute from dist/xemu.app: the user launches that bundle, and a
# harness pointed there inherits their live config and takes their qcow2
# write lock (incident 9.3). APFS clones are instant and space-free.
APP=$WORK/xemu-bench.app
cp -Rc $APP_SRC $APP 2>/dev/null || cp -R $APP_SRC $APP
APPBIN=$APP/Contents/MacOS/xemu
[[ -x $APPBIN ]] || { echo "clone failed: no executable at $APPBIN" >&2; exit 1; }

# Source paths are read from the user's config, then cloned; the live
# files are never opened by xemu here.
if [[ -z $HDD_SRC ]]; then
  HDD_SRC=$(python3 - "$CONFIG_SRC" <<'EOF'
import re, sys
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"""\s*hdd_path\s*=\s*['"](.*)['"]\s*$""", line)
    if m:
        print(m.group(1)); break
EOF
)
fi
[[ -n $HDD_SRC && -f $HDD_SRC ]] || { echo "cannot resolve hdd qcow2 (config had '$HDD_SRC'); pass --hdd" >&2; exit 1; }
HDD=$WORK/hdd.qcow2
cp -c $HDD_SRC $HDD 2>/dev/null || { echo "note: cp -c clone failed (non-APFS?); plain copy" >&2; cp $HDD_SRC $HDD; }

EEPROM_SRC=$(python3 - "$CONFIG_SRC" <<'EOF'
import re, sys
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"""\s*eeprom_path\s*=\s*['"](.*)['"]\s*$""", line)
    if m:
        print(m.group(1)); break
EOF
)
EEPROM=""
if [[ -n $EEPROM_SRC && -f $EEPROM_SRC ]]; then
  EEPROM=$WORK/eeprom.bin
  cp -c $EEPROM_SRC $EEPROM 2>/dev/null || cp $EEPROM_SRC $EEPROM
fi

CONFIG=$WORK/xemu.toml
python3 - "$CONFIG_SRC" "$CONFIG" "$HDD" "$EEPROM" <<'EOF'
import re, sys
src, dst, hdd, eeprom = sys.argv[1:5]
out = []
for line in open(src, errors="replace"):
    if re.match(r"\s*hdd_path\s*=", line):
        line = "hdd_path = '%s'\n" % hdd
    elif eeprom and re.match(r"\s*eeprom_path\s*=", line):
        line = "eeprom_path = '%s'\n" % eeprom
    elif re.match(r"\s*fullscreen_on_startup\s*=", line):
        line = "fullscreen_on_startup = false\n"
    out.append(line)
open(dst, "w").writelines(out)
EOF

# ---- launch-path parity: canonical MVK_CONFIG_* (mirrors main()) ----------
export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export MVK_CONFIG_FAST_MATH_ENABLED=1
export MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0
export MVK_CONFIG_RESUME_LOST_DEVICE=1

# ---- binary identity (of what will actually run) --------------------------
GIT_DESC=$(git -C $ROOT describe --tags --match 'v*' --always --dirty 2>/dev/null || echo unknown)
BIN_SHA=$(shasum -a 256 $APPBIN | cut -d' ' -f1)
MVK_DYLIB=$(ls $APP/Contents/Libraries/*/libMoltenVK.dylib 2>/dev/null | head -1)
MVK_VER=$( [[ -n $MVK_DYLIB ]] && strings $MVK_DYLIB | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1 || echo unknown )

echo "bench: binary $GIT_DESC sha256 ${BIN_SHA[1,16]}... MoltenVK ${MVK_VER:-unknown}"
echo "bench: app clone $APP"
echo "bench: hdd clone $HDD (source $HDD_SRC untouched)"

typeset -a RUN_JSONS INTERLEAVE

# ---- one run ---------------------------------------------------------------
run_one() {
  local label=$1 val=$2 secs=$3 warmupflag=$4
  local log=$OUT/ab_$label.log cpul=$OUT/ab_$label.cpu
  # Socket lives in /tmp, NOT $WORK: unix socket paths are capped at
  # 104 bytes on macOS, and a deep outdir (session scratchpads) makes
  # xemu exit before the socket exists — every run reads as DEAD
  # "launch failed". Same trap class as the movement probe's sockets.
  local sock=/tmp/xemu-bench.$$.$label.sock
  local runjson=$OUT/run_$label.json
  RUN_JSONS+=($runjson)
  INTERLEAVE+=($label)
  rm -f $sock

  env $VAR=$val XEMU_NV2A_NSPROF=1 \
      $APPBIN -config_path $CONFIG -monitor unix:$sock,server,nowait \
      > $log 2>&1 &
  local pid=$!

  # Wait for the monitor socket instead of blind-sleeping; a dead
  # launch becomes a recorded DEAD run, not a batch abort.
  local up=0 i
  for i in $(seq 1 30); do
    if ! kill -0 $pid 2>/dev/null; then break; fi
    if [[ -S $sock ]]; then up=1; break; fi
    sleep 1
  done
  if (( ! up )); then
    kill -9 $pid 2>/dev/null || true
    python3 $RECEIPT_PY dead --label $label --log $log --out $runjson \
        --reason "launch failed (no monitor socket)" \
        ${warmupflag:+--warmup} --var-value $val || true
    return 0
  fi

  sleep $BOOT_WAIT   # boot to a loadvm-safe point (proven protocol value)

  local loadvm_ok=1
  python3 - "$sock" "$SNAP" >> $log 2>&1 <<'EOF' || loadvm_ok=0
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.5)
s.recv(4096)
s.sendall(("loadvm %s\n" % sys.argv[2]).encode())
time.sleep(3)
resp = s.recv(8192).decode(errors="replace")
s.close()
sys.stdout.write("monitor: " + resp.replace("\n", "\nmonitor: ") + "\n")
if "Error" in resp or "error" in resp:
    sys.exit(1)
EOF
  if (( ! loadvm_ok )); then
    kill $pid 2>/dev/null || true; sleep 1; kill -9 $pid 2>/dev/null || true
    python3 $RECEIPT_PY dead --label $label --log $log --out $runjson \
        --reason "loadvm failed (see log tail)" \
        ${warmupflag:+--warmup} --var-value $val || true
    return 0
  fi

  : > $cpul
  local j n
  n=$((secs / 5))
  for j in $(seq 1 $n); do
    sleep 5
    ps -o %cpu= -p $pid >> $cpul 2>/dev/null || break
    # Mid-run visual artifact check (white screen / stuck frame /
    # magenta tiles). Soft: SKIPs when uncapturable, never aborts.
    if (( SCREENSHOT )) && [[ $j -eq $(( (n + 1) / 2 )) ]]; then
      # Verdict goes to its own file: xemu's non-append stdout redirect
      # overwrites bytes appended to the shared log.
      python3 $SHOT_PY $pid $OUT/shot_$label.png \
          > $OUT/shot_$label.verdict 2>&1 || true
    fi
  done
  kill $pid 2>/dev/null || true
  sleep 2
  kill -9 $pid 2>/dev/null || true
  rm -f $sock

  # Scene-identity gate (incidents 9.1/9.2). A failed gate marks the
  # run invalid in its JSON; the receipt carries the verdict.
  python3 $RECEIPT_PY gate --label $label --log $log --out $runjson \
      --summarizer $SUMMARIZER --band $DRAWS_BAND --cv-max $CV_MAX \
      ${warmupflag:+--warmup} --var-value $val || true
  local shotline=""
  (( SCREENSHOT )) && \
    shotline=$(grep '^screenshot:' $OUT/shot_$label.verdict 2>/dev/null | tail -1)
  echo "$label done ($VAR=$val, asserts=$(grep -c Assertion $log || true))${shotline:+ [$shotline]}"
}

run_one warmup $E_VALUE 40 warmup
for (( k = 1; k <= PAIRS; k++ )); do
  run_one B$k 0 $SECS ""
  run_one E$k $E_VALUE $SECS ""
done

# ---- receipt ---------------------------------------------------------------
META=$OUT/meta.json
python3 - "$META" <<EOF
import json, os, platform, subprocess, sys
meta = {
  "config": {
    "snapshot": "$SNAP", "env_var": "$VAR", "pairs": int("$PAIRS"),
    "secs": int("$SECS"), "draws_band": "$DRAWS_BAND",
    "cv_max": float("$CV_MAX"), "boot_wait": int("$BOOT_WAIT"),
    "interleave": "${(j:,:)INTERLEAVE}".split(","),
  },
  "binary": {
    "git_describe": "$GIT_DESC", "binary_sha256": "$BIN_SHA",
    "moltenvk_version": "$MVK_VER", "app_clone": "$APP",
    "app_source": "$APP_SRC", "config_path": "$CONFIG",
    "config_source": "$CONFIG_SRC", "hdd_clone": "$HDD",
    "hdd_source": "$HDD_SRC",
  },
  "session": {
    "host": platform.node(), "os": platform.platform(),
    "uptime": subprocess.run(["uptime"], capture_output=True,
                             text=True).stdout.strip(),
  },
  "run_jsons": "${(j:,:)RUN_JSONS}".split(","),
}
json.dump(meta, open("$META", "w"), indent=2)
EOF

RC=0
python3 $RECEIPT_PY receipt --meta $META --out $OUT/receipt.json || RC=$?
echo "bench: receipt at $OUT/receipt.json"

# ---- visual-check summary --------------------------------------------------
if (( SCREENSHOT )); then
  echo "bench: visual checks:"
  grep -h '^screenshot:' $OUT/shot_*.verdict 2>/dev/null | sed 's/^/  /' || true
  if grep -hq '^screenshot: ARTIFACT' $OUT/shot_*.verdict 2>/dev/null; then
    echo "bench: VISUAL ARTIFACT flagged — review the shot_*.png above" >&2
    (( RC == 0 )) && RC=3
  fi
fi
exit $RC
