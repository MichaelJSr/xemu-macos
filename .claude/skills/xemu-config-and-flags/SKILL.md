---
name: xemu-config-and-flags
description: >
  Complete catalog of every configuration axis in the xemu-macos fork:
  xemu.toml settings (config_spec.yml -> genconfig -> g_config, defaults,
  restart semantics, portable mode, -config_path), all XEMU_* runtime
  environment variables (diagnostics, escape hatches, test harness, A/B
  knobs, POSIX guards), the five canonical MVK_CONFIG_* MoltenVK values
  and the Info.plist/main() launch-path parity rule, and build.sh /
  build-moltenvk.sh build-time knobs (XEMU_ARM_CPU, XEMU_PGO, XEMU_VIS,
  XEMU_STRIP, -j/-p/-a/--debug, x86_version). Load this skill when you
  need to: look up a setting or env var name/default/effect, decide
  production vs experimental vs test-only status, add a new xemu.toml
  setting / XEMU_* env var / MVK_CONFIG_* var (checklists included),
  find where a flag is read in code, understand DSP JIT engine toggle
  precedence (audio.dsp_jit.enabled vs audio.use_dsp_jit vs
  XEMU_DSP_JIT), locate the user's config file, or re-verify the flag
  tables after the tree changes. Keywords: xemu.toml, g_config,
  config_spec.yml, XEMU_INPUT_PIPE, XEMU_NV2A_NSPROF, XEMU_VTX_EXACT,
  XEMU_REPORTS_SYNC, XEMU_DSP_JIT, MVK_CONFIG_PREFILL, LSEnvironment,
  portable mode, escape hatch, default value, add a flag.
---

# xemu-macos configuration and flags catalog

Every knob in this fork lives on one of four axes. All facts below were
verified against the tree on **2026-07-04** (branch `macos-optimizations`,
v0.9 era); env-var/spec counts, the `ui/xemu.c` setenv anchor, and MVK
parity were re-verified 2026-07-11 at `7e2e6e7256`. The final section
gives one-line commands to re-derive each
table, because flags drift and this catalog must be re-checkable, not
trusted.

| Axis | Mechanism | Set where | Takes effect |
|---|---|---|---|
| 1. `xemu.toml` settings | `config_spec.yml` -> generated `g_config` struct | user config file / in-app Settings UI | mostly live; some read-once (noted) |
| 2. `XEMU_*` env vars | `getenv()` in C code, latched at first use | shell environment before launch | at launch only (all are read-once) |
| 3. `MVK_CONFIG_*` | MoltenVK driver env config | `Info.plist` LSEnvironment + `ui/xemu.c` `main()` | at launch, before MoltenVK is dlopen'd |
| 4. Build knobs | env vars + args consumed by `build.sh` / `scripts/build-moltenvk.sh` | build shell | baked into the binary |

Jargon used below: **g_config** is the global generated config struct
(`struct config g_config` in `ui/xemu-settings.cc:40`, extern in
`ui/xemu-settings.h:43`). **Escape hatch** = an env var that restores
legacy behavior after a default was changed (this fork ships one with
every behavior change — established practice inferred from the record;
provenance in xemu-change-control "Rule provenance"). **PFIFO thread** =
the NV2A renderer thread. **zeta** = depth/stencil buffer. **zpass
report / occlusion query** = guest asking how many pixels passed depth.
**FIFO (file)** = a POSIX named pipe. **LSEnvironment** = the
`Info.plist` dictionary of env vars that macOS LaunchServices applies
to Finder/`open` launches only.

---

## Axis 1: xemu.toml settings

### Generation chain (never hand-edit the generated header)

`config_spec.yml` (repo root, 404 lines as of 2026-07-11, the authoritative schema)
-> meson custom_target at `meson.build:3806-3809` runs
`subprojects/genconfig/gen_config.py` -> generates `build/xemu-config.h`
-> runtime access via `g_config.<path>`. The genconfig subproject is
pinned in `subprojects/genconfig.wrap` (mborgerson/genconfig). Enum
values become constants named `CONFIG_<PATH>_<VALUE>`, e.g.
`CONFIG_DISPLAY_METALFX_MODE_TEMPORAL`.

