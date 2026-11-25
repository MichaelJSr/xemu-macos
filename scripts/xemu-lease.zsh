# Sourced by the zsh benches: how an automated xemu launch identifies
# itself. Python twin and the full contract: scripts/xemu_lease.py (the
# AUTOMATION_ENV list lives there only).
#
#   $XEMU_NICE  (nice -n 10)               prefix of every automated launch
#   $XEMU_MARK  (-name xemu-fork-auto)     first arguments after the binary
#   $XEMU_LEASE_DIR                        lease folder: as inherited, else /tmp/xemu-leases
#   xemu_slot_acquire                      wait for one of $XEMU_FORK_SLOTS (4):
#                                          flock(2) on an fd the next xemu inherits;
#                                          first waits while an owner-demo.json names a
#                                          live pid (XEMU_OWNER_DEMO_WAIT=0: exit 75);
#                                          each minute of waiting names the holders
#                                          past their expected end
#   xemu_lease_write <pid> <purpose> <s>   lease for the xemu just started, which
#                                          now holds the slot alone (until it exits);
#                                          also takes that pid out of PRIO_DARWIN_BG
#   xemu_auto_end                          drop that lease (and a slot not handed on)
#   xemu_wait_ready <log> <pid> [binary] [settle s=5]
#                                          return once <log> has `xemu: ready`, then
#                                          settle seconds more; 1 if the pid exits or
#                                          no line comes in $XEMU_READY_TIMEOUT (120) s.
#                                          A binary older than that line (4fb1afb755,
#                                          2026-10-13) waits 20 s + settle instead
#   (sourcing it)                          takes this shell out of PRIO_DARWIN_BG
#                                          (taskpolicy -B: an agent's shell can sit at
#                                          PRI 4, where a 7.5 s run took 37 min), and
#                                          unsets the inherited $XEMU_AUTOMATION_ENV
#   xemu_bench_cache <xemu binary> <isolate 0|1>
#       refuses (exit 2) a binary inside this checkout's dist/ (the
#       owner's bundle: launch a clone). Sets XEMU_BENCH_BASE, the data
#       base path the run passes as
#       XEMU_BASE_PATH, and XEMU_BENCH_PORTABLE=1 when the caller must
#       plant its scratch config in the clone's Contents/Resources: for
#       --isolate-caches, and for a binary that predates XEMU_BASE_PATH
#       (it would otherwise write the owner's pref dir). Warns when that
#       folder holds a KosmicKrisp strike marker: runs whose config says
#       driver = 'kosmickrisp' then use MoltenVK until the build changes
#       (docs/kosmickrisp.md)

