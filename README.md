# xemu-macos: Optimized Xbox Emulator for Apple Silicon

Fork of [xemu](https://github.com/xemu-project/xemu) tuned for Apple Silicon
(M1/M2+) on macOS 14+ (best on macOS 26 Tahoe). Adds Vulkan on macOS
(MoltenVK, with Mesa KosmicKrisp as an opt-in on macOS 26; unsupported
upstream), an inline ARM64 x87 FPU, full DSP JIT, MetalFX upscaling +
frame interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

## Documentation map

| Doc | Purpose |
|---|---|
| This README | What the fork is, how to build/run/configure it, troubleshooting |
| [docs/optimizations.md](docs/optimizations.md) | The optimization ledger: every landed change with its receipts, the runtime knob catalog (diagnostics, tuning and the few normal-mode switches), and every attempted-and-settled experiment |
| [docs/roadmap.md](docs/roadmap.md) | Open candidates: the ranked performance roadmap and feature/quality vectors, each with its decision inputs |
| [docs/RELEASING-macos.md](docs/RELEASING-macos.md) | Release runbook |
| [docs/README.md](docs/README.md) | Index of every fork doc: campaigns in progress, future proposals, finished designs and audits (e.g. the [DSP56300 ARM64 JIT design](docs/Finished/dsp-jit-design.md)) |

---

## Quick start

Prerequisites: macOS 14.0+ (macOS 26 for MetalFX + frame interpolation),
Apple Silicon, Xcode Command Line Tools,
`brew install pkg-config ninja cmake dylibbundler`,
`pip3 install pyyaml requests`, and network on the first build.

```bash
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git submodule update --init --recursive
./build.sh
open dist/xemu.app   # your own interactive launch: it reads your xemu.toml
```

Clean rebuild: `rm -rf macos-libs macos-pkgs build dist && ./build.sh`.
Automation never launches xemu this way: agents, benches and test
harnesses run bundle clones marked `-name xemu-fork-auto`, with
`XEMU_BASE_PATH` and their own `-config_path` (Headless mode
(automation), "Automated-run marker").

### Building for Windows

The upstream Windows build paths are preserved and CI-tested. All
portable optimizations (the Vulkan renderer and APU work, the
pfifo / BQL fixes, the `XEMU_NV2A_NSPROF` profiler, the hard FPU on
x86_64, whose hot x87 ops run inline on the host x87 unit under the
guest's precision and rounding) apply automatically; macOS-only pieces
(MetalFX, Metal/IOSurface presentation, vDSP, the ARM64 DSP JIT, the
AArch64 inline x87 FPU) compile out.

- **Native (MSYS2/MINGW64):** `./build.sh` from a MINGW64 shell. Release
  builds default to `-Dx86_version=3` (AVX2/BMI2/FMA) at `-O3` with the
  stack protector off, as on macOS and Linux (x86_64 only; Windows arm64
  stays at `-O2`).
  `XEMU_PGO` works on the stock MSYS2 GCC toolchain since 2026-09-08:
  `build.sh` probes the compiler family and drives gcov `.gcda` profiles
  (`-fprofile-generate` / `-fprofile-use`) instead of `llvm-profdata`,
  which cannot read them. The native flow was validated on 2026-10-18
  (det heavy-f1 −3.1% core cycles); CI's cross GCC cannot use a native
  profile, so `pgo/default.profdata` stays clang/arm64 only and a Windows
  `use` build needs its own `generate` run.
  Preferred route — first done for real on 2026-09-07 (Windows 10,
  NVIDIA TITAN Xp, Vulkan renderer renders correctly) and run again on
  2026-10-17/18. The exact `pacman` package list, the traps a fresh
  MSYS2 hits (CRLF checkout, Python ≥ 3.13 prefix, MSYS tar,
  `USERPROFILE`) and the validation-layer recipe are in
  [docs/Campaigns/windows-port-2026-09.md](docs/Campaigns/windows-port-2026-09.md)
  §1 (also `docs/windows-test-plan.md` §7.1); what still needs real
  hardware is [docs/windows-gating-audit.md](docs/windows-gating-audit.md) §W.
- **Cross (Docker):** `./build.sh -p win64-cross` from a Linux
  container with the `xemu-win64-toolchain` image and
  `CROSSPREFIX=x86_64-w64-mingw32.static-` set — see
  `.github/workflows/build-windows.yml` for exact image tags/env.
  Verified from an Apple Silicon Mac via colima
  (`colima start --vm-type vz --vz-rosetta`); use a copy of the tree
  kept under `$HOME`.

### Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use`. A trained profile is committed at `pgo/default.profdata` and applied by CI to arm64 release builds (+9.4% fps). Retrain after large code churn: `generate` build → `scripts/pgo-train.py` on a clone of the bundle → `use` build → commit the regenerated profdata. A `use` build prints a staleness summary (hot-path CFG-hash mismatches + commits since retrain) and warns when the profile has drifted — see the ledger |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Hardened-runtime codesign via `xemu.entitlements` |
| `XEMU_MOLTENVK_VERSION` | `1.4.2` | MoltenVK release auto-vendored into `macos-libs` when no usable system copy exists. This default is the single home for the expected version — macOS CI asserts the packaged bundle's provenance line matches it |
| `XEMU_KK_PREFIX` | unset | arm64: bundle `$XEMU_KK_PREFIX/lib/libvulkan_kosmickrisp.dylib` (a local `scripts/build-kosmickrisp.sh` install) instead of the pinned KosmicKrisp release asset |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for `scripts/build-moltenvk.sh` (the maintained optimized MoltenVK) |

Runtime diagnostic and tuning env vars (`XEMU_*`), and the few
normal-mode switches, are catalogued with the optimizations they belong
to in [docs/optimizations.md](docs/optimizations.md).

### How Vulkan is provisioned (all platforms)

The NV2A Vulkan renderer loads Vulkan at runtime through **volk** —
nothing links against a Vulkan library at build time.

- **macOS:** xemu dlopens the driver bundled in
  `xemu.app/Contents/Libraries/<arch>/` by path (`@rpath/…`, never a
  Vulkan loader's search): Mesa KosmicKrisp or MoltenVK (below).
  `build.sh` bundles `libMoltenVK.dylib`; one resolution rule
  (`resolve_moltenvk`): `macos-libs` (vendored), then `/usr/local`
  (Vulkan SDK / `scripts/build-moltenvk.sh`), then Homebrew — a dylib
  counts only if it has the target arch slice; when none exists the
  pinned official release is vendored automatically. `build.sh` logs
  the bundled version + UUID so tested-vs-shipped driver drift is
  visible. arm64 builds also bundle `libvulkan_kosmickrisp.dylib` from
  the pinned CI-built release asset (`kk_asset_sha256` in `build.sh`,
  cached in `macos-libs/arm64` and checked on every build) and log
  `Bundling KosmicKrisp from … (Mesa …)`. End users need nothing
  installed.
- **Windows:** volk loads the system `vulkan-1.dll` (ships with GPU
  drivers). No Vulkan ICD in a VM → automatic OpenGL fallback with a
  notification; test Vulkan on real hardware.
- **Linux:** the distro `libvulkan` loader + the GPU's ICD.

On Windows and Linux the renderer additionally calls `volkLoadDevice()`
after creating its `VkDevice` (since 2026-09-08), so `vkCmd*` and
friends enter the driver's ICD directly instead of going through the
loader trampoline. On macOS MoltenVK is the ICD and there is no
trampoline to bypass; KosmicKrisp hands out per-device trampolines, so
it gets `volkLoadDevice()` too.

**Vulkan driver on macOS.** `[display.vulkan] driver` (Settings >
Display > Vulkan driver on Apple silicon) picks it once per launch
(restart to change; a fallback never rewrites it): `auto` (the default)
and `moltenvk` run MoltenVK; `kosmickrisp` opts in to Mesa
KosmicKrisp from `Contents/Libraries/arm64/libvulkan_kosmickrisp.dylib`
on macOS 26 or later with a Metal 4 GPU and the Metal presentation
backend, and otherwise runs MoltenVK with a toast that says why.
MoltenVK stays bundled, so macOS 14/15 and Intel Macs keep working.
Against MoltenVK, KosmicKrisp peaks 0.4-0.6 GB lower in memory and
applies the NV2A texture LOD bias that MoltenVK drops; but xemu as a
whole uses no less CPU at the 60 fps cap, and KosmicKrisp hitches more
on shaders and pipeline states it hasn't seen yet, starts slower, draws
wireframe filled, can lose occlusion results under heavy host load, and
a lost device ends xemu ([docs/kosmickrisp.md](docs/kosmickrisp.md)).
KosmicKrisp must load, start and pass an acceptance check (its
copy-present path and every surface and texture format) before xemu
commits to it; any failure falls back to MoltenVK in the same launch
(`xemu: KosmicKrisp unavailable (<why>); using MoltenVK`). Deterministic
(`-icount`) and offscreen (`XEMU_HEADLESS=1`) runs always use MoltenVK.
stderr names the choice and the device: `xemu: Vulkan driver: MoltenVK
(auto)` or `KosmicKrisp (kosmickrisp)`, then `- Driver: MoltenVK, 1.4.3
(id 14)`. The Settings line names the driver in use (`In use: MoltenVK,
1.4.3`; `, restart to apply` after a change). Where KosmicKrisp can't
run, the line ends with why instead (`(KosmicKrisp: needs macOS 26)`,
`needs a Metal 4 GPU`, `needs Metal presentation`), and the combo is
greyed unless it is set to KosmicKrisp. Help > About has a `Vulkan:`
line.

With the setting at `kosmickrisp`, a fatal Vulkan error on KosmicKrisp,
or a crash while it starts or in its first frames, leaves a strike,
`kosmickrisp-strike.txt` in the data folder: until xemu or the bundled
KosmicKrisp changes, later launches start MoltenVK and say so in a
toast. Picking KosmicKrisp in Settings again removes the strike, for one
more try. Building and bundling KosmicKrisp, and `XEMU_VK_DRIVER` (a
diagnostic that overrides the setting and the pin):
[docs/kosmickrisp.md](docs/kosmickrisp.md).

### Recommended `xemu.toml`

`~/Library/Application Support/xemu/xemu/xemu.toml`:

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'        # off | spatial | temporal
frame_interpolation = '2x'      # off | 2x | 4x (see Performance)

[display.quality]
surface_scale = 2               # 1 = 640x480 ... 4 = 2560x1920

[display.vulkan]
driver = 'auto'                 # auto (MoltenVK) | kosmickrisp (opt-in) | moltenvk

[display.window]
presentation_backend = 'auto'   # auto | opengl | metal (auto = Metal w/ Vulkan renderer)
fullscreen_on_startup = true
fullscreen_exclusive = true
startup_size = '1920x1080'

[display.ui]
fit = 'stretch'

[perf]
hard_fpu = true                 # ARM64 inline x87
cache_shaders = true

[audio.dsp_jit]
enabled = true                  # ARM64 inline DSP JIT (the default)
```

In-app Settings covers the main toggles.

**Guest RAM.** `[sys] mem_limit` (Settings > System > System Memory) is
`'64'` (the default) or `'128'`, plus two experimental values, `'256'`
and `'512'` (see "More than 128 MiB" under Headless mode): stock kernels
use 64 MiB, NV2A DMA objects can then start anywhere in the first 256 MiB
(see "NV2A reads above 128 MiB"), and a savestate loads only into the RAM
size it was saved with.

### PAL 50 Hz (opt-in)

The guest vblank runs at 60 Hz by default, for every disc and EEPROM.
`[sys] vblank_rate` in `xemu.toml` (Settings > System > Guest Vblank)
changes that:

| `vblank_rate` | Guest vblank |
|---|---|
| `'60hz'` (default) | 60 Hz (16,666,666 ns), as before |
| `'eeprom'` | 50 Hz (20,000,000 ns) while the machine's EEPROM is a PAL50 console's: video standard PAL-I (`0x00800300`) without the PAL-60 flag (`0x00400000` in the video flags at `0x94`); 60 Hz otherwise |
| `'50hz'` | 50 Hz always |

PAL titles then run at PAL speed: Azurik's Europe release on a PAL50
EEPROM sets its frame-rate latch to 25 and plays about 25 player ticks
per guest second, as on a PAL50 console, instead of about 30. xemu
generates NTSC-M EEPROMs, so `'eeprom'` changes nothing until you use a
PAL EEPROM. The rate is latched when the machine starts, at every reset
and at every load (`loadvm`, `xemu-checkpoint-restore`, an incoming
migration: the EEPROM is in every snapshot, so a PAL50 state runs at 50
Hz under `'eeprom'` whatever EEPROM the launch named), never mid-run, so
deterministic replays stay exact; a guest that rewrites its EEPROM's
video standard gets the new rate at its next reboot. Everything that
counts frames counts these vblanks: `xemu-frame-advance` frames, `+N`
pipe input, the live pads' sampling, value stops. QMP
`query-xemu-frame` (and the `xemu-frame-advance` reply) report the
period as `vblank-ns`, so a harness converts frames to guest seconds
with it instead of assuming 60. stderr says `xemu: vblank 50 Hz
(sys.vblank_rate ...)` at launch whenever the key isn't `'60hz'`, and
again with `at load` or `at reset` whenever the rate changes, also
after a Settings change, which takes effect at the next reset or load
(back to `'60hz'` included). Present
pacing and the frame interpolation gate follow the guest's measured
frame rate already.

### Headless mode (automation)

For harnesses that drive xemu only through QMP, the gdbstub and
`XEMU_INPUT_PIPE`:

| `XEMU_HEADLESS` | Effect |
|---|---|
| unset or empty | Normal windowed xemu |
| `1` | Offscreen: hidden window, no Dock icon, never takes focus, no UI, no host audio. The guest still renders, so `screendump` returns real frames, at render resolution (no MetalFX upscaling or frame interpolation) |
| `null` | As `1`, plus the null renderer: no GPU work. Game logic, vblank timing, input, gdbstub and QMP work as usual; `screendump` returns the VGA scanout (usually blank). The configured renderer is not saved over |

At startup xemu prints `xemu: headless mode: offscreen` (or `null`) to
stderr. Screenshots come from QMP, windowed or headless:
`{"execute": "screendump", "arguments": {"filename": "/tmp/f.png", "format": "png"}}`
(default format `ppm`). The command returns once the file is written.
It writes the frame on screen, the guest's last flip, once the GPU has
finished compositing it; while no renderer frame covers the scanout
(right after a load, before the guest draws), it writes the guest RAM
there instead, at the video mode's size. Earlier builds could write the
previous screendump's picture while the GPU was still busy: most often
the first rendered frame after a load, and a stop at a gdbstub
breakpoint just after a flip.
It is not available from HMP. Config, portable mode, savestates, the HDD
and EEPROM and all command-line options behave as windowed. In-app
notifications go to stderr instead, as `xemu: notification: <text>`.

Other automation interfaces (windowed or headless):

- **gdbstub `m` reads** that run into an unmapped page (the game heap
  has guard pages between allocations) return the readable prefix, as
  the protocol allows, instead of `E14`.
- **`XEMU_INPUT_PIPE` lines** (keyboard pad, port 2 in the fixtures):
  `down <scancode>`, `up <scancode>`, `clear`, plus analog
  `axis lx|ly|rx|ry <-1..1>` and `trigger lt|rt <0..1>` (positive is
  right/up; `0` hands the axis back to the keys). Prefix any line with
  `@<frame>` or `+<frames>` to apply it at that frame instead of on
  arrival. An untagged `clear` releases every key and axis and drops
  every line still queued, so nothing written before it lands later
  (under icount it releases at the next vblank). Earlier builds kept the
  queue; `+0 clear` still does, releasing at the next vblank.
- **Pipe lines over QMP**: `xemu-input-queue {"lines": [...]}` queues
  the same lines with the same meaning as if written to the pipe at
  that moment, in one call of up to 262,144 lines (and under QEMU's
  64 MiB limit on one JSON request), so a long recording needs no 8 KiB
  pipe windows. Lines already written to the pipe are
  read first, so both keep their order; `XEMU_INPUT_PIPE` need not be
  set (the lines drive the keyboard pad, on whichever port
  `xemu-input-bind` gave the `keyboard` source; port 2 in the
  fixtures). Every line is checked first: one that isn't valid refuses
  the whole call as `line <n>: <why>` (n from 1) and queues nothing.
  Blank lines and `#` comments (as `XEMU_INPUT_RECORD` writes them)
  are skipped. It returns `{accepted, skipped, queued, frame}`: lines
  taken, lines skipped, the queue's length after the call, and the
  frame counter its `+N` lines count from. The queue is kept in the
  order the vblanks apply it (a batch in any order is sorted once), so
  a vblank costs the same however many lines wait behind it.
- **Input state**: QMP `query-xemu-input` returns `{frame, ports, keys,
  queued}`. `ports` gives each port's source and the `buttons` (`a`,
  `dpad-up`, `white`, ...), sticks `lx`/`ly`/`rx`/`ry` and triggers
  `lt`/`rt` its pad reports from the next vblank on, raw (a pipe value
  v reads as round(v × 32767); the guest reads triggers shifted right
  by 7). For the keyboard pad that is what the pipe holds once the lines
  due at that vblank apply, plus the host keyboard where it counts;
  for a host gamepad, its last reading. `keys` lists the scancodes the
  pipe holds then, `queued` the lines still waiting, each with the
  `frame` of the vblank that applies it (`frame + 1` is the next). It
  reads the pipe first and never changes the run.
- **Frames** count vblanks while the VM runs. QMP `query-xemu-frame`
  returns `{frame, running}`; `xemu-frame-advance {"frames": N}` runs N
  frames, pauses, and returns (`0` just pauses). It also starts a VM the
  gdbstub stopped, from the instruction it stopped at (lift a breakpoint
  under the PC first, or use `Qxemu.stepover:1`); a breakpoint or step
  that stops the VM during the advance ends it early with
  `debug-stop: true`. Any other stop it didn't make ends it early too,
  with the VM left as that stop left it and the reply's `stopped`
  naming the run state: `paused` when a gdbstub client attaches, sends
  ^C, or sends any byte while the VM runs (a breakpoint gives `debug`
  beside `debug-stop`), `shutdown` on SIGTERM. Earlier builds waited
  for vblanks that never came, holding QMP: the command never returned,
  new connections got no greeting or were refused even after its client
  gave up, and SIGTERM never ended xemu. A save or load (HMP `savevm`
  or `loadvm`, the Snapshots menu) doesn't end an advance, since it
  starts the VM again before it returns, unless it fails and leaves the
  VM stopped, as a failed HMP `loadvm` or `snapshot-load` does: the
  advance then ends with `stopped: restore-vm`, and the VM stays there
  until `cont` or a load that works. In real time, replays
  land within a frame or two: guest work per vblank depends on host
  speed. Deterministic mode (below) makes them bit-exact.
- **Picture age**: QMP `query-xemu-display` returns `{frame, loads,
  flips, flip-frame, flip-loads, scanout}`: the frame counter and load
  count now, the guest's flips so far (writes of the CRTC start address,
  `scanout`; Azurik makes two per present) and the frame counter and
  load count at the last one. `screendump` writes the frame that flip
  put on screen. When `flip-loads` is below `loads`, the guest has not
  flipped since the last load and the picture is the loaded state's
  own, as it was saved; Azurik's first flip comes 1-3 frames after a
  paused load. Two exceptions: a game that draws into the scanout
  before it flips (Azurik keeps one scanout buffer, resolves each frame
  into it, then flips) shows that frame to a screendump taken in
  between, and windowed frame interpolation can show a step between
  the last two flips. It only reads counters.
  Since 0.16.3 it also returns `frame-ends`: the guest's frame ends
  since launch (NV097 `FLIP_STALL`, what the GPU counters' `flips`
  count), kept whether or not counting runs, so the difference over
  a span of `frame` is the frame rate at no counting cost.
  Since 0.16.1 it also returns `picture-stale`, `picture-loads` and
  `picture-frame`. `picture-stale` (present whenever a renderer that
  draws runs, from the first query on, so a harness can detect the
  feature by the key) is true while the picture on screen predates the
  last load: no GPU write (a draw, clear or 2D blit) into the buffer on
  screen has counted since, where writes count only once a frame the
  guest began after the load has ended. `picture-loads` and
  `picture-frame` are the load count and frame counter at the last
  counted write (absent before the first since launch, and with the
  null renderer). Date a picture by `picture-stale`, not by
  `flip-loads`. A checkpoint restore is a load here, as in `loads`. The
  case `flip-loads` misses: a load drops the renderer's surfaces, and
  Azurik writes the same CRTC start every vblank and resolves each
  frame into it as the next begins, so for 1-3 vblanks after a restore
  it flips (so `flip-loads` equals `loads`) a frame built from the
  loaded RAM: the save's frame, upscaled from guest resolution at
  surface scale 2, older and softer than the one the run that went on
  from the save shows. A fresh process that restores the same
  checkpoint shows the same picture. A buffer on screen that only the
  CPU or the video overlay writes stays stale after a load until the
  GPU writes it, so bound a loop that advances until the picture is
  fresh.
- **Input recording**: `XEMU_INPUT_RECORD=<file>` writes, at every
  vblank, the changes in what one pad gave the guest
  (`XEMU_INPUT_RECORD_PORT=1-4`, default the first bound port) as
  `XEMU_INPUT_PIPE` lines tagged `+N` from the last load, each load
  opening a `# load` section. Buttons come out as `down`/`up` of their
  keyboard-map scancodes and sticks and triggers as values that give
  back the same 16-bit reading (a stick reaches -32768 at
  `-1.000030518`), so a gamepad session replays through the keyboard
  pad. While recording, pads change only at vblanks, as under icount,
  and stderr warns once per port when another port's pad gets input
  the recording leaves out.
  To replay: bind the keyboard pad to that port with the same keyboard
  map, load the same state paused, write the section's lines, advance.
  A recording made under icount replays bit-identically; one made in
  normal mode replays bit-identically from one replay to the next, and
  follows the original closely (Azurik's player within 0.001 units over
  240 frames) but not exactly, since guest time followed the host clock.
- **Pads across loads, and rebinding them**: an emulated pad reads its
  port's host source at every guest poll, so whatever is bound to a port
  (the keyboard pad, which the pipe drives, or a host gamepad) keeps
  driving the pad on that port across `loadvm`, checkpoint restores and
  `system_reset`, with no replug. The state must have been saved with a
  pad of the same type (Duke or S) on that port: the game doesn't see a
  pad the state lacks, so use the port the state was saved with (port 2
  for states saved with the keyboard pad there). QMP `xemu-input-bind
  {"port": 2, "source": "gamepad", "guid": "<32 hex digits>"}` changes a
  port's host source without unplugging its pad (`"keyboard"` or
  `"pipe"`: the keyboard pad; `"none"`: neutral input). A source bound to
  another port moves, and that port keeps its pad; a port with no pad
  gets one plugged (except with `XEMU_ICOUNT_LIVE_PADS=1`). The port is
  then pinned: its pad stays plugged while its host gamepad disconnects
  (neutral input meanwhile), the gamepad drives it again when it
  reconnects, auto-binding leaves it alone, and picking a device for it
  in the settings ends the pin. Nothing is saved to the config. Under
  icount and while recording, the change applies at the next vblank
  (`"pending": true` until then). A gamepad whose GUID isn't open yet is
  waited for. `query-xemu-input-bindings` lists the ports (source, pad,
  its type, pinned, pending) and the open gamepads with their GUIDs;
  host gamepads open only in normal mode and with
  `XEMU_ICOUNT_LIVE_PADS=1`.
