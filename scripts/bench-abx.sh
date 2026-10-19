#!/bin/zsh
# Cross-binary / multi-variable interleaved savestate A/B for xemu.
#
#   scripts/bench-abx.sh <snapshot> [pairs] [secs] [outdir]
#                        --b-app PATH --e-app PATH [flags]
#
# Same protocol, gates and receipt engine as scripts/bench-savestate-ab.sh
# (scene-identity gate, dead-run records, protocol receipt), for the shapes
# that harness cannot hold: it pins B to VAR=0 and runs one bundle. Here
# each arm has its OWN bundle (--b-app / --e-app) and its own env SET
# (--b-env "A=1 B=2" / --e-env ...), so it covers
#   - cross-binary  (release vs release, before vs after a dependency bump)
#   - multi-variable arms (two knobs toggled together)
#   - pinned non-zero baselines (--b-env "KNOB=300" --e-env "KNOB=50")
# Point both --b-app and --e-app at one bundle for the last two.
#
# Isolation is the stock harness's: both bundles APFS-cloned into a work
# dir, the qcow2 and eeprom cloned once (the arms run sequentially),
# a scratch xemu.toml passed with -config_path, monitor socket in /tmp
# (104-byte unix-socket path cap). Never runs dist/xemu.app or the
# user's config/hdd in place. Shader and pipeline caches live in the
# bench cache (XEMU_BASE_PATH=$XEMU_BENCH_CACHE, default
# ~/Library/Caches/xemu-fork-bench), never the owner's pref dir; if
# either arm's binary predates XEMU_BASE_PATH, both get --isolate-caches.
# Every xemu runs under nice -n 10 with the argv marker
# `-name xemu-fork-auto`, one of XEMU_FORK_SLOTS (4) slots and a
# /tmp/xemu-leases lease (scripts/xemu-lease.zsh).
#
# Pair order alternates (B,E) / (E,B) to cancel monotone drift. Each arm
# gets its own 40 s warmup run.
#
# Flags:
#   --b-app PATH / --e-app PATH   bundles to clone (required)
#   --b-env "K=V ..." / --e-env   extra environment per arm
#   --isolate-caches   plant the scratch config as a portable marker in
#                      EACH cloned bundle, so every arm owns its shader and
#                      pipeline caches (cold at batch start). Use it
#                      whenever the arms differ in glslang or MoltenVK: a
#                      shared pipeline_cache.bin is rejected and rewritten
#                      on every arm switch.
#   --fullscreen       keep fullscreen_on_startup = true. macOS Game Mode
#                      then engages ~2 s after launch (verified 2026-10-10)
#                      and lifts xemu's CPU/GPU priority over background
#                      work, which is the only way to prioritize xemu
#                      without root. Any focus change (a click, another
#                      app activating) PAUSES it, so every run's gamepolicyd
#                      log is recorded (gm_<label>.txt) and pairs where Game
#                      Mode did not hold are reported separately. Mid-run
#                      screenshots are skipped: window captures of an active
#                      fullscreen Space come back black.
#   --config-src PATH  config to clone (default: $XEMU_TEST_CONFIG, else
#                      ../fixtures/xemu.toml beside the checkout)
#   --hdd PATH         qcow2 to clone (default: hdd_path from config-src)
#   --boot-wait N      seconds from launch to loadvm (default 25)
#   --draws-band LO[:HI] / --cv-max X   scene gate (defaults 100:inf, 0.10)
#   --no-screenshot    skip the mid-run visual check
#   --keep-work        keep the work dir afterwards
#   --dry-run          resolve and print both arms, launch nothing
#
# Exit: 0 valid receipt; 3 visual ARTIFACT flagged; otherwise the receipt
# engine's code (invalid / dead runs).
set -e

ROOT=${0:a:h:h}
source $ROOT/scripts/xemu-lease.zsh
RECEIPT_PY=$ROOT/scripts/bench-receipt.py
SHOT_PY=$ROOT/scripts/bench-screenshot.py
SUMMARIZER=$ROOT/scripts/nsprof_summarize.py

