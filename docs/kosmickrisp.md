# KosmicKrisp: the opt-in Vulkan driver

**Status (2026-10-19).** Owner decision for v0.16: MoltenVK stays the
default, and KosmicKrisp (KK), Mesa's Vulkan-on-Metal driver, ships as
an opt-in on macOS 26 and later on Apple Silicon. Settings > Display >
Vulkan driver keeps Auto / KosmicKrisp / MoltenVK: Auto runs MoltenVK;
picking KosmicKrisp (or `[display.vulkan] driver = 'kosmickrisp'`) runs
KK where it can (Metal presentation, a Metal 4 GPU), else MoltenVK with
a toast that names the reason. The v0.16 gate (below) found KK level with
MoltenVK at the 60 fps cap but without a CPU lead, and failing C2
(compile hitches), so the 2026-10-18 middle path (KK the default, MoltenVK
the fallback) is deferred: Auto may return to KK once KK, on Mesa 26.3.0
final or a later pin, passes the Auto-back gate, C1 and C2 included
(What flips Auto back, below). Deterministic (`-icount`) and offscreen
(`XEMU_HEADLESS=1`) runs pin MoltenVK whatever the setting says.
MoltenVK stays bundled, so macOS 14/15 and Intel Macs keep working. The
2026-10-16/17 measurements are kept below; that verdict ("better, not
much better": MoltenVK stays the default) and the plan before it (K1-K8,
the opportunity and risk tables) are in this file at tag
`archive/kosmickrisp-proto`.

## What ships

| Piece | What it does |
|---|---|
| Driver choice | `[display.vulkan] driver = auto \| kosmickrisp \| moltenvk` (default `auto`, which runs MoltenVK; `kosmickrisp` is the opt-in), resolved once per process at the first renderer init; a fallback never rewrites it (`instance.c`, next section) |
| Acceptance | KK must show copy-present (`VK_EXT_external_memory_metal`, `vkGetMemoryMetalHandleEXT`, timeline semaphores, a 64x1 MTLHeap-exported buffer with a linear BGRA texture on it, made and freed) and every surface and texture format (`pgraph_vk_formats_supported`) before xemu commits to it; anything else falls back to MoltenVK in the same launch |
| Strike marker | With the setting at `kosmickrisp`, a KK crash in its start or first frames, or a fatal Vulkan error, makes later launches use MoltenVK until the build changes or KosmicKrisp is picked again (below) |
| Per-driver pipeline caches | MoltenVK keeps `pipeline_cache.bin`; KK writes `pipeline_cache-kosmickrisp.bin`. Each driver rejects the other's blob, so one shared file went cold at every switch |
| Paths off MoltenVK, by driver ID | `volkLoadDevice()` (KK hands out device trampolines) |
| CPU triangle fans, every Apple driver | Guest fans go out as CPU-built triangle lists: KK's GPU unroll splits the render pass and mangled Azurik's HUD gauges, and MoltenVK's own conversion restarts it per fan draw (heavy-f1: 101 Metal encoders a flip instead of 38) |
| Copy-present, by capability | A driver without `VK_EXT_metal_objects` but with `VK_EXT_external_memory_metal` (stock KK) gets its frames to Metal through a buffer exported as an MTLHeap, plus a forwarder thread that turns each compositor timeline value into an xemu-owned MTLSharedEvent |
| MoltenVK-only paths, by driver ID | The visibility primer query (MoltenVK's per-command-buffer visibility flag) and the Apple surface-upload fast path (it leans on MoltenVK's tracked queue; KK heaps are untracked) |
| Settings > Display > Vulkan driver | Apple silicon builds (`main-menu.cc`). Its line says `In use: <driver, version>`, plus `, restart to apply` while the choice differs from the one this launch read (`nv2a_vk_driver_setting()`); a change toasts. Where KK can't run, the line ends `(KosmicKrisp: <nv2a_vk_kk_ineligible_reason()>)` instead and the combo is greyed, unless it is set to KosmicKrisp (it must stay possible to move off that). Picking KosmicKrisp removes a strike marker (`nv2a_vk_driver_retry()`). Help > About shows the full `nv2a_vk_driver_desc()` as its `Vulkan:` line |