- **Loads that plug missing pads**: with `XEMU_LOADVM_ADD_PADS=1`, a
  load (`loadvm`, QMP `snapshot-load`, the Snapshots menu, CLI
  `-loadvm`, `xemu-checkpoint-restore`) that stops at a pad hub or pad
  the machine lacks (`Unknown section or instance
  'pci.0:02.0/1.3/usb-hub'`) plugs it with no host source and loads
  again, and a pad of the other type (Duke or S) on that port is
  swapped for the one the state has. stderr says `xemu: loadvm: plugged
  ...`. Every attempt resets the machine and loads the whole state, so
  the result equals a load with the pad already there (azurik-heavy-f1
  without pad stubs gives the `f300` golden). CLI `-loadvm` works too:
  it runs once the config's pads are bound, just before `xemu: ready`
  (and before the VM starts, without `-S`), so it equals a `loadvm`
  sent after `xemu: ready` in the same config. Binding a source to that
  port afterwards (config, settings or `xemu-input-bind`) drives the
  plugged pad without a replug. Only pads and their hubs are plugged,
  never memory units.
- **Snapshots and the disc**: a savevm records the tray's disc path and
  a fingerprint of its image, the size and an XXH3-128 digest of every
  byte (two builds can differ in one level file rewritten in place, so
  nothing less tells them apart). It rides after the fields older xemu
  builds read, so either build loads the other's snapshots. Each image is
  hashed once per process (cached by path, size, mtime, ctime and inode),
  so the first savevm with a disc in the tray reads all of it, on up to 8
  threads, and prints `xemu: disc fingerprint: <path>: <n> bytes,
  xxh3-128 <hex> (<t> s)` (0.1-0.6 s for the 2.7 GB fixture disc at load
  50-65); later savevms cost what they did. A read projected past 10 s
  (a network share) is given up, and that snapshot has no fingerprint.
  `loadvm`, `snapshot-load` and CLI `-loadvm` still compare no discs and
  load with whatever is in the tray. The Snapshots menu (and F5-F8)
  loads with no dialog when the tray holds the same image under another
  path, such as a sandbox's clone, and says so on stderr: `xemu: snapshot
  '<tag>': loading with the tray's disc <tray>, the same as its disc
  <saved> (<how>)`. A snapshot saved before fingerprints is compared with
  its saved path's file while that file is unchanged since the save
  (mtime and ctime at or before the snapshot's date; on Windows, which
  has no change time, such snapshots always ask). Anything else asks,
  as before, and stderr says why (`... the tray holds <tray> (<reason>);
  asking`). In the dialog, Yes opens the saved image before the tray's
  disc comes out, so a Yes that fails leaves the tray as it was (`...;
  the tray keeps <tray>; not loaded`), and a saved image that no longer
  exists is marked missing and gets no Yes (Load Anyway or Cancel, with
  Cancel focused). With a disc in the tray the dialog also says what a
  load without the saved image means (the snapshot's RAM still runs the
  code the game loaded from the saved image while it reads files from
  the tray's disc, until a reset boots the tray's disc) and offers Load
  With Tray Disc, which loads as No does and says so on stderr: `xemu:
  snapshot '<tag>': loading with the tray's disc <tray> instead of its
  disc <saved>; RAM keeps that disc's code until a reset`.
