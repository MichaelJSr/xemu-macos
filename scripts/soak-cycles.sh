#!/bin/zsh
# Arm (b) promotion cycle-soak: boot once, alternate loadvm f8/f5 with a
# dwell between loads, every would-skip store refute-validated (mode 2).
# Exercises xemu_sf_clear_all + mirror resync across tb_remove_all — the
# loadvm reload hazard class. Ends with a clean monitor `quit` so the
# atexit refute dump (total/VIOLATIONS) lands in run.log (SIGTERM would
# skip atexit and lose it).
#   soak-cycles.sh <app> <outdir> [cycles=3] [dwell=10]
set -e
APP_SRC=${1:?app}; OUT=${2:?outdir}; CYC=${3:-3}; DWELL=${4:-10}
CONFIG_SRC=$HOME/Library/Application\ Support/xemu/xemu/xemu.toml
F8=$(grep -E "^\s*f8\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
F5=$(grep -E "^\s*f5\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
[[ -n $F8 && -n $F5 ]] || { echo "soak-cycles: f5/f8 tags not found in toml"; exit 1; }
mkdir -p $OUT
WORK=$OUT/work.$$
mkdir -p $WORK
SOCK=/tmp/xsc-$$.sock
PID=""
cleanup() { [[ -n $PID ]] && kill -9 $PID 2>/dev/null; rm -rf $WORK; rm -f $SOCK; }
trap cleanup EXIT

APP=$WORK/xemu-soak.app; cp -Rc $APP_SRC $APP 2>/dev/null || cp -R $APP_SRC $APP
HDD_SRC=$(grep -E "^\s*hdd_path\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
HDD=$WORK/hdd.qcow2; cp -c $HDD_SRC $HDD 2>/dev/null || cp $HDD_SRC $HDD
EEPROM_SRC=$(grep -E "^\s*eeprom_path\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
EEPROM=""; [[ -f $EEPROM_SRC ]] && { EEPROM=$WORK/eeprom.bin; cp -c $EEPROM_SRC $EEPROM; }
CONFIG=$WORK/xemu.toml
python3 - "$CONFIG_SRC" "$CONFIG" "$HDD" "$EEPROM" <<'EOF'
import re, sys
src, dst, hdd, eeprom = sys.argv[1:5]
out = []
for line in open(src, errors="replace"):
    if re.match(r"\s*hdd_path\s*=", line): line = "hdd_path = '%s'\n" % hdd
    elif eeprom and re.match(r"\s*eeprom_path\s*=", line): line = "eeprom_path = '%s'\n" % eeprom
    elif re.match(r"\s*fullscreen_on_startup\s*=", line): line = "fullscreen_on_startup = false\n"
    out.append(line)
open(dst, "w").writelines(out)
EOF
export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0 MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export MVK_CONFIG_FAST_MATH_ENABLED=1 MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0 MVK_CONFIG_RESUME_LOST_DEVICE=1

env XEMU_SUBPAGE_FAST=1 XEMU_SUBPAGE_FAST_REFUTE=1 XEMU_NV2A_NSPROF=1 \
    $APP/Contents/MacOS/xemu -config_path $CONFIG \
    -monitor unix:$SOCK,server,nowait > $OUT/run.log 2>&1 &
PID=$!
for i in $(seq 1 30); do kill -0 $PID 2>/dev/null || { echo "soak-cycles: died at launch"; exit 1; }; [[ -S $SOCK ]] && break; sleep 1; done
sleep 25

mon() {
python3 - "$SOCK" "$1" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.5)
s.recv(4096); s.sendall((sys.argv[2] + "\n").encode()); time.sleep(3)
print(s.recv(8192).decode(errors="replace")); s.close()
EOF
}

for (( c = 1; c <= CYC; c++ )); do
  kill -0 $PID 2>/dev/null || { echo "soak-cycles: xemu died in cycle $c"; exit 1; }
  echo "cycle $c: loadvm f8 ($F8)"; mon "loadvm $F8" > $OUT/load_${c}_f8.out 2>&1
  grep -qi error $OUT/load_${c}_f8.out && { echo "soak-cycles: loadvm f8 failed"; exit 1; }
  sleep $DWELL
  echo "cycle $c: loadvm f5 ($F5)"; mon "loadvm $F5" > $OUT/load_${c}_f5.out 2>&1
  grep -qi error $OUT/load_${c}_f5.out && { echo "soak-cycles: loadvm f5 failed"; exit 1; }
  sleep $DWELL
done

mon quit > /dev/null 2>&1 || true
for i in $(seq 1 15); do kill -0 $PID 2>/dev/null || break; sleep 1; done
kill -0 $PID 2>/dev/null && { echo "soak-cycles: quit hung, killing"; kill -9 $PID; }
PID=""

echo "---- refute dump ----"
grep "subpage-fast" $OUT/run.log || echo "soak-cycles: NO REFUTE DUMP (fail)"
V=$(grep -o "VIOLATIONS=[0-9]*" $OUT/run.log | tail -1 | cut -d= -f2)
[[ -z $V ]] && { echo "soak-cycles: no violation counter"; exit 1; }
[[ $V != 0 ]] && { echo "soak-cycles: $V VIOLATIONS"; exit 3; }
echo "soak-cycles: CLEAN ($CYC cycles x 2 loadvms, 0 violations)"
