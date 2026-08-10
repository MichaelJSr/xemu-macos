#!/bin/zsh
# Counter-probe run: boot, loadvm one tag, dwell in-scene, clean monitor
# quit so every atexit dump (INV_PROF a-e, subpage-fast, ras) lands in
# run.log. Extra VAR=V args exported to xemu.
#   probe-run.sh <app> <outdir> <vm-tag> [dwell=80] [VAR=V ...]
set -e
APP_SRC=${1:?app}; OUT=${2:?outdir}; SNAP=${3:?vm-tag}; DWELL=${4:-80}
shift 4 2>/dev/null || shift 3
typeset -a EXTRA_ENV; EXTRA_ENV=("$@")
CONFIG_SRC=$HOME/Library/Application\ Support/xemu/xemu/xemu.toml
mkdir -p $OUT
WORK=$OUT/work.$$
mkdir -p $WORK
SOCK=/tmp/xpr-$$.sock
PID=""
cleanup() { [[ -n $PID ]] && kill -9 $PID 2>/dev/null; rm -rf $WORK; rm -f $SOCK; }
trap cleanup EXIT

APP=$WORK/xemu-probe.app; cp -Rc $APP_SRC $APP 2>/dev/null || cp -R $APP_SRC $APP
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

env "${EXTRA_ENV[@]}" XEMU_NV2A_NSPROF=1 \
    $APP/Contents/MacOS/xemu -config_path $CONFIG \
    -monitor unix:$SOCK,server,nowait > $OUT/run.log 2>&1 &
PID=$!
for i in $(seq 1 30); do kill -0 $PID 2>/dev/null || { echo "probe-run: died at launch"; exit 1; }; [[ -S $SOCK ]] && break; sleep 1; done
sleep 25

mon() {
python3 - "$SOCK" "$1" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.5)
s.recv(4096); s.sendall((sys.argv[2] + "\n").encode()); time.sleep(3)
print(s.recv(8192).decode(errors="replace")); s.close()
EOF
}

mon "loadvm $SNAP" > $OUT/loadvm.out 2>&1
grep -qi error $OUT/loadvm.out && { echo "probe-run: loadvm failed"; cat $OUT/loadvm.out; exit 1; }
echo "probe-run: in scene, dwelling ${DWELL}s (env: ${EXTRA_ENV[*]:-none})"
sleep $DWELL
mon quit > /dev/null 2>&1 || true
for i in $(seq 1 15); do kill -0 $PID 2>/dev/null || break; sleep 1; done
kill -0 $PID 2>/dev/null && { echo "probe-run: quit hung, killing (dumps lost)"; kill -9 $PID; }
PID=""
echo "---- atexit dumps ----"
grep -E "^xemu: " $OUT/run.log | grep -v "^xemu: nsprof" | head -40