arm64 bundles carry KK beside MoltenVK in `Contents/Libraries/arm64`
(`build.sh` `resolve_kk`: the pinned CI-built asset, or
`XEMU_KK_PREFIX`; Building, below); x86_64 bundles don't. Still to
come: scaled Z16 through compute (KK has no `BLIT_DST` on D16), and
GS-less wireframe.

## How the driver is chosen

`pgraph_vk_init_instance` decides once per process (a later init after a
renderer switch loads the same driver):

1. **`XEMU_VK_DRIVER`** (diagnostic, below) overrides everything,
   including the pin. If it fails, xemu prints why and exits 1, rather
   than let `pgraph.c` persist `renderer = OPENGL`.
2. **Not Apple:** `volkInitialize()`, as upstream.
3. **Apple:** KK is tried only if the setting is `kosmickrisp` (Auto
   and `moltenvk` run MoltenVK; `resolve_driver`'s `try_kk` is the one
   line that would put Auto back on KK) and
   `nv2a_vk_kk_ineligible_reason()` returns NULL. Its reasons, cheapest
   first and all before any `dlopen` (170-370 ms): `needs Apple silicon`
   (x86_64 builds), `pinned: deterministic` (`icount_enabled()`),
   `pinned: offscreen` (`XEMU_HEADLESS=1`), `needs Metal presentation`,
   `needs macOS 26`, `needs a Metal 4 GPU` (the `MTL4CommandQueue`
   protocol and `supportsFamily:5002` on the presenter's device, KK's
   own device filter; works with any SDK and skips paravirtual GPUs).
   A strike that names this build also skips KK.
4. **The KK attempt:** `dlopen("@rpath/libvulkan_kosmickrisp.dylib")`
   (never closed), the loader-ICD negotiation (interface 5 or later),
   instance, a device whose `driverID` is `MESA_KOSMICKRISP`, logical
   device, acceptance, VMA. Every step returns an error instead of
   aborting; on failure the instance is torn down, the renderer state
   zeroed, and stderr says `xemu: KosmicKrisp unavailable (<why>); using
   MoltenVK`. A toast says the same (arm64 only), as one does for an
   ineligible launch (`KosmicKrisp: needs macOS 26; using MoltenVK`).
5. **MoltenVK** loads by path, `@rpath/libMoltenVK.dylib`, so a Vulkan
   loader in `/usr/local` can't take its place. If it fails after a KK
   dylib loaded in the same process, xemu writes a strike and exits 1
   without persisting `OPENGL`; a MoltenVK-only failure keeps upstream's
   chain.

stderr then names the choice and the accepted device:

```
xemu: Vulkan driver: MoltenVK (auto)
- Driver: MoltenVK, 1.4.3 (id 14)
```