- **The console language for a run**: `XEMU_LANGUAGE=<lang>` (from
  launch) or QMP `xemu-set-language {"language": "<lang>"}` (VM
  stopped; `"off"` gives the guest the EEPROM's own again; no argument
  only reports) sets the language the guest reads, where `<lang>` is
  `en ja de fr es it ko zh pt`, a name (`french`) or an XC_LANGUAGE
  number. The guest's EEPROM reads return it with a matching checksum,
  so a boot (cold, or a reset) reads it; and since the kernel keeps a
  copy of the EEPROM in guest RAM from its first read, the call and
  every later load (`loadvm`, `xemu-checkpoint-restore`, an incoming
  migration) set the language in that copy too, so every language
  query after a load returns it. That copy is guest RAM: a load of a
  state saved in another language then ends on other RAM than a plain
  load (8 bytes on one page; the same in every process, so replays stay
  exact), and a state already in that language loads unchanged. The
  device's contents, and with them savestates, stay the EEPROM's: a
  guest EEPROM write (the guest builds it from what it reads) lands
  with the EEPROM's own language and a checksum of its own, unless it
  sets another language itself. The EEPROM file is never written while
  the language is set (a guest EEPROM write stays in the machine;
  stderr says so once), and stderr names the language at launch and
  the copy a load set. A title that
  asks once per boot keeps what it got: Azurik asks in its `main()`
  (USA `0x58EA8`, Europe `0x58EC8`), and Europe also when it starts
  each Bink movie (`0xD03BC`, to pick the movie's audio track), so a
  loaded state's speech stays the language its session booted with
  until the next boot, while Europe's movies follow the setting.
  `XEMU_LANGUAGE` is a launcher automation variable: set it per run.
- **Demos as played (live pads under icount)**: with
  `XEMU_ICOUNT_LIVE_PADS=1` and `-icount shift=2,align=on,sleep=on`
  (windowed or headless), host gamepads, and the host keyboard on the
  keyboard pad, drive their ports as in normal mode while the run stays
  deterministic. They are read once per vblank, where pipe lines apply,
  so `XEMU_INPUT_RECORD` (set `XEMU_INPUT_RECORD_PORT` to the pad's
  port) holds exactly the input the guest got: its last section, written
  after a load of the same state in another process (`sleep=off` for
  speed), ends on the session's RAM, and the session's `savevm`s are
  icount saves. The recording holds one port, so keep every live source
  on it (the keyboard pad included, when the window has focus); stderr
  warns once per port when another port's pad gets input. Windowed runs
  replay bit-exactly on macOS, where the Vulkan push ring keeps the
  window's frame pulls out of a running frame; elsewhere, record runs
  that must replay headless (`XEMU_HEADLESS=1` or `null`). Idle time
  warps as with `sleep=off`, by exactly the time to the next timer, but
  only once real time gets there. `align=on` keeps the guest at most
  3 ms ahead of real time and at most 0.1 s behind: a scene the host
  can't run at full speed plays slower instead of racing to catch up
  later, and stderr says `xemu: icount live pads: the guest ran at N% of
  real time over the last 10 s` when it does. A gamepad that connects
  drives, from the next vblank, the port waiting for it
  (`xemu-input-bind`), else its configured port or (auto-bind) a free
  one, if that port's pad is already plugged; one that disconnects
  leaves its pad plugged with neutral input and its port waiting for it.
  Nothing is plugged or unplugged, which a replay could not repeat
  (except that a pad with a memory unit on its hub unplugs at a vblank,
  as in normal mode): `xemu-input-bind` refuses a port with no pad
  plugged, so use a port the loaded state has a pad on. Rebind with
  `xemu-input-bind` (applied at a vblank), not the settings menu. A
  heavy scene can run below 60 vblanks/s windowed (the Vulkan renderer's
  deterministic cost, which `sleep=off` doesn't beat either); the guest
  then plays slower, never more than 0.1 s behind, and stderr reports
  the shortfall.
- **Per-frame traces**: `xemu-frame-advance {"frames": N, "trace":
  [{"address": A, "size": S}, ...]}` also returns `trace`, one sample
  per vblank (`"trace-every": K` for every Kth, the last always
  included) with each region's bytes as hex, read before the guest sees
  that vblank. `"virtual": true` reads through CPU 0's page tables; a
  region that RAM doesn't fully back reads as `""`. Up to 256 regions
  of 1-4096 bytes, each ending at or below 4 GiB; exact under icount.
- **Run until a guest PC**: `xemu-run-until {"until": [{"pc": P,
  "cond": [...]}, ...], "hits": N, "frames": F}` runs a paused (or
  gdbstub-stopped) VM until execution reaches one of the PCs with all
  its conditions true for the Nth time (default 1), or for F vblanks,
  and pauses it. The reply's `reason` is `hit` (with `pc`, `index` of
  the entry, `hits`, `frame` and `regs`: eax..edi, eip, eflags), `budget`,
  `debug` (a gdbstub breakpoint stopped it first) or `stopped` (another
  stop first, in `stopped` as for `xemu-frame-advance`). A hit stops the VM
  before that instruction, outside the gdbstub, so a debugger that
  attaches finds it halted there with no stop to report. Arrivals that
  don't match never stop the VM. A condition is `{"reg": "ecx", "op":
  "eq", "value": V}` or `{"addr": E, "size": 1|2|4|8, "type": "u"|"s"|"f",
  "op": "eq"|"ne"|"lt"|"gt"|"le"|"ge", "value": V}`, where `E` is a
  register or constant plus offsets with up to 8 `[ ]` pointer reads,
  e.g. `"esp+0x14"` or `"[[ecx+0x20]]+0x15c"` (guest RAM through CPU 0's
  page tables; unreadable memory is false). The instruction the run
  starts at doesn't count, so repeated calls walk hit to hit; `trace`
  and `trace-every` work as for `xemu-frame-advance`. Under icount it is
  exact and repeatable, and a run cut into hits ends on the same RAM as
  one straight advance. The hooks are compiled into the translated code,
  so the fast paths stay on; gdbstub breakpoints are built the same way
  and keep working beside them. The first call at a PC retranslates its
  page once, and so does the first run through each page the PC maps to
  later (after a reset the guest maps its XBE only as it loads it, often
  over pages an earlier boot used); later calls there cost about 1-2 ms
  each.
- **Collect at guest PCs**: `xemu-hooks-set {"hooks": [{"pcs": [P,
  ...], "cond": [...], "key": {"addr": E, "size": N}, "collect":
  [{"addr": E, "size": N}, ...], "regs": true, "expect": [...]}]}`
  installs hooks (paused VM; at most 64) that copy guest memory at each
  hit, before that instruction runs, and leave the VM running. It
  returns each hook's `id`; `xemu-hooks-clear {"ids": [...]}` (default
  all) removes them and `query-xemu-hooks` lists them with their `hits`.
  A hit is an arrival at one of the `pcs` (1-16, numbered together, e.g.
  a function's RETs) with all of `cond` true (as for `xemu-run-until`)
  and, given `key` (1-64 bytes, e.g. `"ecx+0x18"`, an event clock), the
  key changed since the hook's last hit, so a repeated call in one tick
  is folded. `collect` takes up to 64 regions of 1-4096 bytes (64 KiB a
  hit) at address expressions as for `xemu-run-until`, such as
  `"[[[[0x15fe410+0x20]+0x154]+0xcc]+0x20]+0x38"`. The next
  `xemu-frame-advance` or `xemu-run-until` reply carries `hooks`:
  `records`, one per hit in execution order (`hook`, `pc`, `frame`,
  `hit` from 0, `data` with one hex string per region, `""` when
  unreadable, and `regs` with `"regs": true`), plus `dropped` (hits past
  262,144 waiting records or 16 MiB). `"frames": 0` just returns them,
  also to a VM the gdbstub stopped. `xemu-hooks-drain` returns them too
  (`{records, dropped}`) without pausing the VM: it may run on, its
  hooks recording from where the call cut, so polling it keeps every
  hit once, e.g. one record per player tick from a hook keyed on the
  event clock (sent during an advance, it waits for the advance's reply,
  which takes them). `expect` holds one hex string per hit that the
  hit's collected bytes, concatenated, must start with (`??` matches
  any byte, `""` checks nothing). A mismatch marks the record and ends
  the running advance before that instruction: `hooks` then has `stop`,
  the record, and `xemu-run-until` returns reason `mismatch` with `pc`
  and `regs`. With `"stop": false` it only marks the record. Under
  icount the records are exact and repeatable, and collect hooks never
  change guest RAM. Stopping and resuming at a
  mismatch gives no repeated or missing hit. gdbstub breakpoints and
  run-until at the same PC work alongside, and the hook still sees each
  arrival once. Hooks, hit numbers, keys and queues stay across loads,
  so clear and set them again to start over.
- **Write at guest PCs**: a hook with `"write": [{"addr": E, "size":
  N}, ...]` and `"set": [hex, ...]` writes, at each hit and before that
  instruction runs, the next entry of its queue over the write regions
  (concatenated, as `expect` covers `collect`; `??` keeps a byte, a
  shorter string writes only that much, `""` writes nothing). With a
  `key`, a repeated call in one tick takes no entry. Only guest RAM is
  written, the way a gdbstub write does it (translated code over it is
  dropped; page permissions don't apply); a region that doesn't resolve
  to RAM is skipped and the record says `"unwritten": true`. Records
  add `set` (which entry, from 0) or `"empty": true`. With
  `"stop-empty": true` the first hit with no entry left ends the advance
  before its instruction (`stop`; `xemu-run-until` reason `empty`), e.g.
  at the entry after a replay's last tick. `xemu-hooks-push {"id": N,
  "set": [...]}` appends entries while the VM is stopped (1,048,576 or
  64 MiB may wait); a hit stopped on an empty queue takes the first
  pushed entry when the VM resumes, and is recorded again with it. A
  hook's mismatch stop comes before its write, which lands on resume at
  the same instruction, so the stop shows what the check saw and a run
  cut by stops writes what an uncut run does. Replaying an input string
  as the Azurik harness does takes one `xemu-frame-advance` with three
  hooks: the pad's 18 floats at the player update's entry (`ENTRY` =
  0x8C230, decimal in JSON), `{"pcs": [ENTRY], "cond": [{"reg": "ecx",
  "op": "eq", "value": PLAYER}], "key": {"addr": "ecx+0x18", "size":
  8}, "write": [{"addr": PAD, "size": 72}], "set": [...],
  "stop-empty": true}` (`PAD`: the active ControllerState, resolved
  once); the camera where the game reads it (`CAMREAD` = 0x85276),
  `{"pcs": [CAMREAD], "key": ..., "write": [{"addr": "esp+0x14",
  "size": 8}], "set": [...]}`; and the observation, a collect hook at
  the update's RETs. Under icount the same queues give the same RAM
  every run, equal to writing the same bytes over the gdbstub at the
  same instructions.
- **One-call candidate runs**: `xemu-run {"checkpoint": "c", "input":
  ["+0 axis ly 1.0", "+12 down 81", ...], "frames": F, "trace": [...],
  "trace-every": K, "stop-when": [...], "hits": N}` restores an
  `xemu-checkpoint-save` checkpoint (optional; incrementally, as
  `xemu-checkpoint-restore`'s `"incremental": true` does, unless
  `"incremental": false`), queues the input lines
  as if the pipe delivered them right after the restore (pipe syntax;
  `+N` counts vblanks from there, blank and `#` lines are skipped, and a
  malformed line refuses the call), then runs as `xemu-run-until` does,
  for F vblanks or until a `stop-when` entry fires, and pauses. The VM
  must be stopped (paused, prelaunch or held by the debugger). Nothing
  runs between the restore, the queueing and the start, so there is no
  pipe write to race the advance. A `stop-when` entry is either a PC,
  `{"pc": P, "cond": [...]}` as for `xemu-run-until` (`hits` counts
  them, default 1), or a value, `{"addr": E, "size": 1|2|4|8, "type":
  "u"|"s"|"f", "lo": A, "hi": B}` (either bound optional), which fires
  at the first vblank where the value lies outside [A, B] (a NaN or an
  unreadable value counts as outside), or `{"addr": E, ..., "change":
  true}`, which fires when its bytes differ from the run's start. Values
  are read at each vblank, where the trace samples are, so the run ends
  on that vblank, sampled. The reply is `xemu-run-until`'s (`reason`,
  now also `value`; `index`, the `stop-when` entry that fired; `pc` and
  `regs` at a hit; `frame`, `hits`, `trace`, `hooks`) plus `frames` (run
  by the call) and, at a value stop, `data` and `previous` (the value's
  bytes as hex, `""` when unreadable). Under icount the same checkpoint,
  input and stops give the same RAM, reply and trace every time (but for
  `frame` numbers: the counter never rewinds) while no `xemu-hooks-set`
  hook is installed, as hooks keep their hit numbers, keys and queues
  across the restore (clear and set them again before each run to start
  over); and a run stopped anywhere, then continued, ends on the same
  RAM as an uncut one. A 16-frame candidate from a field state
  (azurik-stream-e2) takes about 100 ms on one core of an M2 Ultra at
  load 32, and a 1-frame one about 16 ms (20 ms with a full restore);
  stops add no measurable cost.
