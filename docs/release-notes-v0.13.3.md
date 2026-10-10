# v0.13.3 release notes — DRAFT

Draft body for the GitHub release, in the shape `docs/RELEASING-macos.md`
prescribes. Written 2026-10-10 against `920e645b69`, before the RC gauntlet
(`docs/windows-wave1-review-2026-09-08.md` §8) has run: the two lines marked
**OWED** must be replaced with measured results or deleted before the tag is
pushed. Delete this file, or move it under the published release, once the
draft is published.

Suggested title: `v0.13.3 — Two upstream syncs, Windows hardening wave 1, SDL3 3.4.18`

---

macOS (Apple Silicon) release. Attached build is an ad-hoc-signed `xemu.app` for macOS 14+ (macOS 26 Tahoe recommended for MetalFX + frame interpolation).

## Highlights

Two upstream merges (xemu master through `478b4f4961`), the first batch of
hardening from the native Windows port audit, and a refresh of every
vendored dependency. No performance claim in this release: the headline
change for a macOS user is that upstream's PTIMER alarm interrupts are now
emulated, which bumps the NV2A savestate format, and that the shader cache
re-keys on the new glslang.

**Read before upgrading**

- **Savestates saved on this build do not load on v0.13.2 or older.** The
  NV2A vmstate moved to v4 for the PTIMER alarm state. Old savestates still
  load here.
- **First launch is cold.** The SPIR-V cache is keyed on the linked
  glslang's runtime version (now 16.6.0), so every shader recompiles once;
  expect stutter for the first minutes of each title until the cache
  refills.
- **DSP engine default is the fork's again.** Upstream flipped
  `audio.use_dsp_jit` off "for first release"; this fork keeps it on, so a
  stock config runs upstream's dsp56300 JIT as before, and
  `[audio.dsp_jit] enabled = true` still selects the fork's inline ARM64
  JIT.

### NV2A

- **PTIMER alarm interrupts, with correct rounding.** Upstream's
  `NV_PTIMER_ALARM_0` implementation (first merge) now fires guest alarm
  IRQs; the second merge rounds the host-timer distance up to a whole
  internal tick with a 1 ns floor (`muldiv64_round_up`,
  `get_next_alarm_time`) so an alarm can no longer land one tick early and
  re-fire. The fork adds a guard for the window before the guest programs
  `NUMERATOR` / `DENOMINATOR` and the PRAMDAC PLL, when every divisor is
  zero: the clock reads as stopped and no alarm is armed instead of a
  divide trap. Upstream's new on-target unit test (`tests/xbox/ptimer`, 20
  cases) runs in the `xbox` suite on every CI leg.

### Vulkan renderer (pgraph/vk)

- **SPIR-V disk cache hardened.** Cache files open through `qemu_fopen`
  (UTF-16 paths on Windows), are written atomically (temp + rename), carry
  a `-dbg` tag when `debug_shaders` is on, and are validated before
  SPIRV-Reflect sees them: the instruction stream is walked and the last
  instruction must be `OpFunctionEnd`, because about a fifth of
  word-aligned truncations land on an instruction boundary and the
  parser's own bounds check is a live `assert`. A bad blob is regenerated
  instead of aborting every launch. Orphaned `.tmp` files are swept at
  cache open. `XEMU_SPIRV_CACHE=0` bypasses the cache,
  `XEMU_SPIRV_CACHE_ATOMIC=0` restores in-place writes.
