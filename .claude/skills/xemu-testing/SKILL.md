---
name: xemu-testing
description: >-
  Automated in-game testing for this xemu fork — reproducible savestate
  benchmarks, background input injection, visual artifact detection, and the
  environment-parity rules that make results trustworthy. Also covers
  cross-binary and multi-variable A/B shapes the stock harness cannot express,
  the quiet-machine and leftover-harness-loop checks a timed run depends on,
  and the hard limit on stacking refuters/differential validators in one
  session.
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
5. **Stacked validators are their own failure mode.** Refuters and
   differential validators are budgeted one at a time against the
   thread each instruments; nobody budgets the sum. Two is the
   maximum per session — see "Refuter / validator stacking" below.

## Reproducible perf A/B (±0.02 fps on static scenes)

One binary, env-var toggle, interleaved B/E pairs (`scripts/
bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir]`;
`--e-value V` for N-valued knobs — B stays 0). Boot ~25 s, then
`loadvm` via monitor socket (CLI `-loadvm` breaks on USB topology).
Metrics: `XEMU_NV2A_NSPROF=1` 5 s intervals; the receipt gate anchors
each run at the TRAILING contiguous in-band (draws/flip > 100)
interval run and drops one post-load transient — position-based
dropping is wrong because warm launches print boot-menu intervals
before loadvm while a cold first launch boots flip-silent (archaeology
9.7). Every run also captures one mid-run screenshot scored for
white-screen / stuck-frame / magenta classes
(`scripts/bench-screenshot.py`; verdicts in `shot_<label>.verdict`
sidecars — never append to the live log, archaeology 9.8; ARTIFACT
sets batch exit code 3). **Live-movement runs have ±5 fps route
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

## Bench mechanics learned 2026-07-11 (fix-batch validation session)

- **Monitor unix-socket paths are capped at 104 bytes on macOS.** The
  session scratchpad path alone blows the limit and xemu exits with
  "UNIX socket path is too long" before the socket exists. Put the
  socket (and only the socket) in `/tmp` (e.g. `/tmp/xemu-mon.$$.sock`);
  everything else stays in the scratch dir.
- **Cross-binary A/B: never use a CI/release binary as the base arm.**
  Release legs carry PGO (+9.4% class) and the pinned MoltenVK, while a
  local build has neither — the comparison confounds three variables.
  Build the base locally from the same tree with the candidate change
  stashed (`git stash push -- <files>` keeps concurrent doc edits
  intact), same toolchain, same MoltenVK, PGO off in both arms.
- **Quiet-machine enforcement is not optional for CPU-bound scenes.**
  A first F8 parity attempt ran while a worktree build was compiling:
  the base arm alone swung 26.3→30.1 fps between its own pairs and the
  deltas were sign-mixed garbage. The quiet re-run of the identical
  comparison read +0.12 ± 1.46 (clean parity). F5-class GPU-slack
  scenes tolerate load far better than F8-class CPU-bound ones; check
  `pgrep -fl "ninja|clang"` before any timed run, and treat a base arm
  disagreeing with itself by >1 fps as an environment failure, not a
  result.
- The hardened `scripts/bench-savestate-ab.sh` (2026-07-11) forces
  fullscreen OFF in its scratch config — within-experiment deltas are
  unaffected, but its absolute numbers are not comparable to the
  historical fullscreen anchors (F5 46.8, F8 ~29.5-31.5 on the
  2026-07-11 local no-PGO build, user config, fullscreen).
- **Monitor `loadvm` needs the real `vm-*` qcow2 tag, not the F5/F8
  shortcut name — and it fails SILENTLY.** The `[general.snapshots.
  shortcuts]` names in xemu.toml are UI keybindings; the QEMU monitor
  only knows the underlying tags (e.g. `f8 = 'vm-20260704173701'`).
  `loadvm F8` prints its error to the monitor socket nobody reads and
  the guest keeps running the BOOT workload — two independent harnesses
  hit this on 2026-07-11 (a cc_op census published idle-workload numbers
  as "F5/F8" before the erratum; see README roadmap item 5). Map
  shortcut→tag from the toml in every runner, and ALWAYS verify scene
  identity from nsprof (which prints to **stdout**, not stderr — capture
  both) before believing any run.

