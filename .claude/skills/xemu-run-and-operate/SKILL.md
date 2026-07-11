---
name: xemu-run-and-operate
description: >-
  Operating the built xemu-macos fork. Launching dist/xemu.app two ways with
  different environment semantics (open vs direct binary, LSEnvironment vs
  shell env), app-bundle anatomy (Contents/MacOS, Libraries/<arch>, Resources
  as SDL3's base path), runtime data locations (xemu.toml, eeprom.bin,
  pipeline_cache.bin, spirv cache, screenshots, savestate thumbnails), CLI
  flags xemu intercepts (-config_path, -dvd_path) vs QEMU passthrough
  (-monitor, -device, -full-screen, -loadvm), scripting the QEMU monitor
  socket to load snapshots (and why CLI -loadvm fails but in-app F5-F8 works),
  qcow2 write-locking and safe cloning, where logs land, and the tag-push ->
  draft-release -> owner-publish CI pipeline plus the manual packaging
  fallback. Load when asked to run or launch xemu, locate
  config/eeprom/pipeline-cache/screenshot files, drive or script the QEMU
  monitor, list/load/save a snapshot, cut or verify a GitHub release, or set
  up an isolated test copy of the app bundle or hard disk image.
---

# xemu run & operate playbook

This is the operational counterpart to **xemu-build-and-env**: it assumes
`dist/xemu.app` already exists and covers what happens when you run it —
the two launch paths and why they differ, the bundle and data-directory
anatomy, the CLI/monitor surface, where output lands, and how a tagged
commit becomes a published GitHub release.

## When NOT to use this skill

- Building from scratch, meson/configure flags, MoltenVK provisioning at
  build time, PGO, Windows native/cross builds, CI build matrix → **xemu-build-and-env**.
- The full catalog of `xemu.toml` settings / `XEMU_*` env vars / `MVK_CONFIG_*`
  defaults, precedence, and semantics → **xemu-config-and-flags** (this skill
  only uses the handful needed to operate the app: `-config_path`,
  `-dvd_path`, the MVK setenv/plist pair, `XEMU_INPUT_PIPE`, `XEMU_NV2A_NSPROF`).
- The savestate A/B benchmark protocol, background input injection, and the
  visual-artifact oracle → **xemu-testing** (this skill states the
  operational facts that method depends on: the monitor socket, snapshot
  lifecycle, qcow2 locking).