Implicit defaults when a spec entry omits `default:`
(`gen_config.py:72-83`): `bool` -> false, `string` -> `''`,
`integer` -> 0, `number` -> 0.0, `enum` -> **first listed value**,
`array` -> empty. Prefer writing an explicit `default:`.

A plain `ninja` in `build/` regenerates `xemu-config.h` after a spec
edit (do not edit the generated file).

### Config file location, portable mode, CLI override

- Normal mode: `SDL_GetPrefPath("xemu", "xemu")`
  (`ui/xemu-settings.cc:85`) ->
  `~/Library/Application Support/xemu/xemu/xemu.toml` on macOS. The
  same base dir holds `eeprom.bin` (default) and the Vulkan
  `pipeline_cache.bin` (`hw/xbox/nv2a/pgraph/vk/draw.c:457-461`).
- Portable mode: if a file named `xemu.toml` exists at
  `SDL_GetBasePath()`, that directory becomes the base path
  (`ui/xemu-settings.cc:51-61,73-91`). This fork uses **SDL3**
  (`meson.build:1687`), and SDL3's base path for a macOS app bundle is
  `Contents/Resources/` — NOT `Contents/MacOS/`. For a bare
  `build/qemu-system-i386` binary it is the executable's directory.
  **Trap:** `./build.sh` repackaging starts with `rm -rf dist`
  (`build.sh` `package_macos`), so a portable config planted inside
  `dist/xemu.app/Contents/Resources/` vanishes on every rebuild.
- CLI override: `xemu -config_path <file>` (`ui/xemu.c:1651-1658` ->
  `xemu_settings_set_path`). Harnesses use this to avoid hijacking the
  user's real config.
- Load failure = fatal: error dialog then `exit(1)`; settings are
  saved back on exit via `atexit(xemu_settings_save)`
  (`ui/xemu.c:1661-1670`).

### Operational settings table (verified in `config_spec.yml` + consumers)

Status legend: **prod** = supported default path; **rec** = production,
recommended non-default on Apple Silicon; **debug** = development aid;
**legacy** = kept only for migration.

