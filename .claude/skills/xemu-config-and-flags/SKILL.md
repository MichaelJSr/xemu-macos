---
name: xemu-config-and-flags
description: >
  Complete catalog of every configuration axis in the xemu-macos fork:
  xemu.toml settings (config_spec.yml -> genconfig -> g_config, defaults,
  restart semantics, portable mode, -config_path), all XEMU_* runtime
  environment variables (diagnostics, escape hatches, test harness, A/B
  knobs, POSIX guards), the six canonical MVK_CONFIG_* MoltenVK values
  and the Info.plist/main() launch-path parity rule, and build.sh /
  build-moltenvk.sh build-time knobs (XEMU_ARM_CPU, XEMU_PGO,
  XEMU_VIS, XEMU_STRIP, -j/-p/-a/--debug, x86_version).
  Load this skill when you
  need to: look up a setting or env var name/default/effect, decide
  production vs experimental vs test-only status, add a new xemu.toml
  setting / XEMU_* env var / MVK_CONFIG_* var (checklists included),
  find where a flag is read in code, understand DSP JIT engine toggle
  precedence (audio.dsp_jit.enabled vs audio.use_dsp_jit vs
  XEMU_DSP_JIT), locate the user's config file, or re-verify the flag
  tables after the tree changes. Keywords: xemu.toml, g_config,
  config_spec.yml, XEMU_INPUT_PIPE, XEMU_NV2A_NSPROF, XEMU_VTX_EXACT,
  XEMU_REPORTS_SYNC, XEMU_DSP_JIT, XEMU_ELIDE_CANDOIO, XEMU_DIRTY_FAST,
  XEMU_X87_ELIDE_FT0, XEMU_X87_DEFER_FIP, XEMU_MFX_RESIZE_QUANTIZE,
  MVK_CONFIG_PREFILL, MVK_CONFIG_LOG_LEVEL,
  LSEnvironment, portable mode, escape hatch, default-on knob,
  default value, add a flag.
---

# xemu-macos configuration and flags catalog

Every knob in this fork lives on one of four axes. All facts below were
verified against the tree on **2026-07-04** (branch `macos-optimizations`,
v0.9 era); env-var/spec counts, the `ui/xemu.c` setenv anchor, and MVK
parity were re-verified 2026-07-11 at `7e2e6e7256`. The **2026-08-04
optimization wave** (implementing `docs/Finished/fork-optimization-audit-2026-08.md`)
added fourteen runtime knobs and three build knobs and changed two
defaults — those rows carry that date, and every build.sh line anchor
below was re-derived the same day. The final section
gives one-line commands to re-derive each
table, because flags drift and this catalog must be re-checkable, not
trusted.

> **Scope note (2026-10-10).** The Axis 2 runtime tables cover every
> `getenv("XEMU_` site again: `grep` reports **62** distinct `XEMU_*`
> variables in fork code as of 2026-10-10 (68 on 2026-08-04), and the
> campaign-era knobs that went unfiled from v0.9 to 2026-08-04
> (subpage/xpage/superblock/region, present ring, MMIO profiling, the
> 2026-09-08 Windows-port hatches) were filed on 2026-10-10. The
> Provenance grep stays the authority; the `docs/optimizations.md`
> "Runtime escape-hatch / diagnostic knobs" table is the user-facing
> home. If the count moves, a row is missing here.

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

`config_spec.yml` (repo root, 413 lines as of 2026-10-10, the authoritative schema)
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
  (`ui/xemu-settings.cc:51-63,73-92`). This fork uses **SDL3**
  (`meson.build:1687`), and SDL3's base path for a macOS app bundle is
  `Contents/Resources/` — NOT `Contents/MacOS/`. For a bare
  `build/qemu-system-i386` binary it is the executable's directory.
  **Trap:** `./build.sh` repackaging starts with `rm -rf dist`
  (`build.sh` `package_macos`), so a portable config planted inside
  `dist/xemu.app/Contents/Resources/` vanishes on every rebuild.
- CLI override: `xemu -config_path <file>` (`ui/xemu.c:1989-1998` ->
  `xemu_settings_set_path`). Harnesses use this to avoid hijacking the
  user's real config.
- Load failure = fatal: error dialog then `exit(1)`; settings are
  saved back on exit via `atexit(xemu_settings_save)`
  (`ui/xemu.c:2000-2009`, as of 2026-10-10).

### Operational settings table (verified in `config_spec.yml` + consumers)

Status legend: **prod** = supported default path; **rec** = production,
recommended non-default on Apple Silicon; **debug** = development aid;
**legacy** = kept only for migration.

| Setting | Type / values | Default | Status | Effect / notes |
|---|---|---|---|---|
| `display.renderer` | `NULL`\|`OPENGL`\|`VULKAN` | `VULKAN` | prod | Renderer choice. The fork's optimizations live in the Vulkan/MoltenVK path. |
| `display.window.presentation_backend` | `auto`\|`opengl`\|`metal` | `auto` | prod | macOS-only: which API presents the final frame + UI. `auto` -> Metal when renderer is VULKAN. **Resolved once before window creation and fixed for the process lifetime** (`ui/xemu.c:94-123` `xemu_present_is_metal`); changing it, or switching renderer under `auto`, requires a restart. |
| `display.metalfx_mode` | `off`\|`spatial`\|`temporal` | `spatial` | prod | MetalFX upscale mode (UI: `ui/xui/main-menu.cc:832`). |
| `display.metalfx_upscale` | bool | `false` | legacy | Old on/off bool. Migrated at load: `true` + `metalfx_mode=off` -> `metalfx_mode=spatial`, then always reset to `false` (`ui/xemu-settings.cc:206-213`). Never write new code against it. |
| `display.frame_interpolation` | `off`\|`2x`\|`4x` | `off` | prod | MTLFXFrameInterpolator; requires macOS 26 (`@available` check in `metalfx_upscale.m`). UI: `main-menu.cc:837`. |
| `display.quality.surface_scale` | integer | `1` | prod | Internal render-resolution multiplier; UI drives it via `nv2a_set_surface_scale_factor` (`main-menu.cc:814-827`). |
| `display.window.vsync` | bool | `true` | prod | Present sync. |
| `display.window.fullscreen_on_startup` | bool | `false` | prod | With `fullscreen_exclusive` (bool, `false`). |
| `display.window.startup_size` | enum | `1280x960` | prod | Plus `last_width`/`last_height` (640/480) when `last_used`. |
| `display.vulkan.validation_layers` | bool | `false` | debug | Enables VK validation if the layer is installed (`pgraph/vk/instance.c:247`). Siblings: `debug_shaders`, `assert_on_validation_msg`, `preferred_physical_device`. |
| `audio.dsp_jit.enabled` | bool | **`false`** | rec | Fork's ARM64 inline basic-block DSP JIT (AArch64 POSIX hosts — Apple Silicon and Linux arm64; see the precedence box). README recommends `true` on Apple Silicon; the shipped default is still false — both statements are correct, state them together. UI: `main-menu.cc:929`. |
| `audio.use_dsp_jit` | bool | **`true`** | prod | Upstream dsp56300-subproject JIT engine. **Ignored while the fork JIT is supported+enabled** — see precedence box below. UI: `main-menu.cc:936`. **Fork-owned default** (2026-09-08): upstream flipped it to `false` in `fc13b78060` and the 2026-09-07 merge took the flip; the fork restored `true` because with `dsp_jit.enabled` also false a stock config selected *no* JIT engine on any platform. Re-check after every upstream merge. |
| `audio.vp.num_workers` | integer | `0` (= auto) | prod | Voice-processor worker threads; 0 -> `SDL_GetNumLogicalCPUCores()`, clamped to `[1, MAX_VOICE_WORKERS]` (`hw/xbox/mcpx/apu/vp/vp.c:2356-2357`). |
| `audio.use_dsp` | bool | `false` (implicit) | prod | Bare-`bool` spec entry -> implicit false. |
| `perf.hard_fpu` | bool | `true` | prod | ARM64 inline x87 FPU (native AArch64 FP instead of softfloat). UI: `main-menu.cc:70`. |
| `perf.cache_shaders` | bool | `true` | prod | Gates the **OpenGL** shader disk cache only (`pgraph/gl/shaders.c:283,493,819`). The Vulkan `pipeline_cache.bin` (saved at most every 30 s at flip boundaries, write-then-rename) is **always on and not gated by this setting** (`pgraph/vk/draw.c:457-551`). |
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
(`hw/xbox/mcpx/apu/dsp/dsp.c:112-127`):