- **Trace recording**: `XEMU_TRACE_RECORD=<file>` writes a line per
  vblank while the VM runs, in any mode, windowed or headless:
  `<n> <frame> <hex per region>`, where `n` counts vblanks from the last
  load (the input recording's `+N`), `frame` is the process's frame
  counter and `-` stands for a region RAM doesn't fully back. A
  `# load frame=F` line marks each load and a `# regions ...` line the
  list. Regions come from `XEMU_TRACE_REGIONS=0x157b000:256,v0x10000:64`
  (`v` reads through CPU 0's page tables; 1-4096 bytes ending at or
  below 4 GiB, at most 256), and QMP `xemu-trace-regions {"regions":
  [{"address": A, "size": S, "virtual": true}]}` replaces them from the
  next vblank (an empty list leaves only the counts). An address may
  also be a hook address expression (as in `xemu-hooks-set`'s
  `collect`, `[ ]` up to 8 deep), such as
  `XEMU_TRACE_REGIONS='[[[[0x1BF63C]+0x2C]+0xC0]+0x180]+0x38:12'` or
  `{"addr": "[[0x1BF63C]+0x2C]+0xC0", "size": 4}` over QMP (a hook
  region object works as is): it is virtual, and evaluated afresh with
  CPU 0's registers and page tables for every line, so a row follows a
  pointer the game re-allocates (a respawn, a level change, a load) from
  the first line after the move, with no stale row and no pause. Its
  field is `?` when a dereference reads memory that isn't guest RAM
  (such as a null pointer; a pointer to a freed heap block still reads
  it) and `-` when the bytes it points at aren't all RAM. The
  `# regions` line writes it as `v<expression>:<size>` without blanks,
  so the line's items parse back as `XEMU_TRACE_REGIONS`. Under icount
  the line is taken at the vblank, before the guest sees it, where
  `xemu-frame-advance` samples its trace, from RAM as it is, so
  recording never changes the run: a live-pads session's lines equal its
  replay's (windowed, on macOS only: see live pads), and under the null
  renderer a traced frame advance of it. With a GPU renderer, a report
  slot the guest hasn't read yet holds old or new bytes (deterministic
  reports are lazy), while a traced frame advance first delivers the
  reports pending in the bytes it samples, as a guest read would, so
  compare a report slot's samples recording to recording. In normal
  mode the vCPU thread takes the sample between blocks of guest code
  around that vblank's interrupt: never mid-instruction, but where the
  guest is depends on host timing.
- **Readiness lines** on stderr: `xemu: ready` once the machine and pads
  exist (about 30 ms after launch in headless null mode, which starts
  no video or gamepad subsystem; `loadvm` works from then on, no fixed
  sleep needed) and `xemu: state loaded` after every `loadvm` or incoming
  migration.
- **QMP events** carry the same news, so nothing needs to poll the log
  (the lines stay):
  - `XEMU_READY`, once per launch, with `xemu: ready`. QMP sends events
    only to a client that has sent `qmp_capabilities`, and the socket
    answers about when xemu gets ready, so the event may go out first:
    after negotiating, call `query-xemu-status` (`{ready, loads}`) and
    wait for `XEMU_READY` only while `ready` is false.
  - `XEMU_STATE_LOADED {source, name, frame, loads}`, once per
    successful load, with `xemu: state loaded`: `source` is `loadvm`
    (any load of a disk-image snapshot; `name` is its tag),
    `checkpoint` (`xemu-checkpoint-restore`; `name` is the checkpoint)
    or `incoming` (an incoming migration, no `name`). `frame` is the
    `query-xemu-frame` counter at the load; `loads` counts this
    process's successful loads, this one included. It arrives before
    the reply of the `loadvm` (or `human-monitor-command`) or
    `xemu-checkpoint-restore` that caused it. After
    `migrate-incoming`, which replies before it loads, it is the signal
    that the load is done. A failed load sends nothing.
  - `XEMU_DEVICE_LOST {reason, where, vk-result}`, at most once, just
    before xemu dies of a lost Vulkan device: `reason` is
    `device-lost` (a headless xemu then prints `xemu: Vulkan device
    lost; exiting` and exits with status 1, a windowed one aborts) or
    `fence-timeout` (no fence within 60 s, after a `still waiting`
    line at 5 s; a headless xemu then prints
    `xemu: Vulkan call failed; exiting` and exits with status 1, a
    windowed one aborts). `where` names
    the fence wait, if a wait saw it. Best effort: the event is lost
    if the client left earlier output unread.
  - `XEMU_PRESENT_STALL {waited-ms, count}` (xemu-macos 0.16.1), windowed
    Metal presentation only: once per stall, when a present has waited
    past `XEMU_PRESENT_STALL_MS` (default 100 ms) for a CAMetalLayer
    drawable. The window drops frames until one comes, and every stall
    prints `xemu: present stall N: no drawable for M ms; dropping frames
    until one comes` and `xemu: present stall N over after M ms, F
    frames dropped` on stderr; emulation, input and `screendump` go on,
    the menus wait for the next drawable (see Performance, frame
    interpolation).
- **Background priority** (macOS): a headless xemu started under
  `taskpolicy -b` stays in the background whatever it asks for. In null
  and offscreen mode alike every thread runs at priority 4, almost only
  on the efficiency cores, and its CPU time bills to background QoS:
  neither xemu's user-interactive thread requests nor null mode's app
  role lift it, so it takes no performance-core time from a windowed
  session beside it. Deterministic runs
  (`-icount shift=2,align=off,sleep=off`) give the same RAM as at normal
  priority, since their vblanks are virtual, but they and their QMP
  replies go only as fast as idle efficiency cores allow: on a host
  loaded on every core (load 95-150), a 6000-frame advance that took 3 s
  at normal priority took 3-16 minutes, launch to `xemu: ready` 1-111 s
  (null mode at normal priority: under 0.5 s), a `loadvm` 0.3-25 s
  (normal: 0.05 s) and QMP round trips up to 0.56 s, so give background
  runs longer timeouts. Real-time runs drop vblanks when starved.
  `taskpolicy -c utility` is the middle ground (priority 20,
  performance cores allowed).
- **Park and resume a machine** without touching the HDD's snapshots:
  QMP `stop`, then `migrate {"uri": "file:/path"}` (poll `query-migrate`
  for `completed`). Resume in a new xemu started with `-incoming defer`
  on an unchanged copy of the same HDD: wait for `xemu: ready`, run
  `migrate-incoming {"uri": "file:/path"}`, wait for `xemu: state
  loaded`, then `cont`.
- **Checkpoints in host memory**: QMP `xemu-checkpoint-save {"name":
  "c"}` keeps the whole machine state (what `savevm` writes) in host
  memory; `xemu-checkpoint-restore {"name": "c"}` loads it the way
  `loadvm` does (reset, load, disk sectors the guest wrote since put
  back, runs on only if it was running), any number of times, with the
  same determinism. `query-xemu-checkpoints` lists them,
  `xemu-checkpoint-delete` frees one. A checkpoint is its device state
  (about 6 MB, nv2a and APU mostly) plus a copy of each nonzero RAM page;
  the qcow2 is never written for the state. With 128 MiB a save takes
  about 10 ms and a restore about 8 ms on a lightly loaded M2 Ultra
  (about 15 ms each at load 34).
- **Branching from checkpoints**: `xemu-checkpoint-save {"name": "c1",
  "base": "c0"}` stores only the RAM pages that differ from checkpoint
  `c0` and shares the rest with it (deleting or replacing `c0` later
  keeps them): about 1,600 pages (6.5 MB) plus the device state after 16
  frames of play, in about 4 ms. `xemu-checkpoint-restore {"name": "c0",
  "incremental": true}` compares and writes back only the pages written
  since the last checkpoint save or restore (QEMU's migration dirty
  log, which this and `base` start) plus those the two checkpoints hold
  differently, and ends on the same machine as a full restore: about 2
  ms with nothing run, 4-5 ms after 16-128 frames (most of it the
  reset, the device state and rechecking translations, not RAM). The
  first one, and the first after a `loadvm`, an incoming migration or a
  failed restore, compares every page. Replies carry `ram-pages`
  (pages it holds itself), `base`, and for restores `pages-checked` and
  `pages-copied`. Add `"verify": true` to either command to also
  compare every page the log didn't name: `pages-missed` must be 0, or
  something wrote RAM behind the log's back (another process writing
  through `XEMU_RAM_SHM` does; nothing inside xemu should). The log
  stops when the last checkpoint is deleted.
- **Host cost of one instance** (headless null, icount, 128 MiB): paused,
  no thread runs (0.1% of a core, about 45 timer wakeups a second);
  advancing, one core, nearly all of it the vCPU thread (the APU, PFIFO
  and IDE run on it). The footprint is about 165 MB after a `loadvm` and
  215-230 MB after a few thousand frames. A full checkpoint adds about
  75 MB, one saved against a base its own pages plus about 7 MB of
  device state; deleting it gives that back, as a finished `loadvm` or
  `savevm` gives back its buffer. Page copies a deleted checkpoint
  shared with newer ones stay only while one uses them (a delete moves
  them out of page blocks that are at least half unused, about 2 ms with
  8 checkpoints): 8 kept from a chain of 10-frame saves, each against
  the last, held about 200 MB from the 8th save to the 120th. 16-frame
  candidate runs from one checkpoint (incremental restore, then advance)
  reach about 140 frames per CPU second on Azurik's heavy scene.
- **Guest RAM size per launch**: `-m 64` or `-m 128` on the command
  line overrides `[sys] mem_limit` for that run (a later `-m` wins), and
  `xemu-ram-digest`'s `ram-size` reports what the machine got. Stock
  retail kernels manage 64 MiB on either size; a 128 MiB machine just
  leaves the upper half to the BIOS memory test.
- **More than 128 MiB (experimental)**: `mem_limit` `'256'` or `'512'`
  (Settings > System > System Memory, rows marked experimental), or `-m`
  above 128 in multiples of 16 MiB up to 3840 MiB (where NV2A BAR1 and
  the other PCI devices start; `-m 192`, `256`, `512`, `1024` and `3840`
  tested), gives the CPU that much RAM, mapped from physical 0. Other
  sizes exit 1 with `-m N MiB: guest RAM above 128 MiB must be a
  multiple of 16 MiB, 3840 MiB at most`. It is useful only with a kernel
  or mod that uses it: stock kernels use 64 MiB (for GPU data placed
  there, see "NV2A reads above 128 MiB"). To the guest the
  machine looks like 128 MiB hardware: NV2A BAR1 (the GPU aperture), the
  VGA window and `NV_PFB_CSTATUS` cover the first 128 MiB.
  `XEMU_CSTATUS_FULL_RAM=1` makes `NV_PFB_CSTATUS`
  report the full size. The flash's X-codes program the NV2A through BAR0
  at `0x0F000000`, where RAM above 240 MiB lies, so BAR0 is mapped over
  that RAM until they move it. Stderr says `xemu: guest RAM N MiB
  (experimental above 128 MiB): ...`. The harness's 4627 flash
  (`Complex_4627v1.03.bin`, 1de4c87effe4) cold-boots Azurik to its title
  at 192, 256, 512, 1024 and 3840 MiB. Its kernel still counts 0x4000
  pages, and the first 128 MiB of RAM at the title are byte for byte a
  128 MiB run's (deterministic mode, null and offscreen); 256 and 512
  MiB also reach the title windowed. The Vulkan renderer's vertex mirror
  stays as big as guest RAM (host memory, touched only where vertices
  are). Savestates load only into a machine of the size they were saved
  on (but see "Savestates and RAM size").
- **NV2A reads above 128 MiB (experimental)**: at 256 MiB of guest RAM
  or more (`mem_limit` `'256'`/`'512'` or `-m`) the NV2A's mask on DMA
  object bases, the PCRTC start and the 2D blit offsets is 28 bits
  instead of 27, so a DMA object can sit in RAM's second 128 MiB, which
  real hardware can't do. Offsets added to a base (textures, vertices,
  3D surfaces) were never cut to 27 bits, so a base-0 object whose
  limit covers them reaches past 128 MiB at any size. The mask stays 28
  bits above 256 MiB (a wider one would shift the GPU's 0xF0000000 and
  D3D's AGP address forms) and 27 below it (a 28-bit base could pass the
  end of RAM); BAR1, the VGA window (which wraps a scanout above 128
  MiB) and `NV_PFB_CSTATUS` stay as above. Stderr says `xemu: NV2A wide
  addressing: 28 bits (guest RAM 256 MiB or more)`. A stock kernel
  manages 64 MiB, so a game's DMA objects lie below it and run as at 27
  bits (cold boots at 256 MiB reach the same RAM either way). The mask
  follows the RAM size alone, so savestates carry nothing new and load
  across releases at every size. v0.16.1 and older run a 256 MiB state
  at 27 bits, as does a state shrunk from 256 MiB or more into 144-240
  MiB (`XEMU_LOADVM_SHRINK_RAM`, below): a DMA base above 128 MiB then
  wraps for textures, vertices, the pushbuffer and 2D blits, and aborts
  the process as a render target.
- **Savestates and RAM size**: `loadvm` of a snapshot saved with another
  RAM size fails as before (`Size mismatch: pc.ram: 0x8000000 !=
  0x4000000 ...`). The error now ends with a line that names both sizes
  and how to get the right one: `xemu: this snapshot was saved with 128
  MiB of guest RAM; this machine has 64 MiB (mem_limit in xemu.toml, or
  -m on the command line). To load it, set mem_limit '128' in xemu.toml
  or pass -m 128, ...`. The line reaches the monitor reply, stderr for
  `-loadvm`, and the error popup for a menu load. With
  `XEMU_LOADVM_SHRINK_RAM=1`, a snapshot saved with more RAM loads into
  a smaller machine if nothing uses its RAM above the machine's size:
  every page there must be zero, except the 16 bytes the BIOS memory
  test leaves in each of `0x04555548-0x0455557B` and
  `0x06555548-0x0655557B`. Stderr then says `xemu:
  XEMU_LOADVM_SHRINK_RAM: loaded a 128 MiB snapshot into 64 MiB of guest
  RAM (above it: N zero pages, M with the BIOS memory test's
  footprints)`. Any other data refuses the load and names the first page
  that holds it. A stock kernel uses 64 MiB of a 128 MiB machine, so
  Azurik's 128 MiB states load this way into 64 MiB. Replayed
  deterministically, they reach the same RAM as the low 64 MiB of the
  128 MiB run.