| Setting | Type / values | Default | Status | Effect / notes |
|---|---|---|---|---|
| `display.renderer` | `NULL`\|`OPENGL`\|`VULKAN` | `VULKAN` | prod | Renderer choice. The fork's optimizations live in the Vulkan/MoltenVK path. |
| `display.window.presentation_backend` | `auto`\|`opengl`\|`metal` | `auto` | prod | macOS-only: which API presents the final frame + UI. `auto` -> Metal when renderer is VULKAN. **Resolved once before window creation and fixed for the process lifetime** (`ui/xemu.c:82-110` `xemu_present_is_metal`); changing it, or switching renderer under `auto`, requires a restart. |
| `display.metalfx_mode` | `off`\|`spatial`\|`temporal` | `spatial` | prod | MetalFX upscale mode (UI: `ui/xui/main-menu.cc:832`). |
| `display.metalfx_upscale` | bool | `false` | legacy | Old on/off bool. Migrated at load: `true` + `metalfx_mode=off` -> `metalfx_mode=spatial`, then always reset to `false` (`ui/xemu-settings.cc:206-213`). Never write new code against it. |
| `display.frame_interpolation` | `off`\|`2x`\|`4x` | `off` | prod | MTLFXFrameInterpolator; requires macOS 26 (`@available` check in `metalfx_upscale.m`). UI: `main-menu.cc:837`. |
| `display.quality.surface_scale` | integer | `1` | prod | Internal render-resolution multiplier; UI drives it via `nv2a_set_surface_scale_factor` (`main-menu.cc:814-827`). |
| `display.window.vsync` | bool | `true` | prod | Present sync. |
| `display.window.fullscreen_on_startup` | bool | `false` | prod | With `fullscreen_exclusive` (bool, `false`). |
| `display.window.startup_size` | enum | `1280x960` | prod | Plus `last_width`/`last_height` (640/480) when `last_used`. |
| `display.vulkan.validation_layers` | bool | `false` | debug | Enables VK validation if the layer is installed (`pgraph/vk/instance.c:163`). Siblings: `debug_shaders`, `assert_on_validation_msg`, `preferred_physical_device`. |
| `audio.dsp_jit.enabled` | bool | **`false`** | rec | Fork's ARM64 inline basic-block DSP JIT (Apple Silicon only). README recommends `true` on Apple Silicon; the shipped default is still false — both statements are correct, state them together. UI: `main-menu.cc:929`. |
| `audio.use_dsp_jit` | bool | **`true`** | prod | Upstream dsp56300-subproject JIT engine. **Ignored while the fork JIT is supported+enabled** — see precedence box below. UI: `main-menu.cc:936`. |
| `audio.vp.num_workers` | integer | `0` (= auto) | prod | Voice-processor worker threads; 0 -> `SDL_GetNumLogicalCPUCores()`, clamped to `[1, MAX_VOICE_WORKERS]` (`hw/xbox/mcpx/apu/vp/vp.c:2218-2219`). |
| `audio.use_dsp` | bool | `false` (implicit) | prod | Bare-`bool` spec entry -> implicit false. |
| `perf.hard_fpu` | bool | `true` | prod | ARM64 inline x87 FPU (native AArch64 FP instead of softfloat). UI: `main-menu.cc:70`. |
| `perf.cache_shaders` | bool | `true` | prod | Gates the **OpenGL** shader disk cache only (`pgraph/gl/shaders.c:282,479,794`). The Vulkan `pipeline_cache.bin` (saved at most every 30 s at flip boundaries, write-then-rename) is **always on and not gated by this setting** (`pgraph/vk/draw.c:457-545`). |
| `sys.mem_limit` | `'64'`\|`'128'` | `'64'` | prod | Guest RAM (MiB). UI: `main-menu.cc:1631`. |
| `sys.avpack` | enum | `hdtv` | prod | AV pack type. |
| `sys.files.*` | strings | `''` | prod | `bootrom_path`, `flashrom_path`, `eeprom_path`, `hdd_path`, `dvd_path`. |
| `general.snapshots.shortcuts.f5`..`f8` | strings | `''` | prod | F5-F8 map to named savestates; consumed at `ui/xemu-snapshots.c:47-50`. Plus `filter_current_game` (bool). |
| `input.auto_bind` | bool | `true` | prod | Auto-bind new controllers. |
| `input.background_input_capture` | bool | `false` (implicit) | prod | Keep reading input while unfocused (relevant to test automation; see xemu-testing). |
| `input.bindings.port1..4[_driver]` | strings | `''` | prod | Per-port device binding; `input.keyboard_controller_scancode_map.*` holds per-button SDL scancodes (explicit defaults in the spec). |

The full schema (net.*, display.ui.*, debug window geometry, gamepad
mapping arrays, etc.) is `config_spec.yml` itself — read it directly
rather than trusting any secondary list.

### DSP engine precedence (the one genuinely confusing axis)

Two independent toggles select among three engines
(`hw/xbox/mcpx/apu/dsp/dsp.c:113-128`):

```c
static bool dsp_want_external_jit_engine(void)
{
#if DSP56K_JIT_SUPPORTED                 /* __APPLE__ && __aarch64__ */
    if (g_config.audio.dsp_jit.enabled) {
        return false;                    /* fork inline JIT wins */
    }
#endif
    return g_config.audio.use_dsp_jit;
}
```

- `audio.dsp_jit.enabled = true` (Apple Silicon): C interpreter engine
  with the fork's inline ARM64 JIT. `audio.use_dsp_jit` becomes a
  no-op (`dsp_set_engine` early-outs, `dsp.c:245-252`).
- `audio.dsp_jit.enabled = false` (default) + `audio.use_dsp_jit =
  true` (default): upstream dsp56300 JIT engine.
- Both false: plain C interpreter.

`DSP56K_JIT_SUPPORTED` is `defined(__APPLE__) && defined(__aarch64__)`
(`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h:15-19`); the JIT
source compiles to nothing elsewhere
(`hw/xbox/mcpx/apu/dsp/interp/meson.build`).

**`XEMU_DSP_JIT` does NOT switch engines.** It only overrides the
inline JIT's on/off *within the interpreter engine*
(`dsp56k_jit_arm64.c:170-183`; the config value reaches the JIT via
`dsp56k_jit_set_enabled_from_config()` called from
`mcpx_apu_dsp_init`, `gp_ep.c:519-525` — an indirection that exists so
the `tests/xbox/dsp` binary can link `libdsp` without `g_config`).
Consequences, verified in code:

