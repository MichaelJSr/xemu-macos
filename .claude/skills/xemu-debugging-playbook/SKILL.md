---
name: xemu-debugging-playbook
description: >
  Symptom-to-subsystem triage for the xemu-macos fork. Load this skill when a
  user, tester, or CI run reports any of: magenta/pink tiles, pink squares or
  rectangles on screen, texture corruption, garbled or stale textures, depth
  artifacts after a render-target switch, black screen on a level transition,
  crash in MoltenVK / AGX setVisibilityResultMode, "game frozen" or hang
  (especially after loading a snapshot), loadvm failing with "Unknown section
  ...usb-hub", audio crackle/glitches/static, an fps regression, startup hang
  with a bouncing dock icon, SIGSEGV/SIGILL under load after JIT changes,
  "my fix didn't take effect" / old version running, different behavior when
  launched from Finder vs a terminal, build failures (GLIB_SIZEOF_SIZE_T
  mismatch, duplicate LC_RPATH), or Windows "Failed to initialize Vulkan
  renderer". Each symptom maps to first checks, one discriminating experiment
  (usually a single XEMU_* escape-hatch flag), and the owning subsystem, so
  triage does not re-fight settled battles.
---

# xemu-macos debugging playbook

Symptom → subsystem triage for this fork. Every row gives the fastest checks,
the ONE experiment that discriminates between candidate causes, and where the
full history lives. All flags, paths, and function names below were verified
against the repo at v0.9 (`cf85e96597`, 2026-07-04). Run all commands from the
repo root: `/Users/michaelsrouji/Documents/Xemu/tools/xemu-macos`.

Terms used below: **NV2A** = the Xbox GPU, emulated in `hw/xbox/nv2a/`.
**PFIFO thread** = the dedicated renderer thread feeding NV2A's 3D engine.
**flip** = guest frame boundary; flips/s ≈ in-game fps. **finish** = a full
submit+wait sync point on the PFIFO thread. **zeta** = depth/stencil buffer.
**Morton/swizzled** = the Xbox's tiled texture memory layout — corruption from
the texture path inherits its small-square shape. **zpass report / occlusion
query** = the guest asking "how many pixels passed the depth test" (drives
lens-flare/glow intensity; theory in `xbox-hardware-reference`). **MoltenVK**
= the Vulkan-on-Metal layer xemu renders through on macOS; "prefill" is its
command-buffer encoding mode (`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS`).

## Triage order (always in this sequence)

1. **Reproduce and identify the binary.** Window title is
   `xemu | v<version>` where version comes from `git describe` — compare it
   against `git describe --tags --match 'v*'` in your checkout before
   anything else. A shocking number of "bugs" are a stale binary or a still-
   running old process (see the environment table below).
2. **Verify scene identity before comparing any numbers.** Set
   `XEMU_NV2A_NSPROF=1` and read the `draws  ... per_flip=` line. The Azurik
   attract flow is bimodal — title screen ≈ 3 draws/flip at 60 fps vs demo
   reel 350–390 draws/flip at 20–48 fps (2026-07 measurements). Comparing a
   menu against gameplay produces confident nonsense.
3. **Check launch-path parity.** Finder launches get `Info.plist`
   `LSEnvironment`; terminal launches do not. `ui/xemu.c` `setenv(...,0)`
   calls level the floor, but any variable exported in your shell wins over
   both. Checklist below.
4. **Bisect with ONE escape-hatch flag per run.** Every fork behavior change
   ships a legacy-restore flag (established practice, inferred from the
   record). Flip exactly one, rerun, compare. Semantics table below.
5. **Read the source (including driver source) before theorizing.** The
   AGX visibility crash was only solved by reading MoltenVK's
   `_needsVisibilityResultMTLBuffer` logic after local repro failed. When a
   crash is under a driver frame, get the driver/layer source first.

## Symptom tables

Columns: symptom | first checks | discriminating experiment | likely
subsystem | full story. "Full story" rows point at the `xemu-failure-
archaeology` skill — do not re-derive settled root causes.

