# Sourced by the zsh benches: how an automated xemu launch identifies
# itself. Python twin and the full contract: scripts/xemu_lease.py (keep
# the two in step).
#
#   $XEMU_NICE  (nice -n 10)               prefix of every automated launch
#   $XEMU_MARK  (-name xemu-fork-auto)     first arguments after the binary
#   xemu_slot_acquire                      wait for one of $XEMU_FORK_SLOTS (4)
#   xemu_lease_write <pid> <purpose> <s>   lease for the xemu just started
#   xemu_auto_end                          drop that lease, free the slot
#   xemu_bench_cache <xemu binary> <isolate 0|1>
#       sets XEMU_BENCH_BASE, the data base path the run passes as
#       XEMU_BASE_PATH, and XEMU_BENCH_PORTABLE=1 when the caller must
#       plant its scratch config in the clone's Contents/Resources: for
#       --isolate-caches, and for a binary that predates XEMU_BASE_PATH
#       (it would otherwise write the owner's pref dir)

zmodload zsh/system zsh/datetime
setopt no_bg_nice   # background launches get exactly nice 10, not zsh's +5 on top
XEMU_LEASE_DIR=/tmp/xemu-leases
XEMU_NICE=(nice -n 10)
XEMU_MARK=(-name xemu-fork-auto)
typeset -g XEMU_SLOT="" XEMU_SLOT_FD="" XEMU_LEASE_PID="" XEMU_BENCH_BASE="" XEMU_BENCH_PORTABLE=0

xemu_leases_sweep() {   # stale = older than 10 s and its pid no longer a marked xemu
  local f c rc   # every step tolerates failure: the benches run under set -e
  for f in $XEMU_LEASE_DIR/<->.json(Nms+10); do
    rc=0; c=$(ps -o command= -p ${${f:t}%.json} 2>/dev/null) || rc=$?
    (( rc > 1 )) && continue   # ps itself failed: can't tell, keep it
    [[ $c == *"-name xemu-fork-auto"* ]] || rm -f $f
  done
  return 0
}

xemu_slot_acquire() {
  local n=${XEMU_FORK_SLOTS:-4} i fd t0=$EPOCHSECONDS said=-1
  [[ -n $XEMU_SLOT_FD ]] && return 0
  [[ $n == <1-> ]] || n=4
  mkdir -p $XEMU_LEASE_DIR/slots
  xemu_leases_sweep
  while :; do
    for (( i = 0; i < n; i++ )); do
      [[ -e $XEMU_LEASE_DIR/slots/$i.lock ]] || : >> $XEMU_LEASE_DIR/slots/$i.lock
      if zsystem flock -t 0 -f fd $XEMU_LEASE_DIR/slots/$i.lock 2>/dev/null; then
        XEMU_SLOT=$i XEMU_SLOT_FD=$fd
        (( said >= 0 )) && echo "lease: slot $i free after $(( EPOCHSECONDS - t0 )) s" >&2
        return 0
      fi
    done
    if (( said < 0 || EPOCHSECONDS - t0 - said >= 60 )); then
      echo "lease: all $n fork-xemu slots busy (XEMU_FORK_SLOTS); waiting" >&2
      said=$(( EPOCHSECONDS - t0 ))
    fi
    sleep 2
  done
}

xemu_lease_write() {   # <pid> <purpose> <expected seconds>
  local pid=$1 purpose=${2//[\"\\]/} secs=${3:-600} now=$EPOCHSECONDS f
  local fmt='%Y-%m-%dT%H:%M:%S%z' s e
  strftime -s s $fmt $now; strftime -s e $fmt $(( now + secs ))
  mkdir -p $XEMU_LEASE_DIR
  f=$XEMU_LEASE_DIR/$pid.json
  print -r -- "{\"pid\": $pid, \"who\": \"${ZSH_ARGZERO:t}\", \"purpose\": \"$purpose\"," \
    "\"started\": \"$s\", \"expected_end\": \"$e\", \"started_epoch\": $now," \
    "\"expected_end_epoch\": $(( now + secs )), \"slot\": ${XEMU_SLOT:-null}," \
    "\"launcher_pid\": $$, \"marker\": \"xemu-fork-auto\"}" > $f.$$.tmp && mv -f $f.$$.tmp $f
  XEMU_LEASE_PID=$pid
}

xemu_auto_end() {
  [[ -n $XEMU_LEASE_PID ]] && rm -f $XEMU_LEASE_DIR/$XEMU_LEASE_PID.json
  [[ -n $XEMU_SLOT_FD ]] && { zsystem flock -u $XEMU_SLOT_FD 2>/dev/null || true; }
  XEMU_LEASE_PID="" XEMU_SLOT="" XEMU_SLOT_FD=""
  return 0
}

xemu_bench_cache() {   # <xemu binary> <isolate 0|1>
  if (( $2 )) || ! grep -qa XEMU_BASE_PATH $1; then
    XEMU_BENCH_PORTABLE=1 XEMU_BENCH_BASE=${1:h:h}/Resources/
  else
    XEMU_BENCH_PORTABLE=0 XEMU_BENCH_BASE=${XEMU_BENCH_CACHE:-$HOME/Library/Caches/xemu-fork-bench}/
    mkdir -p $XEMU_BENCH_BASE
  fi
}