zmodload zsh/datetime
setopt no_bg_nice   # background launches get exactly nice 10, not zsh's +5 on top
XEMU_LEASE_DIR=${XEMU_LEASE_DIR:-/tmp/xemu-leases}
XEMU_LEASE_PY=${${(%):-%x}:A:h}/xemu_lease.py
XEMU_REPO_ROOT=${${(%):-%x}:A:h:h}
# Nice 10 stays; PRIO_DARWIN_BG goes, here and on each xemu (xemu_lease_write).
() {
  (( $+commands[taskpolicy] )) || return 0
  local pri=${$(ps -o pri= -p $$ 2>/dev/null)//[^0-9]/}
  taskpolicy -B -p $$ 2>/dev/null || return 0
  [[ -n $pri ]] && (( pri <= 4 )) && echo "lease: launcher was at PRIO_DARWIN_BG (PRI $pri); cleared" >&2
  return 0
}
# The lease folder's owner-demo.json and, always, the harness's: a moved
# lease folder has its own leases and slots, never its own demo guard.
typeset -gaU XEMU_OWNER_DEMO_FILES=($XEMU_LEASE_DIR/owner-demo.json /tmp/xemu-leases/owner-demo.json)
XEMU_NICE=(nice -n 10)
XEMU_MARK=(-name xemu-fork-auto)
typeset -g XEMU_SLOT="" XEMU_SLOT_FD="" XEMU_LEASE_PID="" XEMU_BENCH_BASE="" XEMU_BENCH_PORTABLE=0
# An automated xemu never inherits these (xemu_lease.py's AUTOMATION_ENV):
# the caller passes them per run (VAR=V arguments, --b-env/--e-env).
XEMU_AUTOMATION_ENV=(${(f)"$(python3 -I -B $XEMU_LEASE_PY --automation-env)"})
(( ${#XEMU_AUTOMATION_ENV} )) || { echo "lease: no AUTOMATION_ENV from $XEMU_LEASE_PY --automation-env" >&2; return 1; }
() {
  local v
  for v in $XEMU_AUTOMATION_ENV; do
    (( ${+parameters[$v]} )) || continue
    echo "lease: not passing the inherited $v to xemu (set it per run)" >&2
    unset $v
  done
}

xemu_leases_sweep() {   # stale = older than 10 s and its pid no longer a marked xemu
  local f c rc   # every step tolerates failure: the benches run under set -e
  for f in $XEMU_LEASE_DIR/<->.json(Nms+10); do
    rc=0; c=$(ps -o command= -p ${${f:t}%.json} 2>/dev/null) || rc=$?
    (( rc > 1 )) && continue   # ps itself failed: can't tell, keep it
    [[ $c == *"-name xemu-fork-auto"* ]] || rm -f $f
  done
  return 0
}

xemu_owner_demo_pid() {   # "<pid> <file>": the first owner-demo.json whose pid runs
  local f pid
  for f in $XEMU_OWNER_DEMO_FILES; do
    [[ -e $f ]] || continue
    # parsed as xemu_lease.owner_demo does: a file that doesn't parse is no demo
    pid=$(python3 -I -c '
import json, sys
p = json.load(open(sys.argv[1]))["pid"]
if isinstance(p, bool): raise TypeError
print(int(p))' $f 2>/dev/null) || continue
    [[ $pid == <1-> ]] && ps -p $pid > /dev/null 2>&1 || continue
    print -r -- "$pid $f"
    return 0
  done
  return 1
}

xemu_owner_demo_wait() {   # return once no owner demo runs
  local demo pid f t0=$EPOCHSECONDS said=-1
  while demo=$(xemu_owner_demo_pid); do
    pid=${demo%% *} f=${demo#* }
    if [[ $XEMU_OWNER_DEMO_WAIT == 0 ]]; then
      echo "lease: owner demo running (pid $pid; $f): not starting xemu (XEMU_OWNER_DEMO_WAIT=0)" >&2
      exit 75
    fi
    if (( said < 0 || EPOCHSECONDS - t0 - said >= 60 )); then
      echo "lease: owner demo running (pid $pid; $f): no new xemu until it ends; waiting" >&2
      said=$(( EPOCHSECONDS - t0 ))
    fi
    sleep 2
  done
  (( said < 0 )) || echo "lease: owner demo over after $(( EPOCHSECONDS - t0 )) s" >&2
  return 0
}

# A slot is flock(2)ed through a descriptor the xemu launched next inherits:
# the lock belongs to the open file, so it outlives this script (SIGKILL
# included) for as long as that xemu runs. zsystem flock takes fcntl locks,
# which a child doesn't inherit, so python3 locks our descriptors for us.
xemu_slot_acquire() {
  local n=${XEMU_FORK_SLOTS:-4} i fd got t0=$EPOCHSECONDS said=-1
  local -a fds
  [[ -n $XEMU_SLOT_FD ]] && return 0
  [[ $n == <1-> ]] || n=4
  mkdir -p $XEMU_LEASE_DIR/slots
  xemu_leases_sweep
  while :; do
    xemu_owner_demo_wait
    fds=()
    for (( i = 0; i < n; i++ )); do
      exec {fd}<>$XEMU_LEASE_DIR/slots/$i.lock
      fds+=($fd)
    done
    got=$(python3 -I -c '
import fcntl, sys
for i, fd in enumerate(sys.argv[1:]):
    try:
        fcntl.flock(int(fd), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        continue
    print(i)
    break' $fds 2>/dev/null) || got=
    for (( i = 0; i < n; i++ )); do
      [[ $i == $got ]] && continue
      fd=${fds[i+1]}; exec {fd}<&-
    done
    if [[ -n $got ]] && xemu_owner_demo_pid > /dev/null; then
      fd=${fds[got+1]}; exec {fd}<&-   # a demo began meanwhile: wait for it
      continue
    fi
    if [[ -n $got ]]; then
      XEMU_SLOT=$got XEMU_SLOT_FD=${fds[got+1]}
      (( said >= 0 )) && echo "lease: slot $got free after $(( EPOCHSECONDS - t0 )) s" >&2
      return 0
    fi
    if (( said < 0 || EPOCHSECONDS - t0 - said >= 60 )); then
      echo "lease: all $n fork-xemu slots busy (XEMU_FORK_SLOTS); waiting" >&2
      # the holders past their expected end, with CPU against runnable seconds
      XEMU_LEASE_DIR=$XEMU_LEASE_DIR python3 -I -B $XEMU_LEASE_PY 2>/dev/null |
        grep 'past its expected end' | sed 's/^ */lease:   holder pid /' >&2 || true
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
  (( $+commands[taskpolicy] )) && taskpolicy -B -p $pid 2>/dev/null || true
  # That xemu inherited the slot's descriptor: drop ours, so the slot lasts
  # exactly as long as the xemu (and later children don't hold it).
  if [[ -n $XEMU_SLOT_FD ]]; then local fd=$XEMU_SLOT_FD; exec {fd}<&-; XEMU_SLOT_FD=""; fi
  return 0
}

xemu_auto_end() {
  local fd=$XEMU_SLOT_FD
  [[ -n $XEMU_LEASE_PID ]] && rm -f $XEMU_LEASE_DIR/$XEMU_LEASE_PID.json
  [[ -n $fd ]] && exec {fd}<&-
  XEMU_LEASE_PID="" XEMU_SLOT="" XEMU_SLOT_FD=""
  return 0
}

xemu_wait_ready() {   # <log> <pid> [binary] [settle s]
  local log=$1 pid=$2 bin=$3 settle=${4:-5} t0=$EPOCHREALTIME
  if [[ -n $bin ]] && ! grep -qaF 'xemu: ready' $bin; then
    sleep $(( 20 + settle ))   # no readiness line in this build: the old fixed wait
  else
    until grep -qF 'xemu: ready' $log 2>/dev/null; do
      kill -0 $pid 2>/dev/null || return 1
      (( EPOCHREALTIME - t0 < ${XEMU_READY_TIMEOUT:-120} )) || return 1
      sleep 0.1
    done
    sleep $settle
  fi
  kill -0 $pid 2>/dev/null
}

xemu_bench_cache() {   # <xemu binary> <isolate 0|1>
  if [[ ${1:A} == $XEMU_REPO_ROOT/dist(|/*) ]]; then
    echo "lease: refusing $1: dist/ is the owner's bundle; launch a clone (scripts/refresh-clone.sh)" >&2
    exit 2
  fi
  if (( $2 )) || ! grep -qa XEMU_BASE_PATH $1; then
    XEMU_BENCH_PORTABLE=1 XEMU_BENCH_BASE=${1:h:h}/Resources/
  else
    XEMU_BENCH_PORTABLE=0 XEMU_BENCH_BASE=${XEMU_BENCH_CACHE:-$HOME/Library/Caches/xemu-fork-bench}/
    mkdir -p $XEMU_BENCH_BASE
  fi
  if [[ -f ${XEMU_BENCH_BASE}kosmickrisp-strike.txt ]]; then
    echo "lease: ${XEMU_BENCH_BASE}kosmickrisp-strike.txt exists: runs with driver = 'kosmickrisp' use MoltenVK" \
         "for the build it names ($(head -1 ${XEMU_BENCH_BASE}kosmickrisp-strike.txt));" \
         "remove it if a killed run left it" >&2
  fi
}