### Graphics artifacts

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| Magenta/pink **small squares** (Morton-tiled blocky pattern, inside rendered content) | Binary current? nsprof `tex_snapshot`/`tex_upload` active in scene? | `XEMU_TEX_BIND_RECHECK=1` — restores per-bind texture content checks. Artifacts gone ⇒ once-per-frame verified-bind fast path missed a mid-frame CPU write | Texture snapshot/upload + dirty tracking (`hw/xbox/nv2a/pgraph/vk/texture.c`) | xemu-failure-archaeology (pink-tile saga) |
| Magenta/pink **large screen-aligned rectangles** (whole bands/blocks, ~frame-sized) | `env \| grep MVK_CONFIG` in the launching shell; prefill must be 0 in bundle plist AND effective env (checks below) | Relaunch with `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0` explicitly exported. Fixed ⇒ something (shell profile, wrapper) was overriding to 2 — xemu's own `setenv` is no-overwrite, so YOUR env wins | Present/driver config — MoltenVK prefill encoding | xemu-failure-archaeology (pink-tile saga: prefill=2 corrupted streamed textures, ~5% of frames, Finder-only) |
| Garbled/flickering **geometry** | Binary current? Does it track a recent vertex-path change? | `XEMU_VTX_EXACT=0` — restores legacy page-granular vertex-conflict finishes (disables byte-exact skip). Fixed ⇒ conflict-refinement false-negative | Vertex upload conflict refinement (`hw/xbox/nv2a/pgraph/vk/vertex.c`, `uploaded_span_content_differs`) | xemu-failure-archaeology |
| **Stale/late-updating textures** (UI elements, scoreboards update a frame late) | Is the texture written twice per frame by the game? | `XEMU_TEX_BIND_RECHECK=1` (same lever as row 1; README Troubleshooting documents this exact use) | Once-per-frame texture bind fast path | README Troubleshooting §"Stale/late-updating textures" |
| **Depth artifacts after a render-target switch** (z-fighting, wrong occlusion right after cutscene/RT change) | Reproduces at the same switch every time? | `XEMU_ZETA_SHAPE_READBACK=1` — restores the full GPU→CPU round trip on zeta shape switches (costs ~2 finishes + multi-MB copies/frame). Fixed ⇒ shape-switch fast path; report the title | Zeta shape-switch fast path (`hw/xbox/nv2a/pgraph/vk/surface.c`, `zeta_shape_readback_forced`) | README Troubleshooting §"Depth artifacts" |
| **Black screen on level transition** (esp. rapid double-level-load) | Binary current? This is SETTLED — fixed by wall-clock surface-expiry throttling (bisected v0.8.141→v0.8.142) | If it reproduces on a current build: `XEMU_PFIFO_HEARTBEAT=1` and capture the heartbeat trend; that is a NEW bug, not the old one | Surface cache expiry (`expire_old_surfaces` / 33 ms wall-clock throttle in `hw/xbox/nv2a/pgraph/vk/surface.c`) | xemu-failure-archaeology (wall-clock surface expiry) |

Morphology is the primary magenta discriminator: **small Morton-tiled squares
= texture snapshot/upload path; large screen-aligned rectangles =
present/driver config.** Capture + pixel-scoring method (window capture,
magenta score, flagged-frame review) lives in the `xemu-testing` skill
("Visual artifact oracle") — use it to collect evidence, use this table to
interpret it.

Prefill checks (both must show `0`):

```sh
grep -n -A2 PREFILL Info.plist                      # repo template (Finder path)
grep -n 'setenv("MVK_CONFIG' ui/xemu.c              # parity floor, all launch paths
# and in a built bundle:
plutil -p <path-to>/xemu.app/Contents/Info.plist | grep -A1 PREFILL
```

