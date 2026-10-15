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
  `~/Library/Application Support/xemu/xemu/` (`xemu.toml`, `eeprom.bin`).
  Tests share only its shader/pipeline caches.
- **Every launch** uses a `cp -Rc` bundle clone, `cp -c` HDD and EEPROM
  clones, and `-config_path <scratch copy of ../fixtures/xemu.toml>`. xemu
  saves its config on exit, so skipping `-config_path` overwrites the
  owner's. (Adding a savestate: `../fixtures/README.md`.)
- **Mute:** per-run `[audio] volume_limit = 0` (`scripts/harness-config.py`).
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
and `meson.build`) doesn't update any bundle. To refresh a clone's binary
(a bare `cp` dies in dyld):

```sh
B=$W/xemu.app/Contents/MacOS/xemu L=@executable_path/../Libraries/arm64   # zsh
cp build/qemu-system-i386 $B && xattr -c $B
otool -L $B | awk '/\/opt\/local\//{print $1}' | while read d; do install_name_tool -change $d $L/${d:t} $B; done
otool -l $B | awk '/LC_RPATH/{f=1} f&&/path /{print $2;f=0}' | while read r; do install_name_tool -delete_rpath $r $B 2>/dev/null; done
install_name_tool -add_rpath $L/ $B && codesign -f --deep --preserve-metadata=entitlements,requirements,flags,runtime -s - $B
```

**PGO:** CI arm64 releases use the committed `pgo/default.profdata`;
locally, set `XEMU_PGO=use`. To retrain, build with `XEMU_PGO=generate`,
play, monitor `quit`, then rebuild with `use`.

**MoltenVK:** build.sh bundles the first arm64 dylib in
`macos-libs/<arch>/opt/local/lib`, `/usr/local/lib` or `/opt/homebrew/lib`,
and otherwise downloads `XEMU_MOLTENVK_VERSION` (1.4.2, which CI ships).
This Mac uses `/usr/local/lib` from `scripts/build-moltenvk.sh` (1.4.3, no
LTO). Look for `Bundling MoltenVK from …` in the log.

| Failure | Fix |
|---|---|
| `GLIB_SIZEOF_SIZE_T` mismatch | `rm -rf macos-libs macos-pkgs build dist && ./build.sh` |
| `libintl.h` missing at an old path (repo moved from `~/Documents/Xemu/tools/xemu-macos`) | `python3 scripts/download-macos-libs.py arm64` (build.sh runs it). Otherwise fix the include in `macos-libs/arm64/opt/local/include/glib-2.0/glib/gi18n.h`, or `rm -rf macos-libs macos-pkgs` |
| dyld: duplicate `LC_RPATH` | Rebuild, or use the refresh recipe |
| No `libMoltenVK.dylib` | Network, or `scripts/build-moltenvk.sh` |
| `SDK >= 14.0 not found` | Install a CLT with a `MacOSX<N>.<M>.sdk` |
| Moved wrap pin ignored | `rm -rf subprojects/<name>` (a dirty `roms/edk2` is harmless) |

## Run

The direct binary inherits only the shell env. `open -n -a <app> --env K=V
--stdout F --stderr G --args …` adds `LSEnvironment`, but then xemu hangs on
a hidden TCC prompt if its ROM/HDD paths are under `~/Documents` (the
fixtures are), so clone them to `/tmp`. The title
(`xemu | v<git describe>`) names the build.

```sh
W=$(mktemp -d /tmp/xr.XXXX) F=../fixtures
cp -Rc dist/xemu.app $W/; cp -c $F/xbox_hdd.qcow2 $W/hdd.qcow2; cp -c $F/eeprom.bin $W/eeprom.bin
sed -e "s|^hdd_path = .*|hdd_path = '$W/hdd.qcow2'|;s|^eeprom_path = .*|eeprom_path = '$W/eeprom.bin'|" \
    -e "s|^fullscreen_on_startup = .*|fullscreen_on_startup = false|;s|^screenshot_dir = .*|screenshot_dir = '$W'|" \
    $F/xemu.toml > $W/xemu.toml   # the fixture's screenshot_dir is personal
PADS=(${(f)"$(python3 -I scripts/harness-config.py $W/xemu.toml)"})   # mute + pad stubs (zsh)
XEMU_NV2A_NSPROF=1 $W/xemu.app/Contents/MacOS/xemu -config_path $W/xemu.toml \
  -monitor unix:$W/mon.sock,server,nowait $PADS > $W/log 2>&1 &
sleep 25; (echo 'loadvm azurik-heavy-f1'; sleep 3) | nc -U $W/mon.sock | grep -i error
```

- **Monitor:** `loadvm`/`savevm <tag>`, `info snapshots`, `info jit`,
  `quit` (flushes atexit dumps, PGO and config). Send `loadvm` after ~25 s:
  CLI `-loadvm` runs before the pad hubs exist (`Unknown section …usb-hub`).
  Use qcow2 tags; errors only reach the socket; socket path ≤104 bytes.