- **Bulk RAM dumps**: QMP `xemu-dump-ram {"filename": "/tmp/ram.bin"}`
  writes all of guest RAM (64 or 128 MiB) in one write, ~20-60 ms for
  128 MiB, pausing the VM for the copy unless `"pause": false`. Add
  `"address"`/`"size"` for a physical range, or `"virtual": true` for a
  range of CPU 0's virtual addresses, where pages that no RAM backs read
  as zeros (counted in `unmapped`). `"gpu-sync"` (default on) first
  writes GPU-rendered surfaces back to RAM, as savevm does. `pmemsave`
  and `memsave` work too, 1 KiB at a time.
- **RAM digests**: QMP `xemu-ram-digest` returns `sha256`, the sha256 of
  all of guest RAM (what `xemu-dump-ram`'s file hashes to), with no
  file and no GPU write-back, pausing a running VM for it (~55 ms for
  128 MiB). `"pages": true` adds `page-hashes`: 16 hex digits per 4 KiB
  page in address order, the first 8 bytes of each page's sha256,
  hashed on all cores (~5 ms more). `"algorithm": "xxh3-128"` returns
  `xxh3-128` instead of `sha256`, the XXH3-128 of all of RAM (32 hex
  digits, as `xxh128sum` prints it; ~4 ms), and makes `page-hashes`
  each page's XXH3-64 (~1 ms): not cryptographic, but equal across
  identical runs and different after any changed byte.
  `"against": "<checkpoint>"` returns `differ`, the page
  numbers (address / 4096) whose bytes differ from the RAM that
  checkpoint holds (~5 ms; no `sha256` unless `"sha256": true`).
- **Load check**: with `XEMU_LOAD_VERIFY=1`, every load (`loadvm`,
  `xemu-checkpoint-restore`, an incoming migration) keeps a copy of
  guest RAM, and when the VM next starts, or the next load begins
  first, any page that changed meanwhile is reported in one stderr
  line: `xemu: load verify: RAM changed between the load and the VM
  start (frame F): N pages 0x... 0x...` (up to 8 listed). Nothing
  should change a loaded VM's RAM before it runs; your own writes in
  that window (gdbstub `M`, a `gpu-sync` dump under Vulkan) count too,
  while what the start itself does on the vCPU (overdue device timers,
  PFIFO catching up) does not. It costs a RAM-sized copy (64 or 128
  MiB) and a few ms per load and per start.
- **Zero-copy RAM**: `XEMU_RAM_SHM=<name>` (or the player's
  `[automation] ram_shm`, below) puts guest RAM in named shared memory
  that another process can map (Python:
  `multiprocessing.shared_memory.SharedMemory("<name>")`, then
  `resource_tracker.unregister`, or `track=False` from 3.13, so Python
  does not unlink it): POSIX shm `/<name>` on macOS and Linux, a named
  file mapping `<name>` on Windows (it lives while any process has it
  open, a reader included, so close readers before relaunching or the
  new xemu keeps private RAM). stderr says `xemu: guest RAM in
  shared memory /<name>`. A name a live xemu holds is never taken: the
  new xemu keeps private RAM and says why (`xemu: XEMU_RAM_SHM: /<name>
  is held by a live xemu (pid N); guest RAM stays private`, plus a
  notification). The holder keeps an flock on
  `/tmp/xemu-shm-<uid>/<name>.lock` while it runs; a name nobody holds
  (a crashed run) is replaced (`replaced a stale object`), unless its
  bytes still change (a running build from before the lock, skipped
  too). A build from before the lock takes no lock, so only its changing
  bytes protect it: while it is paused or stopped (`-S`, a gdbstub
  stop) its name is replaced, and its exit unlinks the name whoever
  holds it by then. At exit the name is unlinked only if it is still
  that xemu's object. Other failures to create it still end the launch. Savestates
  are unaffected.
- **`[automation]` in the player's `xemu.toml`** turns endpoints on for
  a tool that reads the running game, so a Dock or Finder launch needs
  no arguments:

  ```toml
  [automation]
  ram_shm = "azurik-live"                        # as XEMU_RAM_SHM
  qmp = "tcp:127.0.0.1:4444,server=on,wait=off"  # what -qmp takes
  ```

  The table applies only to the player's own launch: `xemu.toml` is the
  SDL pref dir's (no `-config_path`, no `XEMU_BASE_PATH`, not portable
  mode) and the guest name is not `xemu-fork-auto`. A harness's scratch
  config (its own `-config_path`) or a bench (`XEMU_BASE_PATH`) never
  takes the player's name or port, even from a copied toml. A non-empty
  `XEMU_RAM_SHM` overrides `ram_shm`, and a QMP monitor on the command
  line (`-qmp`, `-qmp-pretty`, `-mon mode=control`) overrides `qmp`.
  Each key that is set gets one stderr line, `xemu: [automation] <key> =
  "<value>": applied`, `ignored (<why>)` or `skipped: <why>`. An
  endpoint that can't open (something listens on that address and port,
  a live listener holds the unix socket path, a live xemu holds the
  name) is skipped with that line and an in-app notification, and xemu
  keeps running. On macOS a listener on another address doesn't count:
  with another program on `*:4444`, `127.0.0.1:4444` still opens (QEMU
  sets `SO_REUSEADDR`), the key says `applied`, and loopback connections
  to that port reach xemu. The monitor's `wait` is always off, so a
  launch never waits for a client. `open` without `-n` starts no second
  copy of a bundle id: while any windowed or offscreen xemu runs, a
  bench's or an agent's clone included (clones keep `app.xemu.xemu`;
  headless `null` runs don't count), it starts nothing, and a Dock
  launch may only bring that copy forward.
  Settings > General > Automation endpoints
  (`automation.enabled`, shown once a key is set) turns the table off
  without deleting it. A build without the table (upstream xemu, or a
  fork build from before it) drops it when it saves `xemu.toml` at exit.
- **gdbstub reads** may ask for up to 4 MiB per `m` (was 64 KiB), and the
  binary `x addr,len` read (GDB 16, advertised as `binary-upload+`)
  halves the bytes on the wire. In `Qqemu.PhyMemMode:1` a 150 MB sweep
  takes under a second.
- **gdbstub memory packets reach RAM and ROM only**: `m`, `M`, `x` and
  `qxemu.read`, virtual or in `Qqemu.PhyMemMode:1`, return `E14` (or
  stop at the readable prefix) on device MMIO, such as the NV2A at
  `0xFD000000`-`0xFDFFFFFF`, the APU, USB and the NIC, without touching
  the device, so a stray read or write can't abort xemu or change guest
  state. A physical address nothing backs is `E14` too (it read as
  zeros). HMP `x`/`xp` and QMP `pmemsave`/`memsave` still reach devices.
- **gdbstub step-over**: `Qxemu.stepover:1` (or `xemu.stepover+` in the
  client's `qSupported`; the stub advertises `Qxemu.stepover+`) makes a
  resume from a halt on an armed `Z0` breakpoint (`c`, `vCont;c`, or
  QMP `cont` and `xemu-frame-advance`) run the instruction under the PC
  first instead of reporting that breakpoint again. The stub single-steps
  it itself with the `s` flags (no IRQs, no timers), so the guest runs
  exactly as under `z0`/`s`/`Z0`/`c` (same RAM under icount), in one
  packet. `s` steps over a breakpoint under the PC in every build. Off by
  default and for each new connection; `Qxemu.stepover:0` turns it off,
  `qxemu.stepover` reads it.
- **gdbstub breakpoints cost nothing until hit**: a `Z0`/`Z1` breakpoint
  is compiled into the translated code as an `xemu-run-until` PC is, so
  an armed one leaves the rest of its page at full speed (it used to run
  one instruction per block: about 30% more host cycles per frame for a
  breakpoint on hot code), and only the first `Z0` at a PC retranslates
  its page; `z0` and a later `Z0` there invalidate nothing, so a
  `z0`/`s`/`Z0`/`c` stop costs about a quarter of what it did. DR7
  hardware breakpoints the guest sets itself still single-step their
  page.
- **gdbstub batched reads**: `qxemu.read:addr,len;addr,len;...`
  (advertised as `qxemu.read+`; hex, up to 4 MiB in all) answers many
  `m` reads in one round trip. The reply has one `;`-separated field per
  region, each what `m` would answer: the bytes in hex, the readable
  prefix when the region runs into an unmapped page, or `E14` when none
  of it is readable. A malformed request gets `E22`.
- **gdbstub watchpoints**: `Z2` (write), `Z3` (read) and `Z4` (access)
  stop the VM after the access, as on hardware, in normal and
  deterministic mode, with QMP answering throughout. Under icount a run
  with a watchpoint ends with the same RAM as one without. Each stop
  retranslates the block that hit. A hit on the frame the CPU pushes for
  an interrupt or exception (a watched stack slot) stops at the handler's
  first instruction, after the whole frame. A QMP resume (`cont`,
  `xemu-frame-advance`, which returns `debug-stop: true` at each hit)
  runs to the next hit as `c` does. (Before this fix the first hit wedged
  the vCPU thread, and QMP and the gdbstub went silent.)
- **Memory access log** (who reads or writes a field, without stops):
  QMP `xemu-watch-start {"ranges": [{"address": A, "size": S, "access":
  "w"}]}` logs every CPU load or store that overlaps a range while the
  VM runs (`xemu-frame-advance`, `cont`, the gdbstub). `"access"` is
  `r`, `w` or `rw`. A range is physical by default and then matches every
  mapping of that RAM (or device MMIO, for a range outside RAM);
  `"virtual": true` matches virtual addresses. Up to 64 ranges of 1 byte
  to 16 MiB, below 4 GiB. `xemu-watch-drain` returns the hits so far and
  keeps logging; `xemu-watch-stop` returns them and stops. A hit is
  `{frame, pc, address, physical, size, access, value}`: `pc` is the
  accessing instruction, and `value` holds the bytes in hex (at most 64),
  that is, what a read read and what a write left. MMIO hits have no
  `value`. A hit that reached RAM through another mapping of it, NV2A
  BAR1's window at `0xF0000000` (all of RAM), matches a physical range
  at its RAM address and also carries `ram-address`, the RAM address it
  reached (its offset in `XEMU_RAM_SHM` and in `xemu-dump-ram`'s file),
  while `physical` stays the bus address: a store at `0xF4100000` on 128
  MiB logs `physical` `0xf4100000`, `ram-address` `0x4100000`. Hits at
  RAM's own addresses have no `ram-address`, as before (on 128 MiB the
  window's upper half needs the bridge widened: next item). Hits past
  `"max"` (default 100000, up to 1000000) are only counted, in
  `dropped`. Under icount the log is the same in every run and
  the guest's RAM is the same as without it. Start the log while paused
  for an exact first instruction. Not logged: device DMA and page-table
  walks. An access that crosses a page boundary gives one hit per page.
  Every access to a watched page takes the slow path, and a logged
  write makes the vCPU leave its chain of translated blocks once to read
  the value back, so a busy page costs some speed. Beside gdb `Z2`-`Z4`
  watchpoints the log still sees every access, the one that hit
  included. Example: Azurik's player entity, `+0x18` (event
  clock, virtual): two dword stores every frame, from `pc` `0xab8a1` and
  `0xab8a4`.
- **NV2A BAR1's window onto RAM** (128 MiB): the kernel maps VA
  `0xF0000000`-`0xF7FFFFFF` onto NV2A BAR1, an alias of all of guest
  RAM, so VA `0xF4000000`-`0xF7FFFFFF` could reach RAM
  `0x04000000`-`0x07FFFFFF`, which the stock 4627 kernel never uses.
  But the AGP bridge in front of the NV2A (`00:1e.0`) forwards only its
  windows: the X-codes set the prefetchable one to
  `0xF0000000`-`0xF7FFFFFF`, then the kernel cuts it to
  `0xF0000000`-`0xF3FFFFFF` at any RAM size, so stores to the upper half
  reach nothing and are dropped (gdbstub reads there fail). Either the guest
  widens it again (PCI config `0x8000F024` = `0xF7F0F000`, which
  savestates keep), or `XEMU_AGP_FULL_DECODE=1` makes the bridge also
  forward what the NV2A decodes outside its windows (stderr: `xemu:
  XEMU_AGP_FULL_DECODE: ...`; real hardware doesn't, so it is off by
  default). The opt-in changes nothing where a window or another device
  decodes, nor the bridge's registers, savestates or deterministic
  replays. Through the window a store is a store to RAM for everything
  that tracks RAM: the dirty log (checkpoints, savevm), `xemu-ram-digest`,
  `xemu-dump-ram`, `XEMU_RAM_SHM`, translated code on the page, and
  `xemu-watch` physical ranges (with `ram-address`). Readers that follow
  the guest's page tables (hook `collect`, `key` and `cond` reads,
  `xemu-run` value stops, frame-advance and `XEMU_TRACE_RECORD` virtual
  regions, a virtual `xemu-dump-ram`) report the whole window unmapped,
  as before 0.16.1; with `XEMU_AGP_FULL_DECODE=1` they read the RAM it
  reaches, in the lower half `0xF0000000`-`0xF3FFFFFF` too and at 64 MiB
  as well. Without the opt-in, read such data at its RAM address.
- **Fixed clock**: `-rtc base=2001-01-01T00:00:00` pins the RTC on a
  fresh boot. After `loadvm` the RTC is the snapshot's plus the wall time
  since it was saved; with `-rtc clock=vm` a load continues from the
  date the snapshot saved, with no wall time added.
- **Strict CPUID**: QEMU's decoder raises #UD for a missing CPUID
  feature only on instructions whose table entry names one, so on the
  Xbox's `pentium3` (MMX and SSE, no SSE2) every SSE2 instruction but
  MFENCE and MOVNTI runs, as do SSE3's FISTTP and LFENCE, and code built
  for a later CPU works here but faults on a real Xbox.
  `XEMU_STRICT_CPUID=1` makes them raise #UD as the CPU model would (the
  legacy SSE/SSE2/SSE3/MMX opcodes by prefix, FISTTP and LFENCE; the
  inline x87 and host SSE fast paths sit behind the same decoder). Each
  fault prints `xemu: strict CPUID: #UD at <linear pc>: F2 0F 58 C1
  needs SSE2` and sends QMP `XEMU_CPUID_FAULT {pc, feature, opcode,
  count}` before the guest's #UD handler runs (the first 32 per launch;
  the Xbox kernel's handler usually bugchecks next). Off by default,
  where translation is unchanged. Retail Azurik and the cold boot never
  trip it (the deterministic goldens match with it on), and it is
  deterministic under icount.
