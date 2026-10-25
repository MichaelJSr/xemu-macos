#!/bin/zsh
# Counter-probe run: boot, loadvm each tag, dwell in-scene after each load,
# clean monitor quit so every atexit dump (INV_PROF a-e, subpage-fast, ras)
# lands in run.log. Extra VAR=V args exported to xemu. Caches in the bench
# cache; nice, marker, slot and lease per scripts/xemu-lease.zsh.
#   [PROBE_CYCLES=N] probe-run.sh <app> <outdir> <tag[,tag2,...]> [dwell=80] [VAR=V ...]
# PROBE_CYCLES (default 1) repeats the loads: each cycle loads every tag in
# order. Before each load xemu must still run, and a load whose reply says
# error ends the run (exit 1); each load's reply is in load_<cycle>_<i>.out.
# A loadvm soak with the sub-page refuter (the old soak-cycles.sh):
#   PROBE_CYCLES=3 probe-run.sh APP OUT azurik-heavy-f1,vm-20260704032357 10 XEMU_SUBPAGE_FAST_REFUTE=1
# Refuters (an XEMU_*REFUTE*=1 argument): a quit that hangs exits 1 (the
# dump is lost), and so does a run.log without a VIOLATIONS= line when the
# subpage-fast, xpage or sub-page refuter is armed (they print that field;
# the reval, vtx-span, x87, tlb-wclean and push-present refuters don't:
# read their exit lines). Any nonzero VIOLATIONS= in run.log exits 3.
set -e
source ${0:a:h}/xemu-lease.zsh
APP_SRC=${1:?app}; OUT=${2:?outdir}; TAGS=(${(s:,:)${3:?tag[,tag2,...]}}); DWELL=${4:-80}
shift 4 2>/dev/null || shift 3
typeset -a EXTRA_ENV; EXTRA_ENV=("$@")
CYC=${PROBE_CYCLES:-1}
[[ $CYC == <1-> ]] || { echo "probe-run: PROBE_CYCLES must be a positive integer" >&2; exit 2; }
REFUTE=0 VREFUTE=0
for kv in $EXTRA_ENV; do
  [[ $kv == XEMU_*REFUTE*=1 ]] && REFUTE=1
  [[ $kv == XEMU_(SUBPAGE_FAST|XPAGE|SUBPAGE)_REFUTE=1 ]] && VREFUTE=1
done
CONFIG_SRC=${XEMU_TEST_CONFIG:-${0:a:h:h:h}/fixtures/xemu.toml}
[[ -f $CONFIG_SRC ]] || { echo "probe-run: no test config at $CONFIG_SRC (set XEMU_TEST_CONFIG)" >&2; exit 1; }
mkdir -p $OUT
WORK=$OUT/work.$$
mkdir -p $WORK
SOCK=/tmp/xpr-$$.sock
PID=""
# (|| true: under set -e a failed kill of a dead xemu ended the trap early,
# leaving the work dir and its clones behind)
cleanup() { [[ -n $PID ]] && kill -9 $PID 2>/dev/null || true; xemu_auto_end; rm -rf $WORK; rm -f $SOCK; }
trap cleanup EXIT

APP=$WORK/xemu-probe.app; cp -Rc $APP_SRC $APP 2>/dev/null || cp -R $APP_SRC $APP
HDD_SRC=$(grep -E "^\s*hdd_path\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
HDD=$WORK/hdd.qcow2; cp -c $HDD_SRC $HDD 2>/dev/null || cp $HDD_SRC $HDD
EEPROM_SRC=$(grep -E "^\s*eeprom_path\s*=" $CONFIG_SRC | sed -E "s/.*'(.*)'.*/\1/")
EEPROM=""; [[ -f $EEPROM_SRC ]] && { EEPROM=$WORK/eeprom.bin; cp -c $EEPROM_SRC $EEPROM; }
CONFIG=$WORK/xemu.toml; cp $CONFIG_SRC $CONFIG
# The clones, screenshots in $WORK, fullscreen off, muted, pad stubs:
# see scripts/harness-config.py.
PAD_ARGS=(${(f)"$(python3 -I ${0:a:h}/harness-config.py $CONFIG --workdir $WORK --hdd $HDD ${EEPROM:+--eeprom} $EEPROM)"})
export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0 MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
export MVK_CONFIG_FAST_MATH_ENABLED=1 MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0 MVK_CONFIG_RESUME_LOST_DEVICE=1

xemu_bench_cache $APP/Contents/MacOS/xemu 0
(( XEMU_BENCH_PORTABLE )) && { cp $CONFIG $APP/Contents/Resources/xemu.toml; CONFIG=$APP/Contents/Resources/xemu.toml; }
xemu_slot_acquire
$XEMU_NICE env XEMU_BASE_PATH=$XEMU_BENCH_BASE "${EXTRA_ENV[@]}" XEMU_NV2A_NSPROF=1 \
    $APP/Contents/MacOS/xemu $XEMU_MARK -config_path $CONFIG "${PAD_ARGS[@]}" \
    -monitor unix:$SOCK,server,nowait > $OUT/run.log 2>&1 &
PID=$!
xemu_lease_write $PID "probe-run ${(j:,:)TAGS} x$CYC ${DWELL}s" $(( 60 + CYC * ${#TAGS} * (DWELL + 4) ))
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

echo "probe-run: ${#TAGS} tag(s) x $CYC cycle(s), dwelling ${DWELL}s after each load (env: ${EXTRA_ENV[*]:-none})"
for (( c = 1; c <= CYC; c++ )); do
  for (( i = 1; i <= ${#TAGS}; i++ )); do
    kill -0 $PID 2>/dev/null || { echo "probe-run: xemu died in cycle $c"; exit 1; }
    # (a just-killed xemu can still pass kill -0 until it is reaped)
    mon "loadvm ${TAGS[i]}" > $OUT/load_${c}_${i}.out 2>&1 ||
      { echo "probe-run: xemu died in cycle $c (no monitor for loadvm ${TAGS[i]})"; exit 1; }
    grep -qi error $OUT/load_${c}_${i}.out && { echo "probe-run: loadvm ${TAGS[i]} failed (cycle $c)"; cat $OUT/load_${c}_${i}.out; exit 1; }
    echo "probe-run: cycle $c: ${TAGS[i]} loaded"
    sleep $DWELL
  done
done
kill -0 $PID 2>/dev/null || { echo "probe-run: xemu died before the quit"; exit 1; }
mon quit > /dev/null 2>&1 || true
for i in $(seq 1 15); do kill -0 $PID 2>/dev/null || break; sleep 1; done
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID
  (( REFUTE )) && { echo "probe-run: quit hung with a refuter armed, killed (no dump): FAIL"; exit 1; }
  echo "probe-run: quit hung, killing (dumps lost)"
fi
PID=""
xemu_auto_end
echo "---- atexit dumps ----"
grep -E "^xemu: " $OUT/run.log | grep -v "^xemu: nsprof" | head -40 || true
(( VREFUTE )) && ! grep -q 'VIOLATIONS=' $OUT/run.log && { echo "probe-run: a refuter was armed but run.log has no VIOLATIONS= line"; exit 1; }
V=$(grep -o 'VIOLATIONS=[0-9]*' $OUT/run.log | grep -v '=0$' | head -1 || true)
[[ -n $V ]] && { echo "probe-run: $V in run.log"; exit 3; }
(( VREFUTE )) && echo "probe-run: CLEAN (${#TAGS} tag(s) x $CYC cycle(s), every VIOLATIONS= is 0)"
exit 0