### Crashes and hangs

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| Crash with **`setVisibilityResultMode`** / AGX driver frames in the stack | Effective MVK config: prefill MUST be 0 (prefill=2 re-enables the first-pass crash). Primer query present in this rev? `grep -n primer_index hw/xbox/nv2a/pgraph/vk/draw.c` — expect a `vkCmdBeginQuery`/`vkCmdEndQuery` pair outside any pass at command-buffer start | `XEMU_MAX_QUERIES=64` forced-fire test: a tiny pool makes the begin_draw capacity guard submit constantly (historically 3312 guard fires/interval, no crash). Crash gone at 64 but present at default ⇒ pool exhaustion; crash regardless ⇒ visibility-flag/first-pass problem — read the MoltenVK source, don't guess | Occlusion queries / reports (`hw/xbox/nv2a/pgraph/vk/reports.c` pool + guard, `draw.c` primer) | xemu-failure-archaeology (visibility-buffer crash, two acts) |
| **SIGSEGV/SIGILL ~1 min into load** after touching DSP-JIT (or any JIT) code | Crash address inside a MAP_JIT region? Recent diff touches code emission or patching? | `XEMU_DSP_JIT=0` — vanishes ⇒ your JIT change. Then audit: EVERY write to code pages (including chain-site repatching) must sit inside the `qemu_thread_jit_write()` … `qemu_thread_jit_execute()` window (W^X). Grep your diff for stray writes | DSP JIT code emission (`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`) | xemu-failure-archaeology (W^X discipline) |
| **Startup hang, dock icon bounces forever** | Is `XEMU_DSP_JIT_DIFF` (or `_SYNC`) set? The diff validator is the only known cause | Unset `XEMU_DSP_JIT_DIFF*` and relaunch. Historically: validator without the per-translation gate starved `d->lock`; current code gates via `DspJitBlock.diff_checked` (one validation per unique translation). If it hangs WITHOUT diff flags, sample the process (`sample xemu 5`) — new bug | DSP JIT diff validator (`mcpx.dsp_diff` worker thread) | xemu-failure-archaeology (validator lock starvation) |
| **"Game frozen"** (rendering stopped, app alive) | Did the user just load a snapshot? See snapshot table. Otherwise: is the PFIFO thread alive? | `XEMU_PFIFO_HEARTBEAT=1` → a line every ~2 s: `xemu: pfifo heartbeat iters=… halt=… flush=… sync=… waiting_flip=… waiting_nop=… waiting_ctxsw=…`. Heartbeat STOPS ⇒ PFIFO thread stuck. `flush`/`sync` pinned at 1 ⇒ renderer hung in `vkWaitForFences`/`vkQueueSubmit`. `waiting_flip` pinned ⇒ guest waiting on flip completion | PFIFO / VK renderer sync (`hw/xbox/nv2a/pfifo.c`) | — (live triage; escalate with the heartbeat categorization) |

### Snapshots / savestates

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| **"Game frozen" right after loading a snapshot** | Binary current? Historically a FAILED load left the VM stopped — looked like a hard freeze with only a transient toast. Current `xemu_snapshots_load()` (`ui/xemu-snapshots.c`) resumes the VM on failure and surfaces the error | On a current build, a failed load shows an error toast and gameplay continues from where it was. If genuinely frozen: `XEMU_PFIFO_HEARTBEAT=1` (previous table) — that's a real hang, not this | Snapshot load error path | xemu-failure-archaeology (part of pink-tile saga cleanup) |
| **Controller detected in the Input menu but the character doesn't move** (diagnosed live 2026-07-04) | Three-part signature proves a PORT-BINDING mismatch, not a game/guest bug: (1) Input menu lists the pad — that's host-side SDL detection, independent of bindings; (2) no movement — the emulated port-1 pad has no host binding, and unbound pads deliberately report *neutral* input (`hw/xbox/xid.c` guard, v0.9); (3) the game shows no "controller disconnected" screen — the emulated pad device still exists in the guest (snapshot topology or boot attach), it just reads idle. Root cause seen in the field: one physical controller presenting **two GUIDs** over time (Bluetooth vs cable, firmware rev — both start `0500` = BT bus); `[input.bindings] port1` pins ONE GUID, and when the pad enumerates as the other, detection succeeds but the binding no longer matches | In the Input menu, click Port 1 and re-select the controller (bindings hot-apply, no restart; xemu persists the new GUID). Cross-check the config: `grep -A6 'input.bindings' …/xemu.toml` vs the `gamepad_mappings` GUID list — two similar entries = the dual-identity case. To check whether a snapshot itself carries the pad: `strings <qcow2-clone> \| grep -c usb-xbox-gamepad` (≈ pads-per-snapshot × snapshot count) | Host input binding (`ui/xemu-input.c`), neutral-input guard (`hw/xbox/xid.c`) | — (live triage record, this row) |
| **`loadvm` fails: "Unknown section …usb-hub"** | Were controllers bound when the snapshot was SAVED? Snapshots embed per-pad USB trees (`usb-hub,port=1.<port_map[N-1]>,ports=3` + `usb-xbox-gamepad,port=1.<port_map[N-1]>.1,index=<N-1>`, with `port_map = {3,4,1,2}` — `ui/xemu-input.c:261,783-807`; player 1 lands on hub port 1.3, gamepad index 0). A machine without them can't load it. Note: the frozen `xemu-testing` skill's topology formula omits `port_map` — this corrected mapping supersedes it (drift item in `xemu-docs-and-writing` §4). CLI `-loadvm` ALWAYS fails this way — it runs before xemu attaches USB | Load via monitor instead: launch with `-monitor unix:/tmp/xemu-mon.sock,server,nowait`, wait for boot (~25 s), then `printf 'loadvm <name>\n' \| nc -U /tmp/xemu-mon.sock`. For headless runs, replicate topology with raw `-device` args — full method in the `xemu-testing` skill ("Snapshots with controllers") | USB topology vs snapshot contents (`hw/xbox/xid.c` guards unbound pads to neutral input) | xemu-testing skill has the method; history in xemu-failure-archaeology |

