# KosmicKrisp: the opt-in Vulkan driver

KosmicKrisp (KK), Mesa's Vulkan-on-Metal driver, is an opt-in on macOS
26 and later on Apple silicon; arm64 bundles carry it in
`Contents/Libraries/arm64`. MoltenVK is the default and stays bundled,
so macOS 14/15 and Intel Macs keep working (owner decision for v0.16,
2026-10-19; the result is under The Auto-back gate). Deterministic
(`-icount`) and offscreen (`XEMU_HEADLESS=1`) runs pin MoltenVK. The
prototype plan is in this file at tag `archive/kosmickrisp-proto`; the
full v0.16 receipts and the prototype measurements at `b94dcaec29`.

## Selecting it

`[display.vulkan] driver = 'auto' | 'kosmickrisp' | 'moltenvk'`
(default `auto`, which runs MoltenVK), or Settings > Display > Vulkan
driver on Apple silicon builds (`main-menu.cc`). It is read once per
process at the first renderer init; a fallback never rewrites it.

The Settings line says `In use: <driver, version>` (the part of
`nv2a_vk_driver_desc()` before the first `(`), plus `, restart to
apply` while the choice differs from the one this launch read
(`nv2a_vk_driver_setting()`). Where KK can't run it ends `(KosmicKrisp:
<reason>)` instead and the combo is greyed, unless it is set to
KosmicKrisp (a user must be able to move off it). Picking KosmicKrisp
calls `nv2a_vk_driver_retry()` (Strike marker). Help > About shows the
full `nv2a_vk_driver_desc()` as its `Vulkan:` line.

| Piece | What it does |
|---|---|
| Acceptance | KK must show copy-present (`VK_EXT_external_memory_metal`, `vkGetMemoryMetalHandleEXT`, timeline semaphores, a 64x1 MTLHeap-exported buffer with a linear BGRA texture) and every surface and texture format (`pgraph_vk_formats_supported`), or xemu falls back to MoltenVK in the same launch |
| Copy-present | Without `VK_EXT_metal_objects` (stock KK), frames reach Metal through a buffer exported as an MTLHeap, plus a forwarder thread that turns compositor timeline values into an xemu-owned MTLSharedEvent |
| Per-driver pipeline caches | MoltenVK keeps `pipeline_cache.bin`; KK writes `pipeline_cache-kosmickrisp.bin`. KK has no Mesa disk cache (`MESA_SHADER_CACHE_DIR` does nothing) |
| CPU triangle fans, every Apple driver | Guest fans go out as CPU-built triangle lists: KK's GPU unroll split the render pass and mangled Azurik's HUD gauges, and MoltenVK's conversion restarted it per fan draw (heavy-f1: 101 Metal encoders a flip instead of 38) |
| By driver ID | `volkLoadDevice()` off MoltenVK (KK hands out device trampolines); the visibility primer query and the Apple surface-upload fast path on MoltenVK only |

Still to come: scaled Z16 through compute (KK has no `BLIT_DST` on D16)
and GS-less wireframe.

## How the driver is chosen

`pgraph_vk_init_instance` decides once per process:

1. `XEMU_VK_DRIVER` (below) wins, the pin included. If it fails, xemu
   exits 1 rather than let `pgraph.c` persist `renderer = OPENGL`.
2. On Apple, KK is tried only with the setting at `kosmickrisp`
   (`resolve_driver`'s `try_kk`), no strike for this build, and no
   `nv2a_vk_kk_ineligible_reason()`: `needs Apple silicon`, `pinned:
   deterministic`, `pinned: offscreen`, `needs Metal presentation`,
   `needs macOS 26`, `needs a Metal 4 GPU` (`MTL4CommandQueue`,
   `supportsFamily:5002`), all checked before any `dlopen`.
3. The attempt: `dlopen("@rpath/libvulkan_kosmickrisp.dylib")` (170-370
   ms), ICD interface 5 or later, a `MESA_KOSMICKRISP` device,
   acceptance, VMA. A failure logs `xemu: KosmicKrisp unavailable
   (<why>); using MoltenVK` and toasts (arm64 only), as an ineligible
   launch does (`KosmicKrisp: needs macOS 26; using MoltenVK`).
4. MoltenVK loads by path (`@rpath/libMoltenVK.dylib`). If it fails
   after a KK dylib loaded, xemu writes a strike and exits 1 without
   persisting `OPENGL`; a MoltenVK-only failure keeps upstream's
   fallback chain.

stderr then names the choice and the accepted device:

```
xemu: Vulkan driver: MoltenVK (auto)
- Driver: MoltenVK, 1.4.3 (id 14)
```

The parenthesis is the setting (`MoltenVK (moltenvk)`, or `KosmicKrisp
(kosmickrisp)` with `- Driver: KosmicKrisp, Mesa 26.3.1
(git-fec8830e9c) (id 28)`) or why KK was skipped: a step 2 reason,
`KosmicKrisp failed in an earlier launch: crashed while starting` or
`KosmicKrisp unavailable`. In the last case the `xemu: KosmicKrisp
unavailable (<why>); using MoltenVK` line comes earlier in the same
launch, before MoltenVK's instance and device lines, and on windowed
runs the toast's `xemu: notification:` line follows it. Device selection
is upstream's (`display.vulkan.force_physical_device`, never written by
xemu). Driver-specific code tests `r->driver_id` (`driver_is_moltenvk`),
never the setting.

### Strike marker

`kosmickrisp-strike.txt` in the data folder names the build (xemu
version, bundled dylib size and mtime) and why. A `kosmickrisp` launch
writes `crashed while starting (pid N)` before the attempt (`MoltenVK
crashed after a KosmicKrisp attempt (pid N)` before a fallback with the
dylib loaded) and removes it after 600 guest flips (about 10 s), a clean
exit or renderer teardown, so a crash or SIGKILL in that window leaves
it. A fatal Vulkan
error (`pgraph_vk_fatal_error()`, KK device loss and copy-present target
failures included) writes its reason on a setting-chosen KK, or inside
the window, where the line keeps its `(pid N)` tag. A line whose pid
still runs is another xemu's and doesn't count. While a line names this
build, `kosmickrisp` runs MoltenVK with one toast per launch
("KosmicKrisp failed in an earlier launch; using MoltenVK. Pick
KosmicKrisp in Settings > Display to retry."). Picking KosmicKrisp, even
when already selected, removes the marker unless a running xemu wrote
it. Auto, `moltenvk` and `XEMU_VK_DRIVER` never read or write it.

## `XEMU_VK_DRIVER`

A diagnostic override, `kosmickrisp`, `moltenvk` or a dylib path in
`dlopen()` syntax (a dev KK, a Vulkan loader); headless KK runs need it.
The launchers drop an inherited one (`AUTOMATION_ENV`,
`scripts/xemu_lease.py`); set it per run with an arm or `--env`.
Windowed launchers follow the setting, so a windowed KK run sets it
(pgo-train's second heavy-f1 session does) or uses a toml with `driver =
'kosmickrisp'` (boot-smoke's `--config-src`). Signed releases refuse
out-of-bundle dylibs. Validation layers:
`XEMU_VK_DRIVER=/opt/homebrew/lib/libvulkan.1.dylib`,
`VK_DRIVER_FILES=<kk icd json>`, `VK_LAYER_PATH=<layer manifests>` and
`display.vulkan.validation_layers = true` (KK: zero sync messages).

## Building KosmicKrisp

`scripts/build-kosmickrisp.sh [package]` builds the Mesa tag pinned in
`build.sh` (`kk_mesa_tag`, `kk_mesa_sha`; today `mesa-26.3.1`) into
`../kk/install-<tag>/lib/`, installing nothing system-wide; `package`
also writes the release tarball (pin bumps: `docs/RELEASING-macos.md`).
`build.sh` (arm64) bundles the pinned asset
(`kosmickrisp-<tag>[-<respin>]-arm64.tar.xz`, cached in
`macos-libs/arm64`, checked against `kk_asset_sha256`) and logs
`Bundling KosmicKrisp from <tarball> (Mesa 26.3.1
(git-fec8830e9c))`; `licenses/kosmickrisp` must match the asset's
`share/kosmickrisp/LICENSES`. `XEMU_KK_PREFIX=../kk/install-<tag>
./build.sh` bundles a local build instead and sets its install name to
`@rpath`; nothing else is ever picked up, `/usr/local` and Homebrew
included. Into an existing clone:

```
D=<clone>.app/Contents/Libraries/arm64/libvulkan_kosmickrisp.dylib
cp ../kk/install-<tag>/lib/libvulkan_kosmickrisp.dylib $D
install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib $D && codesign -f -s - $D
```

Not used: the LunarG SDK's KK (not a pinned Mesa tag), Homebrew `mesa`
(links Homebrew libraries), and the `VK_EXT_metal_objects` patch in
`../kk/patches/` (LunarG won't take it, Mesa #14209; copy-present
matches it).

## Testing on KosmicKrisp

- **A/B:** `bench-det.py --renderer offscreen mvk=APP
  kk=APP:XEMU_VK_DRIVER=kosmickrisp`; windowed `bench-abx.sh <tag> N S
  OUT --b-app A --e-app A --b-env "XEMU_VK_DRIVER=moltenvk" --e-env
  "XEMU_VK_DRIVER=kosmickrisp"` (the per-driver caches make
  `--isolate-caches` unnecessary). bench-det prints `- Driver:` per line
  and stops when a run's driver isn't its arm's; the windowed receipts
  refuse an arm that ran two drivers. Warm up: a new bundle path costs
  KK about 1.6-2.2 s of device creation and about 3 s over the first 60
  det frames.
- **Strikes in the bench cache:** a `kosmickrisp` run killed in its
  first 10 s (^C in the zsh benches is SIGKILL) leaves one in
  `~/Library/Caches/xemu-fork-bench`; the launchers warn, so remove it.
  A strike test gets its own `XEMU_BASE_PATH`.
- **RAM hashes are per driver** in offscreen det runs (null goldens
  aren't); take timeline markers on the same driver.
- **Gaps:** wireframe draws filled, custom border colors need
  `MESA_KK_EXPERIMENTAL=custom_border`, device loss is fatal, no GS.

## The Auto-back gate

On Mesa 26.3.0 final (and every later pin while KK is the default), on
the CI universal zip against the published release, quoting the load:
null goldens; MoltenVK's `-icount` digests unchanged; KK det digests 5/5
on f1 and e2; the fallback matrix (no dylib, corrupt dylib, failed
acceptance, a KK fatal error or first-frame crash then a relaunch, a
Settings re-pick, a quit inside the window, MoltenVK failing after KK,
Auto with a strike on disk, `presentation_backend = 'opengl'`,
offscreen without icount); C1's check and C2's bar; screendumps against
MoltenVK (PSNR >= 45 dB, <= 0.01% of pixels off by more than 32); sync
validation clean; windowed cross-binary A/Bs at scale 3-4 with MetalFX
and interpolation; a 30-minute flat-footprint soak, the 8-anchor loadvm
cycle, the refuter soak (`PROBE_CYCLES=3 scripts/probe-run.sh APP OUT
azurik-heavy-f1,vm-20260704032357 10 XEMU_SUBPAGE_FAST_REFUTE=1`),
`boot-smoke-corpus.sh`, the meson xbox suite, CI on all OSes (Roswell's
`- Driver:` line included), a KK smoke run, and a launch smoke on macOS
15 and Intel.

**v0.16 result** (tag `archive/kosmickrisp-v016-gate`, M2 Ultra, rc1):
both drivers hold 60 fps on heavy-f1 and W2; CPU fans gave MoltenVK
KK's CPU lead (against v0.15.3: KK -8.9 / -3.3 points of a core,
MoltenVK -9.9 / -8.1), and C2 failed. KK keeps a 0.4-0.6 GB lower peak
footprint and applies the NV2A LOD bias (MoltenVK drops it).

### What flips Auto back

The code change is `resolve_driver`'s `try_kk` line. Texts that say Auto
runs MoltenVK change with it: the README, `config_spec.yml`, the
Settings help, `docs/optimizations.md`, `docs/README.md`, `instance.c`'s
comments, this file, the skills, and pgo-train (its KK session goes back
to `XEMU_VK_DRIVER=moltenvk`).

## Open items

- **C1, occlusion results read as zeros under load** (offscreen icount
  on KK). The stream-e2 walk (`-t azurik-stream-e2 -i "axis ly 1.0" -f
  600`) deviates in 4 of about 150 runs at load 47-112, none of 165 at
  26-40; in both caught deviants a run of query results reads exact
  zeros. Instrumentation: `../kk/patches/c1-gpu-event-log.patch`;
  receipts in `../kk/research/c1-rc1/` (deviants `hunt3/log2.txt`,
  `hunt4/log28.txt`; modal `log1.txt`). Next: re-read and
  drain-before-submit experiments at load 70+, then the fix (a parking
  query index nobody reads, or a drain before submit). Check: the walk
  on KK at least 40 times at load 70+, all identical; small +/-N
  differences (H1, AGX split variance) get documented instead of fixed.
- **C2, first-encounter cost.** Cold walk: 11 hitch intervals on KK, 6
  on MoltenVK; warm, one KK frame of 224 ms from 2 new pipelines. KK
  bakes depth-stencil, blend and vertex-input state into its shader
  keys; the fix is upstream's (the fork files no Mesa issues). These
  numbers predate `perf.async_pipeline_compile`. Next: on Mesa 26.3.0
  final, `movement-probe.sh --isolate-caches`, 3 cold and 3 warm per
  driver. Bar: no warm KK frame over 50 ms, and KK's cold count at most
  one above MoltenVK's.
- **Workarounds:** on macOS 26.x KK enables 1-8, 11, 12, 17 and 18 (18
  is inactive on M2); 2 and 6 cover Apple bugs and stay. Measure
  `MESA_KK_DISABLE_WORKAROUNDS=2,6,17` once, with per-arm caches (KK's
  pipeline hash ignores the set); decide only on 17. A default would go
  in `main()` with overwrite 0.
- **Limits and coverage:** `maxPushConstantsSize` 256 is exactly xemu's
  need; `maxMemoryAllocationCount` 4096 (count dedicated VMA allocations
  in the soak); 16-bit index out-of-bounds (workaround 13) is still open
  upstream; KK #15998 (render-pass memory never reclaimed) needs the
  30-minute footprint trace; only Apple8 (M2) on macOS 26.7.1 is
  measured.
- **Settled from source:** all three xemu command-buffer begins use
  ONE_TIME_SUBMIT, so KK's re-record penalty never applies; queries are
  read on the CPU (`vkGetQueryPoolResults` with WAIT) from a 4096-entry
  pool, under KK's device-wide 32768.