or `MoltenVK (moltenvk)`, and with the setting at `kosmickrisp`
`KosmicKrisp (kosmickrisp)` (`- Driver: KosmicKrisp, Mesa 26.3.0-rc1
(git-4706512989) (id 28)`), `MoltenVK (pinned: offscreen)`, `MoltenVK
(needs macOS 26)`, `MoltenVK (KosmicKrisp failed in an earlier launch:
crashed while starting)` or, after a failed attempt, `MoltenVK
(KosmicKrisp unavailable)`, whose reason is logged earlier in the same
launch, before MoltenVK's instance and device lines (`xemu: KosmicKrisp
unavailable (<why>); using MoltenVK`, then the toast's `xemu:
notification:` line on windowed runs).
`preferred_physical_device` is saved only for the accepted device.
`nv2a_vk_driver_desc()` returns the same text for the UI (`MoltenVK,
1.4.3 (auto)`; Settings shows the part before the first `(`).

Code that depends on one driver's behaviour tests `r->driver_id`
(`driver_is_moltenvk`), never the setting: `XEMU_VK_DRIVER` can load
either driver, or a loader in front of one.

### Strike marker

`kosmickrisp-strike.txt` in the data folder holds the build it applies
to (xemu version, the bundled dylib's size and mtime) and why. A launch
that tries KK (the setting at `kosmickrisp`) opens a start window: it
writes `crashed while starting (pid N)` before the attempt, and after a
clean refusal with the dylib loaded `MoltenVK crashed after a
KosmicKrisp attempt (pid N)` before MoltenVK starts. Only that process
removes its own line: after 600 guest flips (about 10 s of play), at a
clean exit (quit, SIGTERM) or at renderer teardown. So a crash anywhere
in the driver's start or first frames (a signal in Mesa's compiler, a
Mesa abort, a Metal assertion) leaves it; so does SIGKILL. A fatal
Vulkan error (`pgraph_vk_fatal_error()`, KK's device loss and
copy-present target failures included) writes its reason when the active
driver is a KK the setting chose (not `XEMU_VK_DRIVER`), or during the
window; inside the window the line keeps the `(pid N)` tag. A tagged
line whose pid still runs belongs to another xemu on the same data
folder and doesn't count; a `kosmickrisp` launch beside that xemu then
writes its own start line over it, so a crash of the first inside its
window goes unrecorded (an edge case that errs toward fewer strikes).

While a line names the running build, the setting at `kosmickrisp` runs
MoltenVK and shows one toast per launch ("KosmicKrisp failed in an
earlier launch; using MoltenVK. Pick KosmicKrisp in Settings > Display
to retry."), so a KK that fails can't crash every launch before
Settings can be reached. Picking KosmicKrisp in Settings, even
when it is already selected, removes the marker unless a running xemu
wrote it, so each pick gets one more attempt. Auto and `moltenvk` never
read or write the marker, and neither does `XEMU_VK_DRIVER`. (While Auto
tried KK, 2026-10-18, start lines carried the setting, `(pid N, auto)`,
Auto honoured any line and the explicit setting only its own start's;
now every line comes from the explicit setting, which honours them all.)

## Diagnostics: `XEMU_VK_DRIVER`

| Value | Driver |
|---|---|
| unset or empty | The setting (above) |
| `kosmickrisp` | `@rpath/libvulkan_kosmickrisp.dylib` (the bundled KK in `Contents/Libraries/arm64`) |
| `moltenvk` | `@rpath/libMoltenVK.dylib` |
| anything else | A dylib path in `dlopen()` syntax: a dev KK (`$PWD/../kk/install/lib/libvulkan_kosmickrisp.dylib`) or a Vulkan loader |

It overrides the pin, so headless KK runs (offscreen det, the C1 work)
set it. The launchers drop an inherited one (`AUTOMATION_ENV` in
`scripts/xemu_lease.py`); an arm or `--env` sets it per run, even to the
exported value. Windowed launchers (bench-savestate-ab, bench-abx,
movement-probe, soak-cycles, boot-smoke-corpus, pgo-train's windowed
sessions) follow the setting, so they run MoltenVK on Auto; a windowed
KK run sets `XEMU_VK_DRIVER=kosmickrisp` (pgo-train's second heavy-f1
session does, so KK's paths keep a profile) or, to exercise the opt-in
itself, a toml whose `[display.vulkan]` says `driver = 'kosmickrisp'`
(boot-smoke's `--config-src`). Signed releases refuse out-of-bundle
dylibs (library validation), so paths work on dev and ad-hoc bundles
only.

Validation layers: `XEMU_VK_DRIVER=/opt/homebrew/lib/libvulkan.1.dylib`
(the Homebrew loader), `VK_DRIVER_FILES=<kk icd json>`,
`VK_LAYER_PATH=<layer manifests>` and `display.vulkan.validation_layers
= true`. With sync validation, KK reports zero messages on the anchors.

## Building KosmicKrisp

```
scripts/build-kosmickrisp.sh            # or: scripts/build-kosmickrisp.sh package
```

It builds the Mesa tag pinned in `build.sh` (`kk_mesa_tag`,
`kk_mesa_sha`; today `mesa-26.3.0-rc1`) into
`../kk/install-<tag>/lib/libvulkan_kosmickrisp.dylib`, installing nothing
system-wide; `package` also writes the release tarball (dylib with
install name `@rpath/libvulkan_kosmickrisp.dylib`, ad-hoc signed). Needs,
pins and the CI asset: the script's header and `docs/RELEASING-macos.md`
(KosmicKrisp pin bump). The prototype's measurements below used Mesa
`335d5698dc` (main, 27.0.0-devel).

`build.sh` (arm64) bundles the pinned asset
(`kosmickrisp-<tag>[-<respin>]-arm64.tar.xz` from this repository's
releases, cached in `macos-libs/arm64` and checked against
`kk_asset_sha256` on every build, downloaded when missing or different)
and logs `Bundling KosmicKrisp from <tarball> (Mesa 26.3.0-rc1
(git-4706512989))`. `LICENSE.txt`'s Mesa section is
`licenses/kosmickrisp`, a copy of the asset's `share/kosmickrisp/LICENSES`
that `build.sh` checks. The rc1 dylib exports more than the loader's
three `_vk_icd*` entry points: 268 symbols, the rest SPIRV-Tools' C and
C++ API (40 weak, which dyld may bind across images; nothing else xemu
loads carries SPIRV-Tools today). The next asset (26.3.0 or a respin)
links with an `-exported_symbols_list` of `_vk_icd*`, and its asset and
bundle asserts check `nm -gU` in the same commit.
`XEMU_KK_PREFIX=../kk/install-<tag> ./build.sh` bundles a local build
instead (its install name is set to `@rpath`); nothing else is ever
picked up, `/usr/local` and Homebrew included. An installed dylib's
install name is absolute, so to drop one into an existing clone:

```
D=<clone>.app/Contents/Libraries/arm64/libvulkan_kosmickrisp.dylib
cp ../kk/install-<tag>/lib/libvulkan_kosmickrisp.dylib $D
install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib $D && codesign -f -s - $D
```

Not used: the LunarG SDK's KK (not a pinned Mesa tag), Homebrew `mesa`
(links Homebrew libraries), and the ~120-line `VK_EXT_metal_objects`
patch (`../kk/patches/`; LunarG won't take it, Mesa #14209, and
copy-present matches it).

## Testing on KosmicKrisp

- **A/B:** offscreen det `bench-det.py --renderer offscreen
  mvk=APP kk=APP:XEMU_VK_DRIVER=kosmickrisp`; windowed `bench-abx.sh
  <tag> N S OUT --b-app A --e-app A --b-env "XEMU_VK_DRIVER=moltenvk"
  --e-env "XEMU_VK_DRIVER=kosmickrisp"` (windowed Auto is MoltenVK on
  both arms; the per-driver caches make `--isolate-caches`
  unnecessary). Read `- Driver:` from stderr; bench-det prints it per
  line (and per timeline run) and stops when a run's driver isn't the one
  its arm asked for (MoltenVK without `XEMU_VK_DRIVER`); the windowed
  receipts list every driver an arm ran and refuse an arm that ran two.
- **Strikes in the bench cache:** a windowed run with `driver =
  'kosmickrisp'` killed in its first 10 s (SIGKILL: ^C in the zsh
  benches) leaves a strike in the shared
  `~/Library/Caches/xemu-fork-bench`, and every later `kosmickrisp` run
  of that build then uses MoltenVK (`XEMU_VK_DRIVER` runs never read or
  write it). The launchers warn when the file is there; remove it if a
  killed run left it. A strike test gets its own `XEMU_BASE_PATH`.
- **Warm up first:** the first launch of a new bundle path costs KK
  about 1.6-2.2 s of extra device creation (Metal's cache) plus about 3 s
  over the first 60 det frames.
- **RAM hashes are per driver** in offscreen det runs (surface downloads
  write rendered pixels into guest RAM), deterministic per driver except
  KK's stream-e2 walk under load (C1). The null goldens don't depend on
  the driver. Take timeline markers from a run on the same driver.
- **Gaps on KK:** no `fillModeNonSolid` (wireframe comes out filled);
  custom border colors need `MESA_KK_EXPERIMENTAL=custom_border` (no
  pixel change on the anchors); device loss is fatal (the strike marker
  catches it); no geometry shaders (the GS-less paths stay).

## The gate

**The merge (KK as the opt-in)**, reduced, on Mesa 26.3.0-rc1: null
goldens; the MoltenVK pinned path (offscreen `-icount` digests equal
master's, bar the occlusion counts CPU fans move on standing stream-e2:
`docs/optimizations.md`); the fallback matrix (no dylib, corrupt dylib,
failed acceptance; with the setting at `kosmickrisp`, a KK fatal error
then a relaunch, a crash in KK's first frames then a relaunch, a
Settings re-pick, a quit inside the window, MoltenVK failing after KK;
Auto with a strike on disk; `presentation_backend = 'opengl'`, offscreen
without icount); CI on all OSes including Roswell's `- Driver:` line; a
KK smoke run.

**Auto back on KK**, once on Mesa 26.3.0 final (also every later pin
bump while KK is the default), on the CI universal zip against the
published release: the above; KK det digests 5/5 on f1 and e2 as
per-driver references; C1's stream-e2 walk check (Open items);
screendumps against MoltenVK (PSNR ≥ 45 dB, ≤ 0.01% of pixels off by
more than 32); sync validation 0 messages; windowed cross-binary A/Bs
(`kosmickrisp` and `moltenvk` against the release, scale 3-4 with
MetalFX and interpolation); C2's bar; a 30-minute soak with a flat
footprint, the 8-anchor loadvm cycle, `soak-cycles.sh` and
`boot-smoke-corpus.sh`; the meson xbox suite; a launch smoke on macOS 15
and Intel. Quote the load for every number.

### v0.16 results (2026-10-19)

On tag `archive/kosmickrisp-v016-gate` (cb66bae46c, the branch's tip
before the rebase, Auto still on KK), M2 Ultra, macOS 26.7.1, Mesa
26.3.0-rc1:

- **Frame rate:** windowed heavy-f1 and W2 hold the 60 fps cap on both
  drivers.
- **Process CPU at the cap, against v0.15.3:** KK −8.9 / −3.3 points of
  a core (heavy-f1 / W2), MoltenVK with CPU triangle fans −9.9 / −8.1.
  CPU fans gave MoltenVK what KK had over it, so KK has no CPU lead.
- **C2 fails.** Cold, the stream-e2 walk had 11 hitch intervals on KK
  against MoltenVK's 6 (3 runs each). Warm, 1 of 3 KK runs had a 224 ms
  frame from 2 new pipelines (the slower took 132 ms to compile; new
  content, which the other two warm runs didn't meet). KK's warm compile
  time per walk is lower in total (49-56 ms against 211-273 ms), but new
  state combinations compile again, because KK bakes depth-stencil, blend
  and vertex-input state into its shader keys.
- **Soak:** 30 minutes on KK, footprint flat (6.5-6.6 GB, peak 6648 MB),
  no device loss.
- **KK's benefits:** peak footprint −0.4 to −0.6 GB, the NV2A LOD bias
  applied (MoltenVK drops it), sync validation clean. The prototype's
  driver CPU −33% and about 78% fewer Metal encoders were against
  MoltenVK before CPU fans (Measurements, below) and weren't measured
  again: fans took MoltenVK's heavy-f1 encoders from 101 to 38 a flip,
  against the prototype KK's about 18.
- **KK's costs:** compile hitches (C2), occlusion results read back as
  zeros under heavy load (C1), nondeterministic under icount (so det and
  offscreen runs pin MoltenVK), +1.6-2.2 s on the first launch of a new
  bundle, ready in 0.81 s against 0.49 s, wireframe drawn filled, device
  loss fatal (a strike), only M2 measured.

Owner decision: v0.16 ships MoltenVK as the default and KK as the
opt-in. Checked on the opt-in (windowed, its own `XEMU_BASE_PATH`):
Auto logs `MoltenVK (auto)`, with or without a strike on disk;
`kosmickrisp` runs KK, a quit removes its start line, a SIGKILL inside
the window leaves `crashed while starting (pid N)`, and the next
`kosmickrisp` launch runs MoltenVK with the toast, as it does after an
untagged `Vulkan call failed`; offscreen, Auto logs `MoltenVK (auto)`
and `kosmickrisp` logs `MoltenVK (pinned: offscreen)` with its toast.

### What flips Auto back

Auto may run KK again once KK, on Mesa 26.3.0 final or a later pin,
passes the Auto-back-on-KK gate above, C1's walk check and C2's bar
included (C2: no warm KK frame over 50 ms; KK's cold hitch count at most
MoltenVK's + 1). The code change is `resolve_driver`'s `try_kk` line (the
strike, the log reason and the toasts follow from it); the texts that say
Auto runs MoltenVK change with it: the README, `config_spec.yml`, the
Settings help text, `docs/optimizations.md` (Vulkan driver choice, CPU
triangle fans), `docs/README.md`'s index row, `instance.c`'s comments
(the resolver's header, `try_kk`), this file, the skills and pgo-train
(its windowed sessions would run KK on Auto again, so its KK session
goes back to `XEMU_VK_DRIVER=moltenvk`).

## Open items

- **C1, KK det RAM instability** (offscreen icount,
  `XEMU_VK_DRIVER=kosmickrisp`). The pin keeps it out of the goldens;
  the question is whether KK loses occlusion results in normal play, and
  on rc1 it looks like it does. The prototype (335d5698dc) ran heavy-w2
  15/19 (3 deviant hashes, before f300). On rc1 (2026-10-18/19) W2 runs
  20/20 at f300 and f1200 (load 67-85) and heavy-f1 10/10 (73-86); the
  stream-e2 walk (`-t azurik-stream-e2 -i "axis ly 1.0" -f 600`)
  deviates in 4 of about 150 runs at load 47-112 and none of 165 at
  26-40. A local GPU-event log (submit, raw query results, report write,
  download, upload, flip; `../kk/patches/c1-gpu-event-log.patch`) diffed
  against the modal run matches up to a query readback in which a
  contiguous run of results reads exact zeros, in both caught deviants:
  submission 938 (a flip-stall finish) read 0 at indices 3-6 instead of
  4, 4, 6, 84, after submission 939 had gone in; submission 37 (a
  stalled finish) read 0 at 0-8 instead of 11…226, with no later main
  submission before the read. That is H2's signature (KK keeps every
  query in one device-wide visibility buffer and re-sends a stale result
  offset at each encoder start under Metal's default `Reset` result
  type), but the second case can't be a clobber by the next command
  buffer: results landing late, or a clobber inside the same one,
  remain. A re-read after device idle never differed from the first read
  (0 of 135 runs), but those ran at load 26-40 and none deviated, so it
  settles nothing. Receipts: `../kk/research/c1-rc1/` (deviants
  `hunt3/log2.txt`, `hunt4/log28.txt`; `log1.txt` modal). Next: the
  re-read and drain-before-submit experiments at load 70+, to tell late
  landing from clobbering, then the fix (a parking query index nobody
  reads, or a drain before submit). Its check, and every pin bump's: the
  stream-e2 walk on KK, at least 40 runs at load 70+ (beside other bench
  runs), all identical; small ±N differences (H1, AGX split variance)
  would get documented instead.
- **C2, first-encounter cost** (fails at v0.16, above; it keeps Auto
  on MoltenVK). KK compiled 1.35-2x slower on first encounter in the
  prototype; on rc1 it hitches more cold (11 intervals against
  MoltenVK's 6) and warm (a 224 ms frame from 2 new pipelines), though
  its warm compile time per walk is lower in total: new depth-stencil,
  blend and vertex-input combinations compile again because KK bakes them
  into its shader keys. Per-driver caches are in. The fix is upstream's
  (keep that state out of the shader key; cache Metal binaries in the
  pipeline cache), and the fork files no Mesa issues (owner decision
  2026-10-19). Next: measure Mesa 26.3.0 final the same way (`movement-probe.sh --isolate-caches`, 3 cold and 3 warm per
  driver). Bar: no warm KK frame over 50 ms; KK's cold count at most
  MoltenVK's + 1.
- **Workarounds:** on macOS 26.x KK turns on workarounds 1-8, 11, 12, 17
  and 18 (18 is inactive on M2). 2 and 6 cover Apple bugs on every 26.x,
  so they stay; measure `MESA_KK_DISABLE_WORKAROUNDS=2,6,17` once for
  information (per-arm caches: KK's pipeline hash ignores the set) and
  decide only on 17. A default would go in `main()` with overwrite 0.
- **Limits:** `maxPushConstantsSize` 256 is exactly xemu's need;
  `maxMemoryAllocationCount` 4096 (count dedicated VMA allocations in the
  soak); 16-bit index out-of-bounds (workaround 13) is still open
  upstream; KK #15998 (render-pass memory never reclaimed) needs the
  30-minute footprint trace.
- **Coverage:** only Apple8 (M2) on macOS 26.7.1 has been measured;
  other GPU families and 26.x point releases run other workaround sets
  and Metal compilers. The strike marker is the safety net.

## Measurements (2026-10-16/17)

Same prototype binary for both drivers, non-PGO; KK is Mesa 335d5698dc
with CPU fans. Load is quoted per batch.

| Metric | MoltenVK 1.4.3 | KosmicKrisp | Read |
|---|---|---|---|
| Windowed fps at cap, f1 / w2 at scale 2 (load 37-75) | 59.89 / 59.90 | 60.06 / 60.12 | tie (+0.3%) |
| Windowed f1 at scale 3, interpolation active (load 48-56) | 60.03 | 59.48 | −0.9% (0/3); a second batch was flat |
| Process CPU at cap, five batches | 113-134% of a core | 100-123% | **−3.5 to −14 points, 0/3 signs in every batch** |
| Driver CPU, windowed f1 (`sample`) | 2.4 ms/frame | 1.6 ms/frame | **−33%** |
| Peak footprint, windowed | 6395-6959 MB | 5763-6539 MB | **−0.42 to −0.64 GB** |
| GPU time per frame (xctrace f1, contended) | 6.8 ms | 6.9 ms | tie |
| Offscreen det fps, f1 / w2 / e2 (load 33-40) | 39.9 / 75.0 / 45.1 | 42.3 / 81.2 / 46.4 | +3 to +8% |
| Offscreen det fps (load 41-84) | 25.0 / 50.9 / 32.9 | 29.8 / 57.1 / 34.3 | +6 to +23% (lower submit latency: `fence_wait` 78.6 → 50.0 ms per flip) |
| Det Minsns/frame | — | +4 to +8% | KK worse, every sign |
| `pipeline_gen`, f1 (69 pipelines): warm / xemu cold with Metal warm / fully cold | 36 / 117 / 2922 ms | 16 / **227** / **3952** ms | **1.9× and 1.35× worse on first encounter** |
| Movement probe: intervals with a >50 ms frame (cold 1 / cold 2 / warm) | 3 / 2 / 0 | 5 / 3 / 2 | **KK hitches more** (n = 1-2) |
| Ready time, warm / first launch of a new bundle | 0.49 s / — | 0.81 s / +1.6-2.2 s | KK slower |
| heavy-w2 det RAM hash stability | 14 / 14 | **15 / 19 (3 different deviant hashes)** | **KK nondeterministic** (C1) |
| Screendump PSNR vs MoltenVK | — | 46-55 dB | OK: KK applies the NV2A LOD bias, MoltenVK drops it |
| Validation (sync) | portability notes only | 0 messages | KK clean |

Other measurements:
- **Encoders:** KK issues about 78% fewer Metal encoders (about 1100/s
  vs 4700-5100/s). Part of that is the CPU-fan path, which removes
  MoltenVK's "convert indirect buffers" compute encoder (0.29-0.37
  ms/frame).
- **CPU fans on MoltenVK too (2026-10-18):** these MoltenVK columns
  predate them. With them MoltenVK's process CPU at the cap falls 5-12
  points of a core (PGO cross-binary, load 15-57: heavy-f1 119.9 →
  109.4%, W2 106.9 → 101.4%; the non-PGO vetting toggle, load 35-100:
  139.0 → 126.8%, 111.8 → 100.0%), much of KK's −3.5 to −14 point lead
  above.
- **Per-thread CPU, windowed f1:** PFIFO is +4.3 points of a core on KK,
  because KK encodes Metal inline on PFIFO. The driver, Metal and GCD
  threads are −12.6 points.
- **Copy-present vs the patched driver:** PFIFO idle 12.3 vs 12.1 ms per
  flip; windowed +0.25%, 2/0; offscreen screendumps bit-identical on w2.
- **Robustness:** 8 loadvm cycles across 6 anchors with no device loss.
  Footprint stayed flat over 3 minutes windowed. The meson xbox suite
  passes 7/7.
- **Startup:** `dlopen` takes 170-370 ms (about 1.5G instructions); the
  first device creation 1.8 s (libkk kernels plus the Metal cache), then
  9 ms warm.

Facts settled by reading source:
- **No disk cache:** KK never creates a Mesa disk cache
  (`kk_physical_device.c` only frees one), so `MESA_SHADER_CACHE_DIR`
  does nothing; its pipeline-cache blob is about 4× MoltenVK's.
- **Command buffers:** all three xemu command-buffer begins use
  ONE_TIME_SUBMIT, so KK's re-record-at-submit penalty never applies.
- **Query readback:** xemu reads query results on the CPU
  (`vkGetQueryPoolResults` with WAIT); its pool holds 4096 entries, well
  under KK's device-wide limit of 32768.
- **Formats:** KK and MoltenVK both lack R6G5B5's `R8G8B8_SNORM` (Metal
  has no 3-byte formats), which acceptance therefore skips; every other
  surface and texture format works on both.
- **Z16 depth** maps to `D16_UNORM` and takes the `vkCmdBlitImage` path
  at `surface_scale` > 1; KK has no `BLIT_DST` on D16.
