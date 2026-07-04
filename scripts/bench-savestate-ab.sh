#!/bin/zsh
# Reproducible in-game perf A/B for xemu.
#
#   scripts/bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir]
#
# Runs interleaved pairs (B: ENV_VAR=0, E: ENV_VAR=1) of a single
# binary, loading <snapshot> through the QEMU monitor after boot (CLI
# -loadvm loads before xemu attaches USB controllers and fails with a
# topology mismatch). Emits per-run nsprof logs + 5 s CPU samples to
# <outdir> (default ./bench-out). Requires the app at dist/xemu.app
# and a windowed config (fullscreen skews present pacing).
#
# Analysis: use only nsprof intervals with draws/flip > 100 (in-game),
# drop the first (load transient), and verify draws/flip matches
# across runs (same content). Baseline reproducibility measured at
# +/-0.02 fps with this protocol.
set -e
SNAP=${1:?usage: bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir]}
VAR=${2:?need env var name}
PAIRS=${3:-3}
SECS=${4:-75}
OUT=${5:-./bench-out}
ROOT=${0:a:h:h}
APPBIN=$ROOT/dist/xemu.app/Contents/MacOS
mkdir -p $OUT

run_one() {
  local label=$1 val=$2 secs=$3
  local log=$OUT/ab_$label.log cpul=$OUT/ab_$label.cpu
  local sock=/tmp/xemu-bench-mon.sock
  rm -f $sock
  env $VAR=$val XEMU_NV2A_NSPROF=1 \
      $APPBIN/xemu -monitor unix:$sock,server,nowait > $log 2>&1 &
  local pid=$!
  sleep 25
  python3 - "$sock" "$SNAP" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.5)
s.recv(4096)
s.sendall(("loadvm %s\n" % sys.argv[2]).encode())
time.sleep(3)
s.recv(8192)
s.close()
EOF
  : > $cpul
  local j n
  n=$((secs / 5))
  for j in $(seq 1 $n); do
    sleep 5
    ps -o %cpu= -p $pid >> $cpul 2>/dev/null || break
  done
  kill $pid 2>/dev/null || true
  sleep 2
  kill -9 $pid 2>/dev/null || true
  echo "$label done ($VAR=$val, asserts=$(grep -c Assertion $log || true))"
}

run_one warmup 1 40
for k in $(seq 1 $PAIRS); do
  run_one B$k 0 $SECS
  run_one E$k 1 $SECS
done