### Audio

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| **Crackle / dropouts / static** | `XEMU_APU_PROF=1` → `apu: utilization  NN.N% (M frames/s)` per second. Utilization near 100% ⇒ APU thread over budget; 19–25% is the healthy 2026-07 range in the bench title | Run the DSP bisect chain below, one step per run | MCPX APU: CoreAudio buffer, VP, or DSP engine | xemu-failure-archaeology (DSP JIT battles) |
| Crackle only, engines sound identical | Small output buffer vs machine load | `XEMU_COREAUDIO_FRAMES=2048` (default 1024; read once at startup, `audio/coreaudio.m`). Fixed ⇒ buffer sizing, not DSP | CoreAudio output layer | README Troubleshooting §"Audio glitches" |

**DSP engine bisect chain** (config lives in
`~/Library/Application Support/xemu/xemu/xemu.toml`; engine choice is
config-driven — see `dsp_want_external_jit_engine()` in
`hw/xbox/mcpx/apu/dsp/dsp.c`):

1. `[audio.dsp_jit] enabled = false` (fork JIT off; `audio.use_dsp_jit`
   default true takes over → upstream dsp56300 JIT engine; that default
   is fork-owned and was briefly `false` between the 2026-09-07 merge
   and the 2026-09-08 restore, so confirm it in the run's config rather
   than assuming). Glitch persists
   ⇒ not the fork JIT — suspect VP/CoreAudio (also try step 1b:
   `use_dsp_jit = false` too → C interpreter engine with no JIT of either
   kind, the most conservative configuration).
2. `XEMU_DSP_JIT=0` with `dsp_jit.enabled = true` (keeps the fork's C engine
   selected but disables its inline JIT → pure interpreter, same plumbing).
   Glitch gone here but present with JIT ⇒ JIT codegen bug → step 3.
3. `XEMU_DSP_JIT_DIFF=1` — bit-exact validation of every unique block
   translation against the interpreter (2–10x slowdown; `N≥2` samples every
   Nth block). On divergence it prints
   `xemu: DSP JIT DIFF FAILURE in block pc_start=0x…` + validator stats and
   aborts — the abort pinpoints the divergent block. Add
   `XEMU_DSP_JIT_DIFF_SYNC=1` only for bring-up (abort fires immediately,
   at APU-latency cost).
4. `XEMU_DSP_JIT_NO_THROTTLE=1` — disables the retranslation-churn
   auto-throttle. Changes the symptom ⇒ churn/throttle interaction, not
   codegen.

Engine selection semantics — which config keys pick which of the three
engines, the defaults, and why `XEMU_DSP_JIT=0/1` can NOT change which
engine is selected (it only toggles the inline JIT within the fork's C
engine) — live in `xemu-config-and-flags`, the precedence owner; that
constraint is what makes step 2 above meaningful. README recommends
`dsp_jit.enabled=true` on Apple Silicon.

