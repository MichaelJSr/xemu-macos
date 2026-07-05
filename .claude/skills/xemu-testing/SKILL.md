---
name: xemu-testing
description: Automated in-game testing for this xemu fork — reproducible savestate benchmarks, background input injection, visual artifact detection, and the environment-parity rules that make results trustworthy.
---

# xemu automated testing playbook

Proven methods for benchmarking and debugging this fork on macOS,
distilled from the 2026-07 optimization and pink-tile campaigns.

## Cardinal rules (each learned the hard way)

1. **Replicate the launch environment.** `Info.plist` `LSEnvironment`
   (the `MVK_CONFIG_*` tuning) applies ONLY to Finder/LaunchServices
   launches. A shell-launched binary gets library defaults — a
   texture-corruption bug hid for weeks behind exactly this split.
   xemu now sets the canonical values in `main()` (launch-path
   parity), but any NEW plist env var must be passed explicitly by
   harnesses until mirrored in code.
2. **Never point a harness at `dist/xemu.app`.** The user launches
   that bundle; a planted portable config (`Contents/Resources/
   xemu.toml`) hijacks their session — config, saves, and hdd path.
   Test from a dedicated copy: `cp -Rc dist/xemu.app <scratch>/xemu-test.app`
   (APFS clone, instant), refresh after every rebuild.
3. **A user's running process keeps its old binary.** "Why is my
   build old" = they launched before the last rebuild. The window
   title shows the git describe — trust it.
4. **Verify inputs actually arrived** (draws/flip variance across
   nsprof intervals). A static scene silently invalidates a run.

## Reproducible perf A/B (±0.02 fps on static scenes)

One binary, env-var toggle, interleaved B/E pairs (`scripts/
bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir]`).
Boot ~25 s, then `loadvm` via monitor socket (CLI `-loadvm` breaks on
USB topology). Metrics: `XEMU_NV2A_NSPROF=1` 5 s intervals; use only
intervals with draws/flip > 100, drop the first post-load one; verify
scene identity via draws/flip. **Live-movement runs have ±5 fps route
variance** — use ≥3 interleaved pairs and compare means, or prefer
static scenes for small deltas.

**Scene-evolution caveats (learned 2026-07-04):** (1) a "static" save
can still evolve — combat saves can kill the idle player mid-run and
respawn into a lighter scene (one 2026-07 save did; the owner fixed
it, but death still shifts fps slightly) — keep captures ≤60 s,
check the per-interval fps/draws trajectory for a regime change, and
when in doubt compare the first 5-8 in-game intervals per run;
(2) frame-rate feedback — the guest adapts effect load to frame
time, so the faster arm can legitimately show a few % different
draws/flip from the first interval on (seen: 786 vs 740 at +1.4 fps);
interleaved consistent-sign deltas still carry the conclusion;
(3) cross-experiment floor drift is real (both arms of a later
experiment ran ~3 fps above an earlier experiment's arms —
thermal/context) — only within-experiment interleaved deltas are
citable, never arms from different experiments.

## Background input injection (no focus, no OS events)

`XEMU_INPUT_PIPE=<fifo>` + lines `down <sdl_scancode>` /
`up <sdl_scancode>` / `clear` → OR'd into SDL keyboard state through
the user's bindings (`ui/xemu-input.c`). Works with xemu fully
unfocused. OS-level injection cannot work: System Events targets the
frontmost app; `CGEventPostToPid` delivers but AppKit drops key
events for inactive apps.

The keyboard is a bound Xbox controller on **port2**
(`[input.bindings] port2 = 'keyboard'`); the authoritative scancode
map is `[input.keyboard_controller_scancode_map]` in the user's
`~/Library/Application Support/xemu/xemu/xemu.toml` — **re-read it
per session** (owner-editable; a scratch config copied from that
toml carries the bindings along). Map as of 2026-07-04 (SDL
scancodes): left stick W/A/S/D = 26/4/22/7; right stick (camera)
KP5/KP1/KP3/KP2 = 93/89/91/90; A = 81 (Down-arrow), B = 79 (Right),
X = 80 (Left), Y = 44 (Space); dpad up/left/right/down =
94/37/38/92; start = 42 (Backspace), back = 46; white = 45, black =
82; ltrigger = 225 (LShift), rtrigger = 229 (RShift);
lstick_btn/rstick_btn/guide = 30/31/39. Helper:
`.claude/skills/xemu-diagnostics-and-tooling/scripts/inject_input.sh`
(`create/down/up/tap/clear`).

## Visual artifact oracle (window capture + pixel scoring)

QEMU `screendump` does not work (custom Metal present path). Use
`screencapture -x -o -l<windowID>` at ~2-4 fps; find the window via
Quartz `CGWindowListCopyWindowInfo` matched on `kCGWindowOwnerPID`
with `kCGWindowListOptionAll` (works across Spaces; retry ~15 s for
window creation). Score downscaled frames in PIL (magenta:
`R>170 and B>140 and G<0.55*min(R,B)`, cluster on a coarse grid),
keep flagged frames and view them — morphology identifies the
subsystem (small Morton squares = texture path; large screen-aligned
rects = compositor/present/driver). Reference implementation:
scratchpad `pinkhunt4.sh` of session 21e903dd (recreate from this
description if absent).

Observing the USER's live session works the same way (their window:
owner "xemu" when Finder-launched) — capture-only, zero interference,
and their instance holds the qcow2 write lock, so clone the disk with
`cp -c` (instant, snapshots ride along) to run in parallel.

## Snapshots with controllers (topology matching)

Snapshots embed per-pad USB trees: `usb-hub,port=1.N,ports=3` +
`usb-xbox-gamepad,port=1.N.1,index=N-1` per bound xbox port N. A
machine missing them fails `loadvm` ("Unknown section ...usb-hub").
Headless: replicate with raw `-device` args (unbound pads read
neutral input — guarded in `hw/xbox/xid.c`). List snapshots by
parsing the qcow2 header (offset 60: nb u32, table offset u64; entry:
40-byte fixed struct `>QIHHIIQII` + extra + id + name, 8-aligned).

## MoltenVK maintenance

`scripts/build-moltenvk.sh` — pinned commit, `-O3`,
`-mcpu` per machine (`XEMU_MVK_MCPU`), no LTO (breaks ShaderConverter
xcframework packaging), installs to `/usr/local/lib` which build.sh
prefers and logs (version + UUID provenance line). Bump the pin
deliberately; re-run the artifact hunt + fps A/B before releasing.
Keep `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0`: immediate
encoding corrupts streamed textures with whole-frame command
buffers, enabled an AGX visibility crash, and measures slower.
