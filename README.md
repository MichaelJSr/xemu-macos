# xemu-macos: Optimized Xbox Emulator for Apple Silicon

Fork of [xemu](https://github.com/xemu-project/xemu) tuned for Apple Silicon
(M1/M2+) on macOS 14+ (best on macOS 26 Tahoe). Adds MoltenVK support
(unsupported upstream), an inline ARM64 x87 FPU, full DSP JIT, MetalFX
upscaling + frame interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

## Documentation map

| Doc | Purpose |
|---|---|
| This README | What the fork is, how to build/run/configure it, troubleshooting |
| [docs/optimizations.md](docs/optimizations.md) | The optimization ledger: every landed change with its receipts, the runtime escape-hatch/diagnostic knob catalog, and every attempted-and-settled experiment |
| [docs/roadmap.md](docs/roadmap.md) | Open candidates: the ranked performance roadmap and feature/quality vectors, each with its decision inputs |
| [docs/RELEASING-macos.md](docs/RELEASING-macos.md) | Release runbook |
| [docs/README.md](docs/README.md) | Index of every fork doc: campaigns in progress, future proposals, finished designs and audits (e.g. the [DSP56300 ARM64 JIT design](docs/Finished/dsp-jit-design.md)) |

---

## Quick start

Prerequisites: macOS 14.0+ (macOS 26 for MetalFX + frame interpolation),
Apple Silicon, Xcode Command Line Tools, Homebrew `pkg-config`.

```bash
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git checkout macos-optimizations
git submodule update --init --recursive
./build.sh
open dist/xemu.app
```

Clean rebuild: `rm -rf macos-libs macos-pkgs build dist && ./build.sh`.

### Building for Windows

The upstream Windows build paths are preserved and CI-tested. All
portable optimizations (the Vulkan renderer and APU work, the
pfifo / BQL fixes, the `XEMU_NV2A_NSPROF` profiler, the helper-based
hard FPU on x86_64) apply automatically; macOS-only pieces (MetalFX,
Metal/IOSurface presentation, vDSP, the ARM64 DSP JIT, the
AArch64 inline x87 FPU) compile out.

- **Native (MSYS2/MINGW64):** `./build.sh` from a MINGW64 shell. Release
  builds default to `-Dx86_version=3` (AVX2/BMI2/FMA) at meson's project
  default `optimization=2`, i.e. `-O2`; `XEMU_WIN_O3=1` opts into `-O3`
  with the stack protector disabled, which is experimental — no CI leg
  builds that configuration and the 2026-09-08 review names exactly that
  flip as the prime suspect for an intermittent crash on the Windows box.
  `XEMU_PGO` works on the stock MSYS2 GCC toolchain since 2026-09-08:
  `build.sh` probes the compiler family and drives gcov `.gcda` profiles
  (`-fprofile-generate` / `-fprofile-use`) instead of `llvm-profdata`,
  which cannot read them — but that arm has not been exercised on
  Windows yet, and the committed `pgo/default.profdata` is a clang
  profile, so a Windows `use` build needs its own `generate` run.
  Preferred route — first done for real on 2026-09-07 (Windows 10,
  NVIDIA TITAN Xp, Vulkan renderer renders correctly). The exact
  `pacman` package list, the traps a fresh MSYS2 hits (CRLF checkout,
  Python ≥ 3.13 prefix, MSYS tar, `USERPROFILE`), the validation-layer
  recipe and the open work list are in [docs/Campaigns/windows-port-2026-09.md](docs/Campaigns/windows-port-2026-09.md);
  the ranked real-hardware audit is
  [docs/Campaigns/windows-audit-2026-09-summary.md](docs/Campaigns/windows-audit-2026-09-summary.md).
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
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use`. A trained profile is committed at `pgo/default.profdata` and applied by CI to arm64 release builds (+9.4% fps). Retrain after large code churn: `generate` build → play the bench scenes → `use` build → commit the regenerated profdata. A `use` build prints a staleness summary (hot-path CFG-hash mismatches + commits since retrain) and warns when the profile has drifted — see the ledger |
| `XEMU_WIN_O3` | `0` | Native MSYS2/MinGW only: `1` builds the release at `-Doptimization=3 -Dstack_protector=disabled` instead of meson's `-O2` + probed `-fstack-protector-strong`. Experimental, uncovered by CI, and the leading suspect for the 2026-09-07 native-Windows crash — see the ledger before using it. Ignored on `win64-cross` |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Hardened-runtime codesign via `xemu.entitlements` |
| `XEMU_MOLTENVK_VERSION` | `1.4.2` | MoltenVK release auto-vendored into `macos-libs` when no usable system copy exists. This default is the single home for the expected version — macOS CI asserts the packaged bundle's provenance line matches it |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for `scripts/build-moltenvk.sh` (the maintained optimized MoltenVK) |

Runtime debug / escape-hatch env vars (`XEMU_*`) are catalogued with
the optimizations they belong to in
[docs/optimizations.md](docs/optimizations.md).

### How Vulkan is provisioned (all platforms)

The NV2A Vulkan renderer loads Vulkan at runtime through **volk** —
nothing links against a Vulkan library at build time.

- **macOS:** volk dlopens `libMoltenVK.dylib`, which `build.sh` bundles
  into `xemu.app/Contents/Libraries/<arch>/`. One resolution rule
  (`resolve_moltenvk`): `macos-libs` (vendored), then `/usr/local`
  (Vulkan SDK / `scripts/build-moltenvk.sh`), then Homebrew — a dylib
  counts only if it has the target arch slice; when none exists the
  pinned official release is vendored automatically. `build.sh` logs
  the bundled version + UUID so tested-vs-shipped driver drift is
  visible. End users need nothing installed.
- **Windows:** volk loads the system `vulkan-1.dll` (ships with GPU
  drivers). No Vulkan ICD in a VM → automatic OpenGL fallback with a
  notification; test Vulkan on real hardware.
- **Linux:** the distro `libvulkan` loader + the GPU's ICD.

On Windows and Linux the renderer additionally calls `volkLoadDevice()`
after creating its `VkDevice` (since 2026-09-08), so `vkCmd*` and
friends enter the driver's ICD directly instead of going through the
loader trampoline; `XEMU_VK_VOLK_DEVICE=0` restores the old
instance-level dispatch. On macOS nothing changes — MoltenVK is the ICD
and there is no trampoline to bypass.

### Recommended `xemu.toml`

`~/Library/Application Support/xemu/xemu/xemu.toml`:

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'        # off | spatial | temporal
frame_interpolation = '4x'      # off | 2x | 4x

[display.quality]
surface_scale = 2               # 1 = 640x480 ... 4 = 2560x1920

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
enabled = true                  # ARM64 inline basic-block DSP JIT
```

In-app Settings covers the main toggles.

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
It is not available from HMP. Config, portable mode, savestates, the HDD
and EEPROM and all command-line options behave as windowed.

Other automation interfaces (windowed or headless):

- **gdbstub `m` reads** that run into an unmapped page (the game heap
  has guard pages between allocations) return the readable prefix, as
  the protocol allows, instead of `E14`.
- **`XEMU_INPUT_PIPE` lines** (keyboard pad, port 2 in the fixtures):
  `down <scancode>`, `up <scancode>`, `clear`, plus analog
  `axis lx|ly|rx|ry <-1..1>` and `trigger lt|rt <0..1>` (positive is
  right/up; `0` hands the axis back to the keys). Prefix any line with
  `@<frame>` or `+<frames>` to apply it at that frame instead of on
  arrival.
- **Frames** count vblanks while the VM runs. QMP `query-xemu-frame`
  returns `{frame, running}`; `xemu-frame-advance {"frames": N}` runs N
  frames, pauses, and returns (`0` just pauses). It also starts a VM the
  gdbstub stopped, from the instruction it stopped at (lift a breakpoint
  under the PC first); a breakpoint or step that stops the VM during the
  advance ends it early with `debug-stop: true`. In real time, replays
  land within a frame or two: guest work per vblank depends on host
  speed. Deterministic mode (below) makes them bit-exact.
- **Input recording**: `XEMU_INPUT_RECORD=<file>` writes, at every
  vblank, the changes in what one pad gave the guest
  (`XEMU_INPUT_RECORD_PORT=1-4`, default the first bound port) as
  `XEMU_INPUT_PIPE` lines tagged `+N` from the last load, each load
  opening a `# load` section. Buttons come out as `down`/`up` of their
  keyboard-map scancodes and sticks and triggers as values that give
  back the same 16-bit reading (a stick reaches -32768 at
  `-1.000030518`), so a gamepad session replays through the keyboard
  pad. While recording, pads change only at vblanks, as under icount.
  To replay: bind the keyboard pad to that port with the same keyboard
  map, load the same state paused, write the section's lines, advance.
  A recording made under icount replays bit-identically; one made in
  normal mode replays bit-identically from one replay to the next, and
  follows the original closely (Azurik's player within 0.001 units over
  240 frames) but not exactly, since guest time followed the host clock.
- **Per-frame traces**: `xemu-frame-advance {"frames": N, "trace":
  [{"address": A, "size": S}, ...]}` also returns `trace`, one sample
  per vblank (`"trace-every": K` for every Kth, the last always
  included) with each region's bytes as hex, read before the guest sees
  that vblank. `"virtual": true` reads through CPU 0's page tables; a
  region that RAM doesn't fully back reads as `""`. Up to 256 regions
  of 1-4096 bytes; exact under icount.
- **Readiness lines** on stderr: `xemu: ready` once the machine and pads
  exist (about 30 ms after launch in headless null mode, which starts
  no video or gamepad subsystem; `loadvm` works from then on, no fixed
  sleep needed) and `xemu: state loaded` after every `loadvm` or incoming
  migration.
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
  `xemu-checkpoint-delete` frees one. About 20-30 ms to save and 20-25
  ms to restore 128 MiB on a loaded M2 Ultra; the qcow2 is never written
  for the state.
- **Bulk RAM dumps**: QMP `xemu-dump-ram {"filename": "/tmp/ram.bin"}`
  writes all of guest RAM (64 or 128 MiB) in one write, ~20-60 ms for
  128 MiB, pausing the VM for the copy unless `"pause": false`. Add
  `"address"`/`"size"` for a physical range, or `"virtual": true` for a
  range of CPU 0's virtual addresses, where pages that no RAM backs read
  as zeros (counted in `unmapped`). `"gpu-sync"` (default on) first
  writes GPU-rendered surfaces back to RAM, as savevm does. `pmemsave`
  and `memsave` work too, 1 KiB at a time.
- **Zero-copy RAM**: `XEMU_RAM_SHM=<name>` puts guest RAM in named POSIX
  shared memory that another process can map (Python:
  `multiprocessing.shared_memory.SharedMemory("<name>")`, then
  `resource_tracker.unregister` so Python does not unlink it). The name
  is replaced if it exists and unlinked at exit; savestates are
  unaffected.
- **gdbstub reads** may ask for up to 4 MiB per `m` (was 64 KiB), and the
  binary `x addr,len` read (GDB 16, advertised as `binary-upload+`)
  halves the bytes on the wire. In `Qqemu.PhyMemMode:1` a 150 MB sweep
  takes under a second.
- **Fixed clock**: `-rtc base=2001-01-01T00:00:00` pins the RTC on a
  fresh boot. After `loadvm` the RTC is the snapshot's plus the wall time
  since it was saved; with `-rtc clock=vm` a load continues from the
  date the snapshot saved, with no wall time added.
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
    and the host keyboard are ignored); an untagged line applies at the
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
    re-save here in normal mode first). Save with `sleep=off`; a state
    saved with sleep on or `shift=auto` loads anywhere but drops its warp
    deadlines;
  - frame numbers (`@N`, `query-xemu-frame`) count the current
    process's frames, so use `+N` after a load.

  Write tagged input while paused, then advance. `shift` is guest time
  per instruction (2^N ns): 1 is about a 733 MHz Pentium III, 2 runs
  ~1.5x faster (about 90 fps on Azurik's heavy scene on a loaded M2
  Ultra, about 46 with 0) and suits games that are not CPU-bound.
  Offscreen Vulkan (`XEMU_HEADLESS=1`) is deterministic too, screendumps
  included when taken paused, at about 60% of the null renderer's speed:
  a GPU report reaches RAM when the guest or a device first touches its
  slot, or at a pause, save or load. `XEMU_REPORTS_SYNC=1` delivers each
  report before PFIFO rests instead: the same RAM, ~3.7x slower, and
  screendumps that can differ in a few pixels. Hash its RAM with
  `xemu-dump-ram` `"gpu-sync": false` (`gpu-sync` and `pmemsave` write
  GPU surfaces back, which changes later RAM), and expect matches only
  on the same Mac, macOS and MoltenVK. A cheap reproducible screenshot
  of frame F of a null run: savevm there, load it in a short
  `XEMU_HEADLESS=1` icount run, advance a few frames, screendump. OpenGL
  is not deterministic.
  Keep the network off and bind no XMU.