- Symptom-first triage ("pink tiles", "frozen after loadvm", "old binary
  running") → **xemu-debugging-playbook**.
- Interpreting nsprof/APU_PROF/DSP-JIT numeric output, or the shipped
  diagnostic scripts themselves → **xemu-diagnostics-and-tooling**.
- Change classification, required evidence per change class, commit/tag
  conventions, upstream-merge rules → **xemu-change-control**.
- Acceptance thresholds and golden-fixture inventory for a performance or
  correctness claim → **xemu-validation-and-qa**.
- README/RELEASING house style, where a fact belongs, release-notes
  templates → **xemu-docs-and-writing**.
- Why a past design was abandoned (the incident, not the mechanism) →
  **xemu-failure-archaeology**.

## 1. Launch paths and why they differ

Two ways to start the same bundle produce different MoltenVK behavior:

| Launch | Command | `LSEnvironment` applies? |
|---|---|---|
| Finder / LaunchServices | `open dist/xemu.app` (or double-click) | Yes |
| Direct binary | `dist/xemu.app/Contents/MacOS/xemu` | No — inherits only the invoking shell's environment |

`LSEnvironment` (`Info.plist:35-53`, a 5-key `MVK_CONFIG_*` dict) is a
LaunchServices feature: it is injected only when macOS launches an app
bundle via Finder/`open`/Dock, never when a Mach-O is exec'd directly. This
split hid a real bug for weeks — every shell-launched test harness ran
MoltenVK library defaults while every Finder-launched user session ran the
tuned config (full story: xemu-failure-archaeology's pink-tile saga).

The fix is launch-path parity in code, not policy. `main()`
(`ui/xemu.c:1602-1606`, inside `#ifdef __APPLE__`) calls `setenv(..., 0)`
(no-overwrite) for the same 5 keys immediately on entry, before MoltenVK
is ever `dlopen`'d — expect the five canonical `MVK_CONFIG_*` values in
both sources (prefill=0 is the load-bearing one); the value table's home
is `xemu-config-and-flags` Axis 3.

No-overwrite semantics mean an explicit `export MVK_CONFIG_X=...` (or
prefixing the invocation) before launch still wins — this is the supported
way to override one of these for an experiment.

**If you add a new `LSEnvironment` entry to `Info.plist`, mirror it in this
`setenv` block, or you reintroduce the exact split above** — this is
xemu-testing's cardinal rule #1, restated here because it's a launch-path
fact. Verify both lists match: `grep -n "MVK_CONFIG" Info.plist ui/xemu.c`.

### Window title = build identity

The title bar is ground truth for "which binary is this window running":
`g_strdup_printf("xemu | v%s" ..., xemu_version)` (`ui/xemu.c:1245-1249`,
appends `" Debug"` under `XEMU_DEBUG_BUILD`), applied via
`SDL_SetWindowTitle` (`ui/sdl2.c:202`). `xemu_version` is baked in at build
time by `scripts/xemu-version.sh`'s `git describe --tags --match 'v*'` — it
is not re-read live. A running process keeps whatever version string it
booted with even if you rebuild underneath it (xemu-testing cardinal rule
#3: "a user's running process keeps its old binary") — trust the title
bar, not the mtime of `dist/xemu.app`.

A sharper corollary: an incremental `cd build && ninja` (see
xemu-build-and-env) updates `build/qemu-system-i386` but does **not** touch
`dist/xemu.app` at all. `package_macos()`'s `cp build/qemu-system-i386
dist/xemu.app/Contents/MacOS/xemu` (`build.sh:135`) runs only as the
`postbuild` step of a full `./build.sh` (`build.sh:581,630-632`). So a
ninja-only rebuild leaves the *launchable* bundle stale — same old title,
same old behavior — until you either rerun `./build.sh` (full reconfigure +
repackage, which also wipes `Contents/Resources`, see §2) or manually
re-copy the fresh binary into the bundle.

## 2. Bundle anatomy

`dist/xemu.app/` is produced by `package_macos()` (`build.sh:127-262`):

```
Contents/
  Info.plist              # LSEnvironment MVK block; CFBundleShortVersionString/CFBundleVersion set from XEMU_VERSION (build.sh:223-224)
  MacOS/xemu               # the Mach-O (copied from build/qemu-system-i386, build.sh:135)
  Libraries/<arch>/        # bundled dylibs incl. libMoltenVK.dylib
  Resources/xemu.icns      # generated from ui/icons/xemu_{16,32,128,256,512}x*.png (build.sh:210-213); also SDL3's "base path" — see below
  _CodeSignature/          # ad-hoc signing artifact
```

`<arch>` is `arm64` or `x86_64` (a universal release lipos the two
single-arch `dist/xemu.app` builds together — build mechanics live in
xemu-build-and-env). Verify on this machine: `ls dist/xemu.app/Contents/Libraries/`.

- The executable's `LC_RPATH` is `@executable_path/../Libraries/<arch>/`
  (`install_name_tool -add_rpath`, `build.sh:170`, after stripping any
  duplicate `LC_RPATH` entries dylibbundler left behind — macOS 26+ dyld
  rejects duplicates, `build.sh:165-169`). Every bundled `.dylib`, including
  `libMoltenVK.dylib`, is re-pathed to `@rpath/<name>` and individually
  re-codesigned (`build.sh:138-163`). `libMoltenVK.dylib` itself is
  never linked — it's `dlopen`'d at runtime through Volk, and dyld resolves
  it through the same rpath (`build.sh:172-205`).
  Verify: `otool -l dist/xemu.app/Contents/MacOS/xemu | grep -A2 LC_RPATH`.
- **`Contents/Resources` doubles as SDL3's application "base path" on
  macOS**, and that has a sharp edge. Portable-mode detection
  (`xemu_settings_detect_portable_mode`, `ui/xemu-settings.cc:51-62`) looks
  for an `xemu.toml` next to `SDL_GetBasePath()` — on macOS that resolves
  to `Contents/Resources/`, *not* `Contents/MacOS/` where the binary
  actually lives. A portable config planted there works right up until the
  next full `./build.sh`: `package_macos()` opens with `rm -rf dist`
  (`build.sh:128`) — the entire previous bundle, `Resources` included, is
  gone before the new one is assembled. Do not rely on a portable config
  surviving a rebuild.

## 3. Data locations (macOS, normal/non-portable mode)

Non-portable base path = `SDL_GetPrefPath("xemu", "xemu")`
(`xemu_settings_get_base_path`, `ui/xemu-settings.cc:73-92`), which on
macOS resolves to:

```
~/Library/Application Support/xemu/xemu/
```

Verify: `ls -la ~/Library/Application\ Support/xemu/xemu/`

| File / directory | Written by | Notes |
|---|---|---|
| `xemu.toml` | `xemu_settings_save` (registered via `atexit`, `ui/xemu.c:1670`) | full config; schema is xemu-config-and-flags' territory |
| `eeprom.bin` | first boot / settings | default path = base + `eeprom.bin` (`xemu_settings_get_default_eeprom_path`, `ui/xemu-settings.cc:107-118`); overridable via `[sys.files] eeprom_path` |
| `pipeline_cache.bin` | `save_pipeline_cache_to_disk` (`hw/xbox/nv2a/pgraph/vk/draw.c:457-524`) | one `VkPipelineCache` blob; write-then-rename (`<path>.tmp` + `g_rename`, `draw.c:508-521`) so a kill mid-write can't leave a torn file for next boot; flushed only at a flip boundary (`pgraph_vk_flip_stall`, `hw/xbox/nv2a/pgraph/vk/renderer.c:201-207`) and only if ≥30 s have passed since the last save (`PIPELINE_CACHE_SAVE_INTERVAL_NS`, `draw.c:534`) *and* something new compiled |
| `spirv_cache_v<major>.<minor>.<patch>/` | `get_spirv_cache_dir` (`hw/xbox/nv2a/pgraph/vk/glsl.c:30-39`) | per-shader `<hash>.spv` files (`get_spirv_cache_path`, `glsl.c:41-48`); the directory name is versioned by `xemu_version_major/minor/patch`, so a version bump starts a fresh cache rather than reusing or invalidating the old one |
| `shaders/`, `shader_cache_list` | OpenGL renderer only (`hw/xbox/nv2a/pgraph/gl/shaders.c:258-265`) | equivalent disk cache for the GL backend; irrelevant while `display.renderer = VULKAN` (the default) |

Machine files (BIOS/HDD/DVD) live wherever `xemu.toml`'s `[sys.files]`
table points — these are user-chosen paths, not fixed locations. The dev
machine's current values (hdd qcow2, firmware, DVD, title corpus) are
owner-volatile golden fixtures whose inventory home is
`xemu-validation-and-qa` §3 — read them fresh instead of copying from any
doc:

```
grep -A6 '\[sys.files\]' ~/Library/Application\ Support/xemu/xemu/xemu.toml
```

Also in the same file: `general.screenshot_dir`, and the in-app F5/F6
snapshot shortcuts under `[general.snapshots.shortcuts]` — date-stamped,
owner-volatile snapshot names (current values: `xemu-validation-and-qa`
§3; never hardcode them across sessions). Re-verify:
`grep -E "screenshot_dir|^f[5-8]" ~/Library/Application\ Support/xemu/xemu/xemu.toml`.
If any of these paths are absent on a different machine, that's a fixture
gap — see xemu-build-and-env for provisioning or point to a different
`[sys.files]` set.

**Portable mode**: drop an `xemu.toml` into the app bundle at
`Contents/Resources/xemu.toml` and every path above resolves relative to
that directory instead of `~/Library/Application Support`. See §2 for why
this is fragile against rebuilds, and §8 / xemu-testing for why you should
never do this inside `dist/xemu.app` for a test run.

## 4. CLI surface

xemu intercepts exactly two flags *before* they reach QEMU's own option
parser — both are stripped out of `argv` (set to `NULL` in place) so
QEMU's `lookup_opt` never sees them and doesn't reject them as unknown:

| Flag | Intercepted in | Effect |
|---|---|---|
| `-config_path <path>` | `ui/xemu.c:1650-1659`, scanned before `xemu_settings_load()` | `xemu_settings_set_path(path)` — load/save config from `<path>` instead of the default base path |
| `-dvd_path <path>` | `system/vl.c:3078-3087`, scanned while building the machine's `-drive` args | overrides `[sys.files] dvd_path` for this run only, without touching the saved config |

Everything else in `argv` passes through **untouched**. `system/vl.c`
builds a synthesized QEMU command line (`fake_argv`, starting at
`vl.c:2974`: machine type + BIOS + `-m` + HDD/DVD drives + `-display xemu`
+ a `usb-hub,port=1,ports=4` daughterboard, `vl.c:3100-3102`), then appends
whatever is left of the *original* argv verbatim (`vl.c:3104-3108`) before
handing the whole thing to QEMU's normal parser. In other words: **xemu
does not filter out standard QEMU flags.** Anything QEMU recognizes for the
`i386-softmmu` target works as a passthrough flag, including (all verified
against `qemu-options.hx`):

- `-monitor unix:<path>,server,nowait` (`qemu-options.hx:4791-4793`) — how
  the savestate benchmark harness drives xemu headlessly; see §5.
- `-device <driver>,<opts>` (`qemu-options.hx:1096`) — used to replicate a
  snapshot's USB pad topology; see §5.
- `-full-screen` (`qemu-options.hx:2477-2478`) — start in fullscreen.
- `-loadvm <tag>` (`qemu-options.hx:5079`) — exists and is accepted, but
  **do not use it** to load a savestate on this fork; see §5 for why it
  fails.
- `-snapshot` (`qemu-options.hx:1819-1822`, "write to temporary files
  instead of disk image files") — unrelated to xemu's savestates despite
  the name; this is QEMU's discard-on-exit copy-on-write mode for the disk
  image, not a way to create or load a named snapshot.

Every boot echoes the fully synthesized command line to **stdout**
(`printf`, `vl.c:3110-3114`): `"Created QEMU launch parameters: ..."` —
capture with `>log 2>&1`, not `2>` alone. It's the fastest way to confirm
what a given invocation actually resolved to, including what
`[sys.files]` values it picked up. (The `xemu_version:` triplet goes to
stderr — §6.)

**Environment-prefix pattern** for one-off diagnostics — prepend the var to
the invocation rather than exporting it globally:

```
XEMU_NV2A_NSPROF=1 dist/xemu.app/Contents/MacOS/xemu 2>prof.log
```

This only works launching the binary directly (§1) — `open` provides no
way to pass ad hoc environment variables to the launched process. The full
catalog of `XEMU_*` variables belongs to xemu-config-and-flags; this skill
only uses the ones needed operationally (input injection and profiling
on/off) in §5-§6.

## 5. Monitor socket: driving xemu headlessly

Standard QEMU HMP (human monitor protocol) over a UNIX socket, unmodified
by the fork. Boot with a monitor attached:

```
dist/xemu.app/Contents/MacOS/xemu -monitor unix:/tmp/xemu-mon.sock,server,nowait
```

Then connect with any UNIX-socket client. `scripts/bench-savestate-ab.sh:36-46`
has the exact socket dance the harness uses (Python's `socket` module):
connect, sleep 0.5 s, drain the greeting banner, send `loadvm <name>\n`,
sleep 3 s, drain the response, close.

**Boot, wait, *then* `loadvm` over the monitor — never CLI `-loadvm`.**
`-loadvm` is a real QEMU option (§4), and QEMU processes it during
`qemu_init()`, calling the same underlying `load_snapshot()` that the
monitor's `loadvm` command and the in-app F5-F8 shortcuts eventually call
(`ui/xemu-snapshots.c:235-255`). The difference is timing: this fork
creates each bound controller's USB sub-tree (`usb-hub` + XID gamepad,
`ui/xemu-input.c:770-822`) from `xemu_input_init()`, which `main()`
(`ui/xemu.c`) calls only *after* display init signals it's done — well
after `qemu_init()` (and therefore `-loadvm`) has already run. A snapshot
saved with pads bound restores VM state that expects those USB devices to
already exist; loading before they're attached fails with an error like
`Unknown section ...usb-hub`. Booting normally, waiting for the pad hubs to
attach and the boot animation/menu to settle (`bench-savestate-ab.sh` uses
a 25 s margin, `bench-savestate-ab.sh:35` — empirically comfortable, not a
documented hard minimum), then issuing `loadvm` over the monitor avoids the
race entirely.

**Headless topology matching**: a snapshot saved with controllers bound to
Xbox ports needs the same USB tree present before `loadvm` runs, or it
fails the same way even with no monitor attached to receive input.
Replicate it with raw `-device` args mirroring what `xemu_input_init()`
builds (`ui/xemu-input.c:783-822`); port N on the Xbox front panel maps to
internal USB address `1.<port_map[N]>`, with `port_map[4] = {3, 4, 1, 2}`
(`ui/xemu-input.c:261`):

```
-device usb-hub,port=1.<port_map[N]>,ports=3
-device usb-xbox-gamepad,port=1.<port_map[N]>.1,index=<N-1>
```

(`usb-xbox-gamepad-s`, `DRIVER_S` in `ui/xemu-input.h:36`, is the Steel
Battalion-style controller S alternative; default is `usb-xbox-gamepad` /
`DRIVER_DUKE`, `ui/xemu-input.h:35`.) A pad slot left unbound still boots
and loads fine — unbound emulated pads simply read neutral input (guarded
in `hw/xbox/xid.c`).

Relevant HMP commands, all standard QEMU: `loadvm <tag>`
(`hmp-commands.hx:350-363`), `savevm <tag>` (`hmp-commands.hx:331-345`),
`delvm <tag>`, `info snapshots` (`hmp-commands-info.hx:381-392`,
human-readable table). **This fork's own tooling doesn't use `info
snapshots`** — both the in-game snapshot browser and
`bench-savestate-ab.sh` enumerate snapshots by reading the qcow2 file
directly (see below) rather than round-tripping the monitor; `loadvm` is
the only monitor command the harness relies on.

### Snapshot lifecycle

- **In-app**: F5-F8 load the snapshot bound to that key
  (`general.snapshots.shortcuts`); holding Shift while pressing F5-F8 saves
  to it instead (`ActionActivateBoundSnapshot`, `ui/xui/actions.cc:94-116`,
  wired to key events in `ui/xui/main.cc:339-345`). Both paths call
  `load_snapshot()`/`save_snapshot()` in-process
  (`ui/xemu-snapshots.c:235-259`) — no monitor socket involved.
- **Failed loads resume the VM; they no longer freeze it.** Before the
  pink-tile-saga fix (`531122e8aa`), a failed `load_snapshot()` (e.g. a USB
  topology mismatch) left the VM parked in `RUN_STATE_RESTORE_VM` — to the
  player, a hard freeze with only a passing error toast to explain it.
  `xemu_snapshots_load` now records whether the VM was running beforehand
  and calls `vm_start()` again on failure (`ui/xemu-snapshots.c:235-255`);
  the session continues and the caller surfaces the error separately.
- **Thumbnails ride inside the snapshot, not as separate files.**
  `xemu_snapshots_save_extra_data` (`ui/xemu-snapshots.c:267-305`) appends
  a PNG framebuffer capture
  (`xemu_snapshots_create_framebuffer_thumbnail_png`,
  `ui/xemu-thumbnail.cc`) plus the XBE title name behind a magic/version
  header — fork-private data riding alongside the standard QEMU savevm
  sections inside the qcow2 image.
- **Snapshots live inside the HDD qcow2** — there is no separate snapshot
  file to look for. To enumerate them without booting xemu, parse the
  qcow2 header directly (mirrors `ui/xemu-snapshots.c`'s own parsing):
  offset 60 is `nb_snapshots` (big-endian u32) followed by
  `snapshots_offset` (big-endian u64); each entry is a 40-byte fixed
  struct (`>QIHHIIQII`) followed by variable-length extra data, VM state
  ID, and name, 8-byte aligned.

### qcow2 locking

A running xemu holds the HDD qcow2 open for read/write. A second process
(another xemu instance, or a naive inspection tool) attempting to open the
same file hits QEMU's own file lock, surfaced as `Is another process using
the image [<path>]?` (`block/file-posix.c:1031,3034`). **Clone before any
parallel use**:

```
cp -c /path/to/hdd.qcow2 /path/to/scratch/hdd-test.qcow2
```

`-c` is an APFS copy-on-write clone (instant regardless of file size;
snapshots ride along since they live inside the file). Never point a
second instance at the user's live `hdd_path`.

## 6. Logs and other outputs

- **stderr is the primary log, always.** There is no macOS-specific
  log-file redirection in this fork — the only `freopen(...)` redirection
  to a log file (`xemu.log`) is `#ifdef _WIN32`-only
  (`ui/xemu.c:1617-1639`); macOS gets nothing equivalent. In practice:
  - Direct-binary launches: stderr goes wherever the invoking shell sends
    it — redirect explicitly (`2>prof.log`) to capture it.
  - `open`/Finder launches: stderr isn't attached to any terminal; it
    lands in the unified logging system, visible via `Console.app` or
    `log stream --predicate 'process == "xemu"'`, not a plain file.
- Every boot prints `xemu_version`, `xemu_commit`, `xemu_date`
  (`ui/xemu.c:1641-1643`) to stderr before anything else; the synthesized
  QEMU launch line (§4) goes to **stdout** (`printf`, `vl.c:3110-3114`) —
  capture both streams from process start (`>log 2>&1`) if you need
  either.
- Diagnostic env vars all write to the same stderr stream:
  `XEMU_NV2A_NSPROF=1` (per-5s renderer summaries,
  `hw/xbox/nv2a/nsprof.c:148-169`), `XEMU_APU_PROF=1`,
  `XEMU_PFIFO_HEARTBEAT=1`, the `XEMU_DSP_JIT_*` family. Interpreting these
  numbers is xemu-diagnostics-and-tooling's job; this skill only
  establishes that they share one stream with everything else above.
- `build.log` is a *build*-time artifact (`time make ... | tee build.log`,
  `build.sh:630`), not a runtime log — see xemu-build-and-env.
- Screenshots: directory is `general.screenshot_dir`, empty defaults to
  `.` (the process's current working directory at the time,
  `ui/xui/gl-helpers.cc:1317-1322`); filename is
  `xemu-%Y-%m-%d-%H-%M-%S.png` (`SaveScreenshot`, `gl-helpers.cc:1302-1346`).
- Pipeline cache write cadence: see §3 — flip-boundary-gated, ≥30 s between
  saves, write-then-rename.

## 7. Release operations (current state, 2026-07-04)

The live pipeline is CI-driven. `docs/RELEASING-macos.md` documents a
manual, pre-CI process end-to-end — its packaging steps are still the
right reference for hand-packaging (see below), but its release-creation
half describes an older workflow; state the two clearly rather than
following the doc verbatim for that half.

```
push tag "v*"
  -> release-on-tag.yml   (trigger: tags ['v*'], owner guard)
    -> release.yml (workflow_call)
      -> build.yml -> build-{macos,windows,linux}.yml   (compiles + packages every target)
      -> release job: softprops/action-gh-release, draft: true, prerelease: false,
         full cross-platform artifact set attached
  -> owner reviews the draft on GitHub, publishes manually
```

- Trigger: `on: push: tags: ['v*']` (`.github/workflows/release-on-tag.yml:3-5`).
- Owner guard: `if: contains(fromJSON('["xemu-project", "MichaelJSr"]'),
  github.repository_owner)` (`release-on-tag.yml:16`) — without this, a
  tag pushed to the fork would silently produce nothing (upstream's
  workflow only expected `xemu-project`).
- Result: a **draft** GitHub release (`draft: ${{ !inputs.pre-release
  }}`, `.github/workflows/release.yml:66`) with the full cross-platform
  artifact set attached. As of `v0.9` that's **17 assets** — macOS
  universal signed+unsigned zips, Windows x86_64/arm64 zips + PDBs, three
  Linux AppImage variants × {release, debug}, a source tarball, and two
  legacy-named Windows aliases (`release.yml:53-58`). Re-verify the count
  against the latest release (latest tag `v0.10.2` as of 2026-07-11)
  before citing it again: `gh release view v0.10.2 --repo
  MichaelJSr/xemu-macos --json assets --jq '.assets | length'`.
- The draft is **not visible to normal users** until the owner clicks
  publish on GitHub — a deliberate human gate before anything ships.

Sibling workflows, easy to confuse with the tag flow:

- `prerelease.yml` (`prerelease.yml:3-6`) — triggers on every push to
  `master`; publishes a *rolling*, non-draft pre-release under the tag
  `pre-release` (not a `v*` tag), gated to `xemu-project` only
  (`prerelease.yml:15`) — not part of this fork's own tagged-release path.
- `delete-prerelease.yml` (`delete-prerelease.yml:3-4`) — a reusable
  `workflow_call` with no direct trigger; deletes the rolling `pre-release`
  tag/release. Invoked by `release.yml` when cutting a pre-release, and by
  `release-published.yml` after any non-prerelease publish, so the rolling
  pre-release doesn't sit stale next to a real release.
- `release-on-dispatch.yml` (`release-on-dispatch.yml:4`) — manual
  `workflow_dispatch` that computes the next `vMAJOR.MINOR.PATCH` from
  existing tags and pushes it itself (`release-on-dispatch.yml:21-38`);
  this fork tags releases by hand instead (see numbering below), so this
  path is effectively unused here.

### Version scheme (dated)

Fork-versioned as of `v0.9` — `v0.9`, `v0.8.99`, `v0.8.93`, … are all real
tags (`git tag | tail`). Older releases used an upstream-suffixed scheme
(`v0.8.153-macos.1` remains published; never touch it).
`scripts/xemu-version.sh` derives everything from `git describe --tags
--match 'v*'`; a suffix like `-macos.1` used to break `windres`'s numeric
`FILEVERSION` fields until `sanitize_numeric()` was added
(`xemu-version.sh:39-49`) to strip non-digits — a bug only reproducible on
an actually-tagged build, which is how it slipped past review once.

### Tag invariants — never violate these

- **Never delete or move a published tag.** Deleting a release's tag
  silently flips that release to DRAFT — hiding it from users with no
  explicit "unpublish" action. Force-updating a tag re-fires
  `release-on-tag.yml`, which will try to replace the published assets;
  cancel the run before its `action-gh-release` step executes, or it
  succeeds and clobbers a real release.
- One release per tag. Never reuse a tag name.
- The full rationale, the incident this codifies, and the required
  evidence gates around cutting a release live in **xemu-change-control**
  — this skill states the mechanical trigger chain; that skill owns the
  "why" and "what must be true first."

### Manual packaging reference

`docs/RELEASING-macos.md` documents packaging `xemu.app` by hand
(replicating `package_macos()`'s steps without a full `./build.sh`
reconfigure) — still the right reference when you need to re-package an
already-built binary without triggering CI. Known-stale parts of that doc,
stated as ground truth here: it recommends a manual `gh release create`
recipe and says "don't pass `--draft`" — current practice is the CI tag
flow above, which always produces a draft regardless; it also references
"README 'Committing' conventions" (a section that no longer exists in
`README.md`) and a `Made-with: Cursor` commit footer (recent commits use
`Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` instead). The
notes-body Markdown shape and title-format rules in that doc remain house
style — maintaining that text is xemu-docs-and-writing's job, not restated
here.

### Post-publish verification

```bash
gh release view vX.Y.Z --repo MichaelJSr/xemu-macos \
  --json tagName,name,assets,isDraft,isPrerelease
```

Confirm `tagName` matches the pushed tag, `isDraft: false`,
`isPrerelease: false`, and the asset list/sizes look right for the current
platform matrix (17 assets as of `v0.9` — re-check the count if the build
matrix has changed since).

`scripts/sign-macos-release.sh` is for **notarized** builds only — it takes
a `.p12` certificate and an App Store Connect API key and re-signs +
notarizes an already-built release zip fetched by tag or local path.
Standard releases ship ad-hoc-signed (`build.sh`'s default codesign step);
don't run this script unless notarization was explicitly requested.

## 8. Test-instance isolation (operational rule; method lives in xemu-testing)

Never run any harness, script, or ad hoc monitor session against
`dist/xemu.app` or the user's live `hdd_path` — that bundle and that disk
image are the user's actual session. Two independent hazards:

1. **Config hijack**: a portable `xemu.toml` planted in `Contents/Resources`
   (§2) redirects config, saves, and the HDD/DVD paths for the *next*
   launch of that bundle — Finder or shell, whichever comes first.
2. **qcow2 write lock**: a running instance holds the HDD file open (§5) —
   a second instance pointed at the same path fails outright.

The fix is a disposable clone of both, per run:

```bash
cp -Rc dist/xemu.app /path/to/scratch/xemu-test.app   # APFS clone, instant; refresh after every rebuild (§1)
cp -c  /path/to/hdd.qcow2 /path/to/scratch/hdd-test.qcow2
```

Point the clone at the cloned disk — either a portable `xemu.toml` inside
`xemu-test.app/Contents/Resources/`, or a scratch config passed via
`-config_path` (§4) — before running anything against it. The full
savestate A/B protocol, input-injection method, and artifact-detection
oracle that actually exercise this isolated copy are xemu-testing's
territory; this skill only states the hands-off rule and the clone recipe.

## Provenance and maintenance

All `build.sh` line anchors above were re-derived 2026-07-11 against
HEAD `7e2e6e7256` (the `a45882eb83` consolidation shifted every
`package_macos` offset); the `ui/xemu.c` setenv anchor is `1602-1606`.
Re-verify each volatile fact before trusting it on a future date:

- MVK setenv/plist parity: `grep -n "MVK_CONFIG" Info.plist ui/xemu.c`
- Window title format: `grep -n "xemu | v\|SDL_SetWindowTitle" ui/xemu.c ui/sdl2.c`
- `-config_path` / `-dvd_path` interception: `grep -n "config_path" ui/xemu.c; grep -n "dvd_path" system/vl.c`
- Bundle rpath/library layout: `otool -l dist/xemu.app/Contents/MacOS/xemu | grep -A2 LC_RPATH; ls dist/xemu.app/Contents/Libraries/*/`
- Package-vs-ninja staleness trap: `grep -n "postbuild\|package_macos" build.sh`
- Data base-path formula: `grep -n "SDL_GetPrefPath\|SDL_GetBasePath" ui/xemu-settings.cc`
- Pipeline/SPIR-V cache paths + save cadence: `grep -n "get_pipeline_cache_path\|PIPELINE_CACHE_SAVE_INTERVAL_NS" hw/xbox/nv2a/pgraph/vk/draw.c; grep -n "get_spirv_cache_dir" hw/xbox/nv2a/pgraph/vk/glsl.c`
- User fixture paths (dated, this machine): `grep -A6 '\[sys.files\]' ~/Library/Application\ Support/xemu/xemu/xemu.toml`
- Snapshot F-key bindings + screenshot dir (dated, this machine): `grep -E "screenshot_dir|^f[5-8]" ~/Library/Application\ Support/xemu/xemu/xemu.toml`
- USB pad topology construction: `grep -n "port_map\|usb-hub" ui/xemu-input.c`
- Snapshot save/load call sites + failure-resume behavior: `grep -n "xemu_snapshots_load\|xemu_snapshots_save" ui/xemu-snapshots.c`
- Release trigger chain + owner guard: `grep -n "tags:\|repository_owner\|draft:" .github/workflows/release-on-tag.yml .github/workflows/release.yml`
- Current release asset count/names (dated; latest tag v0.10.2 as of 2026-07-11): `gh release view v0.10.2 --repo MichaelJSr/xemu-macos --json assets --jq '.assets[].name'`
- qcow2 lock error text: `grep -n "another process using the image" block/file-posix.c`