```c
static bool dsp_want_external_jit_engine(void)
{
#if DSP56K_JIT_SUPPORTED                 /* __aarch64__ && !_WIN32 */
    if (g_config.audio.dsp_jit.enabled) {
        return false;                    /* fork inline JIT wins */
    }
#endif
    return g_config.audio.use_dsp_jit;
}
```

- `audio.dsp_jit.enabled = true` (AArch64 POSIX): C interpreter engine
  with the fork's inline ARM64 JIT. `audio.use_dsp_jit` becomes a
  no-op (`dsp_set_engine` early-outs, `dsp.c:244-251`).
- `audio.dsp_jit.enabled = false` (default) + `audio.use_dsp_jit =
  true` (default): upstream dsp56300 JIT engine. **This is what a
  stock config gets on every platform, macOS included** — the fork
  JIT is opt-in.
- Both false: plain C interpreter. Between the 2026-09-07 upstream
  merge and the 2026-09-08 restoration of the fork's `use_dsp_jit`
  default, this was the stock configuration everywhere; if you are
  reading a measurement from that window, check which engine it used.

`DSP56K_JIT_SUPPORTED` is `defined(__aarch64__) && !defined(_WIN32)`
(`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h:24-28`, verified
2026-09-08) — AArch64 POSIX hosts, so Linux arm64 too, not Apple only;
Windows/ARM64 is excluded because the code buffer uses POSIX `mmap`.
The JIT source compiles to nothing elsewhere
(`hw/xbox/mcpx/apu/dsp/interp/meson.build`).

**`XEMU_DSP_JIT` does NOT switch engines.** It only overrides the
inline JIT's on/off *within the interpreter engine*
(`dsp56k_jit_arm64.c:170-183`; the config value reaches the JIT via
`dsp56k_jit_set_enabled_from_config()` called from
`mcpx_apu_dsp_init`, `gp_ep.c:553-559` — an indirection that exists so
the `tests/xbox/dsp` binary can link `libdsp` without `g_config`).
Consequences, verified in code:

- Defaults + `XEMU_DSP_JIT=1` -> the **external** engine is still
  selected, so the fork JIT does not run. Older notes saying "enable via
  `[audio.dsp_jit] enabled = true` or `XEMU_DSP_JIT=1`" are imprecise
  on this point; the `docs/optimizations.md` "Full inline DSP JIT"
  entry now states selection is config-only (as of 2026-10-10).
- `audio.dsp_jit.enabled=true` + `XEMU_DSP_JIT=0` -> interpreter
  engine with inline JIT forced off = pure-interpreter A/B leg.

---

## Axis 2: XEMU_* runtime environment variables

Authoritative list = every `getenv("XEMU_` site in fork C/ObjC code.
As of **2026-10-10** the grep reports **62 distinct variables** (68 on
2026-08-04, 30 as of 2026-07-11 — the optimization campaigns since then account for the
rest; all 62 have rows below as of 2026-10-10). **Every one is read once and
latched in a static** — set it
before launch; changing the environment mid-run does nothing. Several
are latched at *translate* time (the first TB, before any guest code
runs) so that generated code is uniform across the session; those rows
say so.

Value-parse conventions: *truthy* = any non-empty value whose first
char is not `0`; *strict* = first char must be `1`.

Categories: **diag** = diagnostic output, safe in release;
**escape** = escape hatch restoring pre-optimization legacy behavior;
**test** = test-harness/bring-up facility; **A/B** = experimental
comparison knob.