typeset -a pos
B_APP_SRC=""; E_APP_SRC=""; B_ENV=""; E_ENV=""
ISOLATE=0; FULLSCREEN=0; SCREENSHOT=1; KEEP_WORK=0; DRY_RUN=0
CONFIG_SRC=""; HDD_SRC=""; BOOT_WAIT=25; DRAWS_BAND=100; CV_MAX=0.10
while (( $# > 0 )); do
  case "$1" in
    --b-app)          B_APP_SRC=$2; shift ;;
    --e-app)          E_APP_SRC=$2; shift ;;
    --b-env)          B_ENV=$2; shift ;;
    --e-env)          E_ENV=$2; shift ;;
    --isolate-caches) ISOLATE=1 ;;
    --fullscreen)     FULLSCREEN=1 ;;
    --config-src)     CONFIG_SRC=$2; shift ;;
    --hdd)            HDD_SRC=$2; shift ;;
    --boot-wait)      BOOT_WAIT=$2; shift ;;
    --draws-band)     DRAWS_BAND=$2; shift ;;
    --cv-max)         CV_MAX=$2; shift ;;
    --no-screenshot)  SCREENSHOT=0 ;;
    --keep-work)      KEEP_WORK=1 ;;
    --dry-run)        DRY_RUN=1 ;;
    -h|--help)        awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
    --*)              echo "unknown flag: $1" >&2; exit 2 ;;
    *)                pos+=("$1") ;;
  esac
  shift
done

SNAP=${pos[1]:?usage: bench-abx.sh <snapshot> [pairs] [secs] [outdir] --b-app P --e-app P}
PAIRS=${pos[2]:-3}
SECS=${pos[3]:-75}
OUT=${pos[4]:-./bench-out-abx}
: ${CONFIG_SRC:=${XEMU_TEST_CONFIG:-${ROOT:h}/fixtures/xemu.toml}}

[[ -d $B_APP_SRC && -d $E_APP_SRC ]] || { echo "need --b-app and --e-app bundles" >&2; exit 2; }
[[ -f $CONFIG_SRC ]] || { echo "no config at $CONFIG_SRC (--config-src or XEMU_TEST_CONFIG)" >&2; exit 2; }
[[ -f $SUMMARIZER ]] || { echo "missing $SUMMARIZER (scene gate is mandatory)" >&2; exit 1; }
# The monitor only knows qcow2 tags; `loadvm F8` errors on a socket nobody
# reads and the guest keeps running the boot workload, which then gets
# benchmarked silently.
[[ $SNAP != [Ff][5-8] ]] || { echo "snapshot must be the qcow2 tag, not an F-key name" >&2; exit 2; }
(( FULLSCREEN )) && SCREENSHOT=0

read_toml_path() {
  python3 - "$CONFIG_SRC" "$1" <<'EOF'
import re, sys
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"""\s*%s\s*=\s*['"](.*)['"]\s*$""" % re.escape(sys.argv[2]), line)
    if m:
        print(m.group(1)); break
EOF
}