- Defaults + `XEMU_DSP_JIT=1` -> the **external** engine is still
  selected, so the fork JIT does not run. README line 629 ("enable via
  `[audio.dsp_jit] enabled = true` or `XEMU_DSP_JIT=1`") is imprecise
  on this point.
- `audio.dsp_jit.enabled=true` + `XEMU_DSP_JIT=0` -> interpreter
  engine with inline JIT forced off = pure-interpreter A/B leg.

---

## Axis 2: XEMU_* runtime environment variables

Authoritative list = every `getenv("XEMU_` site in fork C/ObjC code.
As of 2026-07-11 there are exactly **30 distinct variables** (the raw
grep in the Provenance section prints 68 matching lines — several
variables are read at more than one site). **Every one is read once and
latched in a static** — set it
before launch; changing the environment mid-run does nothing.

Value-parse conventions: *truthy* = any non-empty value whose first
char is not `0`; *strict* = first char must be `1`.

Categories: **diag** = diagnostic output, safe in release;
**escape** = escape hatch restoring pre-optimization legacy behavior;
**test** = test-harness/bring-up facility; **A/B** = experimental
comparison knob.

| Env var | Unset behavior | Values | Effect | Cat | Read at |
|---|---|---|---|---|---|
| `XEMU_NV2A_NSPROF` | off | truthy | Wall-time NV2A profiler; 5 s counter/event summaries to stderr (PFIFO thread) | diag | `hw/xbox/nv2a/nsprof.c:83` |
| `XEMU_PFIFO_HEARTBEAT` | off | strict | ~2 s PFIFO-thread heartbeat + pending-flag snapshot (categorize freezes: thread stuck vs waiting) | diag | `hw/xbox/nv2a/pfifo.c:494` |
| `XEMU_APU_PROF` | off | truthy | Per-second APU-thread utilization % + frames/s to stderr | diag | `hw/xbox/mcpx/apu/apu.c:249` |
| `XEMU_ZETA_SHAPE_READBACK` | off (fast path) | truthy | Restore GPU->CPU readback on zeta shape switches (full fidelity, slower) | escape | `pgraph/vk/surface.c:1232` |
| `XEMU_TEX_BIND_RECHECK` | off (fast path) | truthy | Restore per-bind texture dirty rechecks (vs once per frame) — try for stale/late textures | escape | `pgraph/vk/texture.c:816` |
| `XEMU_VTX_EXACT` | **ON** (exact refinement) | `0` disables | `0` restores legacy page-granular vertex-conflict finishes; default is byte-exact conflict refinement (**inverted: this is the one default-on knob**) | escape | `pgraph/vk/vertex.c:118` |
| `XEMU_REPORTS_SYNC` | off (deferred reports) | strict | `1` restores legacy synchronous zpass-report drains (vs ride-the-flip + 5 ms idle fallback) | escape | `pgraph/vk/reports.c:294` |
| `XEMU_MAX_QUERIES` | 4096 | int 8..65536 (strtol, base auto) | Total occlusion-query pool size (`max_queries_in_flight`, partitioned across the 2 flight slots); small values exercise the begin_draw capacity-guard path | test | `pgraph/vk/reports.c:38` |
| `XEMU_INPUT_PIPE` | disabled | FIFO path | Input injection: lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` OR'd into keyboard state through the user's bindings; works unfocused. **POSIX-only**: `#ifdef _WIN32` compiles a stub (`ui/xemu-input.c:522-528`) | test | `ui/xemu-input.c:534` |
| `XEMU_MFX_REAL_DEPTH` | off (synthetic depth) | truthy | Feed real zeta MTLTexture to the MetalFX temporal scaler (known 1-frame-ahead limitation). Compiled only under `HAVE_IOSURFACE_SHARING` (= `__APPLE__`, `pgraph/vk/renderer.h:44-50`) | A/B | `pgraph/vk/surface.c:989` |
| `XEMU_MFX_INTERP_ZERO_MOTION` | off (linked scaler) | truthy | Restore the old zero-motion frame-interpolator binding | A/B | `pgraph/vk/metalfx_upscale.m:1295` |
| `XEMU_COREAUDIO_FRAMES` | 1024 | positive int (atoi) | CoreAudio buffer frames (~21 ms @ 48 kHz at default; try 2048 for audio glitches). **Runtime**, despite README's build-knob table and the in-code comment saying "build time" | escape | `audio/coreaudio.m:599` |
| `XEMU_DSP_JIT` | follow `audio.dsp_jit.enabled` | `1` force on; any other non-empty force off | Dev override of the inline DSP JIT **within the interpreter engine only** (see precedence box — it cannot select the engine) | test | `dsp/interp/dsp56k_jit_arm64.c:180` |
| `XEMU_DSP_JIT_STATS` | off | strict | Exit-time JIT counters (incl. cf_fallback buckets) | diag | `dsp56k_jit_arm64.c:228` |
| `XEMU_DSP_JIT_DIFF` | off | N 1..1000000 | Bit-exact validate JIT vs interpreter; 1 = every unique translation, N = sample every Nth (async worker thread) | test | `dsp56k_jit_arm64.c:192` |
| `XEMU_DSP_JIT_DIFF_MAX` | uncapped | N > 0 | Stop validating after N enqueues (CI: validate first N then run free) | test | `dsp56k_jit_arm64.c:211` |
| `XEMU_DSP_JIT_DIFF_SYNC` | async | strict | Synchronous (on-thread) validation; immediate abort on divergence; not for long runs | test | `dsp56k_jit_arm64.c:225` |
| `XEMU_DSP_JIT_NO_THROTTLE` | throttle on | strict | Disable the retranslation-churn auto-throttle | A/B | `dsp56k_jit_arm64.c:233` |
| `XEMU_DSP_JIT_SENTINEL` | off | `1` (bit 0) | cur_inst poison harness from the round-4 bisect (kept in-tree deliberately) | test | `dsp56k_jit_arm64.c:239` |
| `XEMU_DSP_JIT_FORCE` | off | `1` (bit 0) | Re-apply the deferred round-4 cur_inst skip — **known-regressing**, exists only for the bisect harness | test | `dsp56k_jit_arm64.c:250` |
| `XEMU_DSP_JIT_PIN_AUDIT` | off | strict | Per-op audit that pinned A/B accumulator registers (x26/x27) match memory; ~10 extra insns/op | test | `dsp56k_jit_arm64.c:265` |
| `XEMU_DSP_JIT_DUMP` | off | strict | Hex-dump every emitted ARM64 block to stderr for offline disassembly | diag | `dsp56k_jit_arm64.c:9889` |

Notes:

- All `XEMU_DSP_JIT_*` vars are inert unless `DSP56K_JIT_SUPPORTED`
  (Apple Silicon) and the interpreter engine is running.
- The README table header "All default off / fast-path; set to `1` to
  enable" (README ~line 110) is a simplification: `XEMU_VTX_EXACT` is
  default-on/inverted, `XEMU_MAX_QUERIES` and `XEMU_COREAUDIO_FRAMES`
  are numeric, `XEMU_INPUT_PIPE` takes a path.
- Build-time only: `XEMU_DEBUG_BUILD` is a **compile define** set by
  `./build.sh --debug` (`build.sh:244`), not an env var you export at
  runtime.

---

## Axis 3: MVK_CONFIG_* MoltenVK driver configuration

Five canonical values, set in **two places that must stay identical**:

1. `Info.plist` `LSEnvironment` dict (repo root, lines 35-52; copied
   into the bundle by `build.sh:113`) — applied by LaunchServices to
   **Finder/`open` launches only**.
2. `ui/xemu.c:1602-1606` in `main()`, `setenv(..., ..., 0)` before
   MoltenVK is dlopen'd — covers terminal/harness launches.
   `overwrite=0` means an explicitly exported `MVK_CONFIG_*` in your
   shell **still wins** (and for Finder launches the LSEnvironment
   values are already in the process env before `main()` runs).

| Key | Canonical value | Purpose |
|---|---|---|
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | **`0`** | **Load-bearing.** Prefill=2 (immediate encoding) corrupted streamed textures (magenta blocks ~5% of frames), enabled an AGX visibility-buffer crash, and measured *slower* (46.33 vs 48.50 fps) because encoding lands on the PFIFO thread. Never change from 0. |
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor-binding overhead |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async queue submits |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Survive transient device-lost errors |

**The parity rule** (the pink-tile lesson): LSEnvironment applies only
to LaunchServices launches, so before the `main()` mirror existed,
terminal-launched harnesses tested MoltenVK *defaults* while the user's
Finder launches ran the plist config — a corruption bug hid there for
weeks. Therefore:

- Any **new** MVK_CONFIG var must be added to BOTH the plist and the
  `main()` setenv block, and until a build containing the mirror is
  what you're testing, every harness must `export` it explicitly.
- README's "MoltenVK runtime config" table (README ~lines 718-726) is
  **stale as of 2026-07-04**: it still lists PREFILL=`2` and its header
  mentions only `Info.plist`. The code and the README "Changes" section
  (~line 313) are correct: PREFILL=0, set in both places.

---

## Axis 4: build-time knobs

Consumed by `build.sh` (and `scripts/build-moltenvk.sh`) at build time;
they change the produced binary, not a running one.

| Knob | Default | Effect | Where |
|---|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; otherwise auto-detect `apple-mN` from `sysctl machdep.cpu.brand_string` with a clang-acceptance walk-down, fallback `apple-m1` | `build.sh:388-417` |
| `XEMU_PGO` | unset | `generate` -> `-fprofile-generate`; run games; `use` -> merge `.profraw` via `llvm-profdata` and rebuild `-fprofile-use`. Errors out if `use` finds no profiles. Works on macOS **and** native Windows/MSYS2 | `build.sh:453-489` (Darwin), `:536-565` (Windows) |
| `XEMU_PGO_DIR` | `${PWD}/pgo` | Profile directory for both stages | same blocks |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | `1` = local hardened-runtime codesign with `xemu.entitlements` (allow-jit + allow-unsigned-executable-memory), re-signing every bundled dylib to match. Default is ad-hoc sign preserving metadata. Distributable signing lives in `scripts/sign-macos-release.sh` | `build.sh:137-160` |
| `XEMU_MOLTENVK_VERSION` | `1.4.1` | Official MoltenVK release auto-vendored into `macos-libs/<arch>` **only when no dylib exists** in: vendored dir, `/usr/local/lib`, `/opt/homebrew/lib` (same order used at bundle time; missing dylib at packaging = hard error with provenance version+UUID logging) | `build.sh:329-360`, `package_macos:72-105` |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for the maintained optimized MoltenVK build | `scripts/build-moltenvk.sh:24` |
| `XEMU_VIS` | `0` | `1` = `-fvisibility=hidden` (~-1.5 MiB arm64 binary; perf within noise) | `build.sh:512-515` |
| `XEMU_STRIP` | `0` | `1` = `-Wl,-dead_strip` (combined with VIS ~-4 MiB); validate before baking in — QEMU relies on `__attribute__((constructor))` registration | `build.sh:516-519` |

`scripts/build-moltenvk.sh` specifics: pins MoltenVK commit
`096714a2954fc8e9db9daae97c426d7dd7f8a838` ("main, 2026-07, reports
1.4.2"), builds arm64-only at `-O3` + `-mcpu=$XEMU_MVK_MCPU`,
**deliberately no LTO** (thin-LTO bitcode breaks the ShaderConverter
xcframework packaging; measured no fps benefit), installs to
`/usr/local/lib/libMoltenVK.dylib`, prints version+pin+UUID. Checkout
dir = first arg, default `../../MoltenVK` relative to `scripts/`. Bump
the pin only with the validation gauntlet (see xemu-testing /
xemu-validation-and-qa).

`build.sh` arguments (`build.sh:217-239`):

- `-j<N>` — job count, glued form (`-j16`).
- `--debug` — `--enable-debug --enable-trace-backends=log` +
  `-DXEMU_DEBUG_BUILD=1`; also suppresses all release opts.
- `-p <platform>` — platform case: `Linux`, `Darwin`,
  `CYGWIN*|MINGW*|MSYS*` (native Windows), `win64-cross`.
- `-a <arch>` — target arch (macOS cross-arch).
- Any other argument stops option parsing and is passed through to
  `configure` (e.g. `./build.sh -Dx86_version=1`).

Baked-in release options (non-Windows, non-debug, `build.sh:259-263`):
thin LTO + `.lto-cache`, `-Doptimization=3`, `-Dqom_cast_debug=false`,
`-Dtrace_backends=nop`, `-Dstack_protector=disabled`, plus
`-mcpu=<apple-mN> -ffp-contract=fast` on arm64. Windows release
(`build.sh:248-257,530-534`): **no forced LTO** (CI owns per-toolchain
LTO strategy) and defaults **`-Dx86_version=3`** unless `--debug` or an
explicit `x86_version` argument is present. Every build configures with
`--extra-cflags=-DXBOX=1 --target-list=i386-softmmu`
(`build.sh:591-596`).

---

## Add-a-flag checklists

### (a) New xemu.toml setting

1. Add the entry to `config_spec.yml` under the right table. Use the
   full form with an explicit `default:`; remember implicit defaults
   (bool false, enum = first value) if you omit it.
2. Rebuild (`cd build && ninja`) — meson regenerates
   `build/xemu-config.h` via genconfig (`meson.build:3806-3809`).
   Never edit the generated header.
3. Read it as `g_config.<path>`; enums compare against
   `CONFIG_<PATH>_<VALUE>` constants.
4. Decide and document restart semantics: if you latch the value once
   (static cache / startup-only read), say so in a spec comment — the
   `display.window.presentation_backend` entry
   (`config_spec.yml:255-264`) is the house pattern.
5. Optional Settings-UI hook in `ui/xui/main-menu.cc` (`Toggle` /
   `ChevronCombo` patterns at lines 70, 832-841, 929-940).
6. Renaming/replacing a setting? Add a load-time migration in
   `ui/xemu-settings.cc` next to the `metalfx_upscale` ->
   `metalfx_mode` fixup (`ui/xemu-settings.cc:206-213`); keep the old
   key in the spec as legacy.
7. README documentation row + evidence per xemu-change-control (what
   measurements a default change requires lives there, not here).

### (b) New XEMU_* env var

1. `getenv` at first use, latch in a `static` (every existing site
   does this — copy one, e.g. `nsprof.c:80-87`). Default must be
   off/fast-path; if you must invert (default-on like `XEMU_VTX_EXACT`)
   flag it loudly in the README row.
2. Parse style: strict `== '1'` or truthy like neighbors; numeric via
   `strtol` with an explicit valid range (`XEMU_MAX_QUERIES` pattern,
   `reports.c:38-44`).
3. POSIX-only facilities (FIFOs, signals, `O_NONBLOCK`) get an
   `#ifdef _WIN32` stub so Windows keeps compiling — the
   `test_input_poll` stub (`ui/xemu-input.c:522-528`) is the template.
   The Windows build must never regress for a macOS convenience
   (established practice inferred from the record — provenance in
   xemu-change-control "Rule provenance").
4. Add a row to the README "Runtime debug / escape-hatch knobs" table
   (~README line 112).
5. Change-control pairing rule: if the var exists because you changed
   default behavior, the var IS the escape hatch — the behavior change
   and its escape hatch ship in the same commit, and the README row
   states what `1` (or `0`) restores. See xemu-change-control.
6. If test harnesses must set it, note the env-parity requirements in
   the xemu-testing protocol (interleaved A/B runs toggle exactly one
   var).

### (c) New MVK_CONFIG_* var

1. Add key+value to `Info.plist` `LSEnvironment` (repo root; bundled
   by `build.sh:113`).
2. Mirror it in the `ui/xemu.c` `main()` block (~line 1602) as
   `setenv("MVK_CONFIG_...", "...", 0)` — same value, overwrite=0,
   before any Vulkan/MoltenVK initialization.
3. Until the mirrored build is the one under test, `export` the var in
   every harness (launch-path parity — this exact gap hid the
   pink-tile bug).
4. Update the README MVK table (~line 718) — and note it is already
   stale (PREFILL row); fixing it belongs to xemu-docs-and-writing.
5. `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` stays `0`. Changing it
   is a change-control event with the full artifact-oracle gauntlet,
   not a tweak.

---

## Known README-vs-code drift — flag items (as of 2026-07-05)

State ground truth from this catalog; cite the stale doc when relevant.
The full dated drift list is owned by **xemu-docs-and-writing §4** —
keep the two sections in sync.

The four flag items this skill's 2026-07-04 audit found (misfiled
`XEMU_COREAUDIO_FRAMES`, missing `XEMU_VIS`/`XEMU_STRIP` rows, missing
`XEMU_DSP_JIT` + dev-subflag rows, and the "`XEMU_DSP_JIT=1` enables
the JIT" overclaim) were **all fixed in the 2026-07-05 doc-cleanup
pass**: the README knob tables now match this catalog, the
`coreaudio.m` comment says runtime, and the README states engine
selection is config-only with `XEMU_DSP_JIT=0` as the in-engine
kill-switch. No open flag-drift items; re-audit after the next big
README edit.

---

## When NOT to use this skill

- Running the build, environment setup, MoltenVK provisioning tiers,
  PGO walkthrough, CI matrix -> **xemu-build-and-env**.
- What evidence a flag/default change needs before shipping, commit
  conventions, Windows-preservation gate -> **xemu-change-control**.
- How to actually run an A/B with these env vars (savestate protocol,
  interleaving, input pipe usage) -> **xemu-testing**.
- Interpreting the output the diagnostic vars produce (nsprof
  counters, APU_PROF numbers, DIFF validator) ->
  **xemu-diagnostics-and-tooling**.
- Launching the app, monitor socket, snapshots, CLI flags like
  `-monitor`/`-dvd_path` -> **xemu-run-and-operate**.
- Why a given escape hatch exists (the war story) ->
  **xemu-failure-archaeology**.

---

## Provenance and maintenance

Everything above was read from the tree on 2026-07-04. Regenerate each
table with these commands (run from the repo root); if output differs
from this file, the code wins — update this skill.

```bash
# 1. Authoritative XEMU_* runtime env var list (expect 30 distinct
#    variables across 68 getenv lines as of 2026-07-11)
grep -rn 'getenv("XEMU_' --include='*.c' --include='*.cc' \
  --include='*.m' --include='*.mm' --include='*.inc' --include='*.h' . \
  | grep -v '^\./build/' | grep -v '^\./dist/'

# 2. Build knobs + args
grep -n 'XEMU_ARM_CPU\|XEMU_PGO\|XEMU_CODESIGN\|XEMU_MOLTENVK_VERSION\|XEMU_VIS\|XEMU_STRIP\|x86_version' build.sh
grep -n 'XEMU_MVK_MCPU\|MVK_PIN' scripts/build-moltenvk.sh

# 3. MVK parity (all three must agree; README fixed 2026-07-05 — any
#    disagreement is new drift)
grep -n 'MVK_CONFIG' ui/xemu.c Info.plist README.md

# 4. Config generation chain + implicit defaults
grep -n 'genconfig\|config_spec' meson.build          # expect 2508-2509, 3806-3809
sed -n '72,84p' subprojects/genconfig/gen_config.py    # implicit-default map

# 5. Config file location / portable mode / CLI override
grep -n 'SDL_GetBasePath\|SDL_GetPrefPath\|portable' ui/xemu-settings.cc
grep -n 'config_path' ui/xemu.c

# 6. DSP engine precedence + env override semantics
grep -n -A 10 'dsp_want_external_jit_engine' hw/xbox/mcpx/apu/dsp/dsp.c
grep -n 'XEMU_DSP_JIT' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c

# 7. Spot-check volatile single facts
grep -n 'PREFILL' Info.plist ui/xemu.c README.md       # 0 in all three (README fixed 2026-07-05)
grep -n 'max_queries_in_flight = ' hw/xbox/nv2a/pgraph/vk/reports.c   # 4096
grep -n 'default_frames' audio/coreaudio.m             # 1024
grep -n 'metalfx' ui/xemu-settings.cc                  # legacy-bool migration
grep -n 'cache_shaders' -r hw/ ui/                     # GL-only gate
wc -l config_spec.yml                                  # 404 as of 2026-07-11
```

Drift-prone facts to re-check first when this skill feels wrong: the
env-var count (22), the MVK five-tuple, `XEMU_MOLTENVK_VERSION`
default (1.4.1) and the build-moltenvk pin (096714a2… / "1.4.2"),
`audio.dsp_jit.enabled` default (false), and the README tables listed
in the drift section.