### Performance regressions

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| **"fps dropped"** (vs a previous build or session) | FIRST: scene identity. `XEMU_NV2A_NSPROF=1`; compare the header `nsprof: 5.0s interval, N flips (X/s)` and the `draws … per_flip=` event line between runs. Menus ≈ 3 draws/flip, Azurik attract reel 350–390, heavy in-game 408–485 (2026-07). Different draws/flip ⇒ different scene ⇒ the comparison is void (the bimodal-Azurik trap) | Same scene confirmed: diff the `finish_*` event rates and `fence_wait per_flip` between good/bad. Then bisect levers one at a time: `XEMU_REPORTS_SYNC=1` (legacy synchronous zpass-report drains) and `XEMU_VTX_EXACT=0` (legacy vertex finishes). A lever that restores the old fps names the subsystem | VK renderer sync policy (reports, vertex, surfaces) | Historical numbers: 15.78→38.57±0.80 fps (+144%) report rework per commit `6bfbc22863`'s protocol receipt (README prose rounds this change to 35.6 fps / 2.25x); +5.4% byte-exact vertex (2026-07) — xemu-failure-archaeology |
| fps claim needs to be DEFENSIBLE (before/after a change) | Never eyeball it | Interleaved savestate A/B protocol (±0.02 fps on static scenes) — method in the `xemu-testing` skill; `scripts/bench-savestate-ab.sh` | — | xemu-testing |

Do not start optimization work from this table — that is the
`xemu-gpu-frame-campaign` skill. This table only localizes regressions.

### Environment / "wrong binary" traps

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| **"My fix didn't take effect" / build seems old** | Window title `xemu \| v<version>` and the startup stderr line `xemu_version: …` vs `git describe --tags --match 'v*'` in the repo | `pgrep -fl xemu` — a still-running process keeps executing its old binary image across rebuilds. Quit it, relaunch. Also: `cd build && ninja` updates the binary but a full `./build.sh` re-runs configure AND repackages, wiping `dist/xemu.app/Contents/Resources` | Process lifecycle, not code | xemu-failure-archaeology (benchmarking traps) |
| **Behaves differently from Finder vs terminal** | `LSEnvironment` in `Info.plist` applies ONLY to LaunchServices (Finder/`open`) launches. `ui/xemu.c` sets the same MVK_CONFIG_* with `setenv(..., 0)` (no-overwrite) so both paths now share a floor — but an exported variable in YOUR shell overrides both | Run the launch-parity checklist below; diff `env \| grep -E 'MVK_CONFIG\|XEMU_'` between the two launch contexts | Launch-path environment | xemu-failure-archaeology (pink-tile saga — the asymmetry hid the bug for weeks) |

### Build failures (macOS)

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| Configure/build fails: **`sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`** | Did the deps tree change under you (arch switch, partial download)? | `rm -rf macos-libs macos-pkgs build && ./build.sh` — stale pkg-config paths are the cause; nothing subtler | Dependency staging (`scripts/download-macos-libs.py`) | README Troubleshooting |
| **Crash on launch: "duplicate LC_RPATH"** | Old bundle? macOS 26+ dyld rejects duplicate LC_RPATH entries | Rebuild — current `build.sh` strips duplicates via `install_name_tool -delete_rpath` after dylibbundler. Verify: `otool -l <bundle>/Contents/MacOS/xemu \| grep -A2 LC_RPATH` | Bundle packaging (`build.sh`) | README Troubleshooting |

Deeper build/environment problems (MoltenVK provisioning tiers, PGO, cross
builds) → `xemu-build-and-env` skill.

### Windows

| Symptom | First checks | Discriminating experiment | Likely subsystem | Full story |
|---|---|---|---|---|
| **"Failed to initialize Vulkan renderer" → OpenGL fallback** | Running in a VM (Parallels/UTM/Hyper-V)? VMs usually have NO Vulkan ICD — the fallback is EXPECTED there, not a bug. xemu bundles no loader; `vulkan-1.dll` comes from the GPU driver | `vulkaninfo` (Vulkan SDK) shows what the loader sees. On real hardware: update the vendor GPU driver | Vulkan loader/ICD availability (message from `pgraph_vk_init_instance`, `hw/xbox/nv2a/pgraph/vk/instance.c`) | README Troubleshooting §Windows |