| Env var | Unset behavior | Values | Effect | Cat | Read at |
|---|---|---|---|---|---|
| `XEMU_NV2A_NSPROF` | off | truthy | Wall-time NV2A profiler; 5 s counter/event summaries to stderr (PFIFO thread) | diag | `hw/xbox/nv2a/nsprof.c:101` |
| `XEMU_PFIFO_HEARTBEAT` | off | strict | ~2 s PFIFO-thread heartbeat + pending-flag snapshot (categorize freezes: thread stuck vs waiting) | diag | `hw/xbox/nv2a/pfifo.c:520` |
| `XEMU_APU_PROF` | off | truthy | Per-second APU-thread utilization % + frames/s to stderr | diag | `hw/xbox/mcpx/apu/apu.c:249` |
| `XEMU_ZETA_SHAPE_READBACK` | off (fast path) | truthy | Restore GPU->CPU readback on zeta shape switches (full fidelity, slower) | escape | `pgraph/vk/surface.c:1788` |
| `XEMU_TEX_BIND_RECHECK` | off (fast path) | truthy | Restore per-bind texture dirty rechecks (vs once per frame) — try for stale/late textures | escape | `pgraph/vk/texture.c:816` |
| `XEMU_VTX_EXACT` | **ON** (exact refinement) | `0` disables | `0` restores legacy page-granular vertex-conflict finishes; default is byte-exact conflict refinement (**inverted** — it was the first such knob; see the default-on list below) | escape | `pgraph/vk/vertex.c:118` |
| `XEMU_REPORTS_SYNC` | off (deferred reports) | strict | `1` restores legacy synchronous zpass-report drains (vs ride-the-flip + the idle-budget fallback, 300 µs by default — `XEMU_REPORTS_BUDGET_US`; 5 ms before v0.10.1) | escape | `pgraph/vk/reports.c:295` |
| `XEMU_MAX_QUERIES` | 4096 | int 8..65536 (strtol, base auto) | Total occlusion-query pool size (`max_queries_in_flight`, partitioned across the 2 flight slots); small values exercise the begin_draw capacity-guard path | test | `pgraph/vk/reports.c:38` |
| `XEMU_INPUT_PIPE` | disabled | FIFO path (POSIX) / pipe name or path (Windows) | Input injection: lines `down <sdl_scancode>` / `up <sdl_scancode>` / `clear` OR'd into keyboard state through the user's bindings; works unfocused. **Both host families** since `164fd4c876` (2026-09-08): a POSIX FIFO opened `O_NONBLOCK`, or on Windows a `PIPE_NOWAIT` Win32 named pipe whose name is the full `\\.\pipe\...` value or the path's last component (`#ifdef _WIN32` split at `ui/xemu-input.c:548`) | test | `ui/xemu-input.c:588` (Windows), `:716` (POSIX) |
| `XEMU_MFX_REAL_DEPTH` | off (synthetic depth) | truthy | Feed real zeta MTLTexture to the MetalFX temporal scaler (known 1-frame-ahead limitation). Compiled only under `HAVE_IOSURFACE_SHARING` (= `__APPLE__`, `pgraph/vk/renderer.h:44-50`) | A/B | `pgraph/vk/surface.c:1285` |
| `XEMU_MFX_INTERP_ZERO_MOTION` | off (linked scaler) | truthy | Restore the old zero-motion frame-interpolator binding | A/B | `pgraph/vk/metalfx_upscale.m:1284` |
| `XEMU_COREAUDIO_FRAMES` | 1024 | positive int (atoi) | CoreAudio buffer frames (~21 ms @ 48 kHz at default; try 2048 for audio glitches). **Runtime**: filed in the `docs/optimizations.md` runtime-knob table, and the in-code comment says runtime (both fixed in the 2026-07-05 doc pass) | escape | `audio/coreaudio.m:598` |
| `XEMU_DSP_JIT` | follow `audio.dsp_jit.enabled` | `1` force on; any other non-empty force off | Dev override of the inline DSP JIT **within the interpreter engine only** (see precedence box — it cannot select the engine) | test | `dsp/interp/dsp56k_jit_arm64.c:180` |
| `XEMU_DSP_JIT_STATS` | off | strict | Exit-time JIT counters (incl. cf_fallback buckets) | diag | `dsp56k_jit_arm64.c:228` |
| `XEMU_DSP_JIT_DIFF` | off | N 1..1000000 | Bit-exact validate JIT vs interpreter; 1 = every unique translation, N = sample every Nth (async worker thread) | test | `dsp56k_jit_arm64.c:192` |
| `XEMU_DSP_JIT_DIFF_MAX` | uncapped | N > 0 | Stop validating after N enqueues (CI: validate first N then run free) | test | `dsp56k_jit_arm64.c:211` |
| `XEMU_DSP_JIT_DIFF_SYNC` | async | strict | Synchronous (on-thread) validation; immediate abort on divergence; not for long runs | test | `dsp56k_jit_arm64.c:225` |
| `XEMU_DSP_JIT_NO_THROTTLE` | throttle on | strict | Disable the retranslation-churn auto-throttle | A/B | `dsp56k_jit_arm64.c:233` |
| `XEMU_DSP_JIT_SENTINEL` | off | `1` (bit 0) | cur_inst poison harness from the round-4 bisect (kept in-tree deliberately) | test | `dsp56k_jit_arm64.c:239` |
| `XEMU_DSP_JIT_FORCE` | off | `1` (bit 0) | Re-apply the deferred round-4 cur_inst skip — **known-regressing**, exists only for the bisect harness | test | `dsp56k_jit_arm64.c:250` |
| `XEMU_DSP_JIT_PIN_AUDIT` | off | strict | Per-op audit that pinned A/B accumulator registers (x26/x27) match memory; ~10 extra insns/op | test | `dsp56k_jit_arm64.c:265` |
| `XEMU_DSP_JIT_DUMP` | off | strict | Hex-dump every emitted ARM64 block to stderr for offline disassembly | diag | `dsp56k_jit_arm64.c:9915` |

### Wave knobs added 2026-08-04

From the fork-wide optimization wave (`docs/Finished/fork-optimization-audit-2026-08.md`).
Same columns; every anchor re-derived against the tree on 2026-10-10.

| Env var | Unset behavior | Values | Effect | Cat | Read at |
|---|---|---|---|---|---|
| `XEMU_ELIDE_CANDOIO` | **ON** (stores elided) | `0` restores | Drops both per-TB `set_can_do_io()` bookkeeping stores from every multi-insn TB body, so `cpu->neg.can_do_io` stays true and `io_prepare` never enters `cpu_io_recompile` (`tcg_tb_lookup` + state restore + siglongjmp + `CF_MEMI_ONLY` re-translate) on guest NV2A/APU register traffic. **Auto-forced to 0** when icount is enabled or `replay_mode != REPLAY_MODE_NONE` — the only observers. Latched at the first translation, before the first TB | escape | `accel/tcg/translator.c:50` |
| `XEMU_DIRTY_FAST` | **ON** (fast path) | `0` restores | `notdirty_write` learns the CODE-dirty answer from the invalidation's return instead of re-reading the bitmap via `physical_memory_is_clean()`, and every single-page dirty query (including the TLB-refill check) reads all five clients under **one** RCU guard with one idx/offset + `test_bit` instead of five guards and five `find_next_bit` loops. `0` restores both legacy shapes on the same binary. **Carries no fps claim** — archaeology 6.10 | escape | `system/physmem.c:1064` |
| `XEMU_X87_ELIDE_FT0` | **ON** | `0` restores | Skips the FT0 scratch write-back at a register-cache flush when no reader of that value is still ahead in the same guest instruction. `0` restores the unconditional store | escape | `target/i386/tcg/xemu-x87.h:143` |
| `XEMU_X87_DEFER_FIP` | **ON** | `0` restores | Defers the x87 exception-pointer stores (FIP/FCS/FDP/FDS) from one per instruction to one per contiguous x87 run, and hoists the CS-selector load out of the per-instruction path. `0` restores per-instruction stores | escape | `target/i386/tcg/xemu-x87.h:157` |
| `XEMU_X87_CENSUS` | off | `1` | Arms the x87/SSE census + atexit dump (FT0 write-back elidability split, exception-pointer store counts, x87 run length, SSE scalar-compare mix). Emits counter RMWs into generated code — **measurement mode only, never for an fps run** | diag | `target/i386/tcg/xemu-x87.h:112` |
| `XEMU_X87_REFUTE` | off | `1` | Adversarial shadow-checker for the two shipped x87 mechanisms (the `ELIDE_FT0` elision and the `DEFER_FIP` deferral): keeps every legacy store, computes the elision/deferral decision anyway, and counts every point where the elided state could have been observed. Implies the census dump; promotion gate is zero violations. **Stacking limit applies** — see xemu-testing / archaeology 9.10 | test | `target/i386/tcg/xemu-x87.h:125` |
| `XEMU_SUBPAGE_FAST_STATS` | off (**default flipped 2026-08-04**) | `1` | Re-enables the six diagnostic counter RMWs the arm-(b) store prefilter emits into every aarch64 store slow-path stub, and restores the atexit seen/skips/demote histogram. Production now emits none of them (~39 fewer insns per stub, 12 on the inline-skip path); with it off the dump prints `subpage-fast counters disabled (set XEMU_SUBPAGE_FAST_STATS=1)` so a zeroed histogram can't read as a dead mechanism. Latched at first stub emission; `XEMU_SUBPAGE_FAST_REFUTE=1` forces it on | diag | `accel/tcg/xemu-subpage-fast.c:167` |
| `XEMU_SURFACE_CB_STATS` | off | `1` | Censuses NV2A surface CPU-access-callback register/unregister events (each = `async_safe_run_on_cpu` **plus** a full all-mmuidx TLB flush **plus** an unconditional jump-cache wipe), split by `update_surface_part`'s invalidate-then-create path vs elsewhere; atexit dump. Any first digit `1`-`9` arms it. The `=2` abort level, the reuse-pool counters and the coverage audit were deleted with `XEMU_SURFACE_CB_REUSE` on 2026-10-10 | diag | `pgraph/vk/surface.c:752` |
| `XEMU_PFIFO_KICK_STATS` | off | `1` | Censuses `pgraph_write`'s `pfifo_kick` broadcasts (call site × whether a wait condition actually transitioned × post-write `FIFO_ACCESS` × kick-already-pending), plus the PFIFO thread's idle-park count as the wakeup denominator. **Counter only** — no kick is ever suppressed (archaeology 7.1: a wrongly-skipped kick is a permanent PFIFO hang) | diag | `pgraph/pgraph.c:192`, `pfifo.c:528` |
| `XEMU_MFX_RESIZE_QUANTIZE` | **ON** | `0` restores | Snaps the MetalFX output width to a 64-px ladder and holds a new shape ~200 ms before rebuilding the scaler + its 3-texture ring, so a window drag stops rebuilding at the event rate (each rebuild drains MetalFX in-flight work and stalls the PFIFO thread). Height re-derived from the quantized width; an explicit floor keeps the output above `disp->width` so quantization can never silently disable upscaling; a guest mode change rebuilds immediately | escape | `pgraph/vk/display.c:1959` |
| `XEMU_PRESENT_DRAWABLE_FIRST` | off (opt-in) | truthy | **Experimental, opt-in dark**: the fps delta read +0.50, under the noise line, so it has no promotion claim. No Failed-table row: the ledger files it under Changes, "Drawable-first present ordering" (`docs/optimizations.md` MetalFX + presentation). Acquires the CAMetalLayer drawable **before** consuming the push-present ring, so the step to present is chosen after the `nextDrawable` block. Only the acquire moves — command buffer → shared-event wait → render pass stay together and in order (architecture-contract Invariant 8). Metal presentation backend only; latency-only by construction, so A/B it at the default `XEMU_PUSH_DEBT` or it measures nothing | A/B | `ui/xemu.c:1242` |
| `XEMU_UI_LOCK_STATS` | off (`DEBUG_XEMU_C=1` forces on) | truthy | Enables the once-per-second UI main-loop-mutex + BQL occupancy line (`[[ vblank @NHz avg - bql Nns/iter, N% time avg ]]`) in a normal build. Compiled in on macOS and in `DEBUG_XEMU_C` builds; Windows/Linux release paths preprocess away | diag | `ui/xemu.c:248` |