## Bench mechanics learned 2026-08-04 (fork-wide optimization wave)

- **`bench-savestate-ab.sh` hardcodes the B arm to `VAR=0`.** The
  harness takes exactly one `ENV_VAR` and runs `run_one B$k 0` /
  `run_one E$k $E_VALUE` (`scripts/bench-savestate-ab.sh:334-335`), so
  `--e-value` moves the E arm only. Three shapes it therefore cannot
  express — all three of which this wave needed: a **pinned non-zero
  baseline** (`XEMU_REPORTS_BUDGET_US` 300 vs 50; `0` is not the
  default and would bench a different mechanism), a **multi-variable**
  arm (the combined-x87 test toggles two knobs together), and a
  **cross-binary** interleave (prewave bundle vs wave bundle). Trap
  shape: point it at a knob whose default is non-zero and it silently
  benches "off vs candidate" instead of "default vs candidate", with a
  perfectly clean receipt.
- **Use the `abx.sh` pattern for those three cases.** Same protocol and
  same gates — it reuses `scripts/bench-receipt.py` and this library's
  `nsprof_summarize.py` verbatim — and the same isolation (bundle APFS
  clone, hdd + eeprom clones, scratch `xemu.toml` via `-config_path`
  with fullscreen forced off, monitor socket in `/tmp` per incident
  9.9). The difference is that each arm carries an env **set**
  (`--b-env "A=1 B=2"` / `--e-env ...`) and its own bundle
  (`--b-app` / `--e-app`), plus `--warmup-arm`. The 2026-08-04 copy
  lives in that session's scratchpad at `c2/abx.sh`; **a repo-ified
  copy belongs in `scripts/` and does not exist yet** — until it does,
  re-derive it from this description rather than bending the stock
  harness into a shape it cannot hold.
- **Quiet-machine enforcement must kill leftover harness LOOPS, not
  just xemu processes.** A stale background soak (the
  `scripts/soak-cycles.sh` class: boot once, then cycle
  `loadvm` + walk) outlived its session and relaunched xemu in the
  middle of a timed batch, invalidating it; the comparison was re-run
  from scratch (receipt `b1/t1-candoio-redo2`, the surviving one).
  `pgrep -fl xemu` alone does **not** catch this: between cycles the
  soak owns no xemu process at all, so the quiet check passes and the
  loop relaunches seconds later. Check the loops themselves —
  `pgrep -fl 'soak|probe-run|movement-probe|bench-savestate|abx'` —
  and kill the loop before the process it will otherwise respawn. Same
  logic as the 2026-07-11 compile-load rule: an arm that disagrees
  with itself is an environment failure, not a result.
- **A crashed run is an excluded pair, recorded as such.** In the wave's
  headline batch one pair lost its E run to the known post-`loadvm`
  ohci/flatview SIGSEGV (an upstream-class crash, not a harness fault);
  the pair was dropped and the **reason** written into the receipt, so
  the citation reads "5 clean interleaved pairs" rather than six with a
  silent hole. Never re-run just the failed arm into an existing
  batch — arms are only comparable inside their own interleaved pair.

## Refuter / validator stacking (hard limit, 2026-08-04)

**Never arm all three shadow validators in one session**:
`XEMU_X87_REFUTE=1`, `XEMU_SUBPAGE_FAST_REFUTE=1`,
`XEMU_DSP_JIT_DIFF=1`. **Two is the maximum.**

