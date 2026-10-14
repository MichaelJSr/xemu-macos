#!/bin/bash
# inject_input.sh — drive xemu's XEMU_INPUT_PIPE test-input channel from the
# shell: create the FIFO, send named-key or raw-scancode down/up/tap/clear
# events. No third-party deps; works with bash or zsh.
#
# Background: when XEMU_INPUT_PIPE=<path> is set before launch, xemu opens
# <path> as a non-blocking FIFO reader and OR's "down <sdl_scancode>" /
# "up <sdl_scancode>" / "clear" lines into its SDL keyboard state, which then
# flows through the user's normal keyboard-controller bindings
# (ui/xemu-input.c test_input_poll). This works even when the xemu window is
# NOT focused — the only automation path that does, because macOS drops
# synthetic key events (System Events, CGEventPostToPid) for background
# apps. POSIX-only (the pipe is compiled out on Windows).
#
# Usage:
#   inject_input.sh create <fifo>                  create the FIFO if absent
#   inject_input.sh down   <fifo> <key>             hold a key down
#   inject_input.sh up     <fifo> <key>             release a key
#   inject_input.sh tap    <fifo> <key> [hold_ms]   down, sleep, up (default 80ms)
#   inject_input.sh clear  <fifo>                   release every key
#   inject_input.sh keys                            print the scancode table
#   inject_input.sh --help | -h                     this text
#
# <key> is either a name from the table below (case-insensitive) or a raw
# SDL scancode integer (0-511) if you need a binding this table doesn't
# name. Names and values below are verified against
# macos-libs/*/opt/local/include/SDL3/SDL_scancode.h.
#
# Example — start xemu with the pipe, then drive it from another shell:
#   mkfifo /tmp/xemu.fifo
#   XEMU_INPUT_PIPE=/tmp/xemu.fifo dist/xemu.app/Contents/MacOS/xemu &
#   inject_input.sh tap /tmp/xemu.fifo W 200      # walk forward 200ms
#   inject_input.sh down /tmp/xemu.fifo DOWN      # hold the A button
#   sleep 1
#   inject_input.sh up /tmp/xemu.fifo DOWN
#   inject_input.sh clear /tmp/xemu.fifo
#
# This script only WRITES to the fifo (one open+printf+close per call) — it
# never blocks waiting for xemu, because writing to a FIFO blocks only until
# A reader is present, and xemu's poll loop opens its (non-blocking) reader
# fd once and keeps it open for the process lifetime. If nothing has ever
# opened the fifo for reading (xemu not running yet, or launched without
# XEMU_INPUT_PIPE set), the write below WILL block until something does —
# start xemu first.
#
# Scancode table (name = SDL_Scancode value; the fork's default keyboard
# bindings this table matches are documented in xemu-config-and-flags):
#   W=26 A=4 S=22 D=7            WASD          (left stick, in the default map)
#   UP=82 DOWN=81 LEFT=80 RIGHT=79   arrow keys (Down/Right/Left = A/B/X buttons
#                                     in the default map; Up is provided for
#                                     completeness, unbound by default)
#   SPACE=44                     Space         (Y button in the default map)
#   KP1=89 KP2=90 KP3=91 KP5=93  numpad 1/2/3/5 (right stick in the default map)
#   LSHIFT=225 RSHIFT=229        left/right Shift (triggers in the default map)
#
# The in-game meaning of each scancode depends on the user's
# [input.keyboard_controller_scancode_map] in xemu.toml — this table only
# names scancodes, it does not know what game action they trigger. Re-verify
# scancode values with:
#   grep -n 'SDL_SCANCODE_\(W\|A\|S\|D\|UP\|DOWN\|LEFT\|RIGHT\|SPACE\|KP_[1235]\|[LR]SHIFT\) =' \
#     macos-libs/arm64/opt/local/include/SDL3/SDL_scancode.h

set -u

usage() {
    sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
}