### Campaign-era and later knobs (filed 2026-10-10)

Rows for every remaining `getenv("XEMU_` site, so the Axis 2 tables now
cover all 62 names. Facts mirror the `docs/optimizations.md` "Runtime
escape-hatch / diagnostic knobs" table (the complete user-facing list;
where the two disagree, that table and the code win). Anchors read
2026-10-10 at `b7e2752589`. Status values: **production** (shipped
default path or its hatch), **experimental** (opt-in, no promotion
claim), **test-only** (measurement or falsification, never an fps run).
Closed experiments are not kept in the code (see "Closed experiments"
below).

#### Escape hatches and production tuning

| Env var | Unset behavior | Values | Effect | Cat | Read at |
|---|---|---|---|---|---|
| `XEMU_MMIO_BQL` | off (lockless dispatch) | strict `1` | **Production** hatch: `1` restores BQL-locked TCG dispatch for the audited lockless NV2A regions (PFB, USER, PGRAPH) and the APU VP region | escape | `hw/xbox/nv2a/nv2a.c:392`, `hw/xbox/mcpx/apu/apu.c:457` |
| `XEMU_PGRAPH_LOCKLESS` | **ON** (PGRAPH lockless) | `0` restores | **Production** per-block hatch: `0` restores BQL dispatch for PGRAPH only, leaving PFB/USER lockless (`docs/Finished/pgraph-lockless-audit.md`). Inert when `XEMU_MMIO_BQL=1` | escape | `hw/xbox/nv2a/nv2a.c:398` |
| `XEMU_REPORTS_BUDGET_US` | 300 | int µs (atoll), clamped 0..100000 | **Production** tuning: continuous-idle budget before the deferred-report safety-valve submit; `5000` restores the pre-v0.10.1 value. The 50 µs scan is a Failed row (−12.1 fps), so 300 is validated as above the cliff | escape | `pgraph/vk/reports.c:312` |
| `XEMU_RAS` | **ON** (near-return memo) | `0` disables | **Production**: 4096-entry eip→TB memo probed inline at ret sites (+0.77 fps, 6/6). `-d exec` tracing won't log inline-hit rets, so disable when tracing. Translate-side and helper-side latches read the same var | escape | `target/i386/tcg/translate.c:3189`, `xemu_ras_helper.c:44` |
| `XEMU_SPIRV_CACHE` | **ON** | `0` bypasses | **Production**: `0` skips the on-disk SPIR-V cache entirely (no loads, no stores), a cold-compile A/B without deleting the cache dir. Blob validation on load is not gated by this knob | escape | `pgraph/vk/glsl.c:77` |
| `XEMU_SPIRV_CACHE_ATOMIC` | **ON** (tmp + rename) | `0` restores | **Production**: `0` restores the legacy in-place `.spv` write; default writes `<hash>.spv.<pid>.tmp` and `g_rename`s it over the entry | escape | `pgraph/vk/glsl.c:87` |
| `XEMU_SSE_HOST` | aarch64: **ON** when `perf.hard_fpu`; elsewhere off | `0` off, `1` on (needs `hard_fpu`), `2` both paths + abort on divergence | Host-SIMD fast path for single-precision SSE arithmetic. **Production** on aarch64 (+1.90 fps); **experimental** (opt-in, dark) on x86_64 until `=2` runs clean on real silicon | escape | `target/i386/tcg/fpu_helper.c:3795` |
| `XEMU_SSE_NEON` | — | as `XEMU_SSE_HOST` | Legacy alias since `bb9f464c62` (2026-07-04), read only when `XEMU_SSE_HOST` is unset | escape | `target/i386/tcg/fpu_helper.c:3797` |
| `XEMU_APU_RAM_DIRTY` | **off on Apple, ON elsewhere** | strict `1` | **Production** (landed 2026-09-08): with it on, the APU's fast voice-register stores also mark the written range dirty for the `DIRTY_CLIENTS_NOCODE` clients. Off on Apple because the marking's cost there was never measured; `=1` opts in on Apple, `=0` opts out elsewhere. Read once in `mcpx_apu_vp_init` | escape | `hw/xbox/mcpx/apu/vp/vp.c:2492` |
| `XEMU_VK_VOLK_DEVICE` | **ON** (non-Apple only) | `0` restores | **Production** (2026-09-08): `0` keeps device-level Vulkan calls on the loader trampoline instead of `volkLoadDevice()` direct dispatch. Hygiene, not a measured win. Never read on Apple (`#ifndef __APPLE__`) | escape | `pgraph/vk/instance.c:507` |
| `XEMU_WIN32_DXGI` | **ON** (DXGI flip-model, Windows only) | `0` restores | **Production** (2026-09-08, `164fd4c876`): `0` skips upstream's DXGI/WGL_NV_DX_interop presenter and presents with `SDL_GL_SwapWindow`. Latched in `display_early_init()` | escape | `ui/xemu.c:138` |
| `XEMU_PUSH_PRESENT` | **ON** (Metal backend) | `0` restores | **Production** since 2026-07-12: each flip's present schedule goes into a ring the UI reads with no cross-thread round trip; `0` restores the legacy pull handshake. Ignored on the GL backend | escape | `pgraph/vk/display.c:104` |
| `XEMU_PUSH_DEBT` | interp mode + 2 steps | int (atoi); `0` = strict FIFO | **Production** tuning: consumer catch-up bound for the push-present ring. `0` restores the pre-fix strict FIFO that judders when the guest step rate beats the display | escape | `pgraph/vk/display.c:282` |
| `XEMU_SUBPAGE_DIRTY` | **ON** | `0` restores | **Production** since 2026-07-11: per-page 64-block code bitmap lets a data store that misses every code sub-block skip whole-page invalidation (+14.3% fps F8, +24.6% F5 at promotion). `0` restores whole-page invalidation and also forces `XEMU_SUBPAGE_FAST` off | escape | `accel/tcg/tb-maint.c:407` |
| `XEMU_SUBPAGE_FAST` | **ON on Apple**, off on other hosts | `0` / `1` | **Production** on Apple since 2026-07-18 (arm (b), `ede35b84f9`): the aarch64 store slow-path stub completes provable no-op stores inline (+12% draws/s). Emitted only by the aarch64 TCG backend: non-Apple aarch64 opts in with `1`; on non-aarch64 hosts an explicit `1` is inert and says so once on stderr (`tb-maint.c:419`) | escape | `accel/tcg/xemu-subpage-fast.c:129` |
| `XEMU_XPAGE_CHAIN` | **ON, level 2** (broad) | `0` upstream same-page-only, `1` kernel-identity window only | **Production** since 2026-07-12: cross-page direct chaining, sound by its INVLPG + CR3 + full-flush backstops | escape | `accel/tcg/xemu-xpage.c:72` |
| `XEMU_XPAGE_UNLINK` | **ON** | `0` restores | **Production** since v0.11.1: the full-flush backstop severs only registered cross-page chains; `0` restores the v0.11 whole-`tb_flush` backstop that caused the new-area lag | escape | `accel/tcg/xemu-xpage.c:108` |

