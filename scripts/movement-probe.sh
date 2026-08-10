#!/bin/zsh
# Movement-phase transition probe — the first-visit streaming lag class.
#
#   scripts/movement-probe.sh <outdir> [VAR=V ...]
#
# One run: boot -> monitor loadvm F8 -> settle 15 s -> hold FORWARD 20 s
# into unexplored space (first-visit streaming) -> rest 5 s -> hold
# BACKWARD 20 s through the just-explored route (assets resident = the
# built-in control) -> screenshot -> quit.
#
# Static savestate A/Bs structurally cannot see first-visit costs (the
# v0.11 new-area-lag shipped exactly that way), so this probe is the
# REQUIRED validation class for changes touching TB lifetime or
# invalidation. Metrics:
#   - per-phase `info jit` "TB flush count" deltas (the mechanism
#     signal: 139/20 s forward = the v0.11 bug; ~0 = healthy)
#   - nsprof intervals on stdout (worst forward-phase fps vs settle)
#   - phase timestamps in <outdir>/phases.log for interval attribution
#   - a mid-forward screenshot scored by bench-screenshot.py
#
# Extra env (e.g. XEMU_SUPERBLOCK=4) is passed through to xemu.
# Isolation matches bench-savestate-ab.sh: bundle/hdd/eeprom cloned,
# scratch config via -config_path, fullscreen forced off, user files
# only ever clone sources. Monitor socket + input FIFO live in /tmp
# (104-byte AF_UNIX path cap).
set -e

ROOT=${0:a:h:h}
SHOT_PY=$ROOT/scripts/bench-screenshot.py
SNAP=${MOVEMENT_SNAP:-vm-20260704173701}
FWD_SC=${MOVEMENT_FWD_SC:-26}    # W = left-stick up (owner bindings)
BWD_SC=${MOVEMENT_BWD_SC:-22}    # S = left-stick down

OUT=${1:?usage: movement-probe.sh <outdir> [VAR=V ...]}
shift
typeset -a EXTRA_ENV
EXTRA_ENV=("$@")

mkdir -p $OUT
WORK=$OUT/work.$$
mkdir -p $WORK
SOCK=/tmp/xmp-$$.sock
FIFO=/tmp/xmp-$$.fifo
rm -f $SOCK $FIFO
mkfifo $FIFO

cleanup() {
  [[ -n $PID ]] && kill -9 $PID 2>/dev/null || true
  rm -f $SOCK $FIFO
  rm -rf $WORK
}
trap cleanup EXIT

# ---- isolation (clone sources, never the live files) ----------------------
APP_SRC=$ROOT/dist/xemu.app
CONFIG_SRC=$HOME/Library/Application\ Support/xemu/xemu/xemu.toml
APP=$WORK/xemu-probe.app
cp -Rc $APP_SRC $APP 2>/dev/null || cp -R $APP_SRC $APP
APPBIN=$APP/Contents/MacOS/xemu

HDD_SRC=$(grep -E "^\s*hdd_path\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
[[ -f $HDD_SRC ]] || { echo "cannot resolve hdd qcow2" >&2; exit 1; }
HDD=$WORK/hdd.qcow2
cp -c $HDD_SRC $HDD 2>/dev/null || cp $HDD_SRC $HDD

EEPROM_SRC=$(grep -E "^\s*eeprom_path\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
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

export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export MVK_CONFIG_FAST_MATH_ENABLED=1
export MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0
export MVK_CONFIG_RESUME_LOST_DEVICE=1

LOG=$OUT/run.log
PHASES=$OUT/phases.log
: > $PHASES

env "${EXTRA_ENV[@]}" XEMU_NV2A_NSPROF=1 XEMU_INPUT_PIPE=$FIFO \
    $APPBIN -config_path $CONFIG -monitor unix:$SOCK,server,nowait \
    > $LOG 2>&1 &
PID=$!

for i in $(seq 1 30); do
  kill -0 $PID 2>/dev/null || { echo "probe: xemu died at launch" >&2; exit 1; }
  [[ -S $SOCK ]] && break
  sleep 1
done
sleep 25

mon() {
  python3 - "$SOCK" "$1" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3)
s.recv(4096)
s.sendall((sys.argv[2] + "\n").encode())
time.sleep(1.0)
print(s.recv(65536).decode(errors="replace"))
s.close()
EOF
}

flushes() {
  mon "info jit" | grep -i "TB flush count" | grep -oE '[0-9]+' | tail -1
}

phase() {  # phase <name>
  echo "phase $1 t=$(date +%s) tb_flush=$(flushes)" | tee -a $PHASES
}

inject() {  # inject <down|up|clear> [scancode] — liveness-guarded
  kill -0 $PID 2>/dev/null || { echo "probe: xemu dead, skip inject" >&2; return 1; }
  if [[ $1 == clear ]]; then print "clear" > $FIFO
  else print "$1 $2" > $FIFO
  fi
}

mon "loadvm $SNAP" > $OUT/loadvm.out 2>&1
grep -qi error $OUT/loadvm.out && { echo "probe: loadvm failed:"; cat $OUT/loadvm.out; exit 1; } >&2

phase settle-begin
sleep 15
phase forward-begin
inject down $FWD_SC
sleep 10
python3 $SHOT_PY $PID $OUT/shot_forward.png > $OUT/shot_forward.verdict 2>&1 || true
sleep 10
inject up $FWD_SC
phase forward-end
sleep 5
phase backward-begin
inject down $BWD_SC
sleep 20
inject up $BWD_SC
phase backward-end
inject clear || true

kill $PID 2>/dev/null || true
sleep 2
kill -9 $PID 2>/dev/null || true
PID=""

echo "probe: phases:"
cat $PHASES
echo "probe: forward screenshot: $(cat $OUT/shot_forward.verdict 2>/dev/null || echo none)"
echo "probe: nsprof log at $LOG (map intervals onto phase timestamps)"