- **Snapshots** live in the HDD qcow2 (locked while xemu runs):
  `cp -c ../fixtures/xbox_hdd.qcow2 /tmp/s.qcow2 && python3 -I scripts/list_snapshots.py /tmp/s.qcow2`.
- **Input:** `XEMU_INPUT_PIPE=<fifo>` takes `down`/`up <sdl_scancode>` and
  `clear` lines (`scripts/inject_input.sh`) for the keyboard pad (fixture
  port 2), even unfocused. Keys: WASD 26/4/22/7, A 81, B 79, start 42.
  Writes to a dead xemu hang, so check `kill -0` first.
- `-dvd_path <iso>` applies to one run; other QEMU flags pass through.
  stderr carries diagnostics and stdout the QEMU command line. For `open`:
  `/usr/bin/log stream --predicate 'process == "xemu"'`.
- An `xemu.toml` in `Contents/Resources/` makes a bundle portable, caches
  included (`--isolate-caches`).

## Headless mode

Use it for every agent run that needs no eyes on the window: no focus
steal, no Dock icon, no host audio. `XEMU_HEADLESS=1` hides the window
and skips the UI, but the guest still renders. `XEMU_HEADLESS=null` adds
the null renderer, for logic, RAM and gdbstub work. stderr then shows
`xemu: headless mode: offscreen|null`. For frames, use QMP `screendump`
(PNG with `"format":"png"`); it works windowed too, but not from HMP.
Deterministic, bit-exact replays: add `-S -icount shift=2,align=off,sleep=off`
(null renderer), `loadvm`, write `+N` pipe lines while paused, then
`xemu-frame-advance`; same inputs give the same RAM (README). Compare runs by
hashing `pmemsave 0 0x8000000`.
fps runs stay windowed, because headless never presents and so measures
a different pipeline. Code: `xemu_headless_mode()` in `ui/xemu.c`.
README "Headless mode (automation)" lists the harness interfaces:
analog and frame-tagged pipe input, `xemu-frame-advance`, the `xemu:
ready` / `xemu: state loaded` lines, park/resume via `migrate file:`.

## Test

Bench scripts in `scripts/` accept `-h`. Harnesses read
`../fixtures/xemu.toml` (or `XEMU_TEST_CONFIG`), clone everything, mute,
and turn fullscreen off.

| Script | Use |
|---|---|
| `bench-det.py [-n pairs] [-f frames] [-t tag] [-i line] NAME=APP[:K=V,…] …` | Deterministic headless A/B: host Mcycles and Minsns per frame (load-robust), wall fps, RAM hash per run. Also runs the goldens |
| `bench-savestate-ab.sh <tag> <VAR> [pairs=3] [secs=75] [out]` | B `VAR=0` vs E `VAR=1` (`--e-value`); takes `--app`, `--isolate-caches`. Exit 3 = artifact |
| `bench-abx.sh <tag> [pairs] [secs] [out] --b-app P --e-app P [--b-env/--e-env "K=V"]` | Cross-binary, multi-var or numeric baseline |
| `movement-probe.sh <out> [--app P] [VAR=V…]` | Streaming walk; a healthy TB-flush delta is 0-1 |
| `probe-run.sh <app> <out> <tag>`, `soak-cycles.sh <app> <out>`, `boot-smoke-corpus.sh <iso>…` | atexit dumps; `loadvm` cycling; boot smoke |
| `nsprof_summarize.py`, `score_frames.py`, `bench-screenshot.py` | nsprof tables and A/B; magenta scoring; window capture |

No-game tests: `cd build && ninja test-xbox && ./pyvenv/bin/meson test --suite xbox --no-rebuild`.

**Lean default (CPU, TCG, APU):** `scripts/bench-det.py -n 4 base=A.app
new=A.app:XEMU_FOO=0` (~4 min). The box is usually loaded and that's fine:
judge by Mcycles/frame (back-to-back runs agree to ~0.5% at load 30;
absolute values drift over minutes, so compare arms only within one
interleaved session) and use Minsns/frame (~0.01%) as the mechanism
check. Arms must agree on the RAM hash.
**Renderer or pacing:** `scripts/bench-savestate-ab.sh azurik-heavy-f1
XEMU_FOO 2 45` (~6 min), windowed wall fps. Go by the verdict, mean ± sd
and sign tally, quote the load, and add pairs (3-6 × 75 s) when the delta
is under ~2% or signs are mixed: there's no quiet box to wait for.