## Bisect levers — exact semantics (verified 2026-07-04)

Only the flags this playbook uses; full catalog and the add-a-flag checklist
live in the `xemu-config-and-flags` skill. All are read once at startup.

| Flag | Set to | Effect | Read in |
|---|---|---|---|
| `XEMU_TEX_BIND_RECHECK` | `1` (any non-`0`) | Restore per-bind texture content checks (default: once-per-frame verified-bind fast path) | `pgraph/vk/texture.c` |
| `XEMU_VTX_EXACT` | `0` | Restore legacy page-granular vertex-conflict finishes (default: byte-exact skip) | `pgraph/vk/vertex.c` |
| `XEMU_ZETA_SHAPE_READBACK` | `1` (non-`0`) | Restore GPU→CPU readback on zeta shape switches | `pgraph/vk/surface.c` |
| `XEMU_REPORTS_SYNC` | `1` | Restore legacy synchronous zpass-report drains (default: flip-deferred + 5 ms idle fallback) | `pgraph/vk/reports.c` |
| `XEMU_MAX_QUERIES` | 8–65536 (default 4096) | Occlusion-query pool size; tiny values force-fire the begin_draw capacity guard | `pgraph/vk/reports.c` |
| `XEMU_PFIFO_HEARTBEAT` | `1` | ~2 s PFIFO liveness + pending-flag snapshot to stderr | `hw/xbox/nv2a/pfifo.c` |
| `XEMU_NV2A_NSPROF` | `1` (non-`0`) | 5 s wall-time renderer profile to stderr (release-safe) | `hw/xbox/nv2a/nsprof.c` |
| `XEMU_APU_PROF` | `1` (non-`0`) | Per-second APU-thread utilization to stderr | `hw/xbox/mcpx/apu/apu.c` |
| `XEMU_DSP_JIT` | `0` / `1` | Force fork inline DSP JIT off/on within the C engine (engine selection stays config-driven) | `dsp/interp/dsp56k_jit_arm64.c` |
| `XEMU_DSP_JIT_DIFF` | `1` or `N` | Validate every / every Nth unique block translation vs interpreter; abort + `DIFF FAILURE in block pc_start=…` on divergence | same |
| `XEMU_DSP_JIT_DIFF_SYNC` | `1` | Synchronous validation (immediate abort; APU latency — bring-up only) | same |
| `XEMU_DSP_JIT_NO_THROTTLE` | `1` | Disable retranslation-churn auto-throttle | same |
| `XEMU_COREAUDIO_FRAMES` | frames (default 1024) | CoreAudio output buffer size | `audio/coreaudio.m` |

Interpreting the diagnostics beyond "which line to read" (healthy vs
pathological ranges, every nsprof counter) → `xemu-diagnostics-and-tooling`
skill.

## Launch-path parity checklist (the cardinal rule)

The pink-tile bug survived for weeks because Finder launches and harness
launches ran DIFFERENT MoltenVK configs. Never let a repro/experiment differ
from the user's launch path in environment.

1. What Finder/LaunchServices launches get:
   ```sh
   grep -n -A16 LSEnvironment Info.plist       # repo template
   plutil -p <bundle>/Contents/Info.plist | grep -B1 -A10 LSEnvironment
   ```
2. What EVERY launch gets (parity floor, `setenv(..., 0)` = no-overwrite):
   ```sh
   grep -n 'setenv("MVK_CONFIG' ui/xemu.c
   ```
   Expect the five canonical MVK_CONFIG_* values in both sources — the
   value table's home is `xemu-config-and-flags` Axis 3 (prefill=0 is the
   load-bearing one). The README "MoltenVK runtime config" table now
   agrees (prefill `0`, fixed 2026-07-05; parity across all three homes
   re-verified 2026-07-11) — the code and Info.plist remain ground truth
   if they ever diverge again.
3. When testing from a shell, check what your shell adds/overrides:
   ```sh
   env | grep -E 'MVK_CONFIG|XEMU_'
   ```
   Because the in-app `setenv` is no-overwrite, anything here WINS. An
   experiment flag left exported in `.zshrc` is a classic ghost variable.
