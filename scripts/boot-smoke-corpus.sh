#!/bin/zsh
# Boot-smoke a set of titles on an isolated clone of a bundle — the
# multi-title soak gate for renderer/APU changes that Azurik alone cannot
# exercise (e.g. release-mode bounds checks over state tables, PVIDEO).
#
#   scripts/boot-smoke-corpus.sh [--app PATH] [--secs N] [--out DIR] \
#       [--config-src PATH] [--no-input] [--keep-work] <iso>...
#
# For each ISO: clone the bundle (APFS), clone the user's hdd/eeprom, write
# a scratch xemu.toml with dvd_path=<iso> (fullscreen off), launch with a
# monitor socket + XEMU_INPUT_PIPE, boot for --secs seconds while tapping
# START and A through the user's own keyboard bindings (so menus advance),
# capture two scored screenshots, then quit via the monitor. Reports per
# title: alive/CRASHED (exit status if it died early), assert/abort lines,
# the last nsprof interval (flips, draws/flip) and the screenshot verdicts.
# Exit code: 0 all titles alive + no ARTIFACT; 2 a title died or asserted;
# 3 an ARTIFACT verdict; 1 usage/plumbing.
#
# Isolation rules are the harness's (scripts/bench-savestate-ab.sh): never
# points at dist/xemu.app's config, never opens the user's qcow2.

set -u
ROOT=${0:A:h:h}
APP_SRC=$ROOT/dist/xemu.app
CONFIG_SRC="$HOME/Library/Application Support/xemu/xemu/xemu.toml"
SECS=120
OUT=./boot-smoke-out
INPUT=1
KEEP_WORK=0
SHOT_PY=$ROOT/scripts/bench-screenshot.py
typeset -a ISOS

while (( $# )); do
  case $1 in
    --app) APP_SRC=$2; shift 2 ;;
    --secs) SECS=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --config-src) CONFIG_SRC=$2; shift 2 ;;
    --no-input) INPUT=0; shift ;;
    --keep-work) KEEP_WORK=1; shift ;;
    -h|--help) sed -n 2,22p $0; exit 0 ;;
    *) ISOS+=($1); shift ;;
  esac