- **Guest bugchecks**: `XEMU_GUEST_BUGCHECK=1` names a boot that stops
  in the kernel's halt (the 4627 kernel's `cli; hlt; jmp` at
  0x800151ED, where KeBugCheckEx and the double-fault handler end, so
  EIP reads 0x800151EF with every I/O counter frozen). xemu finds
  KeBugCheckEx (export 96; KeBugCheck, 95, calls it) in the export table
  of the image at 0x80010000 (`xemu: guest bugcheck: KeBugCheckEx at
  0x80019442 hooked` on the 4627 kernel) at each load, before the guest
  runs on, and in a boot at the second vblank that finds the image in
  place (a fixed frame under icount), and hooks it: each call prints
  `xemu: guest bugcheck 0x<code> (<p1>, <p2>, <p3>, <p4>) from <return
  address>` and a line of the registers at the call, and sends QMP
  `XEMU_GUEST_BUGCHECK {code, params, caller, count, regs, stack}`,
  where `stack` is 4 KiB of guest memory from ESP in hex (only in the
  event): for a 0x1E it holds the trap frame and the context record,
  each where the faulting EIP (p2) is followed by CS 0x0008; a HLT with
  interrupts disabled prints `xemu: guest halted with interrupts
  disabled at <eip> (esp <esp>)` and sends `XEMU_GUEST_HALTED {eip, esp,
  count}` (the first 16 of each per launch). Both only read guest
  state: the deterministic goldens match with it on. Without it, a
  halted 4627 guest still holds the bugcheck in RAM: KeBugCheckEx
  stores the code and parameters at 0x8003AB20-0x8003AB33 (five
  dwords, zero until a bugcheck) and halts with EAX = code, ECX/EDX/ESI/
  EDI = p1-p4 (read them with `qxemu.read` or `g` at the halt). A
  planted SSE2 instruction under `XEMU_STRICT_CPUID=1` gives 0x1E
  (KMODE_EXCEPTION_NOT_HANDLED: 0xC000001D, the instruction's address,
  ...).
- **Deterministic fast-forward**: add
  `-icount shift=2,align=off,sleep=off` (with `XEMU_HEADLESS=null`).
  Guest time then follows executed instructions, not the host clock, and
  runs as fast as the host allows. The same savestate, the same
  frame-tagged pipe input and the same QMP calls give bit-identical guest
  RAM after N frames: across runs, processes and pause points (verified
  on 128 MiB over 600 frames, including disc streaming). Under icount:
  - vblank, the APU (8 frames every 5.33 ms of guest time), PFIFO and
    PGRAPH, IDE completions and the RTC follow guest time; the RTC runs
    from 2001-01-01 unless `-rtc` says otherwise;
  - `xemu-frame-advance` stops on the vblank itself;
  - pads change only at vblanks and only from the pipe (physical pads
    and the host keyboard are ignored, unless `XEMU_ICOUNT_LIVE_PADS=1`
    reads them at vblanks); an untagged line applies at the
    next vblank, and an `@N` line read after frame N is warned about;
  - a state saved under icount carries what the run depends on beyond
    guest RAM and registers (resampler, filter and HRTF history, the
    APU schedule, AC'97, the RTC's sub-second phase, the field pin, the
    GPU's vertex array and lighting method state, pad reports and held
    pipe keys and axes), so a run continued past its
    `savevm` (or `xemu-checkpoint-save`) and every load of that save go
    on identically. A state saved without icount starts that host-side
    state as a fresh process does, with neutral pads. Pipe lines queued
    before a load are dropped either way, so write a run's input after
    its load. Start with `-S` so nothing runs before the load;
  - host audio is off, windowed too;
  - savestates cross modes: states saved without icount (the usual
    case) load and replay as above, and states saved under icount load
    in normal mode with guest time and the RTC date continuing from the
    save (v0.14 and earlier read their RTC about 56 years ahead: load and
    re-save here in normal mode first). Save with `sleep=off` (or under
    `XEMU_ICOUNT_LIVE_PADS=1`, whose saves are `sleep=off` saves); a state
    saved with sleep on or `shift=auto` loads anywhere but drops its warp
    deadlines;
  - frame numbers (`@N`, `query-xemu-frame`) count the current
    process's frames, so use `+N` after a load.

  Write tagged input while paused, then advance. `shift` is guest time
  per instruction (2^N ns): 1 is about a 733 MHz Pentium III, 2 runs
  ~1.5x faster and suits games that are not CPU-bound.
  Offscreen Vulkan (`XEMU_HEADLESS=1`) is deterministic too, screendumps
  included when taken paused, at about half the null renderer's speed:
  a GPU report reaches RAM when the guest or a device first touches its
  slot, when a hook, trace sample or value stop reads it, or at a pause,
  save or load, so hooks that read no pending report slot don't slow a
  run or change its pictures. Hash an offscreen run's RAM with
  `xemu-dump-ram` `"gpu-sync": false` (`gpu-sync`, `pmemsave` and
  `xemu-checkpoint-save` write GPU surfaces back, which changes later
  RAM: a checkpoint's restores agree with each other, but not with the
  run that went on from the save or one that never saved), and expect
  matches only on the same Mac, macOS and MoltenVK (offscreen runs
  always use it). A cheap reproducible screenshot of frame F of a null
  run: savevm there, load it in a short
  `XEMU_HEADLESS=1` icount run, advance a few frames, screendump. OpenGL
  is not deterministic.
  Keep the network off and bind no XMU.
- **Data folder**: `XEMU_BASE_PATH=<dir>` moves xemu's data folder (the
  `spirv_cache_v*/` shader cache, `pipeline_cache.bin` and the
  `vk_prewarm-*.bin` list of pipelines rebuilt at launch, plus
  `xemu.toml` and `eeprom.bin` when no `-config_path` or `eeprom_path`
  names them) to `<dir>`, created if missing, ahead of portable mode.
  Unset, it stays the portable folder or the SDL pref dir
  (`~/Library/Application Support/xemu/xemu/` on macOS); `-config_path`
  never moves it. stderr names it: `xemu_settings_get_base_path: base
  path: <dir>/`.