# ---- identity of what each arm will actually run ---------------------------
typeset -A IDENT_DESC IDENT_SHA IDENT_MVK ENVSET
for arm in B E; do
  vn=${arm}_APP_SRC; src=${(P)vn}
  bin=$src/Contents/MacOS/xemu
  [[ -x $bin ]] || { echo "no executable at $bin" >&2; exit 2; }
  IDENT_SHA[$arm]=$(shasum -a 256 $bin | cut -d' ' -f1)
  IDENT_MVK[$arm]=$(strings $src/Contents/Libraries/*/libMoltenVK.dylib 2>/dev/null \
                    | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
  # Local builds embed `git describe` (v0.13.2-56-g…-dirty); release
  # builds only the bare version, so fall back to the embedded commit.
  # Never run the binary for this: even -version reads the user's config.
  IDENT_DESC[$arm]=$(strings -a $bin | grep -E '^v[0-9]+\.[0-9]+' | head -1)
  [[ -z ${IDENT_DESC[$arm]} ]] && \
    IDENT_DESC[$arm]="commit $(strings -a $bin | grep -E '^[0-9a-f]{40}$' | head -1 | cut -c1-12)"
  vn=${arm}_ENV; ENVSET[$arm]=${(P)vn}
  echo "abx: arm $arm = ${IDENT_DESC[$arm]:-unknown} sha256 ${IDENT_SHA[$arm][1,16]}..." \
       "MoltenVK ${IDENT_MVK[$arm]:-unknown} env '${ENVSET[$arm]}' ($src)"
  # A binary without XEMU_BASE_PATH would write the owner's pref dir, so it
  # gets a cold cache in its clone; isolate both arms then, or the A/B
  # would compare a cold cache against the warm shared one too.
  if (( ! ISOLATE )) && ! grep -qa XEMU_BASE_PATH $bin; then
    ISOLATE=1
    echo "abx: arm $arm's binary predates XEMU_BASE_PATH; isolating both arms' caches"
  fi
done
echo "abx: snapshot $SNAP, $PAIRS pairs x $SECS s, cache isolation $( (( ISOLATE )) && echo per-arm || echo off)," \
     "fullscreen $( (( FULLSCREEN )) && echo on || echo off)"

# ---- quiet-machine report (warn, never block) ------------------------------
# A loaded box is legitimate for a best-effort run, but the receipt needs
# to say so; a leftover harness loop is not, it relaunches xemu mid-batch.
loops=$(pgrep -fl 'soak-cycles|probe-run|movement-probe|bench-savestate|bench-abx|boot-smoke' | grep -v "^$$ " || true)
builds=$(pgrep -fl 'ninja|clang' | grep -v clangd || true)
echo "abx: load $(sysctl -n vm.loadavg)"
[[ -n $loops ]] && { echo "abx: WARNING leftover harness loops running:" >&2; echo "$loops" | sed 's/^/  /' >&2; }
[[ -n $builds ]] && echo "abx: WARNING a build is running; CPU-bound scenes will not be self-consistent" >&2
(( DRY_RUN )) && { echo "abx: dry run, nothing launched"; exit 0; }

# ---- isolation -------------------------------------------------------------
mkdir -p $OUT
WORK=$OUT/work.$$
mkdir -p $WORK
cleanup() { xemu_auto_end; (( KEEP_WORK )) || rm -rf $WORK; }
trap cleanup EXIT

[[ -z $HDD_SRC ]] && HDD_SRC=$(read_toml_path hdd_path)
[[ -n $HDD_SRC && -f $HDD_SRC ]] || { echo "cannot resolve hdd qcow2 ('$HDD_SRC'); pass --hdd" >&2; exit 1; }
HDD=$WORK/hdd.qcow2
cp -c $HDD_SRC $HDD 2>/dev/null || cp $HDD_SRC $HDD
EEPROM_SRC=$(read_toml_path eeprom_path); EEPROM=""
if [[ -n $EEPROM_SRC && -f $EEPROM_SRC ]]; then
  EEPROM=$WORK/eeprom.bin
  cp -c $EEPROM_SRC $EEPROM 2>/dev/null || cp $EEPROM_SRC $EEPROM
fi
BASECONF=$WORK/xemu.toml
python3 - "$CONFIG_SRC" "$BASECONF" "$HDD" "$EEPROM" "$FULLSCREEN" <<'EOF'
import re, sys
src, dst, hdd, eeprom, fullscreen = sys.argv[1:6]
out = []
for line in open(src, errors="replace"):
    if re.match(r"\s*hdd_path\s*=", line):
        line = "hdd_path = '%s'\n" % hdd
    elif eeprom and re.match(r"\s*eeprom_path\s*=", line):
        line = "eeprom_path = '%s'\n" % eeprom
    elif re.match(r"\s*fullscreen_on_startup\s*=", line):
        line = "fullscreen_on_startup = %s\n" % ("true" if fullscreen == "1" else "false")
    out.append(line)
open(dst, "w").writelines(out)
EOF
# Mute the run, and stub the pads the savestates expect but this machine
# may lack (an absent bound controller): see scripts/harness-config.py.
PAD_ARGS=(${(f)"$(python3 -I ${0:a:h}/harness-config.py $BASECONF)"})

typeset -A APPBIN CONF BASE
for arm in B E; do
  vn=${arm}_APP_SRC
  app=$WORK/xemu-$arm.app
  cp -Rc ${(P)vn} $app 2>/dev/null || cp -R ${(P)vn} $app
  APPBIN[$arm]=$app/Contents/MacOS/xemu
  xemu_bench_cache ${APPBIN[$arm]} $ISOLATE
  BASE[$arm]=$XEMU_BENCH_BASE
  if (( XEMU_BENCH_PORTABLE )); then
    mkdir -p $app/Contents/Resources
    cp $BASECONF $app/Contents/Resources/xemu.toml
    CONF[$arm]=$app/Contents/Resources/xemu.toml
  else
    CONF[$arm]=$BASECONF
  fi
  echo "abx: arm $arm caches in ${BASE[$arm]}"
done

# ---- launch-path parity: canonical MVK_CONFIG_* (mirrors main()) ------------
export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export MVK_CONFIG_FAST_MATH_ENABLED=1
export MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0
export MVK_CONFIG_RESUME_LOST_DEVICE=1

typeset -a RUN_JSONS INTERLEAVE
# Each run holds a fork-xemu slot and a lease while its xemu lives.
run_one() { xemu_slot_acquire; run_one_xemu "$@"; xemu_auto_end; }
run_one_xemu() {
  local label=$1 arm=$2 secs=$3 warmupflag=$4
  local log=$OUT/ab_$label.log cpul=$OUT/ab_$label.cpu
  local sock=/tmp/xemu-abx.$$.$label.sock runjson=$OUT/run_$label.json
  RUN_JSONS+=($runjson); INTERLEAVE+=($label); rm -f $sock
  local t0=$(date '+%Y-%m-%d %H:%M:%S') tload="" t1=""
  echo "$(date +%H:%M:%S) $label start (load $(sysctl -n vm.loadavg | awk '{print $2}'))" >> $OUT/timeline.txt

  $XEMU_NICE env XEMU_BASE_PATH=${BASE[$arm]} ${=ENVSET[$arm]} XEMU_NV2A_NSPROF=1 \
      ${APPBIN[$arm]} $XEMU_MARK -config_path ${CONF[$arm]} "${PAD_ARGS[@]}" \
      -monitor unix:$sock,server,nowait > $log 2>&1 &
  local pid=$! up=0 i
  xemu_lease_write $pid "bench-abx $label (arm $arm) $SNAP ${secs}s" $(( BOOT_WAIT + secs + 45 ))
  for i in $(seq 1 30); do
    kill -0 $pid 2>/dev/null || break
    [[ -S $sock ]] && { up=1; break; }
    sleep 1
  done
  if (( ! up )); then
    kill -9 $pid 2>/dev/null || true
    python3 $RECEIPT_PY dead --label $label --log $log --out $runjson \
        --reason "launch failed (no monitor socket)" ${warmupflag:+--warmup} --var-value $arm || true
    return 0
  fi

  sleep $BOOT_WAIT
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
        --reason "loadvm failed (see log tail)" ${warmupflag:+--warmup} --var-value $arm || true
    return 0
  fi
  tload=$(date '+%Y-%m-%d %H:%M:%S')

  : > $cpul
  local j n=$((secs / 5))
  for j in $(seq 1 $n); do
    sleep 5
    ps -o %cpu= -p $pid >> $cpul 2>/dev/null || break
    if (( SCREENSHOT )) && [[ $j -eq $(( (n + 1) / 2 )) ]]; then
      # Own file: xemu's non-append stdout redirect overwrites bytes
      # appended to the shared log.
      python3 $SHOT_PY $pid $OUT/shot_$label.png > $OUT/shot_$label.verdict 2>&1 || true
    fi
  done
  local alive=1
  kill -0 $pid 2>/dev/null || { alive=0; echo "$(date +%H:%M:%S) $label: xemu exited early" >> $OUT/timeline.txt; }
  t1=$(date '+%Y-%m-%d %H:%M:%S')
  kill $pid 2>/dev/null || true
  sleep 2
  kill -9 $pid 2>/dev/null || true
  rm -f $sock
  # An early exit is a crash (or a quit): its truncated log can still
  # pass the scene gate, so the run is DEAD, not gated.
  if (( ! alive )); then
    python3 $RECEIPT_PY dead --label $label --log $log --out $runjson \
        --reason "xemu exited mid-run (crash? check DiagnosticReports)" \
        ${warmupflag:+--warmup} --var-value $arm || true
    echo "$label DEAD: xemu exited mid-run"
    return 0
  fi

  if (( FULLSCREEN )); then
    # /usr/bin/log explicitly: zsh has a `log` builtin.
    { echo "launch $t0 loadvm $tload end $t1"
      /usr/bin/log show --start "$t0" --end "$t1" --info --debug --style compact \
          --predicate 'process == "gamepolicyd" AND category == "Common"' 2>/dev/null \
        | grep -E "Game mode status is now|gaming session is now" || true
    } > $OUT/gm_$label.txt
  fi

  python3 $RECEIPT_PY gate --label $label --log $log --out $runjson \
      --summarizer $SUMMARIZER --band $DRAWS_BAND --cv-max $CV_MAX \
      ${warmupflag:+--warmup} --var-value $arm || true
  echo "$(date +%H:%M:%S) $label end (load $(sysctl -n vm.loadavg | awk '{print $2}'))" >> $OUT/timeline.txt
  local shotline=""
  (( SCREENSHOT )) && shotline=$(grep '^screenshot:' $OUT/shot_$label.verdict 2>/dev/null | tail -1)
  echo "$label done (arm $arm, asserts=$(grep -c Assertion $log || true))${shotline:+ [$shotline]}"
}

run_one warmupB B 40 warmup
run_one warmupE E 40 warmup
for (( k = 1; k <= PAIRS; k++ )); do
  if (( k % 2 )); then run_one B$k B $SECS ""; run_one E$k E $SECS ""
  else                 run_one E$k E $SECS ""; run_one B$k B $SECS ""; fi
done

# ---- receipt ---------------------------------------------------------------
# The Vulkan driver each arm ran, as the renderer logs it beside
# "Selected physical device" ("- Driver: MoltenVK, 1.4.3 (id 14)"); every
# distinct one, joined with " + " (the receipt refuses an arm that ran two).
typeset -A IDENT_DRV
for arm in B E; do
  IDENT_DRV[$arm]=$(grep -h '^- Driver: ' $OUT/ab_warmup$arm.log $OUT/ab_$arm<->.log(N) \
                    2>/dev/null | cut -c11- | sort -u |
                    awk 'NR > 1 { printf " + " } { printf "%s", $0 }')
done
META=$OUT/meta.json
python3 - "$META" <<EOF
import json, platform, subprocess
arm = lambda a, d, s, m, e, v: {"git_describe": d, "binary_sha256": s,
                                "moltenvk_version": m, "vk_driver": v,
                                "env": e, "app_source": a}
json.dump({
  "config": {
    "experiment": "cross-binary arms B vs E",
    "snapshot": "$SNAP", "env_var": "(per-arm env sets)", "pairs": int("$PAIRS"),
    "secs": int("$SECS"), "draws_band": "$DRAWS_BAND", "cv_max": float("$CV_MAX"),
    "boot_wait": int("$BOOT_WAIT"),
    "cache_isolation": "per-arm" if "$ISOLATE" == "1" else "off",
    "cache_base": {"B": "${BASE[B]}", "E": "${BASE[E]}"},
    "fullscreen": "$FULLSCREEN" == "1",
    "interleave": "${(j:,:)INTERLEAVE}".split(","),
  },
  "binary": {
    "arms": {
      "B": arm("$B_APP_SRC", "${IDENT_DESC[B]}", "${IDENT_SHA[B]}", "${IDENT_MVK[B]}", "${ENVSET[B]}",
               "${IDENT_DRV[B]:-unknown}"),
      "E": arm("$E_APP_SRC", "${IDENT_DESC[E]}", "${IDENT_SHA[E]}", "${IDENT_MVK[E]}", "${ENVSET[E]}",
               "${IDENT_DRV[E]:-unknown}"),
    },
    "app_clone": "$WORK/xemu-{B,E}.app", "config_path": "${CONF[E]}",
    "hdd_clone": "$HDD", "hdd_source": "$HDD_SRC",
  },
  "session": {
    "host": platform.node(), "os": platform.platform(),
    "uptime": subprocess.run(["uptime"], capture_output=True, text=True).stdout.strip(),
  },
  "run_jsons": "${(j:,:)RUN_JSONS}".split(","),
}, open("$META", "w"), indent=2)
EOF

RC=0
python3 $RECEIPT_PY receipt --meta $META --out $OUT/receipt.json || RC=$?
echo "abx: receipt at $OUT/receipt.json"

# ---- Game Mode continuity (fullscreen only) --------------------------------
# A run is clean when Game Mode was on at loadvm and nothing paused or
# ended it before the measurement stopped. Pairs with an unclean run are
# reported apart; the receipt above still covers every pair.
if (( FULLSCREEN )); then
  python3 - "$OUT" <<'EOF'
import glob, json, os, re, sys
out = sys.argv[1]
state_re = re.compile(r"Game mode status is now (\w+)")
clean = {}
for f in sorted(glob.glob(os.path.join(out, "gm_*.txt"))):
    label = os.path.basename(f)[3:-4]
    lines = open(f).read().splitlines()
    m = re.match(r"launch (\S+ \S+) loadvm (\S* ?\S*) end (\S+ \S+)", lines[0]) if lines else None
    if not m or not m.group(2).strip():
        clean[label] = (False, "no loadvm"); continue
    t_load, t_end = m.group(2), m.group(3)
    state, changes = None, []
    for l in lines[1:]:
        s = state_re.search(l)
        if not s:
            continue
        ts = l[:19]
        if ts <= t_load:
            state = s.group(1)
        elif ts < t_end:
            changes.append("%s %s" % (ts[11:], s.group(1)))
    ok = state == "on" and not changes
    clean[label] = (ok, "on at loadvm" if ok else "at loadvm=%s; %s" % (state, ", ".join(changes) or "-"))
print("abx: Game Mode continuity per run:")
for label, (ok, why) in sorted(clean.items()):
    print("  %-8s %s  %s" % (label, "clean" if ok else "PAUSED", why))
runs = {}
for f in glob.glob(os.path.join(out, "run_*.json")):
    r = json.load(open(f))
    if not r.get("warmup") and not r.get("dead"):
        runs[r["label"]] = r
ks = sorted({int(l[1:]) for l in runs if l[1:].isdigit()})
pairs = [(k, runs["B%d" % k]["fps_mean"], runs["E%d" % k]["fps_mean"]) for k in ks
         if "B%d" % k in runs and "E%d" % k in runs
         and clean.get("B%d" % k, (False,))[0] and clean.get("E%d" % k, (False,))[0]]
if not pairs:
    print("abx: no pair held Game Mode throughout")
else:
    mb = sum(p[1] for p in pairs) / len(pairs)
    me = sum(p[2] for p in pairs) / len(pairs)
    pos = sum(1 for p in pairs if p[2] > p[1])
    print("abx: Game-Mode-clean pairs %s: B %.2f vs E %.2f fps (%+.2f, %+.2f%%), sign +%d/-%d"
          % ([p[0] for p in pairs], mb, me, me - mb, (me - mb) / mb * 100, pos, len(pairs) - pos))
json.dump({k: {"clean": v[0], "detail": v[1]} for k, v in clean.items()},
          open(os.path.join(out, "gamemode.json"), "w"), indent=2)
EOF
fi

# ---- visual-check summary --------------------------------------------------
if (( SCREENSHOT )); then
  echo "abx: visual checks:"
  grep -h '^screenshot:' $OUT/shot_*.verdict 2>/dev/null | sed 's/^/  /' || true
  if grep -hq '^screenshot: ARTIFACT' $OUT/shot_*.verdict 2>/dev/null; then
    echo "abx: VISUAL ARTIFACT flagged; review the shot_*.png above" >&2
    (( RC == 0 )) && RC=3
  fi
fi
exit $RC
