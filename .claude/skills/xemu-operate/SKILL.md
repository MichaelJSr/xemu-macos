---
name: xemu-operate
description: >-
  Build, run, test and ship xemu-macos: build.sh/ninja and build failures,
  launching xemu (open vs binary, monitor, loadvm, input pipe), benchmark
  scripts and savestate anchors, XEMU_* env vars and xemu.toml flags,
  commits, tags, releases.
---

# xemu-operate

Paths are repo-relative. `../fixtures` is the test folder next to the
checkout. Internals and diagnostics are in `xemu-internals`; past attempts
are in `xemu-history`.

## Hard rules

- **Never touch the personal setup:** `~/Documents/Xemu/` and
  `~/Library/Application Support/xemu/xemu/` (`xemu.toml`, `eeprom.bin`,
  caches). Tests keep their caches in the bench cache instead
  (`XEMU_BASE_PATH`, below); the Azurik harness checks that folder is
  unchanged after its sittings.
- **Mark every automated launch** (the Azurik harness waits for any
  unmarked xemu, which may be the owner playing): `nice -n 10 <xemu>
  -name xemu-fork-auto …` (marker right after the binary) with
  `XEMU_BASE_PATH=~/Library/Caches/xemu-fork-bench`. The macOS
  launchers in `scripts/` do it (via `scripts/xemu_lease.py` /
  `xemu-lease.zsh`, which also take one of `XEMU_FORK_SLOTS`=4 slots and
  write a `/tmp/xemu-leases/<pid>.json` lease); the Windows bench adds
  the marker, the mute and its own bench cache
  (`%LOCALAPPDATA%\xemu-fork-bench`), but no nice, slot or lease. A
  launch by hand goes through `python3 -I scripts/xemu_lease.py run
  [--env K=V] -- <xemu> -config_path <toml> …` (nice, marker, bench
  cache, slot, lease; it execs the xemu, which holds the slot until it
  exits, even if the launcher is killed; it refuses a binary without
  `XEMU_BASE_PATH` (no such string in it: a build older than v0.15)
  unless its clone is portable, since that one keeps its caches in the
  owner's pref dir). Never mark an xemu the owner
  plays. Launchers pass xemu none of the automation variables they
  inherited (`XEMU_RAM_SHM`, the input pipe, record and log files, live
  pads, `XEMU_VK_DRIVER`, `XEMU_LANGUAGE`, the run-changing harness
  opt-ins such as `XEMU_RENDER_NULL_RAM` and `XEMU_STRICT_CPUID`:
  `AUTOMATION_ENV` in `xemu_lease.py`); set them per run. The kept
  normal-mode switches pass through. Every
  launch line in a doc or skill carries the marker or `XEMU_BASE_PATH`.
- **Every launch** uses a `cp -Rc` bundle clone, `cp -c` HDD and EEPROM
  clones, and `-config_path <scratch copy of ../fixtures/xemu.toml>`. xemu
  saves its config on exit, so skipping `-config_path` overwrites the
  owner's. (Adding a savestate: `../fixtures/README.md`.)
- **Mute:** per-run `[audio] volume_limit = 0` (`scripts/harness-config.py`).
  To check sound, write it with `XEMU_AUDIO_WAV` (and `XEMU_VOICE_LOG`)
  instead of listening.
- **`dist/xemu.app` is the owner's bundle.** You may rebuild it. Never
  launch it for tests and never add files to it; a
  `Contents/Resources/xemu.toml` hijacks their next launch.
- `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` stays `0` (otherwise textures
  corrupt and AGX crashes).
- Never set `XEMU_X87_REFUTE=1`, `XEMU_SUBPAGE_FAST_REFUTE=1` and
  `XEMU_DSP_JIT_DIFF=1` together: boot deadlocks.
- Published tags are immutable (Ship).

## Build

Needs macOS 14+, Xcode CLT, `brew install pkg-config ninja cmake dylibbundler`,
`pip3 install pyyaml requests`, and network on the first build. The
`roms/*` submodules aren't needed.

`./build.sh` wipes `dist/`, then builds and packages `dist/xemu.app`
(log: `build.log`). Flags: `--debug` turns on asserts, `-j8` takes no
space, `-p <platform>` and `-a <arch>` do. Other args go to configure.

Incremental: `ninja -C build qemu-system-i386` (it tracks `config_spec.yml`
and `meson.build`) doesn't update any bundle. To put the new binary in a
clone (a bare `cp` dies in dyld): `scripts/refresh-clone.sh <dst.app>
build/qemu-system-i386 [--from dist/xemu.app]` (re-points the MacPorts
dylibs, resets rpaths, re-signs; refuses `dist/`).

**PGO:** CI arm64 releases use the committed `pgo/default.profdata`;
locally, set `XEMU_PGO=use`. To retrain, build with `XEMU_PGO=generate`,
`rm -f pgo/*.profraw`, run `scripts/pgo-train.py` on a clone of
`dist/xemu.app` (~15 min; its windowed sessions open muted, unfocused
windows), rebuild with `use` and commit `pgo/default.profdata`.

**MoltenVK:** build.sh bundles the first arm64 dylib in
`macos-libs/<arch>/opt/local/lib`, `/usr/local/lib` or `/opt/homebrew/lib`,
and otherwise downloads `XEMU_MOLTENVK_VERSION` (1.4.2, which CI ships).
This Mac uses `/usr/local/lib` from `scripts/build-moltenvk.sh` (1.4.3, no
LTO). Look for `Bundling MoltenVK from …` in the log. arm64 builds also
bundle KosmicKrisp: the pinned release asset (`kk_asset_sha256`, cached
in `macos-libs/arm64`), or `XEMU_KK_PREFIX=../kk/install-<tag>`; look for
`Bundling KosmicKrisp from … (Mesa …)`.

| Failure | Fix |
|---|---|
| `GLIB_SIZEOF_SIZE_T` mismatch | `rm -rf macos-libs macos-pkgs build dist && ./build.sh` |
| `libintl.h` missing at an old path (repo moved from `~/Documents/Xemu/tools/xemu-macos`) | `python3 scripts/download-macos-libs.py arm64` (build.sh runs it). Otherwise fix the include in `macos-libs/arm64/opt/local/include/glib-2.0/glib/gi18n.h`, or `rm -rf macos-libs macos-pkgs` |
| dyld: duplicate `LC_RPATH` | Rebuild, or `scripts/refresh-clone.sh` |
| No `libMoltenVK.dylib` | Network, or `scripts/build-moltenvk.sh` |
| `SDK >= 14.0 not found` | Install a CLT with a `MacOSX<N>.<M>.sdk` |
| Moved wrap pin ignored | `rm -rf subprojects/<name>` (a dirty `roms/edk2` is harmless) |

## Run

The direct binary inherits only the shell env. `open` passes none of it:
`open -n -a <clone> --env XEMU_BASE_PATH=<bench cache> --env K=V --stdout F
--stderr G --args -name xemu-fork-auto -config_path <scratch toml> …` adds
`LSEnvironment` (no nice: `renice 10` the pid; no slot; a pre-v0.15
binary ignores `XEMU_BASE_PATH` and needs a portable clone,
`Contents/Resources/xemu.toml`, below), but then xemu hangs on
a hidden TCC prompt if its ROM/HDD paths are under `~/Documents` (the
fixtures are), so clone them to `/tmp`. The title
(`xemu | v<git describe>`) names the build.

```sh
W=$(mktemp -d /tmp/xr.XXXX) F=../fixtures
cp -Rc dist/xemu.app $W/; cp -c $F/xbox_hdd.qcow2 $W/hdd.qcow2; cp -c $F/eeprom.bin $W/eeprom.bin
cp $F/xemu.toml $W/xemu.toml   # the clones, screenshots in $W (the fixture's dir is personal),
PADS=(${(f)"$(python3 -I scripts/harness-config.py $W/xemu.toml --workdir $W \
    --hdd $W/hdd.qcow2 --eeprom $W/eeprom.bin)"})   # fullscreen off, mute, pad stubs (zsh)
XEMU_NV2A_NSPROF=1 python3 -I scripts/xemu_lease.py run -- $W/xemu.app/Contents/MacOS/xemu \
  -config_path $W/xemu.toml -monitor unix:$W/mon.sock,server,nowait $PADS > $W/log 2>&1 &
sleep 25; (echo 'loadvm azurik-heavy-f1'; sleep 3) | nc -U $W/mon.sock | grep -i error
```

- **Monitor:** `loadvm`/`savevm <tag>`, `info snapshots`, `info jit`,
  `quit` (flushes atexit dumps, PGO and config). Send `loadvm` after ~25 s:
  CLI `-loadvm` runs before the pad hubs exist (`Unknown section …usb-hub`)
  unless `XEMU_LOADVM_ADD_PADS=1`, which plugs the pads a state names
  and runs CLI `-loadvm` once the config's pads are bound.
  Use qcow2 tags; errors only reach the socket; socket path ≤104 bytes.
- **Snapshots** live in the HDD qcow2 (locked while xemu runs):
  `cp -c ../fixtures/xbox_hdd.qcow2 /tmp/s.qcow2 && python3 -I scripts/list_snapshots.py /tmp/s.qcow2`.
- **Input:** `XEMU_INPUT_PIPE=<fifo>` takes `down`/`up <sdl_scancode>` and
  `clear` lines (`scripts/inject_input.sh`) for the keyboard pad (fixture
  port 2), even unfocused. Keys: WASD 26/4/22/7, A 81, B 79, start 42.
  Writes to a dead xemu hang, so check `kill -0` first. An untagged
  `clear` also drops queued lines (`+0 clear` keeps them); assert pad
  state and the queue with QMP `query-xemu-input` rather than tracking
  them. A port's pad
  keeps its binding across loads, restores and resets; QMP
  `xemu-input-bind` swaps the source (keyboard/pipe, a gamepad GUID,
  none) under the plugged pad. Never bind the owner's pad in a test run.
- `-dvd_path <iso>` applies to one run; other QEMU flags pass through.
  stderr carries diagnostics and stdout the QEMU command line. For `open`:
  `/usr/bin/log stream --predicate 'process == "xemu"'`.
- An `xemu.toml` in `Contents/Resources/` makes a bundle portable, caches
  included (`--isolate-caches`), unless `XEMU_BASE_PATH` names the data
  folder, which wins.

## Headless mode

Use it for every agent run that needs no eyes on the window: no focus
steal, no Dock icon, no host audio. `XEMU_HEADLESS=1` hides the window
and skips the UI, but the guest still renders. `XEMU_HEADLESS=null` adds
the null renderer, for logic, RAM and gdbstub work. stderr then shows
`xemu: headless mode: offscreen|null`. For frames, use QMP `screendump`
(PNG with `"format":"png"`); it works windowed too, but not from HMP.
Deterministic, bit-exact replays: add `-S -icount shift=2,align=off,sleep=off`
(null renderer), `loadvm`, write `+N` pipe lines while paused, then
`xemu-frame-advance`; same inputs give the same RAM (README). Compare runs with
QMP `xemu-ram-digest` (RAM's sha256, no file; `against` a checkpoint lists the
pages that changed). Branch loops: `xemu-checkpoint-save` with `base`, then
`xemu-checkpoint-restore` with `incremental` (README); audit the dirty log
with `verify` (`pages-missed` 0) after touching any device that writes RAM.
A demo played in real time yet deterministic: `XEMU_ICOUNT_LIVE_PADS=1` with
`-icount shift=2,align=on,sleep=on` (host pads read at vblanks); its
`XEMU_INPUT_RECORD` replays under `sleep=off` to the same RAM (it holds one
port, `XEMU_INPUT_RECORD_PORT=1-4`, default the first bound one: keep live
sources on it; windowed replays are exact on macOS).
fps runs stay windowed, because headless never presents and so measures
a different pipeline. Code: `xemu_headless_mode()` in `ui/xemu.c`.
README "Headless mode (automation)" lists the harness interfaces:
analog and frame-tagged pipe input, `xemu-frame-advance` (any stop it
didn't make, such as a gdbstub attach, ^C or SIGTERM, ends it with that
run state in `stopped`), `query-xemu-display` (frame and load count at
the guest's last flip, the picture `screendump` writes),
`xemu-run-until` (stop at a guest PC with conditions and hit counts,
frames as the budget; pause first), `xemu-hooks-set`/`-clear` (copy
memory at guest PCs into records the next advance returns, optionally
stopping on an `expect` mismatch, and write the next entry of a queue
there: `write`/`set`, `stop-empty`, `xemu-hooks-push`; they persist
until cleared; `xemu-hooks-drain` takes the records without pausing),
`xemu-run` (one call: restore a checkpoint, queue pipe
lines, run until frames, a PC or a value leaving `[lo,hi]` or changing;
three calls and a pipe in one; hooks keep hit numbers and queues across
its restore, so clear and set them per candidate), the `xemu: ready` /
`xemu: state loaded` lines and their QMP events (`XEMU_READY`,
`XEMU_STATE_LOADED`, `XEMU_DEVICE_LOST`; `ui/xemu-events.c`),
park/resume via `migrate file:`. QMP sends events only after
`qmp_capabilities`, and `XEMU_READY` often goes out first: check
`query-xemu-status` after negotiating.
gdbstub drivers: only the first `Z0` at a PC retranslates its page, and
lifting and re-arming around a resume (`z0`/`s`/`Z0`/`c`) invalidates
nothing; still, `s` steps over a breakpoint under the PC, and after
`Qxemu.stepover:1` a plain `c` does, which saves the packets. Batch
independent reads at a stop with `qxemu.read`. To find who reads or
writes a field, use the QMP access log (`xemu-watch-start`,
no stops) rather than `Z2`-`Z4`, which work but stop at every hit. gdbstub
memory packets never reach MMIO (`E14`); HMP `xp` and `pmemsave` do, so
keep those to RAM.

## Test

Bench scripts in `scripts/` accept `-h`. Harnesses read
`../fixtures/xemu.toml` (or `XEMU_TEST_CONFIG`), clone everything, mute,
and turn fullscreen off. Each xemu they start is niced and marked, holds
a slot for its whole life (all busy: `lease: all 4 fork-xemu slots busy`,
it waits; raise `XEMU_FORK_SLOTS` only on a quiet box) and a lease, and
starts only while no owner demo runs (`/tmp/xemu-leases/owner-demo.json`,
checked under any `XEMU_LEASE_DIR` too, names a live pid: `lease: owner
demo running`, it waits;
`XEMU_OWNER_DEMO_WAIT=0` exits 75 instead), and keeps its caches
in `$XEMU_BENCH_CACHE` (default `~/Library/Caches/xemu-fork-bench`,
shared by every bench, cold the first time; a binary older than
`XEMU_BASE_PATH` gets a per-batch cache in its clone, and in bench-abx
the other arm does too).
`python3 scripts/xemu_lease.py` lists the live leases.

| Script | Use |
|---|---|
| `bench-det.py [-n pairs] [-f frames] [-t tag] [-i line] NAME=APP[:K=V,…] …` | Deterministic headless A/B: host Mcycles and Minsns per frame (load-robust), wall fps, CPU and runnable seconds (runnable well above CPU: the run waited for a core), RAM hash per run. Also runs the goldens. `--renderer offscreen`: Vulkan on a portable clone with cold caches, the driver named per line (an arm without `XEMU_VK_DRIVER` must run MoltenVK). Arm values may hold commas (a comma splits only before `NAME=`). Each start removes the `/tmp/xbd.*` run dirs (HDD clones) killed runs left |
| `bench-det.py --timeline [-n runs] [--renderer offscreen] NAME=APP …` | Cold boot to steady play under icount, per phase (kernel, logos, prophecy, title, save list, level load, steady): frames, host ms, vCPU/translate/APU/disk/other split; per-phase deltas across arms. Replays must report `same RAM`. `--block-log DIR` adds `XEMU_BLOCK_LOG` per run and checks its rows against each phase's blockstats; `--from-state TAG [--dvd ISO]` boots from a loaded state (loadvm, the disc into the tray, reset: the Azurik harness's boot-build path) |
| `bench-abx.sh <tag> [pairs] [secs] [out] --b-app P --e-app P [--b-env/--e-env "K=V …"]`, `bench-savestate-ab.sh <tag> <VAR> [pairs=3] [secs=75] [out]` | The windowed A/B: cross-binary, multi-var, numeric baseline or one knob; pairs run B,E then E,B; one bundle for both arms means one clone, config and cache (with `--isolate-caches`, one cold cache both share). bench-savestate-ab is its wrapper: `--app` (default `dist/xemu.app`, only cloned) for both arms, B `VAR=0` vs E `VAR=1` (`--e-value`), and `--one-warmup` (one warmup, on E; ignored with a warning when the bundles or the arms' `XEMU_VK_DRIVER` differ). Identity comes from the binary's embedded strings; the receipt has `config.experiment` and `binary.arms`, `cache_isolation` is `off`, `shared` or `per-arm`. Exit 3 = artifact |
| `movement-probe.sh <out> [--app P] [VAR=V…]` | Streaming walk; a healthy TB-flush delta is 0-1 |
| `cast-probe.py [--app P] [--base DIR] [--keep DIR] [--casts N]` | First-use hitches: Azurik's fire-armor cast (LT + B) from `vanilla-rz-w1-dark` (D98 config and drive: `--config`, `--hdd`), real time; per cast the worst frame gap, shader/pipeline creation and their parts, slow creations. Empty caches unless `--base` (a `--keep` base: warm) |
| `[PROBE_CYCLES=N] probe-run.sh <app> <out> <tag[,tag…]> [dwell=80] [K=V…]`, `boot-smoke-corpus.sh <iso>…` | atexit dumps after a dwell per load, every tag loaded each cycle (the refuter soak: `PROBE_CYCLES=3 … azurik-heavy-f1,vm-20260704032357 10 XEMU_SUBPAGE_FAST_REFUTE=1`; a dead xemu or failed load exits 1, a nonzero `VIOLATIONS=` 3); boot smoke |
| `nsprof_summarize.py`, `win/score_shot_win.py`, `bench-screenshot.py` | nsprof tables and A/B; magenta/white/stuck scoring of any PNG, QMP screendumps included (stdlib only, any OS); window capture |

No-game tests: `cd build && ninja test-xbox && ./pyvenv/bin/meson test --suite xbox --no-rebuild`.
Pre-release correctness pass (never per change): `tests/azurik/README.md`, the Azurik harness's replays, physics, heavy scenes and Bink checks on the Modkit's D98 fixtures, with v0.15's results. Its vanilla scenes sit at their 30 fps cap windowed, so they don't replace heavy-f1/W2.

**Lean default (CPU, TCG, APU):** build the base and the change into two
bundle clones (locally, not from CI), then `scripts/bench-det.py -n 4
base=B.app new=E.app` (~4 min). These changes get no `XEMU_*` switch
(CLAUDE.md), so the A/B is cross-binary; `NAME=APP:K=V` arms remain for
existing switches and diagnostics (`new=E.app:XEMU_GUEST_MO=1`). The box is usually loaded and that's fine:
judge by Mcycles/frame (back-to-back runs agree to ~0.5% at load 30;
absolute values drift over minutes, so compare arms only within one
interleaved session, and never with numbers from before 2026-10-17,
when null mode's vCPU ran squashed to legacy QoS) and use Minsns/frame
(~0.01%) as the mechanism check. Arms must agree on the RAM hash.
**Renderer or pacing:** `scripts/bench-savestate-ab.sh azurik-heavy-f1
XEMU_FOO 2 45` (~6 min), windowed wall fps. Go by the verdict, mean ± sd
and sign tally, quote the load, and add pairs (3-6 × 75 s) when the delta
is under ~2% or signs are mixed: there's no quiet box to wait for.
A change to the shader generators or renderer must also pass a det
offscreen cold boot (`bench-det.py --renderer offscreen -n 1 -t - -f 1800
--hash-at 900,1800 --shot DIR`, base against new; `--shot` saves the
f1800 picture from the same run, after its RAM hash; an f900 picture is a
separate `-f 900 --shot` run): Azurik's gameplay states draw with
vertex programs, and only the boot reaches fixed-function shaders (a GLSL
that fails to compile aborts there). Compare pictures only from the same
`-f`/`--hash-at` schedule, by pixel-difference counts rather than sha.

**Vulkan driver** (macOS; `docs/kosmickrisp.md`): icount and offscreen
runs pin MoltenVK; windowed runs follow `[display.vulkan] driver` (Auto:
MoltenVK; the opt-in `kosmickrisp`: KK from the arm64 bundle's
`Contents/Libraries/arm64`; another build into a clone: docs/kosmickrisp.md,
Building). KK runs set `XEMU_VK_DRIVER=kosmickrisp` (headless: bench-det arm
`kk=APP:XEMU_VK_DRIVER=kosmickrisp` with `--renderer offscreen`; bench-det
stops when a run's driver isn't the arm's); a windowed MoltenVK-vs-KK A/B
is bench-abx with one bundle that sets it on both arms (`--b-env
XEMU_VK_DRIVER=moltenvk --e-env XEMU_VK_DRIVER=kosmickrisp`; each arm
warms its own driver's cache). stderr's `xemu: Vulkan driver: <name>
(<why>)` and `- Driver: <name>, <version> (id N)` say which ran; the
windowed receipts list every driver an arm ran and refuse a mix. Each
driver keeps its own pipeline cache (`pipeline_cache.bin`,
`pipeline_cache-kosmickrisp.bin`), so no `--isolate-caches` for driver
A/Bs; offscreen RAM hashes are per driver. A KK run killed in its first
600 flips with `driver = 'kosmickrisp'`, or a strike test, leaves
`kosmickrisp-strike.txt` in the base path, which moves every later
`kosmickrisp` run of that build to MoltenVK (`XEMU_VK_DRIVER` runs ignore
it): the launchers warn when the bench cache has one (remove it), and a
strike test gets its own `XEMU_BASE_PATH`.

**Goldens** (deterministic; see the README's deterministic mode):
`python3 -I scripts/bench-det.py --check goldens x=APP` runs all of them
(heavy-f1 f300/f1200, stream-e2 f600, cold boot f900/f1800) in parallel
fresh processes, ~10 s, and asserts the hashes in `scripts/det-goldens.json`
(exit 0 OK, 4 moved, 2 arms disagree); `base=B.app new=E.app` gates two
binaries, `--check tags:FILE` sweeps tags. Re-check after any TCG, x87,
APU, PFIFO or timer change and after a PGO retrain.

Traps:

- bench-savestate-ab's B arm is always `VAR=0`; a numeric baseline or two
  knobs: bench-abx `--b-env`/`--e-env` with one bundle for both arms.
- Build cross-binary bases locally, never from CI, which adds PGO and
  stock MoltenVK.
- A crashed run voids its pair.
- Fullscreen captures come out black.
- DSP JIT: `[audio.dsp_jit] enabled = true` plus `XEMU_DSP_JIT_DIFF=1`
  prints a final validator line per core at `quit` (the worker drains
  first): it must report `checked>0 failures=0 pending=0`, with
  `enqueued` = checked + skipped (blocks with peripheral I/O a replay
  can't reproduce: det heavy-f1 GP 18 of 44, EP 13 of 86) and near
  `blocks_translated` from `XEMU_DSP_JIT_STATS=1` (coverage; `dropped`
  counts ring-full attempts, retried later). `XEMU_DSP_JIT_DIFF_SYNC=1`
  checks every translation inline (fine under icount).

**Anchors** (Azurik only; `../fixtures/README.md` is the source of truth):

| Tag | Role |
|---|---|
| `azurik-heavy-f1` | Primary heavy: ~650-680 draws/flip below the cap, ~576 at it. Usually at the 60 fps cap since the vCPU QoS change; single runs dip to 40-54 fps at load 31-43 (xemu-history, Capped anchors) |
| `azurik-heavy-w2` | Second heavy: ~306-323 draws/flip, vCPU-bound. Usually at the 60 fps cap, same dips as heavy-f1 |
| `vm-20260704032357` (F5) | Close to the 60 fps cap. Regressions and PGO only, never speedups |
| `azurik-stream-e2`, `-transition-f1`, `-movie-e2` | Walk-ins: streaming, area load, cutscene |

Retired: `vm-20260704173701` (F8), F6, F7, `azurik-heavy-e3`,
`azurik-heavy-w2-firewall`.

## Flags

`XEMU_*` vars are read once at launch. `vk/` is `hw/xbox/nv2a/pgraph/vk/`,
`mcpx/` is `hw/xbox/mcpx/`, `tcg/` is `accel/tcg/` (the top-level
`tcg/` is written out), `i386/` is `target/i386/tcg/`. To list them all:
`git grep -n 'getenv("XEMU_' -- '*.c' '*.cc' '*.m' '*.h'`.

| Var | Effect | Read in |
|---|---|---|
| *Off by default* | | |
| `XEMU_GUEST_MO=1` | Keep every guest memory-order barrier (normal mode drops the load-load ones and icount all, until a network backend attaches) | top-level `tcg/tcg-op-ldst.c` |
| `XEMU_SUBPAGE_FAST=1` | Non-Apple aarch64 opt-in for the sub-page store prefilter (arm b), unvalidated there; always on in Apple builds, inert on x86_64 | `tcg/xemu-subpage-fast.c` |
| `XEMU_SSE_HOST=2` | Differential self-check of the host SSE fast path (on by default with `hard_fpu` on every host), any host, which also keeps packed add/sub/mul/div off the inline path | `i386/fpu_helper.c` |
| `XEMU_STRICT_CPUID=1` | Instructions the guest CPU model lacks raise #UD (pentium3: SSE2, SSE3's FISTTP, LFENCE), reported on stderr and as QMP `XEMU_CPUID_FAULT`; by default TCG runs them. A new legacy 0F-map SSE opcode from an upstream merge needs a row in its table | `i386/xemu_strict_cpuid.c` (table: `i386/xemu-strict-cpuid.c.inc`) |
| `XEMU_PFIFO_PUT_LOCK=1` | Normal mode: the DMA_PUT doorbell and USER reads take `pfifo.lock` again | `hw/xbox/nv2a/nv2a.c` |
| `XEMU_PFIFO_STALL_DOORBELL=0` | Normal mode: a DMA_PUT doorbell inside a flip stall wakes the PFIFO thread again (by default it only publishes PUT) | `hw/xbox/nv2a/nv2a.c` |
| `XEMU_PFIFO_RING_FREEZE=0` | Normal mode: the pusher reads a restarted ring's unread tail from guest RAM again instead of the copy the doorbell froze (`pfifo_frozen.h`) | `hw/xbox/nv2a/nv2a.c` |
| `XEMU_NV2A_RESET_IRQ=0` | A machine reset (`system_reset`, and the one every load runs first) keeps the NV2A interrupt enables and PGRAPH's pending bits again, as v0.16 did, instead of clearing them as power-on does (see xemu-internals, NV2A interrupts across a reset) | `hw/xbox/nv2a/nv2a.c` |
| `XEMU_VCPU_QOS=0` | macOS normal mode: leave the vCPU and main-loop threads at default QoS instead of `USER_INTERACTIVE` (less P-core time and power, slower under load); Windows: leave the normal-mode vCPU, PFIFO and APU frame threads at normal priority instead of `THREAD_PRIORITY_ABOVE_NORMAL` | `tcg/tcg-accel-ops-mttcg.c`, `ui/xemu.c`, `hw/xbox/nv2a/pfifo.c`, `mcpx/apu/apu.c` |
| `XEMU_INTERP_GATE=0` | Metal backend: run the configured frame-interpolation mode every flip again, even when the display can't show its steps, with step holds as computed (by default a mode runs only while a guest frame lasts `mode` refreshes, e.g. 2x at ≤33 fps on 60 Hz; 4x drops to 2x, then off; steps held whole refreshes; nsprof `interp_degraded`, `interp_gated`) | `vk/display.c`, `ui/xemu.c` |
| `XEMU_OHCI_HCCA_FIRST=0` | Normal mode: an OHCI frame raises FNO/WDH/SF before its HCCA write-back and rewrites HccaDoneHead every frame again (the order behind Azurik's V22 boot hang; by default the write-back lands first and HccaDoneHead goes only with a done queue) | `hw/usb/hcd-ohci.c` |
| `XEMU_VBLANK_KQUEUE=0` | macOS normal mode: the vblank thread waits with `SDL_DelayPrecise` again (1 ms nanosleep chunks plus an overshoot spin) instead of one kqueue timer and a 25 µs spin | `ui/xemu.c` |
| `XEMU_PFIFO_LOCK_BATCH=0` | Normal mode: the threaded puller swaps `pfifo.lock` and `pgraph.lock` around every command again instead of keeping `pgraph.lock` across a batch | `hw/xbox/nv2a/nv2a.c` (the batching: `hw/xbox/nv2a/pfifo.c`) |
| `XEMU_PFIFO_FUSE=0` | Normal mode: the threaded pusher runs every command per word again instead of running leaf kelvin methods back to back under both locks | `hw/xbox/nv2a/nv2a.c` (the fusing: `hw/xbox/nv2a/pfifo.c`) |
| `XEMU_KI_IDLE_HALT=0` | Normal mode: the vCPU spins in the guest kernel's idle loop again instead of halting after 50 µs (icount always halts) | `i386/translate.c` |
| `XEMU_BQL_SPIN=0` | Normal mode: the vCPU sleeps on a contended BQL at once instead of polling it for up to 10 µs first (icount never spins) | `system/cpus.c` |
| `XEMU_VK_CLEAR_IN_PASS=0` | Vulkan: each clear ends the render pass and gets one of its own again, instead of recording into the open pass (pbkit text frames: ~1700 passes a flip again) | `vk/draw.c` |
| `XEMU_BASE_PATH=<dir>` | Data folder (shader/pipeline caches; `xemu.toml`/`eeprom.bin` when nothing names them), ahead of portable mode. The benches set it | `ui/xemu-settings.cc` |
| `XEMU_LOADVM_ADD_PADS=1` | A load or checkpoint restore that stops at a pad (hub) the machine lacks plugs it and loads again | `ui/xemu-input-bind.c`, `migration/savevm.c` |
| `XEMU_LANGUAGE=<fr\|de\|…\|n>` | The console language the guest reads (EEPROM reads; every load sets it in the kernel's EEPROM copy in RAM, so a load in another language differs from a plain one); QMP `xemu-set-language` while stopped; the EEPROM file stays untouched | `hw/xbox/smbus_storage.c`, `ui/xemu-language.c` |
| `XEMU_TRACE_RECORD=<file>` (+ `XEMU_TRACE_REGIONS`) | A line of guest memory per vblank, `# load` per load; regions are fixed or hook address expressions re-evaluated every line; QMP `xemu-trace-regions` swaps the list; never changes the run | `ui/xemu-trace-record.c` |
| `XEMU_ICOUNT_LIVE_PADS=1` | Under icount, host gamepads and keyboard drive their ports, read at vblanks (recordable); with `sleep=on`, exact idle warps paced on real time, `align=on` at most 0.1 s behind | `ui/xemu-input.c`, `accel/tcg/icount-common.c` |
| `XEMU_RENDER_NULL_RAM`, `XEMU_QMP_WATCH`, `XEMU_SOCKET_BACKLOG`, `XEMU_LOADVM_SHRINK_RAM`, `XEMU_AGP_FULL_DECODE`, `XEMU_CSTATUS_FULL_RAM` | Harness opt-ins that change a run (README; `RENDER_NULL_RAM` and `QMP_WATCH` also in xemu-internals). Launchers drop inherited ones | `vk/renderer.c`, `monitor/xemu-qmp-watch.c`, `chardev/char-socket.c`, `migration/ram.c`, `hw/xbox/xbox_pci.c`, `hw/xbox/nv2a/pfb.c` |
| `XEMU_VK_DRIVER=kosmickrisp\|moltenvk\|<dylib>` | Diagnostic: the Vulkan driver, overriding `display.vulkan.driver` and the MoltenVK pin (KK benches, windowed or headless; no strike marker); a failure exits 1. Launchers drop an inherited one | `vk/instance.c` |
| *Diagnostics (stderr)* | | |
| `XEMU_NV2A_NSPROF=1` | fps, draws, passes, fence every 5 s | `hw/xbox/nv2a/nsprof.c` |
| `XEMU_APU_PROF=1`, `XEMU_PFIFO_HEARTBEAT=1` | APU load; PFIFO liveness | `mcpx/apu/apu.c`, `hw/xbox/nv2a/pfifo.c` |
| `XEMU_LOAD_VERIFY=1` | One line when guest RAM changed between a load and the VM's start (or the next load) | `ui/xemu-load-verify.c` |
| `XEMU_GUEST_BUGCHECK=1` | A line and QMP event per guest KeBugCheckEx call (code, parameters, caller; found from the kernel's export table) and per HLT with interrupts disabled (the 4627 kernel's halt, EIP 0x800151EF) | `i386/system/xemu-bugcheck.c` |
| `XEMU_VTX_SPAN_REFUTE=1` | Checks every skipped vertex finish/wait against the draws' actual vertex bindings; exit line `xemu: vtx-span refute checks=N violations=M` (needs `quit`) | `vk/vertex.c` |
| `XEMU_DSP_JIT_STATS=1`, `_DIFF=N` | DSP JIT stats; validator | `mcpx/apu/dsp/interp/dsp56k_jit_arm64.c` |
| `XEMU_INV_PROF=1`, `XEMU_TB_PROF=1` | Exit dumps (need `quit`) | `tcg/tb-maint.c`, `tcg/cpu-exec.c` |
| `XEMU_BLOCK_LOG=<file>` | One TSV row per IDE/ATAPI data command (README; QMP `xemu-block-log-start`/`-stop`); 1-2% host time while on | `hw/ide/xemu-block-log.c` |
| `XEMU_VOICE_LOG=<file>`, `XEMU_AUDIO_WAV=<file>` | One TSV row per APU voice start/release/pause/resume/stop (buffer's physical runs, format, pitch, volume); the APU's output as a 48 kHz WAV, with no host device (README; QMP `xemu-voice-log-*`, `xemu-audio-wav-*`). Deterministic under icount | `mcpx/apu/xemu-audio-log.c` |

Never time fps with `*_REFUTE`, `XEMU_X87_CENSUS`, `XEMU_GUEST_PROF` or
`XEMU_MMIO_PROF` set.

| `xemu.toml` (`config_spec.yml`) | Default | Note |
|---|---|---|
| `audio.dsp_jit.enabled` | true | The only switch for the fork's DSP JIT (both cores, GP and EP; AArch64 POSIX hosts). Off falls back to `use_dsp_jit` |
| `audio.use_dsp_jit` | true | Upstream's JIT. The fork owns this default (upstream ships false); re-check it after merges |
| `display.window.presentation_backend` | auto | Metal under Vulkan; restart to change |
| `display.vulkan.driver` | auto | macOS arm64: Auto runs MoltenVK; `kosmickrisp` opts in to KosmicKrisp on macOS 26 (Metal 4, Metal presentation), else or on failure (or after a strike, until picked again) MoltenVK; icount and offscreen pin MoltenVK; restart to change |
| `automation.ram_shm`, `.qmp`, `.enabled` | "", "", true | The player's tracker endpoints (as `XEMU_RAM_SHM`, `-qmp`); applied only to the pref dir's config with no `-config_path`, `XEMU_BASE_PATH` or marker (README) |

Add a knob only for a normal-mode change the det goldens can't verify, a
diagnostic, a harness interface or a user tuning knob (CLAUDE.md). Read
`getenv` once into a static, latch it before the first TB if it changes
emitted code, and give POSIX-only code a `_WIN32` branch.

**MVK parity:** six `MVK_CONFIG_*` values live in `Info.plist`
`LSEnvironment` (applied only by `open`/Finder) and in the `setenv(…, 0)`
calls in `ui/xemu.c`'s `main()`. Add new ones to both, or shell tests
silently diverge, which is how the pink-tile bug hid. Exported values win.

## Ship

- Commit straight to `master` (no PRs; merges only from
  upstream). Title `<subsystem>: <imperative summary>` (see
  `git log --oneline -- <path>`). Body: why, what, numbers, then the
  harness `Co-Authored-By:` trailer. Push only when asked, then
  `gh run list --repo MichaelJSr/xemu-macos --branch master --limit 3`.
  CI also builds Windows and Linux, so `#ifdef` platform-only code in the
  same commit.
- **Release** (when the owner asks):
  `git tag -a vX.Y.Z -m "vX.Y.Z — <headline>" && git push origin vX.Y.Z`.
  `release-on-tag.yml` builds a **draft** release (17 assets). The owner
  writes short notes (`docs/RELEASING-macos.md`) and publishes.
- **Never delete, move, force-push or reuse a published tag.** Deleting one
  turns its release back into a draft. Moving one re-runs the workflow,
  which overwrites the published assets unless you `gh run cancel` it.
  Release a new version instead.