- **Automated-run marker**: xemus started by this fork's tooling carry
  `-name xemu-fork-auto` right after the binary (`ps -axo command` shows
  it; an interactive xemu never has it, and the marker keeps the
  player's `[automation]` table off). The macOS launchers in
  `scripts/` also run them at `nice -n 10` with a lease
  `/tmp/xemu-leases/<xemu pid>.json` and the bench cache as
  `XEMU_BASE_PATH`; the contract is in `scripts/xemu_lease.py`. Each
  holds one of `XEMU_FORK_SLOTS` (4) slots, an `flock` on
  `/tmp/xemu-leases/slots/<i>.lock` through a descriptor the xemu
  inherits, so the slot lasts as long as that xemu, even when its
  launcher is killed. A launch by hand takes a slot with `python3
  scripts/xemu_lease.py run -- <xemu> -config_path <toml> ...` (nice,
  marker, bench cache, lease; the xemu replaces the helper, same pid).
  While `/tmp/xemu-leases/owner-demo.json` names a running pid, the
  launchers and `run` start no new xemu: they wait, saying `lease: owner
  demo running (pid N; ...): no new xemu until it ends; waiting` once a
  minute, or exit 75 with `XEMU_OWNER_DEMO_WAIT=0`. The harness writes
  that file for each owner sitting: a JSON object with an integer
  `"pid"` (a process that lives as long as the sitting) and optional
  `"who"`, `"purpose"` and `"expected_end"` strings; a dead pid or a
  file that doesn't parse counts as no demo, so write it atomically
  (a temp file in the same folder, then rename). `python3
  scripts/xemu_lease.py` shows a running demo and the live leases.
  `XEMU_LEASE_DIR` moves the lease folder (tests only), and with it the
  leases and slots, so those launches don't count against the shared
  slots; the demo rule still holds there (`/tmp/xemu-leases/owner-demo.json`
  and the moved folder's own `owner-demo.json` both count). Every
  launch line in `scripts/`, the agent skills and the docs carries the
  marker or `XEMU_BASE_PATH` (the quick start's `open dist/xemu.app` is
  the user's own launch). The launchers (and the Windows bench) pass xemu
  none of the automation variables they inherited: `XEMU_RAM_SHM`,
  `XEMU_INPUT_PIPE`, `XEMU_INPUT_RECORD` and `_PORT`,
  `XEMU_TRACE_RECORD` and `_REGIONS`, `XEMU_BLOCK_LOG`,
  `XEMU_VOICE_LOG`, `XEMU_AUDIO_WAV`, `XEMU_ICOUNT_LIVE_PADS`,
  `XEMU_VK_DRIVER`, `XEMU_QMP_WATCH`, `XEMU_LANGUAGE`, and the harness
  opt-ins that change what a run does or costs (its RAM, cycles or QMP
  events): `XEMU_RENDER_NULL_RAM`,
  `XEMU_LOADVM_SHRINK_RAM`, `XEMU_AGP_FULL_DECODE`,
  `XEMU_CSTATUS_FULL_RAM`, `XEMU_SOCKET_BACKLOG`, `XEMU_STRICT_CPUID`,
  `XEMU_GUEST_BUGCHECK` (stderr: `lease: not passing the inherited
  ...`), so a bench never takes the owner's shared-RAM name, pipe, files
  or watch socket, leaves the MoltenVK pin, sets the guest's console
  language, or runs both arms under an opt-in someone exported; set one
  per run (`VAR=V` arguments, an arm's `K=V`) to use it. The normal-mode
  switches (`XEMU_VCPU_QOS` and the rest) pass through, because an A/B
  sets them per arm.
- **Unix socket paths** (`-qmp unix:`, `-monitor unix:`, `-gdb unix:`,
  `-chardev socket,path=`, `chardev-add`) are never taken from a live
  process. If a socket at the path still has a listener, the new one
  fails with `UNIX socket <path> is in use by a live listener: Address
  already in use` (at launch, xemu exits 1; the player's `[automation]
  qmp` is skipped instead), and the running one keeps its socket. A
  stale socket or a plain file is replaced, as before. When a listener
  closes, it unlinks the path only if the path is still its own socket
  file. On macOS the QMP monitor serves one client at a time and queues
  one more connection, so further connects fail with `ECONNREFUSED`
  while a client holds it (unless `XEMU_SOCKET_BACKLOG`, below). That
  means busy, not gone: the file is still there.
- **Listen backlog** (opt-in): `XEMU_SOCKET_BACKLOG=<n>` (1-4096)
  queues up to n connections (the kernel may cap it: macOS
  `kern.ipc.somaxconn`, 128 by default) on every socket chardev that
  listens (`-qmp`, `-monitor`, `XEMU_QMP_WATCH`, `-gdb`, a `server=on`
  `-serial` or `-chardev`) instead of 1, so connects made while a client
  holds a monitor wait instead of failing with `ECONNREFUSED`. Each
  queued connection is served, greeting first, once the ones before it
  close, including connections whose client gave up and closed while
  queued (their greeting goes nowhere). Unset, or a value outside
  1-4096 (stderr warns), keeps 1.
- **A watch monitor for queries during an advance** (opt-in):
  `XEMU_QMP_WATCH=<spec>` (what `-qmp` takes, e.g.
  `unix:/tmp/w.sock,server=on,wait=off`) opens one more QMP monitor
  (stderr: `xemu: QMP watch monitor on <spec>`; a spec that can't open
  ends xemu with status 1, as `-qmp` does). Every other monitor's
  commands run one at a time, and `xemu-frame-advance`, `xemu-run-until`
  and `xemu-run` hold them while they wait for vblanks, so a client on a
  second `-qmp` socket gets its greeting but no reply until the advance
  ends. The watch monitor answers at once instead, from the main loop:
  it negotiates as usual (it offers no `oob`), then answers only
  `qmp_capabilities`, `query-commands` (which lists just these),
  `query-version`, `query-status`, `query-xemu-frame`,
  `query-xemu-status`, `query-xemu-display`, `query-xemu-input` and
  `query-xemu-host-profile`, none of which waits, changes guest state or
  RAM, or reads guest time. Any other command gets `CommandNotFound`
  with a desc that names them. It receives events like any QMP monitor.
  Commands on the other monitors keep their order, so
  `xemu-hooks-drain` still never cuts into an advance.
- **Host-time profile**: QMP `query-xemu-host-profile` returns counters
  cumulative since launch: `translate-ns`/`translations` (host time
  translating guest code), `apu-ns`/`apu-ticks` (the APU's ticks under
  icount) and `idle-ns`/`idle-warps` (guest time a halted vCPU skipped
  under `sleep=off`, which costs no host time). Diff two queries for an
  interval. Disk request latency is in `query-blockstats`
  (`rd/wr/flush_total_time_ns`, per drive). `scripts/bench-det.py
  --timeline APP` builds a cold boot's timeline from them under icount:
  guest frames, host ms and where the host time went (vCPU CPU,
  translation, APU, disk requests, qcow2 growth, other threads) for each
  phase from power-on to steady play (kernel, logos, prophecy.bik,
  title, save list, level load, first player tick, steady). It finds
  the phases with gdbstub breakpoints and a frame-tagged menu walk
  once, then times replays of that walk with no debugger; `-n` sets the
  replays, and two or more arms give per-phase deltas. Replays must end
  on the marker pass's RAM (it says so), so the phases are the same
  frames in every arm. `--renderer offscreen` times Vulkan on a
  portable clone (cold shader caches); `--route dpad_left,a` starts a
  new game instead of loading the save under the cursor; `--dvd` boots
  another disc (a Europe disc gets Europe's event addresses, from its
  XBE entry; `--va` moves them for another release);
  `--hdd-io "write -z 0x800 0x200"` empties the scratch HDD's
  cache-partition table first, so the game formats a cache partition
  and copies its files to it again (writes, qcow2 growth), and
  `--hdd-io "reopen -o lazy-refcounts=on"` drops the qcow2's own
  ordering flushes while it allocates (a fifth of that boot's host time;
  QMP `human-monitor-command` `qemu-io ide0-hd0 "reopen -o
  lazy-refcounts=on"` before power-on does the same for any scratch
  image, which a killed xemu leaves marked dirty and the next open
  repairs);
  `--watch-movie` lets prophecy.bik play instead of skipping it with
  START (the fixture's disc ends it at once; the retail disc plays 5,542
  frames).
  `--from-state TAG` times a harness's boot-build path instead of
  power-on: launch on the config's disc, `loadvm TAG` paused, `--dvd`
  into the tray (`blockdev-change-medium`, as the Azurik harness's
  `reset_into` does), `--hdd-io` (after the load, whose disk revert
  would undo it), `system_reset`, then the same phases from the reset,
  with a `load` row (no frames) for the load, swap and reset. Each run
  is a fresh process, so its first-boot translation (0.6-0.9 s of the
  Azurik boot) is paid every time; a sandbox that boots again in the
  same process translates about a tenth of that.
- **NV2A workload counters** (GPU cost under the null renderer):
  `xemu-frame-advance {"frames": N, "gpu-counters": true}` adds
  `gpu-counters` to its reply: the NV2A work of the advance, from its
  start to its stop (`"gpu-counters-per-flip": true` adds one set per
  flip). `xemu-gpu-counters-start` (`{"per-flip": true}` optional),
  `query-xemu-gpu-counters` and `xemu-gpu-counters-stop` count across
  any run instead; one counting run at a time. The reply is `{flips,
  total, max, tail, per-flip, per-flip-dropped}`: a flip is a guest
  `FLIP_STALL` (the first holds only the work since counting began),
  `total` sums everything counted, `max` is each counter's largest
  value in one flip, `tail` the work after the last flip (in `total`,
  not a flip), `per-flip` at most 65,536 sets. Each set counts, per
  flip or summed:
  - `begin-end`: begin/end pairs (the pusher's squash of repeated
    BEGIN, DRAW_ARRAYS, END counts each);
  - `vertices` and its paths `vertices-draw-arrays` (DRAW_ARRAYS
    counts), `vertices-inline-array` (INLINE_ARRAY words × 4 over the
    vertex size of the enabled attributes), `vertices-array-element16`
    (two indices per word), `vertices-array-element32`,
    `vertices-immediate` (SET_VERTEX3F/4F, SET_VERTEX_DATA*);
  - primitives from each pair's vertex count n: `prim-points` (n),
    `prim-lines` (n/2), `prim-line-loop` (n segments), `prim-line-strip`
    (n-1), `prim-triangles` (n/3), `prim-triangle-strip` and
    `prim-triangle-fan` (n-2 triangles), `prim-quads` (n/4),
    `prim-quad-strip` ((n-2)/2 quads), `prim-polygon` (1), and
    `triangles`, the triangles they all make (two per quad, n-2 per
    polygon);
  - `pushbuffer-bytes`: pushbuffer words the pusher consumed (headers,
    data, jumps) × 4;
  - `vertex-bytes`: per draw and enabled array attribute, stride ×
    (last − first vertex) + element size (index and inline data come
    in the pushbuffer);
  - `texture-binds` and `texture-bytes-bound`: enabled texture stages
    per pair and their sizes (all levels, cube faces, palette), summed
    per pair; `texture-bytes`: the distinct textures (address, size,
    format, palette) bound in the flip, each once;
    `texture-bytes-uploaded`: those whose guest bytes (hashed at their
    first bind in the flip) changed since their previous bind in this
    counting run, what any renderer must upload again;
    `texture-bytes-new`: those first bound in this counting run;
  - `surface-bytes`: the distinct render targets pairs drew to in the
    flip (color with a color write mask bit set, zeta with depth or
    stencil writes on), each once, pitch × height with anti-aliasing;
  - `clears`, `clear-pixels` (the clear rectangle clamped to the
    surface, anti-aliasing samples included) and `clear-bytes` (pixels ×
    bytes per pixel of each surface cleared);
  - `reports` (GET_REPORT), `report-bytes` (16 per report, 4 per
    semaphore release), `blits` and `blit-pixels` (NV_IMAGE_BLIT);
  - `read-bytes-low`/`-high` (pushbuffer, vertex and texture bytes,
    blit sources) and `write-bytes-low`/`-high` (surface, clear and
    report bytes, blit destinations), split at 64 MiB physical, and
    `-high` for each source: data placed above 64 MiB shows there, not
    under a 26-bit alias (mem_limit 128, vanilla-ig-a1 with four bound
    textures' headers moved up 64 MiB: 92,888 bytes a flip in
    `texture-bytes-high` and `read-bytes-high`, nothing else high).
    Addresses come from the DMA objects as the renderers compute them;
  - `occlusion-samples`: the zpass pixel counts the renderer wrote into
    reports, when they reached guest RAM (always 0 under the null
    renderer; a fill estimate under Vulkan that depends on the driver
    and the surface scale; under `XEMU_RENDER_NULL_RAM` the counts the
    renderer measured, while RAM gets the null renderer's 0).

  Everything but `occlusion-samples` is counted in PGRAPH's method path
  and the pusher, never in a renderer, so the null and Vulkan renderers
  count the same for the same command stream; but where the game reads
  zpass reports (Azurik's visibility tests) the values Vulkan writes
  change what it draws next, so Vulkan runs can count differently from
  null ones. With `XEMU_RENDER_NULL_RAM=1` (next item) Vulkan keeps the
  null run's RAM and counts exactly what null counts, flip by flip,
  with real `occlusion-samples` (pack 03's e7 fight, 600 frames).
  Under icount, counting started and stopped while paused gives the
  same counts in every process (vanilla-ig-a1, 600 frames: identical
  in two null processes and under Vulkan). Counting writes
  nothing the guest sees: RAM digests are the same on and off. Off, it
  costs one predicted branch per pusher command and per counted method.
- **Pictures with a null run's RAM**: `XEMU_RENDER_NULL_RAM=1` (with
  the Vulkan renderer: offscreen `XEMU_HEADLESS=1`, or windowed) draws
  every frame but leaves guest RAM exactly as the null renderer does:
  each zpass report is 0 in RAM at its GET_REPORT, no rendered pixels
  are written back to RAM (also not at `savevm`), and image blits don't
  run. RAM digests then equal a null run's at every frame, and
  `screendump` shows the frames that state draws. stderr says `xemu:
  render null RAM: all`; any value but `1` and `request` is ignored,
  with a line saying so. A texture the game samples from a render target
  the renderer can't bind directly reads guest RAM, which holds no
  rendered pixels here, and blitted pictures are missing, as under null.
  In Azurik's e6 and e7 field states (the e7 fight included) the
  pictures equal plain offscreen's to the pixel (frames 294-611).
  `XEMU_RENDER_NULL_RAM=request` also skips draws, as the null renderer
  does, so a run costs about what a null run costs, until QMP
  `xemu-render-next {"flips": N}` (`render null RAM: request`): from the
  guest's next flip it draws whole frames and runs the VM, as
  `xemu-frame-advance` does, until N of them are drawn and the last is on
  screen, then pauses and returns the frame info with `render-flips`,
  the frames drawn by then (more than N when the guest drew another
  before the stop, as in normal mode). `"frames"` caps the vblanks
  (default 120); `render-flips` below N means the cap came first, as
  on a first request with cold shader caches in normal mode (a windowed
  KosmicKrisp run on fresh caches drew none in 120): raise it. Take the
  `screendump` then; the next run skips draws again. Without `flips` it
  only arms the draws for the VM's next run and the pause after it (or,
  given while the VM runs, until it is paused and started again; a load
  or save restarts the VM). A request the VM can't run (a failed load
  left in restore-vm, shutdown, an error state) fails and arms nothing.
  The first frame drawn lacks render targets earlier frames would have
  drawn; for Azurik `"flips": 2` matches a run that drew every frame to
  the pixel (e7 and e6 field states, 4-7 vblanks), `"flips": 1` within
  23-48 pixels by one level on a process's first request and to the
  pixel on later ones. RAM stays the null run's throughout (e7, e6, w1:
  the same digest at 600 frames from null, every-frame and request runs;
  checkpoint restores in e6, e7 and f3). A request needs `request` mode
  and the Vulkan renderer; under null (`XEMU_HEADLESS=null`) it fails.
- **Per-drive I/O log**: `XEMU_BLOCK_LOG=<file>` (from launch) or QMP
  `xemu-block-log-start {"filename": F}` (`"append": true` keeps F's rows)
  and `xemu-block-log-stop` (returns `{filename, rows}`) write one
  tab-separated row per IDE or ATAPI command that reads, writes, flushes
  or discards, under a header line: `frame drive op lba sectors bytes
  status cmd guest_ns`. `frame` (as in `query-xemu-frame`) and `guest_ns`
  (the virtual clock) are read when the command is issued; `drive` is
  query-blockstats' device name (`ide0-hd0`, `ide0-cd1`); `op` is `r`,
  `w`, `f` (flush) or `d` (discard); `lba` and `sectors` are what the
  command asked for, in 512-byte sectors on the HDD and 2048-byte ones on
  the DVD; `bytes` is what query-blockstats counted for the command, so
  over any window the rows add up to the blockstats deltas; `status` is
  `ok`, `err`, or `open` when the command was still running as the log
  closed; `cmd` is the ATA (HDD) or ATAPI packet (DVD) opcode in hex. No
  data. ATAPI replies sent by DMA (REQUEST SENSE, READ CAPACITY) count as
  reads in blockstats and show up as `r` rows with `lba` -1. One log at a
  time; a command already running when it starts is not logged. The file
  is flushed when the VM pauses, when the log stops, at exit, and at a row
  250 ms or more after the last flush. Under icount the file is
  deterministic like the RAM. A cold boot to steady play writes ~70,000
  rows and takes 1-2% more host time with the log on; off, it costs
  nothing measurable.
  `scripts/bench-det.py --timeline --block-log DIR` logs each timing run
  and checks the rows against every phase's blockstats.
- **Voice log** (audio checks at volume 0): `XEMU_VOICE_LOG=<file>` (from
  launch) or QMP `xemu-voice-log-start {"filename": F}` (`"append":
  true` keeps F's rows) and `xemu-voice-log-stop` (returns `{filename,
  rows}`) write one tab-separated row per APU voice event under a header
  line: `frame guest_ns voice event why phys bytes fmt ch flags ba cbo lbo
  ebo pitch hz vol bins wav_pos runs`. `event` is `start` (the guest's
  VOICE_ON), `release`, `pause`, `resume` or `stop`, and `why` says what
  stopped it: `guest` (VOICE_OFF), `end` (a buffer without loop, or a
  stream, ran out), `env` (the envelope finished its release) or
  `stream` (a stream without PERSIST left before its release). Only
  `start` rows are written for a voice that isn't playing, and a voice
  already playing when the log opens (a loaded state's) has no `start`.
  `phys` is the physical address of the voice's first sample byte and
  `bytes` its data's length (a buffer voice: BA through sample EBO; a
  stream voice: the SSL segment it is on); `runs` lists the data's
  physical extents in order as `0xADDR+LEN`, pages the VP's
  scatter-gather table maps contiguously merged (game buffers are
  scattered; at most 256 runs, then `...`), so `xemu-dump-ram` of each
  run gives back the exact bytes the voice plays. `fmt` is `u8`, `s16`,
  `s24`, `s32` or `adpcm`, `ch` 1 or 2, `flags` `l`(oop) `s`(tream)
  `p`(ersist) `m`(ultipass) or `-`; `ba` is the buffer's scatter-gather
  offset and `cbo`/`lbo`/`ebo` the current, loop and end sample offsets;
  `pitch` is the raw signed pitch (1/4096 octave from 48 kHz) and `hz`
  the sample rate it gives; `vol` is the 8 volume slots as hex
  attenuations in 1/64 dB (`fff` is silent) and `bins` their mixbins (a
  3D voice, 0-63, sends slots 0-3 to the HRTF submixes); `wav_pos` is
  the sample of the open WAV (below) the next APU frame lands on,
  else -1 (with `audio.use_dsp` on, a voice is heard from about
  `wav_pos + 259`, one EP period of DSP latency). `frame` is
  `query-xemu-frame`'s and `guest_ns` the virtual clock. Pitch and volume
  are the values at the event, not later changes. One log at a time; it is
  flushed when the VM pauses, when it stops, at exit, and at a row 250 ms
  or more after the last flush.
- **Audio to a WAV**: `XEMU_AUDIO_WAV=<file>` (from launch; xemu then
  opens no host audio device, windowed too, and prints `xemu: audio WAV:
  <file> (no host audio device)`) or QMP `xemu-audio-wav-start
  {"filename": F}` and `xemu-audio-wav-stop` (returns `{filename,
  samples}`) write what xemu would play, before `volume_limit`: 48 kHz
  stereo 16-bit PCM, one EP period (256 samples) at a time. Nothing is
  played aloud. Periods the APU doesn't run (VM paused, APU off) leave no
  gap, so the WAV's time is the APU's. The header's sizes are filled in
  at every second of audio, when the VM pauses, when it stops and at
  exit. One WAV at a time; a QMP-started one leaves a windowed run's host
  device as it was (mute it with `volume_limit = 0`).
  Both files are deterministic under icount like the RAM (same run, same
  bytes; a log or WAV stopped and restarted at a frame is the full run's
  file split there), neither changes what the guest sees, and neither
  costs anything measurable, on or off.
- **Lost GPU device**: macOS stops accepting a process's GPU work after
  repeated GPU errors; a headless xemu then prints `xemu: Vulkan device
  lost; exiting` and exits at once with status 1 (a windowed one aborts
  with a crash report). Any other failed Vulkan call prints `vk_result =
  N in <function> (<file>:<line>)`; headless then exits 1 after `xemu:
  Vulkan call failed; exiting`, windowed aborts. Earlier builds instead
  spun forever on the GPU thread at full CPU, answered no QMP and ignored
  SIGTERM (only SIGKILL ended them): that was the flip-stall "hang" after
  a `loadvm`. The errors behind it were GPU reads of freed textures: after
  a `loadvm` or reset freed every surface, the display compositor could
  keep sampling a freed one, and its command buffers could outlive the
  surfaces they sampled. Both are fixed. GPU errors are now logged when
  they happen (MoltenVK warnings, and `xemu: Metal command buffer "..."
  failed on the GPU` for the presenter, HUD and MetalFX), so a remaining
  cause shows up before the device is lost. `[display] renderer =
  'OPENGL'` avoids MoltenVK and the compositor entirely and works
  offscreen, but runs Azurik's heavy scene about 3x slower.

---

## Performance

Everything is measured with the interleaved savestate A/B protocol
(`scripts/bench-savestate-ab.sh`: monitor-loadvm fixtures, scene-gated
nsprof intervals, mid-run visual artifact checks, receipts) — full
numbers and methods live in the
[optimization ledger](docs/optimizations.md).

Headline arc on the heavy in-game bench scene of the 2026-07/08
campaigns (the retired F8 Azurik savestate, M2 Ultra, real fps — no
interpolation): **15.8 → ~48.1 fps** across them — the
occlusion/report rework (+144%), PGO (+9.4%), sub-page code dirty
tracking (+14%), cross-page TB chaining, inline dispatch probes, the
sticky SSE/NEON bracket, the async Metal present chain, the inline
NOTDIRTY store-skip, and the 2026-08-04 optimization wave.
Lighter scenes hold the 60 fps vblank cap.

MetalFX frame interpolation (`frame_interpolation`, Metal backend)
shows a generated midpoint before each real frame without changing the
simulation rate, but only as many steps as the window's display can
show: a mode runs while a guest frame lasts that many refreshes (2 for
2x, 4 for 4x; on from 90% of that, off below 75%). `4x` drops to 2x
when only 2x fits, and both turn off when not even 2x fits: the real
frames then go out as with interpolation off, and the interpolator's
GPU time is saved (~4 ms per frame at 2880x2160 on an M2 Ultra).

- 60 Hz panel: 2x runs while the game is at or below ~33 fps (once on,
  up to 40) and is off at the 60 fps cap. 4x never fits (frames longer
  than 50 ms count as hitches, which skip interpolation), so `4x` acts
  as 2x.
- 120 Hz panel: 2x runs up to the 60 fps cap; `4x` runs 4x at or below
  ~33 fps (once on, up to 40) and 2x above.

Each step is held whole refreshes: a 30 fps game shows the midpoint and
the real frame for one refresh each on a 60 Hz panel. MetalFX makes one
midpoint per real frame, so 4x shows it for three of the four steps and
holds the real frame back longer: 2x is the smoother setting wherever
both fit. With an unknown refresh rate, or on the OpenGL presentation
backend, the configured mode runs every frame, as it does with
`XEMU_INTERP_GATE=0` (which also drops the whole-refresh holds).

The window never waits long for the display: a helper thread asks
CAMetalLayer for each drawable and the UI thread, which also feeds the
pads and the input pipe and runs `screendump`, waits for it at most
100 ms (diagnostic: `XEMU_PRESENT_STALL_MS=<ms>` sets the bound, `0`
waits on the UI thread unbounded as v0.16 did, up to a second per frame
when the layer starves). A frame with no drawable in time is dropped (a
paced interpolation step or a real frame) and the loop keeps polling
input and serving `screendump` at about 60 Hz until drawables come back
(the menus and notifications draw into a drawable, so they wait).
Every stall is reported on stderr (a start and an end line) and as QMP
`XEMU_PRESENT_STALL`; nsprof counts the drops as `present_drop`.

The remaining wall is still guest-TCG throughput. The
superblock/region-formation campaign against the measured ~40%
dead-flag mass at block boundaries closed 2026-07-18 with five policies
measured negative; its TCG machinery was removed on 2026-10-10. The
constant-factor work above is what moved instead. The full record, the
honest verdicts, and what is still open live in
[docs/roadmap.md](docs/roadmap.md) and the
[ledger](docs/optimizations.md).

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Build fails in `download-macos-libs.py` with `tarfile.AbsoluteLinkError`.**
Python 3.14 changed tarfile's default extraction filter and MacPorts'
`dbus` bottle carries an absolute symlink. Fixed on 2026-10-10 (the
script asks for the `tar` filter); on an older checkout, pull or run the
build with a Python ≤ 3.13 first on `PATH`.

**Crash on launch with "duplicate LC_RPATH".** Older builds — rebuild;
`build.sh` now strips rpaths.

**Savestate from this build fails to load on an older xemu-macos.**
Expected: since the 2026-09-07 upstream merge the NV2A device state is
vmstate v4 (PTIMER alarm interrupts). Savestates written by older
builds still load here; the reverse does not work. Keep playing on the
newer build, or re-create the save there.

**Stutter for the first minutes after upgrading.** The SPIR-V shader
cache is keyed on the linked glslang's version (16.6.0 since
2026-10-10), so every shader compiles once into a fresh
`spirv_cache_v*/` directory on first launch. It settles as the cache
refills; the old directory can be deleted.

**MetalFX not activating.** Requires `renderer = 'VULKAN'` and
`metalfx_mode = 'spatial'` or `'temporal'`. Under the GL presentation
backend, `surface_scale = 4` produces 2560×1920 input and is skipped
(1920 px BGRA-IOSurface safe cap); the Metal backend's IOSurface-free
chain has no such cap. Check for
`MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.** Requires macOS 26.0+ and MetalFX
upscaling active.

**Interpolation ghosting on moving objects.** Should be fixed (motion
estimation + paced presentation order); report the title if it
persists.

**Black/garbled window or Metal init failure on launch.** Set
`[display.window] presentation_backend = 'opengl'` in `xemu.toml` to
fall back to the GL presentation path, and report the issue.

**More battery drain or fan noise on a laptop.** Since 2026-10-15 the
emulated CPU's thread runs at macOS's user-interactive QoS class, like
the GPU threads, so it keeps a performance core under background load;
since 2026-10-16 so does the emulator's main-loop thread, which mostly
sleeps. `XEMU_VCPU_QOS=0` leaves both at the default class.

**Hitches while other heavy apps load the GPU and every core.** Since
2026-10-16 the emulated CPU sleeps in the Xbox kernel's idle loop after
50 µs instead of spinning until the next interrupt, which frees a
fifth to two thirds of a core at the 60 fps cap. Waking it can take a
millisecond or more when no core is free; `XEMU_KI_IDLE_HALT=0`
restores the spin.

**A reset hangs on a black screen (v0.16 and older).** About 1 reset in
100 after loading a state (the Reset menu item, `system_reset`) never got
past the Xbox kernel: a GPU interrupt the stopped game had left pending
raised IRQ 3 again before the kernel set up its interrupt controller,
which lost it, so the kernel waited for the GPU forever. Since v0.16.1 a
reset clears the GPU's interrupt state as a cold boot does;
`XEMU_NV2A_RESET_IRQ=0` restores the old behaviour.

**FPU precision bug.** `[perf] hard_fpu = false`. Inline FPU uses IEEE
double (52-bit mantissa) vs x87 extended (64-bit); extremely rare.

**Depth artifacts after a render-target switch.** A depth surface
re-shaped at the same address skips its GPU→CPU readback, because
titles clear it before drawing. Report the title and the scene.

**Stale/late-updating textures.** Textures are content-checked once per
frame, so a CPU write between two binds of the same texture within one
frame lands a frame late. Report the title and the scene.

**Audio glitches.** For DSP-heavy titles, confirm DSP JIT is enabled.

**Vulkan renderer aborts at launch after an unclean exit.** A `.spv`
file in the on-disk shader cache torn by a kill or a power loss used to
abort every subsequent launch. Since 2026-09-08 the cache writes each
entry to a temp file and renames it into place, and validates every blob
it reads back — header, an instruction-stream walk, an `OpFunctionEnd`
trailer, then a SPIRV-Reflect parse — before the driver can see it,
deleting and recompiling anything that fails. Measured over all 2.4M
word-aligned truncations of a warm 394-entry cache: none reached the
driver and none aborted the launch. Deleting the `spirv_cache_v*/`
directory in the xemu data dir is still a valid manual reset, and
turning off Settings > Cache shaders to disk (`perf.cache_shaders`)
bypasses the cache entirely, for the Vulkan renderer too.

**Abort with `<file>:<line>: bounds check failed: <expr>`.** Since
2026-09-08 the Vulkan renderer's guest-driven bounds checks are real
checks in every build on every platform (they previously compiled to
`__builtin_unreachable()` in release, i.e. the out-of-range value was
used anyway). The message names the failing expression and its source
line: please report it with the title and the scene — it is a title
this fork has not seen, not a host problem. There is no runtime switch
for it on purpose.

**Windows: "Failed to initialize Vulkan renderer" / falls back to
OpenGL.** The Vulkan loader (`vulkan-1.dll`) comes from the GPU driver;
xemu bundles nothing. Update the vendor driver on real hardware. Inside
VMs there is usually no Vulkan ICD at all — the OpenGL fallback is
expected. `vulkaninfo` shows what the loader sees.

**Windows: presentation glitches, or a `win32_dxgi present failed or
unavailable` line in the log.** Windows always presents through the GL
path, and since the 2026-09-07 upstream merge that path drives a DXGI
flip-model swapchain via `WGL_NV_DX_interop`, so a vsync wait cannot
stall the pfifo thread behind the driver's GL swap lock. When that
presenter can't start, xemu logs that line once and swaps with
`SDL_GL_SwapWindow` instead (the pre-merge path, which gives the stall
back). Report the GPU/driver.

**Windows: black screen with working audio on Vulkan.** Fixed in
v0.13.2 — an invalid cross-stage uniform-buffer descriptor write that
native Vulkan drivers rendered as black (MoltenVK tolerated it). Update
to v0.13.2 or newer. As a fallback on older builds, set
`renderer = 'OPENGL'` in `xemu.toml`.

More `XEMU_*` switches (diagnostics, tuning, and the few normal-mode
switches) are listed with their owning changes in
[docs/optimizations.md](docs/optimizations.md).

---

## Architecture at a glance

```
Xbox game (30fps, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG, inline x87 → AArch64 FP
       │  floatx80↔double inline
       │
       ├─ GPU: NV2A Vulkan (MoltenVK; KosmicKrisp opt-in on macOS 26)
       │  flight slots · bump-alloc staging · VRAM spatial dirty index
       │  compute unswizzle + YUV · zero-copy display
       │  (Metal: exported MTLTexture or MTLHeap copy, async timeline submit ·
       │   GL: IOSurface + CGL)
       │        │
       │        └─ MetalFX Spatial / Temporal (async, MTLSharedEvent)
       │             │   Metal backend: private-texture ring → panel-fit
       │             │   (1280×960→2880×2160; works at scale 4) · GL: 1920 cap
       │             └─ MTLFXFrameInterpolator (30→60 or 30→120)
       │                  │
       │                  ├─ Metal backend: CAMetalLayer drawable +
       │                  │  MSL gamma blit + ImGui Metal (default)
       │                  └─ GL backend: CGLTexImageIOSurface2D → SDL3 GL
       │
       └─ APU: VP (per-voice pitch / vol) + DSP JIT (ARM64)
           linear resampler (1ch mono / 2ch stereo) · fused mixbin
           host output: SDL3 audio stream
```