#### Diagnostics and test harnesses

| Env var | Unset behavior | Values | Effect | Cat | Read at |
|---|---|---|---|---|---|
| `XEMU_INV_PROF` | off | strict `1` | **Test-only**: exit dump of SMC / TB-invalidation counters plus the `(f)` MMIO line (`io_prepare` / `cpu_io_recompile` rates, the gate counter for `XEMU_ELIDE_CANDOIO`; read it from an `XEMU_ELIDE_CANDOIO=0` run). Also arms `XEMU_XPAGE_PROF` | diag | `accel/tcg/tb-maint.c:240`, `xemu-xpage.c:145` |
| `XEMU_INV_TIMING` | off | strict `1` | **Test-only**: adds a cycle-counter split of the `notdirty_write` body to the `XEMU_INV_PROF` dump; tick source per host (`accel/tcg/xemu-inv-prof.h`) | diag | `accel/tcg/tb-maint.c:255` |
| `XEMU_TB_PROF` | off | strict `1` | **Test-only**: TB jump-cache totals at exit (lookups, hit%, htable walks, translations, `tb_flush` count) | diag | `accel/tcg/cpu-exec.c:618`, `xemu_ras_helper.c:87` |
| `XEMU_GUEST_PROF` | off | strict `1` | **Test-only**: one-run guest profiler (mach-thread sampler resolves vCPU samples to guest TBs vs host symbols). Not benchmark-neutral | diag | `accel/tcg/cpu-exec.c:320`, `xemu_ras_helper.c:88` |
| `XEMU_MMIO_PROF` | off | strict `1` | **Test-only**: exit histogram of guest MMIO traffic (region × page, loads/stores) plus BQL acquire-wait totals | diag | `accel/tcg/cputlb.c:2089` |
| `XEMU_XPAGE_PROF` | off | strict `1` | **Test-only**: cross-page-direct exit census split by target region; atexit summary | diag | `accel/tcg/xemu-xpage.c:144` |
| `XEMU_XPAGE_REFUTE` | off | strict `1` | **Test-only**: cross-page-chaining refuter, routes every cross-page-direct exit through the safe lookup plus a phys-unchanged check. Slow; falsification only | test | `accel/tcg/xemu-xpage.c:131` |
| `XEMU_SUBPAGE_REFUTE` | off | strict `1` | **Test-only**: checks each sub-page skip decision against a live-TB byte-overlap scan and counts violations; no behavior change unless `XEMU_SUBPAGE_DIRTY` is on | test | `accel/tcg/tb-maint.c:437` |
| `XEMU_SUBPAGE_FAST_REFUTE` | off | strict `1` | **Test-only**: routes every would-skip decision to a C validator that counts violations and performs the real store; forces `XEMU_SUBPAGE_FAST_STATS=1` | test | `accel/tcg/xemu-subpage-fast.c:142` |
| `XEMU_PUSH_PRESENT_REFUTE` | off | strict `1` | **Test-only**: peeks the push-present ring and cross-checks the consumed-step stream against the pull path, printing tallies every ~5 s. Re-adds the pull round trip, so measure perf with it off | test | `ui/xemu.c:1096` |
| `XEMU_UI_FRAME_CAP_NS` | 0 (no cap) | int ns (atoll) | **Test-only**: floors the UI present period (`16666666` ≈ a 60 Hz consumer) so ring pacing is visible in unfocused bench windows, which present unpaced | test | `ui/xemu.c:1212` |

#### Closed experiments

None are kept in the code. Since 2026-10-10 a measured neutral or
negative experiment is removed from HEAD and keeps its row in
`docs/optimizations.md` "Failed / reverted experiments"
(`awk '/^## Failed/,/^## Settled/' docs/optimizations.md`); the ones that
had shipped dark are listed under "Retired diagnostics and dark
experiments (2026-10-10)". Restore one by reverting its removal commit.