The triple deadlocks the guest **during boot**, before any flip —
zero nsprof intervals, `XEMU_PFIFO_HEARTBEAT` frozen at `iters=1`,
monitor unresponsive, reproduced 2/2. The DSP validator's work runs
under the APU `d->lock` while the vCPU blocks on that same lock in
`vp_write`; every single knob and **all three pairs** boot clean, so
this is a cost-of-the-sum effect, not a defect in any wave mechanism.
Full bisect, stack traces, and receipts: archaeology **9.10**.

If a soak genuinely needs all three, use **sampled** DSP validation —
`XEMU_DSP_JIT_DIFF=10` boots clean alongside the other two, and the
per-translation `diff_checked` gate means sampling changes *when* a
unique translation is validated, not *whether* it is. The other
accepted answer is splitting the soak across two sessions and keeping
`DIFF=1` in one of them (what the wave did).

Adding a fourth refuter to any soak recipe: boot-probe the exact env
set for 90 s first and require nsprof intervals > 0. That probe costs
ninety seconds and converts "wedge, cause unknown" into "known
combination" — a wedge is not a crash, so nothing else will tell you
(no DiagnosticReports entry, monitor silent).

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

## Movement-phase transition probe (first-visit lag class)

Built 2026-07-12 to pin the v0.11 "new-area lag" (heavy stutter on
map transitions / death reloads / fast movement into unexplored
space, smooth on revisits). The savestate A/B above cannot see this
class — static scenes never stream. **Committed implementation:
`scripts/movement-probe.sh <outdir> [VAR=V ...]`** (2026-07-18 —
isolation-cloned bundle/hdd, /tmp sockets, liveness-guarded FIFO
injection, per-phase `info jit` flush deltas, mid-forward screenshot).
The method spec it implements:

- One run = boot → `loadvm` real `vm-*` tag → settle 15 s → **hold
  forward (lstick_up) 20 s into unexplored space** → rest 5 s →
  **hold backward 20 s through the just-explored route** → quit.
  Forward = first-visit streaming; backward = the built-in control
  (same terrain, assets now resident).
- Instrument: `XEMU_NV2A_NSPROF=1` (stdout), monitor `info jit`
  sampled between phases — **"TB flush count" deltas per phase are
  the mechanism signal** (139 in 20 s of forward = the v0.11 bug;
  0 = healthy). Atexit dumps (`XEMU_XPAGE_CHAIN=2` explicit arms the
  XPAGE dump with identical semantics to default; `XEMU_INV_PROF=1`
  arms SUBPAGE counters).
- Phase→interval attribution: nsprof headers self-report interval
  duration; anchor t=0 at the first interval with draws/flip > 100
  after a < 100 boot regime, then map interval midpoints onto the
  epoch-stamped phase marks (`lag_phase.py`). Headline metric:
  **worst forward-phase interval fps vs settle mean** (the felt lag),
  plus per-phase flush deltas.
- Window shots at phase ends verify the route actually moved (and
  catch death mid-run — combat damage shifts routes; interleave
  arms and compare means, ±5 fps live-movement variance applies).
- Traps: a dead xemu makes every fifo write BLOCK forever — guard
  each injection with `kill -0` liveness checks; a vCPU-thread wedge
  (e.g. W^X fault loop) looks like "monitor answered once then
  empty responses + zero nsprof" — it is NOT a crash, no
  DiagnosticReports entry appears.

## Present-pacing benches (push ring, interp cadence)

Unfocused/occluded bench windows present UNPACED (~200 Hz observed
2026-07-12, no vsync throttle) — display-pacing bugs are structurally
invisible from a background harness. `XEMU_UI_FRAME_CAP_NS=16666666`
(test knob, zero cost unset) floors the UI present period to simulate
a 60 Hz consumer; the push-ring nsprof events (`push_step_publish`,
`push_ring_drop` = UNREAD-entry overwrites only, `push_policy_skip`)
are the oracle. The ring's `present_ring_count` never decreases
(consumption is a cursor) — any "ring full" heuristic must compare
against the consume cursor or it counts healthy recycling as drops
(archaeology 3.3 burned all three of these).
- Soak variant (`bench2/soak_lag.sh`): boot once, N cycles of
  alternating loadvm tags + 8 s forward walk — exercises
  flush-backstop machinery under repeated streaming.

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