- **Cache key follows the linked glslang.** The directory name used to
  come from whichever glslang header the compiler found first (on a dev
  box, Homebrew's), so a wrap bump could serve stale SPIR-V; it now comes
  from `glslang_get_version()` at runtime.
- **Bounds checks are real in release builds.** `nv2a_vk_bounds_check` was
  `__builtin_unreachable()` outside debug builds; it is now a check on
  every platform (28 per-draw sites). Also from the audit: `volkLoadDevice()`
  dispatch on non-Apple drivers, `VK_KHR_external_semaphore_win32` made
  optional, and the LRU eviction in-use filter restored to upstream's
  `lru_try_evict_one` semantics.

### APU

- **Guest voice handles are bounds-checked.** An out-of-range handle used
  to index host `filters[]` / HRTF / SSL tables (an out-of-bounds read,
  then write). The voice-list walk now stops on a bad `NEXT_VOICE_HANDLE`
  and reports once per run; an out-of-range handle is dropped rather than
  falling through to the guest-RAM path, whose `address_space_*` fallback
  could take the BQL from a voice-worker thread while `d->lock` is held
  (a lock-order inversion against `gp_write`). The runtime `use_dsp_jit`
  engine switch is serialised under the BQL and the APU frame thread
  registers with RCU. APU direct RAM stores mark dirty behind
  `XEMU_APU_RAM_DIRTY` (default off on Apple, on elsewhere).

### Input

- **Pad index bounded before `bound_controllers[]`.** A guest-visible XID
  index outside 0..3 no longer reaches the host array
  (`hw/xbox/xid.c`, `hw/xbox/xid-gamepad.c`).

### Windows

- **DXGI presenter activates on NVIDIA.** The WGL_NV_DX_interop FBO was
  completeness-checked before the interop object was locked, which
  returned `GL_FRAMEBUFFER_UNSUPPORTED` and silently fell back to
  `SDL_GL_SwapWindow`. The check now runs under the lock, one lock/unlock
  per frame; `XEMU_WIN32_DXGI=0` restores the SDL swap path.
- **Native MSYS2 build on Python 3.13+.** `build.sh` passes a `cygpath -m`
  prefix (Python's `ntpath.isabs` rejects `/qemu`) and the dsp56300
  prebuilt is extracted with `--force-local`. GCC profile-guided
  optimisation is wired for MinGW (`XEMU_PGO`), and the `-O3 /
  -Dstack_protector=disabled` build is **opt-in** behind `XEMU_WIN_O3=1`
  because the 2026-09-08 review named that flip as the leading suspect
  for an intermittent crash seen with the wave-1 binary; the shipped
  Windows legs stay at `-O2`. The wave-1 source changes have not yet been
  exercised natively at the shipped flags.
- **Harness transport.** `XEMU_INPUT_PIPE` rides a Win32 named pipe
  (same line protocol as the POSIX FIFO), and `nsprof` / heartbeat /
  invalidation timing report under the OpenGL renderer and on x86
  (`rdtsc`). The PVIDEO/display overlay fix from the audit remains on
  branch `windows-wave1-wip`, not in this release.

### Build + packaging

- **Shipped SDL3 moves from 3.4.8 to 3.4.18.** The MacPorts vendoring
  script picked the lexicographically last listing entry, so `3.4.8` beat
  `3.4.18` on every clean build, CI included, for months. It now ranks by
  numeric version and revision. The same change survives Python 3.14's
  stricter default tar filter, which rejected an absolute symlink in the
  `dbus` bottle that MacPorts' current `glib2` pulls in (dbus is not
  bundled; nothing links it). Other vendored bumps: glib2 2.88.3, libusb
  1.0.30, pcre2 10.48.
- **glslang 16.6.0, nlohmann_json 3.12.0, SDL3 (non-macOS subproject)
  3.4.16** via upstream's wrap bumps. `build.sh` warns before configure
  when a subproject checkout no longer matches its wrap's pinned revision,
  which previously left a local build silently on an older glslang than
  CI.
- **MoltenVK.** The macOS bundle still ships the official **1.4.2**
  release. The maintainer's local pipeline now tracks MoltenVK main past
  that tag (self-reports 1.4.3); it is not what this zip contains.

### Testing & tooling

- `scripts/boot-smoke-corpus.sh` boots a list of titles on an isolated
  bundle clone, taps through menus, scores screenshots and reports
  crashes/asserts per title — the soak gate for state-table and bounds
  changes. The Windows benchmark harness (`scripts/bench-savestate-ab-win.py`,
  `scripts/win/`) and upstream's `make check-unit` CI job are in the tree;
  neither has produced a result that gates this release yet.

## Validation

- **OWED:** quiet-machine interleaved fps A/B, this build vs a v0.13.2
  build, F8 heavy savestate, ≥3 pairs — accept parity within noise.
- **OWED:** artifact-oracle soak on the same run (≥300 captures, zero
  magenta/white) and a 20–30 minute Finder session in the owner's real
  configuration.
- Done: arm64 release build from a clean tree; `meson test --suite xbox`
  7/7 (DSP interpreter/JIT differential, swizzle, PTIMER); CI matrix green
  on every leg including both win64-cross jobs as of `bd3d56ee8d`
  (the 2026-10-10 commits have not been pushed yet).

**Full Changelog**: https://github.com/MichaelJSr/xemu-macos/compare/v0.13.2...v0.13.3