**The default-ON (inverted) set as of 2026-08-04** — for these, `=0` is
the escape hatch and the *unset* state is the new behavior:
`XEMU_VTX_EXACT`, `XEMU_ELIDE_CANDOIO`, `XEMU_DIRTY_FAST`,
`XEMU_X87_ELIDE_FT0`, `XEMU_X87_DEFER_FIP`, `XEMU_MFX_RESIZE_QUANTIZE`,
plus the campaign-era `XEMU_SUBPAGE_DIRTY` (`tb-maint.c:407`),
`XEMU_SUBPAGE_FAST`, `XEMU_XPAGE_CHAIN`/`XEMU_XPAGE_UNLINK` and
`XEMU_PUSH_PRESENT` (`display.c:104`). Filed 2026-10-10 with the same
shape: `XEMU_RAS`, `XEMU_PGRAPH_LOCKLESS`, `XEMU_SPIRV_CACHE`,
`XEMU_SPIRV_CACHE_ATOMIC`, and per platform `XEMU_SSE_HOST` (aarch64),
`XEMU_APU_RAM_DIRTY` (non-Apple), `XEMU_VK_VOLK_DEVICE` (non-Apple) and
`XEMU_WIN32_DXGI` (Windows). Reading one of these as
"off unless set" is the most common way to mis-plan an A/B — and
`bench-savestate-ab.sh` hardcodes its B arm to `0`, which is the right
baseline for exactly these and the wrong one for a numeric knob (see
xemu-testing).

Notes:

- All `XEMU_DSP_JIT_*` vars are inert unless `DSP56K_JIT_SUPPORTED`
  (AArch64 POSIX) and the interpreter engine is running.
- The knob-table header "All default off / fast-path; set to `1` to
  enable" (`docs/optimizations.md` "Runtime escape-hatch / diagnostic
  knobs", ~:16 as of 2026-10-10) is a simplification: `XEMU_VTX_EXACT` is
  default-on/inverted, `XEMU_MAX_QUERIES` and `XEMU_COREAUDIO_FRAMES`
  are numeric, `XEMU_INPUT_PIPE` takes a path.
- Build-time only: `XEMU_DEBUG_BUILD` is a **compile define** set by
  `./build.sh --debug` (`build.sh:653`), not an env var you export at
  runtime.

---

## Axis 3: MVK_CONFIG_* MoltenVK driver configuration

**Six** canonical values as of 2026-08-04 (five until `MVK_CONFIG_LOG_LEVEL`
landed with the MoltenVK pin bump), set in **two places that must stay
identical**:

