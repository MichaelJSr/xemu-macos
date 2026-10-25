#!/bin/zsh
# One-knob windowed in-game A/B: B ENV_VAR=0 vs E ENV_VAR=1 on one bundle.
#
#   scripts/bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs=3] [secs=75] [outdir=./bench-out]
#                                 [--e-value V] [--app P] [--isolate-caches] [bench-abx flags]
#   scripts/bench-savestate-ab.sh --dry-run [outdir]
#
# A wrapper over scripts/bench-abx.sh (protocol, gates, receipt, launch
# rules): --b-app and --e-app are both --app (default dist/xemu.app, only
# ever cloned: one clone, config and cache base for both arms), --b-env
# VAR=0, --e-env VAR=<--e-value, default 1>, and --one-warmup (one 40 s
# warmup on the E arm). XEMU_BENCH_ISOLATE_CACHES=1 means --isolate-caches
# (one cold cache, warmed by the warmup, shared by both arms). Other flags
# (--draws-band, --cv-max, --config-src, --hdd, --boot-wait, --keep-work,
# --no-screenshot) pass through. --dry-run runs the receipt engine's
# selftest on synthetic nsprof fixtures (no xemu, no fixtures read). Exit
# codes are bench-abx's: 0 valid, 3 visual artifact, else the receipt's.
ROOT=${0:a:h:h}
typeset -a pos rest
APP=$ROOT/dist/xemu.app E_VALUE=1 DRY=0
(( ${XEMU_BENCH_ISOLATE_CACHES:-0} )) && rest+=(--isolate-caches)
while (( $# > 0 )); do
  case $1 in
    --dry-run) DRY=1 ;;
    --app) APP=$2; shift ;;
    --e-value) E_VALUE=$2; shift ;;
    --draws-band|--cv-max|--config-src|--hdd|--boot-wait) rest+=($1 $2); shift ;;
    -h|--help) awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
    --*) rest+=($1) ;;
    *) pos+=($1) ;;
  esac
  shift
done
if (( DRY )); then
  exec python3 -I $ROOT/scripts/bench-receipt.py selftest --outdir ${pos[1]:-./bench-out-dryrun} \
       --summarizer $ROOT/scripts/nsprof_summarize.py
fi
VAR=${pos[2]:?usage: bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir] | --dry-run}
exec $ROOT/scripts/bench-abx.sh ${pos[1]} ${pos[3]:-3} ${pos[4]:-75} ${pos[5]:-./bench-out} \
     --b-app $APP --e-app $APP --b-env "$VAR=0" --e-env "$VAR=$E_VALUE" --one-warmup $rest