- **Lost GPU device**: macOS stops accepting a process's GPU work after
  repeated GPU errors; a headless xemu then prints `xemu: Vulkan device
  lost; exiting` and exits at once (a windowed one still crashes with a
  report). The errors behind it were GPU reads of freed textures: after a
  `loadvm` or reset freed every surface, the display compositor could
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

Headline arc on the heavy in-game bench scene (Azurik savestate, M2
Ultra, real fps — no interpolation): **15.8 → ~48.1 fps** across the
fork's campaigns — the occlusion/report rework (+144%), PGO (+9.4%),
sub-page code dirty tracking (+14%), cross-page TB chaining, inline
dispatch probes, the sticky SSE/NEON bracket, the async Metal present
chain, the inline NOTDIRTY store-skip (+12% heavy-scene rendering
throughput at equal fps), and the 2026-08-04 optimization wave.
Lighter scenes hold the 60 fps vblank cap. MetalFX frame interpolation
presents 60/120 Hz from real-30+ without changing simulation rate.

The most recent step is the largest single one since the occlusion
rework. Full protocol receipt for it (2026-08-04, interleaved
cross-binary A/B on the same fixture): **38.12 ± 0.59 → ~48.1 fps,
+10.0 fps mean, +26% fps, +20.4% draws/s, 5 clean pairs, unanimously
positive** (one further pair excluded because its run hit a known
upstream-class crash, noted in the receipt). Its two biggest
contributors are the `can_do_io`/`cpu_io_recompile` elision (+6.53 fps,
3/3) and the x87 exception-pointer deferral (+7.46 fps, 3/3) — with
each mechanism's own receipt, the wave's honest kills, and every escape
hatch in the [ledger](docs/optimizations.md).

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
estimation + paced presentation order); `XEMU_MFX_INTERP_ZERO_MOTION=1`
restores the old zero-motion binding for A/B comparison.