scancode_for_name() {
    case "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')" in
        W) echo 26 ;;
        A) echo 4 ;;
        S) echo 22 ;;
        D) echo 7 ;;
        UP) echo 82 ;;
        DOWN) echo 81 ;;
        LEFT) echo 80 ;;
        RIGHT) echo 79 ;;
        SPACE) echo 44 ;;
        KP1) echo 89 ;;
        KP2) echo 90 ;;
        KP3) echo 91 ;;
        KP5) echo 93 ;;
        LSHIFT) echo 225 ;;
        RSHIFT) echo 229 ;;
        '') return 1 ;;
        *)
            # Raw numeric scancode passthrough (0-511 = SDL_SCANCODE_COUNT).
            case "$1" in
                ''|*[!0-9]*) return 1 ;;
                *)
                    if [ "$1" -ge 0 ] && [ "$1" -le 511 ]; then
                        echo "$1"
                    else
                        return 1
                    fi
                    ;;
            esac
            ;;
    esac
}

print_keys() {
    cat <<'EOF'
name    scancode  (fork default map)
W       26        left-stick up
A       4         left-stick left
S       22        left-stick down
D       7         left-stick right
UP      82        (unbound by default)
DOWN    81        A button
LEFT    80        X button
RIGHT   79        B button
SPACE   44        Y button
KP1     89        right-stick left
KP2     90        right-stick down
KP3     91        right-stick right
KP5     93        right-stick up
LSHIFT  225       left trigger
RSHIFT  229       right trigger
EOF
}

require_fifo() {
    fifo=$1
    if [ ! -p "$fifo" ]; then
        echo "error: $fifo is not a FIFO (run: $0 create $fifo)" >&2
        exit 1
    fi
}

write_line() {
    fifo=$1
    line=$2
    require_fifo "$fifo"
    printf '%s\n' "$line" > "$fifo"
}

cmd_create() {
    fifo=${1:?usage: $0 create <fifo>}
    if [ -p "$fifo" ]; then
        echo "already exists: $fifo"
        return 0
    fi
    if [ -e "$fifo" ]; then
        echo "error: $fifo exists and is not a FIFO — refusing to touch it" >&2
        exit 1
    fi
    mkfifo -m 600 "$fifo"
    echo "created: $fifo"
}

cmd_down() {
    fifo=${1:?usage: $0 down <fifo> <key>}
    key=${2:?usage: $0 down <fifo> <key>}
    sc=$(scancode_for_name "$key") || {
        echo "error: unknown key '$key' (run: $0 keys)" >&2; exit 1; }
    write_line "$fifo" "down $sc"
}

cmd_up() {
    fifo=${1:?usage: $0 up <fifo> <key>}
    key=${2:?usage: $0 up <fifo> <key>}
    sc=$(scancode_for_name "$key") || {
        echo "error: unknown key '$key' (run: $0 keys)" >&2; exit 1; }
    write_line "$fifo" "up $sc"
}

cmd_tap() {
    fifo=${1:?usage: $0 tap <fifo> <key> [hold_ms]}
    key=${2:?usage: $0 tap <fifo> <key> [hold_ms]}
    hold_ms=${3:-80}
    sc=$(scancode_for_name "$key") || {
        echo "error: unknown key '$key' (run: $0 keys)" >&2; exit 1; }
    write_line "$fifo" "down $sc"
    # /bin/sleep on macOS accepts fractional seconds.
    sleep "$(awk -v ms="$hold_ms" 'BEGIN { printf "%.3f", ms / 1000 }')"
    write_line "$fifo" "up $sc"
}

cmd_clear() {
    fifo=${1:?usage: $0 clear <fifo>}
    write_line "$fifo" "clear"
}

case "${1:-}" in
    -h|--help|help|"") usage ;;
    keys) print_keys ;;
    create) shift; cmd_create "$@" ;;
    down)   shift; cmd_down "$@" ;;
    up)     shift; cmd_up "$@" ;;
    tap)    shift; cmd_tap "$@" ;;
    clear)  shift; cmd_clear "$@" ;;
    *)
        echo "error: unknown command '$1' (run: $0 --help)" >&2
        exit 2
        ;;
esac