**Goldens** (deterministic, `bench-det.py -n 1` on the final binary;
see the README's deterministic mode): `-f 300` (azurik-heavy-f1)
`f300=6a553d8c7727c6c6`; `-t azurik-stream-e2 -f 600 -i "axis ly 1.0"`
`f600=1e98a85ee122f87c`; `-t - -f 1800 --hash-at 900,1800` (cold boot)
`f900=9ffd0e1d2e1f36ea f1800=85dec4a26bcb49e9`. Re-check after any TCG,
x87, APU, PFIFO or timer change and after a PGO retrain.

Traps:

- bench-savestate-ab's B arm is always `0`; numeric knobs need bench-abx.
- Build cross-binary bases locally, never from CI, which adds PGO and
  stock MoltenVK.
- A crashed run voids its pair.
- Fullscreen captures come out black.
- DSP JIT: `[audio.dsp_jit] enabled = true` plus
  `XEMU_DSP_JIT_DIFF=1 XEMU_DSP_JIT_STATS=1` must report
  `checked>0 failures=0`.

**Anchors** (Azurik only; `../fixtures/README.md` is the source of truth):

| Tag | Role |
|---|---|
| `azurik-heavy-f1` | Primary heavy: ~679 draws/flip, ~29 fps on a loaded box |
| `azurik-heavy-w2` | Second heavy: 323 draws/flip, ~23 fps, cost isn't in draws |
| `vm-20260704032357` (F5) | Close to the 60 fps cap. Regressions and PGO only, never speedups |
| `azurik-stream-e2`, `-transition-f1`, `-movie-e2` | Walk-ins: streaming, area load, cutscene |

Retired: `vm-20260704173701` (F8), F6, F7, `azurik-heavy-e3`,
`azurik-heavy-w2-firewall`.

## Flags

`XEMU_*` vars are read once at launch. `vk/` is `hw/xbox/nv2a/pgraph/vk/`,
`mcpx/` is `hw/xbox/mcpx/`, `tcg/` is `accel/tcg/` (the top-level
`tcg/` is written out), `i386/` is `target/i386/tcg/`. To list them all:
`grep -rn 'getenv("XEMU_' accel target tcg hw ui audio system`.

| Var | Effect | Read in |
|---|---|---|
| *On by default; `=0` restores legacy* | | |
| `XEMU_SUBPAGE_FAST` | Sub-page invalidation, store prefilter (arm b) | `tcg/xemu-subpage-fast.c` |
| `XEMU_ELIDE_CANDOIO` | TCG store elision | `tcg/translator.c` |
| `XEMU_ICOUNT_STEP` | Icount budget cuts step one-instruction TBs instead of translating cut blocks | `tcg/cpu-exec.c` |
| *Off by default* | | |
| `XEMU_GUEST_MO=1` | Keep every guest memory-order barrier (normal mode drops the load-load ones and icount all, until a network backend attaches) | top-level `tcg/tcg-op-ldst.c` |
| `XEMU_SSE_HOST=1`, `=2` | x86_64: host SSE fast path (always on for aarch64 with `hard_fpu`); `=2`: differential self-check, any host. The `XEMU_SSE_NEON` alias is gone on every host | `i386/fpu_helper.c` |
| `XEMU_PFIFO_PUT_LOCK=1` | Normal mode: the DMA_PUT doorbell and USER reads take `pfifo.lock` again | `hw/xbox/nv2a/nv2a.c` |
| *Diagnostics (stderr)* | | |
| `XEMU_NV2A_NSPROF=1` | fps, draws, passes, fence every 5 s | `hw/xbox/nv2a/nsprof.c` |
| `XEMU_APU_PROF=1`, `XEMU_PFIFO_HEARTBEAT=1` | APU load; PFIFO liveness | `mcpx/apu/apu.c`, `hw/xbox/nv2a/pfifo.c` |
| `XEMU_DSP_JIT_STATS=1`, `_DIFF=N` | DSP JIT stats; validator | `mcpx/apu/dsp/interp/dsp56k_jit_arm64.c` |
| `XEMU_INV_PROF=1`, `XEMU_TB_PROF=1` | Exit dumps (need `quit`) | `tcg/tb-maint.c`, `tcg/cpu-exec.c` |

Never time fps with `*_REFUTE`, `XEMU_X87_CENSUS`, `XEMU_GUEST_PROF` or
`XEMU_MMIO_PROF` set.

| `xemu.toml` (`config_spec.yml`) | Default | Note |
|---|---|---|
| `audio.dsp_jit.enabled` | false | The only switch for the fork's DSP JIT (GP core; the EP stays on the interpreter) |
| `audio.use_dsp_jit` | true | Upstream's JIT. The fork owns this default (upstream ships false); re-check it after merges |
| `display.window.presentation_backend` | auto | Metal under Vulkan; restart to change |

To add a knob, read `getenv` once into a static, latch it before the first
TB if it changes emitted code, and give POSIX-only code a `_WIN32` branch.

**MVK parity:** six `MVK_CONFIG_*` values live in `Info.plist`
`LSEnvironment` (applied only by `open`/Finder) and in the `setenv(…, 0)`
calls in `ui/xemu.c`'s `main()`. Add new ones to both, or shell tests
silently diverge, which is how the pink-tile bug hid. Exported values win.

## Ship

- Commit straight to `macos-optimizations` (no PRs; merges only from
  upstream). Title `<subsystem>: <imperative summary>` (see
  `git log --oneline -- <path>`). Body: why, what, numbers, then the
  harness `Co-Authored-By:` trailer. Push only when asked, then
  `gh run list --repo MichaelJSr/xemu-macos --branch macos-optimizations --limit 3`.
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