**Black/garbled window or Metal init failure on launch.** Set
`[display.window] presentation_backend = 'opengl'` in `xemu.toml` to
fall back to the GL presentation path, and report the issue.

**FPU precision bug.** `[perf] hard_fpu = false`. Inline FPU uses IEEE
double (52-bit mantissa) vs x87 extended (64-bit); extremely rare.

**Depth artifacts after a render-target switch.** Set
`XEMU_ZETA_SHAPE_READBACK=1` to restore the full GPU→CPU round trip on
zeta shape switches (costs ~2 finishes + multi-MB copies per frame on
affected titles) and report the title.

**Stale/late-updating textures.** Set `XEMU_TEX_BIND_RECHECK=1` to
restore per-bind content checks (a CPU write between two binds of the
same texture within one frame otherwise lands a frame late).

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
`XEMU_SPIRV_CACHE=0` bypasses the cache entirely.

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
stall the pfifo thread behind the driver's GL swap lock. Set
`XEMU_WIN32_DXGI=0` to skip that presenter and swap with
`SDL_GL_SwapWindow` instead (the pre-merge path — it gives the stall
back), and report the GPU/driver.

**Windows: black screen with working audio on Vulkan.** Fixed in
v0.13.2 — an invalid cross-stage uniform-buffer descriptor write that
native Vulkan drivers rendered as black (MoltenVK tolerated it). Update
to v0.13.2 or newer. As a fallback on older builds, set
`renderer = 'OPENGL'` in `xemu.toml`.

More escape hatches (every optimization ships one) are listed with
their owning changes in [docs/optimizations.md](docs/optimizations.md).

---

## Architecture at a glance

```
Xbox game (30fps, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG, inline x87 → AArch64 FP
       │  floatx80↔double inline
       │
       ├─ GPU: NV2A Vulkan (MoltenVK)
       │  flight slots · bump-alloc staging · VRAM spatial dirty index
       │  compute unswizzle + YUV · zero-copy display
       │  (Metal: exported MTLTexture, async timeline submit ·
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