4. To test a config variant, export it explicitly and say so in the report;
   to test the shipped config, export nothing.
5. Never point a harness at `dist/xemu.app` (it is the user-launched bundle;
   rebuilds also wipe its `Resources/`). Copy it first — rule and method in
   the `xemu-testing` skill ("Cardinal rules").

Prefill MUST stay 0: immediate encoding (2) corrupted streamed textures,
enabled the AGX visibility crash, and measured slower (46.33 vs 48.50 fps,
2026-07 session record) because encoding lands on the PFIFO thread.

## When NOT to use this skill

- **Running a performance campaign / optimizing GPU frame cost** — this
  playbook only localizes regressions. Use `xemu-gpu-frame-campaign`.
- **Setting up benchmarks, input injection, artifact capture, or any
  measurement method** — use `xemu-testing` (savestate A/B protocol, window
  oracle, topology matching).
- **Full failure histories and evidence chains** — the rows above link to
  `xemu-failure-archaeology`; read it before re-attempting anything fenced
  off there.
- **Complete flag/config catalog or adding a new flag** —
  `xemu-config-and-flags`.
- **Build/environment setup, MoltenVK provisioning tiers** —
  `xemu-build-and-env`. **Running/operating the app, monitor socket, release
  process** — `xemu-run-and-operate`.
- **Deep interpretation of profiler output** (every counter, healthy
  ranges) — `xemu-diagnostics-and-tooling`.

## Provenance and maintenance

All claims verified 2026-07-04 at v0.9 (`cf85e96597`); MVK three-home
prefill parity re-verified 2026-07-11 at `7e2e6e7256`. Re-verify before
trusting a row after major merges:

- Env-flag inventory: `grep -rn 'getenv("XEMU_' --include='*.c' --include='*.m' hw/ ui/ audio/`
- MVK prefill + parity floor: `grep -n 'setenv("MVK_CONFIG' ui/xemu.c && grep -n -A2 PREFILL Info.plist`
- Primer query still present: `grep -n primer_index hw/xbox/nv2a/pgraph/vk/draw.c`
- Query-pool default/guard: `grep -n 'XEMU_MAX_QUERIES\|max_queries_in_flight = 4096' hw/xbox/nv2a/pgraph/vk/reports.c`
- Snapshot-load auto-resume: `grep -n -A4 'vm_running' ui/xemu-snapshots.c`
- Heartbeat fields: `grep -n -A12 'pfifo heartbeat' hw/xbox/nv2a/pfifo.c`
- nsprof line formats: `grep -n 'per_flip\|flips (%' hw/xbox/nv2a/nsprof.c`
- Engine precedence: `grep -n -B2 -A10 'dsp_want_external_jit_engine' hw/xbox/mcpx/apu/dsp/dsp.c`
- DSP JIT flag semantics: `sed -n '160,270p' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- DIFF abort diagnostic: `grep -n 'DIFF FAILURE' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- CoreAudio buffer env: `grep -n -A4 'XEMU_COREAUDIO_FRAMES' audio/coreaudio.m`
- Zeta/texture/vertex/reports levers: `grep -n 'XEMU_ZETA_SHAPE_READBACK\|XEMU_TEX_BIND_RECHECK\|XEMU_VTX_EXACT\|XEMU_REPORTS_SYNC' hw/xbox/nv2a/pgraph/vk/*.c`
- Surface-expiry throttle (black-screen fix): `grep -n -B4 -A6 'surface_expire_interval_ns' hw/xbox/nv2a/pgraph/vk/surface.c`
- README Troubleshooting rows (GLIB_SIZEOF, LC_RPATH, audio, Windows Vulkan): `sed -n '856,906p' README.md`
- LC_RPATH strip in build: `grep -n -B2 -A3 'delete_rpath' build.sh`
- Window-title version source: `grep -n 'xemu | v' ui/xemu.c && grep -n 'git describe' scripts/xemu-version.sh`
- README MVK-table parity (all three homes must read prefill 0): `grep -n 'PREFILL' README.md Info.plist ui/xemu.c` — agreed as of 2026-07-11; a reappearing `2` is new drift.