done
(( ${#ISOS} )) || { echo "usage: $0 [opts] <iso>..." >&2; exit 1; }
for iso in $ISOS; do [[ -f $iso ]] || { echo "no such iso: $iso" >&2; exit 1; }; done
[[ -f $CONFIG_SRC ]] || { echo "config not found: $CONFIG_SRC" >&2; exit 1; }
[[ -d $APP_SRC ]] || { echo "bundle not found: $APP_SRC" >&2; exit 1; }
if [[ $APP_SRC:A == $ROOT/dist/xemu.app ]]; then :; fi   # cloned below, never run in place

mkdir -p $OUT
OUT=$OUT:A
WORK=$(mktemp -d /tmp/xemu-bootsmoke.XXXXXX)
cleanup() { (( KEEP_WORK )) || rm -rf $WORK; }
trap cleanup EXIT

APP=$WORK/xemu-smoke.app
cp -Rc $APP_SRC $APP 2>/dev/null || cp -R $APP_SRC $APP
APPBIN=$APP/Contents/MacOS/xemu
[[ -x $APPBIN ]] || { echo "clone failed: $APPBIN" >&2; exit 1; }

read_toml() {   # read_toml <file> <key>
  python3 - "$1" "$2" <<'EOF'
import re, sys
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"""\s*%s\s*=\s*['"]?([^'"\n]*)['"]?\s*$""" % re.escape(sys.argv[2]), line)
    if m:
        print(m.group(1).strip()); break
EOF
}

HDD_SRC=$(read_toml "$CONFIG_SRC" hdd_path)
[[ -n $HDD_SRC && -f $HDD_SRC ]] || { echo "cannot resolve hdd_path from config" >&2; exit 1; }
HDD=$WORK/hdd.qcow2
cp -c $HDD_SRC $HDD 2>/dev/null || cp $HDD_SRC $HDD
EEPROM_SRC=$(read_toml "$CONFIG_SRC" eeprom_path)
EEPROM=""
if [[ -n $EEPROM_SRC && -f $EEPROM_SRC ]]; then
  EEPROM=$WORK/eeprom.bin; cp -c $EEPROM_SRC $EEPROM 2>/dev/null || cp $EEPROM_SRC $EEPROM
fi
SC_START=$(read_toml "$CONFIG_SRC" start); SC_A=$(read_toml "$CONFIG_SRC" a)
SC_START=${SC_START:-42}; SC_A=${SC_A:-81}

export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export MVK_CONFIG_FAST_MATH_ENABLED=1
export MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0
export MVK_CONFIG_RESUME_LOST_DEVICE=1

GIT_DESC=$(git -C $ROOT describe --tags --match 'v*' --always --dirty 2>/dev/null || echo unknown)
BIN_SHA=$(shasum -a 256 $APPBIN | cut -d' ' -f1)
echo "smoke: binary $GIT_DESC sha256 ${BIN_SHA[1,16]}... app clone $APP"
echo "smoke: hdd clone $HDD (source untouched); input taps START=$SC_START A=$SC_A every 6 s: $INPUT"

tap() {   # tap <fifo> <scancode>
  python3 - "$1" "$2" <<'EOF' 2>/dev/null || true
import os, sys, time
try:
    fd = os.open(sys.argv[1], os.O_WRONLY | os.O_NONBLOCK)
except OSError:
    sys.exit(0)   # no reader yet
os.write(fd, ("down %s\n" % sys.argv[2]).encode()); time.sleep(0.12)
os.write(fd, ("up %s\n" % sys.argv[2]).encode()); os.close(fd)
EOF
}

RC=0
for iso in $ISOS; do
  name=${${iso:t}%%[ .(]*}
  name=${name:l}
  log=$OUT/boot_$name.log
  sock=/tmp/xemu-smoke.$$.$name.sock
  fifo=/tmp/xemu-smoke.$$.$name.fifo
  rm -f $sock $fifo; mkfifo $fifo
  cfg=$WORK/xemu-$name.toml
  python3 - "$CONFIG_SRC" "$cfg" "$HDD" "$EEPROM" "$iso" <<'EOF'
import re, sys
src, dst, hdd, eeprom, iso = sys.argv[1:6]
out = []
for line in open(src, errors="replace"):
    if re.match(r"\s*hdd_path\s*=", line): line = "hdd_path = '%s'\n" % hdd
    elif eeprom and re.match(r"\s*eeprom_path\s*=", line): line = "eeprom_path = '%s'\n" % eeprom
    elif re.match(r"\s*dvd_path\s*=", line): line = "dvd_path = '%s'\n" % iso
    elif re.match(r"\s*fullscreen_on_startup\s*=", line): line = "fullscreen_on_startup = false\n"
    out.append(line)
open(dst, "w").writelines(out)
EOF
  env XEMU_NV2A_NSPROF=1 XEMU_INPUT_PIPE=$fifo \
      $APPBIN -config_path $cfg -monitor unix:$sock,server,nowait > $log 2>&1 &
  pid=$!
  up=0
  for i in $(seq 1 30); do
    kill -0 $pid 2>/dev/null || break
    [[ -S $sock ]] && { up=1; break; }
    sleep 1
  done
  if (( ! up )); then
    wait $pid; st=$?
    echo "$name: CRASHED at launch (exit $st) — see $log"; RC=2
    rm -f $sock $fifo; continue
  fi
  n=$(( SECS / 2 )); alive=1; st=0
  for j in $(seq 1 $n); do
    sleep 2
    if ! kill -0 $pid 2>/dev/null; then wait $pid; st=$?; alive=0; break; fi
    if (( INPUT )) && (( j >= 10 )) && (( j % 3 == 0 )); then
      if (( (j / 3) % 2 )); then tap $fifo $SC_START; else tap $fifo $SC_A; fi
    fi
    if [[ $j -eq $(( n / 3 )) || $j -eq $(( 2 * n / 3 )) ]]; then
      python3 $SHOT_PY $pid $OUT/shot_${name}_$j.png > $OUT/shot_${name}_$j.verdict 2>&1 || true
    fi
  done
  if (( alive )); then
    python3 - "$sock" <<'EOF' >/dev/null 2>&1 || true
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
try:
    s.connect(sys.argv[1]); time.sleep(0.3); s.recv(4096); s.sendall(b"quit\n"); time.sleep(1)
except Exception: pass
s.close()
EOF
    sleep 2; kill $pid 2>/dev/null; sleep 1; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
  fi
  rm -f $sock $fifo
  asserts=$(grep -cE 'Assertion|abort\(\)|nv2a_vk_bounds|Segmentation|Bus error|Illegal instruction|SIG(SEGV|ILL|BUS|ABRT)' $log 2>/dev/null || true)
  lastint=$(grep -E 'nsprof: .*interval' $log | tail -1 | sed -E 's/.*interval, //')
  lastdraws=$(grep -E 'nsprof:   draws ' $log | tail -1 | awk '{print $NF}')
  shots=$(grep -h '^screenshot:' $OUT/shot_${name}_*.verdict 2>/dev/null | awk '{print $2}' | tr '\n' ',' | sed 's/,$//')
  if (( ! alive )); then
    echo "$name: CRASHED after $((j*2)) s (exit $st) asserts=$asserts last=[$lastint draws/flip=${lastdraws:-?}] shots=[$shots]"; RC=2
  elif (( asserts > 0 )); then
    echo "$name: alive but asserts=$asserts last=[$lastint draws/flip=${lastdraws:-?}] shots=[$shots]"; RC=2
  else
    echo "$name: alive ${SECS}s asserts=0 last=[$lastint draws/flip=${lastdraws:-?}] shots=[$shots]"
    [[ $shots == *ARTIFACT* ]] && (( RC == 0 )) && RC=3
  fi
done
echo "smoke: done, rc=$RC, logs in $OUT"
exit $RC