1. `Info.plist` `LSEnvironment` dict (repo root, lines 35-56; copied
   into the bundle by `build.sh`'s `package_macos`) — applied by
   LaunchServices to **Finder/`open` launches only**.
2. `ui/xemu.c:1939-1945` (as of 2026-10-10) in `main()`, `setenv(..., ..., 0)` before
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
| `MVK_CONFIG_LOG_LEVEL` | `1` | Errors only (driver default is INFO); added 2026-08-04 with the MoltenVK pin work |

**The parity rule** (the pink-tile lesson): LSEnvironment applies only
to LaunchServices launches, so before the `main()` mirror existed,
terminal-launched harnesses tested MoltenVK *defaults* while the user's
Finder launches ran the plist config — a corruption bug hid there for
weeks. Therefore:

- Any **new** MVK_CONFIG var must be added to BOTH the plist and the
  `main()` setenv block, and until a build containing the mirror is
  what you're testing, every harness must `export` it explicitly.
- **Resolved 2026-08-04**: the MoltenVK table moved out of the README in
  the 2026-07-18 docs split and now lives in `docs/optimizations.md`
  (`### MoltenVK runtime config`, ~:1872 as of 2026-10-10), where it agrees with the code on all six values
  including `PREFILL=0` and `LOG_LEVEL=1`. The old "README lists
  PREFILL=2" drift note that stood here is retired; the README carries
  no MVK table at all.

---

## Axis 4: build-time knobs

Consumed by `build.sh` (and `scripts/build-moltenvk.sh`) at build time;
they change the produced binary, not a running one.

Line anchors re-derived 2026-10-10 (`build.sh` is 1056 lines; it passed
800 in the 2026-08-04 wave; re-grep rather than trusting these after any
build.sh edit).

| Knob | Default | Effect | Where |
|---|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; otherwise auto-detect `apple-mN` from `sysctl machdep.cpu.brand_string` with a clang-acceptance walk-down, fallback `apple-m1`. Since 2026-08-04 a detected `-mcpu` **above** `apple-m1` prints a loud redistribution warning, and macOS CI pins `apple-m1` so a runner-silicon refresh cannot silently raise the shipped ISA floor | `build.sh` Darwin arm (`grep -n XEMU_ARM_CPU build.sh`) |
| `XEMU_PGO` | unset | `generate` -> `-fprofile-generate`; run games; `use` -> rebuild `-fprofile-use`. Errors out if `use` finds no profiles. **Since 2026-09-08 the mechanism follows the compiler**: `pgo_compiler_family()` probes `cc -dM -E -` for `__clang__`; clang merges `.profraw` via `llvm-profdata`, GCC (stock MSYS2 MINGW64, distro Linux) reads `.gcda` straight out of the dir and adds `-fprofile-update=atomic` on generate. Each arm reports the other's artifacts by name instead of building unprofiled. Works on macOS, Linux **and** native Windows/MSYS2 (the Windows arm normalises the profile dir with `cygpath -m` first — untested on Windows so far) | `build.sh` `pgo_compiler_family`/`setup_pgo` (`grep -n "XEMU_PGO:-" build.sh`) |
| `XEMU_PGO_DIR` | `${PWD}/pgo` | Profile directory for both stages. On the native Windows arm it is rewritten to its `cygpath -m` (Windows-absolute) form before `setup_pgo` runs, or a native `gcc.exe`/`clang.exe` bakes the POSIX spelling into the instrumented binary and the profiles land somewhere discovery never looks | `build.sh` (`grep -n XEMU_PGO_DIR build.sh`; the `cygpath -m` rewrite sits on the `CYGWIN*|MINGW*|MSYS*` arm) |
| `XEMU_PGO_STALE_FATAL` | `0` (warn only) | **2026-08-04.** With `XEMU_PGO=use`, `1` turns a PGO CFG-hash mismatch on a hot-path function name (`cpu_exec*`/`helper_*`/`tlb_*`/`tcg_*`/`pgraph_*`) into a build failure instead of a loud warning. Deliberately not enabled in CI yet — flip it once the gate is proven quiet on a freshly retrained tree | `build.sh` `check_pgo_staleness` (`grep -n XEMU_PGO_STALE_FATAL build.sh`) |
| `XEMU_PGO_RETRAIN_REF` | auto | **2026-08-04.** Commit the staleness *age* is measured from. Auto-derivation: last commit touching `pgo/`, else the commit that was HEAD at the newest profile artifact's mtime; degrades to "unknown"/"n/a" with no error in a source tarball with no `.git`. **Known blind spot**: the age half cannot see an uncommitted tree, so a dirty working tree reads as freshly retrained — the CFG-mismatch half is what catches that case | `build.sh` (`grep -n XEMU_PGO_RETRAIN_REF build.sh`) |
| `XEMU_WIN_O3` | `0` (off) | **2026-09-08, native MSYS2/MinGW only.** `1` adds `-Doptimization=3 -Dstack_protector=disabled` to the native Windows release; unset, both Windows arms keep meson's project default `optimization=2` and the probed `-fstack-protector-strong` (which stock MSYS2 MINGW64 does get — `meson.build:512-536` probes by compiling *and* linking). Experimental: no CI leg builds it and the 2026-09-08 wave-1 review names the flip as the differential behind the native-Windows exit-139. `win64-cross` ignores it, with a message, so release artifacts cannot pick it up | `build.sh` native MSYS2 arm (`grep -n "XEMU_WIN_O3:-0" build.sh`) |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | `1` = local hardened-runtime codesign with `xemu.entitlements` (allow-jit + allow-unsigned-executable-memory), re-signing every bundled dylib to match. Default is ad-hoc sign preserving metadata. Distributable signing lives in `scripts/sign-macos-release.sh` | `build.sh` `package_macos` (`grep -n XEMU_CODESIGN_ENTITLEMENTS build.sh`) |
| `XEMU_MOLTENVK_VERSION` | **`1.4.2`** (was `1.4.1` until 2026-08-04) | Official MoltenVK release auto-vendored into `macos-libs/<arch>` **only when no dylib exists** in: vendored dir, `/usr/local/lib`, `/opt/homebrew/lib` (same order used at bundle time; missing dylib at packaging = hard error with provenance version+UUID logging). **This default is the single home for the expected version** — macOS CI awk-parses it out of build.sh and fails the job if the packaged bundle's provenance line disagrees | `build.sh` MoltenVK helpers (`grep -n XEMU_MOLTENVK_VERSION build.sh`), `package_macos` provenance echo |
| `XEMU_MVK_MCPU` | `apple-m2` | `-mcpu` for the maintained optimized MoltenVK build | `scripts/build-moltenvk.sh:34` |
| `XEMU_VIS` | `0` | `1` = `-fvisibility=hidden` (~-1.5 MiB arm64 binary; perf within noise) | `build.sh` Darwin arm (`grep -n XEMU_VIS build.sh`) |
| `XEMU_STRIP` | `0` | `1` = `-Wl,-dead_strip` (combined with VIS ~-4 MiB); validate before baking in — QEMU relies on `__attribute__((constructor))` registration | `build.sh` Darwin arm (`grep -n XEMU_STRIP build.sh`) |

**Harness knob (neither build-time nor read by the binary)**:
`XEMU_BENCH_ISOLATE_CACHES` (default `0`; equivalent flag
`--isolate-caches`) is consumed by `scripts/bench-savestate-ab.sh:102`.
`1` plants the scratch config as a portable-mode marker inside the
cloned bundle's `Contents/Resources`, moving xemu's data base path
(`spirv_cache_v*/`, `pipeline_cache.bin`) into the disposable work dir,
so caches start cold at batch start, warm during the warmup run, and
are shared identically by both arms. Default keeps today's warm-shared
behaviour so existing baselines stay comparable; the choice is recorded
as `cache_isolation` in the run's `meta.json`/`receipt.json`.

**Harness knob**: `XEMU_TEST_CONFIG` (unset = `../fixtures/xemu.toml`
beside the checkout; added 2026-10-10) is the source config the macOS
harnesses clone the HDD, EEPROM and scratch config from:
`bench-savestate-ab.sh`, `bench-abx.sh`, `boot-smoke-corpus.sh`,
`probe-run.sh`, `soak-cycles.sh`, `movement-probe.sh`. `--config-src`
wins where a script takes it. A missing file is a hard error, never a
fallback to the owner's personal `xemu.toml`. The Windows harness
(`bench-savestate-ab-win.py`) still defaults to `%APPDATA%`.

`scripts/build-moltenvk.sh` specifics: pins MoltenVK commit
`67b2682699d7903606b97a0392061d85d27d49e6` (`MVK_PIN`,
`build-moltenvk.sh` — **MoltenVK main, 54 commits past the v1.4.2 tag,
self-reports 1.4.3**; moved there 2026-10-10 from the v1.4.2 tag
`db66022459` of 2026-08-04, itself moved from the 2026-07 `096714a295…`
rc1 commit), builds
arm64-only at `-O3` + `-mcpu=$XEMU_MVK_MCPU`,
**deliberately no LTO** (thin-LTO bitcode breaks the ShaderConverter
xcframework packaging; measured no fps benefit), installs to
`/usr/local/lib/libMoltenVK.dylib`, prints version+pin+UUID. Checkout
dir = first arg, default `../../MoltenVK` relative to `scripts/`. Bump
the pin only with the validation gauntlet (see xemu-testing /
xemu-validation-and-qa).

`build.sh` arguments (`build.sh:626-649`):

- `-j<N>` — job count, glued form (`-j16`).
- `--debug` — `--enable-debug --enable-trace-backends=log` +
  `-DXEMU_DEBUG_BUILD=1`; also suppresses all release opts.
- `-p <platform>` — platform case: `Linux`, `Darwin`,
  `CYGWIN*|MINGW*|MSYS*` (native Windows), `win64-cross`.
- `-a <arch>` — target arch (macOS cross-arch).
- Any other argument stops option parsing and is passed through to
  `configure` (e.g. `./build.sh -Dx86_version=1`).

Baked-in release options (non-Windows, non-debug, `build.sh:709-712`):
thin LTO + `.lto-cache`, `-Doptimization=3`, `-Dqom_cast_debug=false`,
`-Dtrace_backends=nop`, `-Dstack_protector=disabled`, plus
`-mcpu=<apple-mN> -ffp-contract=fast` on arm64. Upstream's hardening
flags (`-fzero-call-used-regs=used-gpr`, `-ftrivial-auto-var-init=zero`)
stay on every arm whose compiler passes meson's probe. The fork's
`XEMU_HARDENING=0` skip arm was removed on 2026-10-10 (archaeology
8.7): a retest is `CFLAGS=-fzero-call-used-regs=skip ./build.sh`,
checked with the compile-database one-liner in 8.7 before any result
is trusted.
Windows release (`build.sh:657-707`, `:941-1001`): **no forced LTO** (CI
owns per-toolchain LTO strategy), no `-Doptimization=3` and no
`-Dstack_protector=disabled` unless the native arm is opted in with
`XEMU_WIN_O3=1`, and defaults **`-Dx86_version=3`** unless `--debug` or
an explicit `x86_version` argument is present. Every build configures with
`--extra-cflags=-DXBOX=1 --target-list=i386-softmmu`
(`build.sh:1044-1049`).

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
   (`config_spec.yml:259-267`) is the house pattern.
5. Optional Settings-UI hook in `ui/xui/main-menu.cc` (`Toggle` /
   `ChevronCombo` patterns at lines 70, 832-841, 929-940).
6. Renaming/replacing a setting? Add a load-time migration in
   `ui/xemu-settings.cc` next to the `metalfx_upscale` ->
   `metalfx_mode` fixup (`ui/xemu-settings.cc:206-213`); keep the old
   key in the spec as legacy.
7. `docs/optimizations.md` documentation row + evidence per xemu-change-control (what
   measurements a default change requires lives there, not here).

### (b) New XEMU_* env var

1. `getenv` at first use, latch in a `static` (every existing site
   does this — copy one, e.g. `nsprof.c:97-105`). Default must be
   off/fast-path; if you must invert (default-on like `XEMU_VTX_EXACT`)
   flag it loudly in the knob-table row.
2. Parse style: strict `== '1'` or truthy like neighbors; numeric via
   `strtol` with an explicit valid range (`XEMU_MAX_QUERIES` pattern,
   `reports.c:38-44`).
3. POSIX-only facilities (FIFOs, signals, `O_NONBLOCK`) get an
   `#ifdef _WIN32` arm so Windows keeps compiling — a stub at minimum,
   a native equivalent where the facility matters on Windows. The
   `test_input_poll` split (`ui/xemu-input.c:548`: Win32 named pipe
   under `#ifdef _WIN32`, POSIX FIFO in the `#else`, since `164fd4c876`)
   is the template.
   The Windows build must never regress for a macOS convenience
   (established practice inferred from the record — provenance in
   xemu-change-control "Rule provenance").
4. Add a row to the runtime escape-hatch knobs table — since the
   2026-07-18 docs split this table lives in `docs/optimizations.md`
   ("Runtime escape-hatch / diagnostic knobs"), not the README.
5. Change-control pairing rule: if the var exists because you changed
   default behavior, the var IS the escape hatch — the behavior change
   and its escape hatch ship in the same commit, and the knob-table row
   states what `1` (or `0`) restores. See xemu-change-control.
6. If test harnesses must set it, note the env-parity requirements in
   the xemu-testing protocol (interleaved A/B runs toggle exactly one
   var).
7. **If the flag changes EMITTED code, latch it before the first TB**
   and say so in its row. A translate-time flag flipped mid-run leaves
   TBs disagreeing with each other, which is not an A/B — it is a
   corrupted binary state. The wave's pattern: `XEMU_ELIDE_CANDOIO`
   (first translation), `XEMU_SUBPAGE_FAST_STATS` (first stub
   emission), and the x87 mode latches (first translated TB).
8. **Default-on knobs need the inversion stated twice**: in the row
   here and in the harness invocation, because
   `bench-savestate-ab.sh`'s B arm is hardcoded to `0` — correct for a
   default-on escape hatch, wrong for a numeric knob whose default is
   not zero (xemu-testing, 2026-08-04 bench mechanics).

### (c) New MVK_CONFIG_* var

1. Add key+value to `Info.plist` `LSEnvironment` (repo root; bundled by
   `build.sh`'s `package_macos`).
2. Mirror it in the `ui/xemu.c` `main()` block (~line 1939 as of 2026-10-10) as
   `setenv("MVK_CONFIG_...", "...", 0)` — same value, overwrite=0,
   before any Vulkan/MoltenVK initialization.
3. Until the mirrored build is the one under test, `export` the var in
   every harness (launch-path parity — this exact gap hid the
   pink-tile bug).
4. Update the MVK table in `docs/optimizations.md` (`### MoltenVK runtime config`) — the
   third home, and the one a reader reaches first.
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

Everything above was read from the tree on 2026-07-04; the wave rows,
build-knob anchors, MVK six-tuple and env-var count were re-derived
**2026-08-04**; the env-var count, `ui/xemu.c` anchors, input-pipe row
and `config_spec.yml`/`meson.build` expectations were re-run
**2026-10-10** at `b7e2752589`, when every `file:line` anchor in this
skill was re-verified and the missing Axis 2 rows were filed. Regenerate each
table with these commands (run from the repo root); if output differs
from this file, the code wins — update this skill.

```bash
# 1. Authoritative XEMU_* runtime env var list (expect 62 distinct
#    variables as of 2026-10-10; 68 on 2026-08-04). Restrict to the
#    source dirs so agent checkouts (e.g. a .claude/worktrees/, if
#    present) can't multiply every hit.
grep -rn 'getenv("XEMU_' --include='*.c' --include='*.cc' \
  --include='*.m' --include='*.mm' --include='*.inc' --include='*.h' \
  accel/ target/ hw/ ui/ audio/ tcg/ system/ \
  | grep -o 'XEMU_[A-Z0-9_]*' | sort -u | wc -l

# 1b. Every one of them has a table row here (expect no output, 2026-10-10)
for v in $(grep -rhn 'getenv("XEMU_' --include='*.c' --include='*.cc' \
    --include='*.m' --include='*.mm' --include='*.inc' --include='*.h' \
    accel/ target/ hw/ ui/ audio/ tcg/ system/ \
    | grep -o 'XEMU_[A-Z0-9_]*' | sort -u); do
  grep -q "^| \`$v\`" .claude/skills/xemu-config-and-flags/SKILL.md || echo "no row: $v"
done

# 2. Build knobs + args
grep -n 'XEMU_ARM_CPU\|XEMU_PGO\|XEMU_CODESIGN\|XEMU_MOLTENVK_VERSION\|XEMU_VIS\|XEMU_STRIP\|x86_version' build.sh
grep -n 'XEMU_MVK_MCPU\|MVK_PIN' scripts/build-moltenvk.sh   # pin = 67b2682699… (main past v1.4.2, reports 1.4.3)

# 3. MVK parity (all three homes must agree: plist, main(), the ledger
#    table — six keys since 2026-08-04)
grep -n 'MVK_CONFIG' ui/xemu.c Info.plist docs/optimizations.md

# 4. Config generation chain + implicit defaults
grep -n 'genconfig\|config_spec' meson.build          # expect 2508-2509, 3808-3809, 4098 (2026-10-10)
sed -n '72,84p' subprojects/genconfig/gen_config.py    # implicit-default map

# 5. Config file location / portable mode / CLI override
grep -n 'SDL_GetBasePath\|SDL_GetPrefPath\|portable' ui/xemu-settings.cc
grep -n 'config_path' ui/xemu.c

# 6. DSP engine precedence + env override semantics
grep -n -A 10 'dsp_want_external_jit_engine' hw/xbox/mcpx/apu/dsp/dsp.c
grep -n 'XEMU_DSP_JIT' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c

# 7. Spot-check volatile single facts
grep -n 'PREFILL' Info.plist ui/xemu.c docs/optimizations.md   # 0 in all three homes
grep -n 'max_queries_in_flight = ' hw/xbox/nv2a/pgraph/vk/reports.c   # 4096
grep -n 'default_frames' audio/coreaudio.m             # 1024
grep -n 'metalfx' ui/xemu-settings.cc                  # legacy-bool migration
grep -n 'cache_shaders' -r hw/ ui/                     # GL-only gate
wc -l config_spec.yml                                  # 413 as of 2026-10-10
```

Drift-prone facts to re-check first when this skill feels wrong (values
as of 2026-08-04 unless dated): the env-var count (**62** as of 2026-10-10), the MVK **six**-tuple,
`XEMU_MOLTENVK_VERSION` default (**1.4.2**) and the build-moltenvk pin
(**67b2682699…**, main past v1.4.2, reports 1.4.3; releases still vendor
1.4.2), the default-ON knob list, every `build.sh` line
anchor (the file passed 800 lines in this wave),
`audio.dsp_jit.enabled`'s default (false), and `audio.use_dsp_jit`'s
default (**true**, fork-owned since 2026-09-08 — upstream ships
`false`, so re-run `grep -n -A3 'use_dsp_jit:' config_spec.yml` after
any merge).