## Multi-title boot smoke (soak gate for state-table / bounds changes)

`scripts/boot-smoke-corpus.sh [--app PATH] [--secs N] [--out DIR] <iso>...`
(added 2026-09-08) boots every ISO given on an isolated clone of a bundle
(same isolation as the A/B harness: APFS bundle clone, cloned hdd/eeprom,
scratch `xemu.toml` with `dvd_path` swapped and fullscreen off, never
`dist/xemu.app`'s config), taps START and A through the user's own
keyboard bindings via `XEMU_INPUT_PIPE` so menus advance, captures two
scored screenshots (`scripts/bench-screenshot.py`), quits via the monitor,
and reports per title: `alive`/`CRASHED` (exit status if it died early),
assert/abort/`nv2a_vk_bounds` lines, the last nsprof interval and the
screenshot verdicts. Exit 0 = all alive and no ARTIFACT; 2 = a title died
or asserted; 3 = an ARTIFACT verdict.

Use it as the gate for changes that only trip on titles other than
Azurik — release-mode bounds checks over renderer state tables, PVIDEO
overlays, new abort paths — by running the same title list on the
baseline bundle first. Baseline recorded 2026-09-08 on `add066354f` (+
phase-A fixes): Azurik, Battlefield 2 MC, Conker, KOTOR, Vexx and
NevolutionX all alive 120 s, 0 asserts, all screenshots PASS. It is a
boot-to-menu/intro smoke, not gameplay: Azurik reaches ~50 draws/flip,
Conker ~280, KOTOR sits on a 2-draw loading screen at 120 s. Lengthen
`--secs` or add a savestate route when a change needs deeper coverage.

## Snapshots with controllers (topology matching)

Snapshots embed per-pad USB trees: `usb-hub,port=1.P,ports=3` +
`usb-xbox-gamepad,port=1.P.1,index=N-1` per bound xbox player slot
N, where P maps through `port_map = {3,4,1,2}`
(`ui/xemu-input.c:261`) — player 1 → hub port 1.3, gamepad index 0.
A machine missing them fails `loadvm` ("Unknown section ...usb-hub").
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

## Provenance and maintenance

Methods here were verified against the tree on 2026-07-04, with the
2026-07-11/12/18 bench-mechanics notes and the 2026-08-04 wave
sections carrying their own dates. Historical measurements are cited
with their date and are not current readings. Re-verify the
drift-prone mechanics before trusting them:

- B-arm hardcode + `--e-value` semantics:
  `grep -n 'run_one B\|E_VALUE\|--e-value' scripts/bench-savestate-ab.sh`
- Harness cache isolation (a controlled variable since 2026-08-04):
  `grep -n 'isolate-caches\|ISOLATE_CACHES' scripts/bench-savestate-ab.sh`
- Receipt gating (scene-identity anchoring, incident 9.7):
  `sed -n '79,100p' scripts/bench-receipt.py` and
  `python3 scripts/bench-receipt.py selftest`
- The three stackable validators still exist under these names:
  `grep -rn 'XEMU_X87_REFUTE\|XEMU_SUBPAGE_FAST_REFUTE' accel/tcg target/i386/tcg`
  and `grep -n 'XEMU_DSP_JIT_DIFF' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- Movement probe + soak loops that a quiet check must catch:
  `ls scripts/movement-probe.sh scripts/soak-cycles.sh scripts/probe-run.sh`
- Keyboard scancode map (owner-editable, re-read per session):
  `grep -n -A 30 'keyboard_controller_scancode_map' "$HOME/Library/Application Support/xemu/xemu/xemu.toml"`
- Whether `abx.sh` has been repo-ified yet: `ls scripts/abx.sh`
  (absent as of 2026-08-04 — the pattern is documented above instead)
