# KosmicKrisp: migration status and plan

**Status (2026-10-17): CONDITIONAL.** KosmicKrisp (KK), Mesa's
Vulkan-on-Metal driver, is better than MoltenVK, but not much better.
MoltenVK stays the default for v0.16. The KK work is banked on the
`kosmickrisp` branch, where one binary runs either driver and the driver
is picked at launch with `XEMU_VK_DRIVER`. KK becomes the default once
conditions C1-C4 below pass and either C5 passes or the owner accepts the
gains without it. This document lives on the `kosmickrisp` branch and is
kept current there until the flip.

Inputs: a research pass, an audit of every MoltenVK dependency, a KK
build from Mesa source, a prototype (six commits, originally `kk/proto`
on `6f9f751565`), and a measurement campaign, all from 2026-10-16/17. On
2026-10-17 the prototype was rebased onto master `e5233326f5` (v0.15,
the upstream v0.8.137-11 merge, the part-9 tests and round 7) with no
conflicts. File:line references are against `e5233326f5`.

## What the branch carries

| Commit | What it does |
|---|---|
| `nv2a/vk: XEMU_VK_DRIVER picks the Vulkan driver at launch` | dlopens the named driver and calls it directly: `vkGetInstanceProcAddr` if exported (MoltenVK), otherwise `vk_icdGetInstanceProcAddr` after ICD interface negotiation (v7; KK exports only `vk_icd*`). `VK_KHR_portability_subset` is enabled only when the device advertises it. stderr prints `VkPhysicalDeviceDriverProperties` as `- Driver: …`. |
| `nv2a/vk: MoltenVK-only workarounds stay on MoltenVK` | The visibility primer query and the Apple surface-upload fast path run only on driver ID `MOLTENVK`. KK heaps are untracked, so the fast path's reliance on MoltenVK's hazard tracking doesn't hold there. `MVK_CONFIG_*` setenvs are skipped only when `XEMU_VK_DRIVER` names KK. |
| `nv2a/vk: convert triangle fans on the CPU off MoltenVK` | Fans and filled polygons become CPU-built triangle lists on Apple drivers other than MoltenVK. KK's GPU fan unroll splits the render pass and mangled Azurik's HUD gauges in offscreen det runs. `XEMU_VK_CPU_FANS=0/1` overrides the default. The output is pixel-identical on MoltenVK. |
| `nv2a/vk: enable shaderDemoteToHelperInvocation` | SPIR-V 1.6 lowers `discard` to demote, and validation flagged every fragment shader on both drivers. Driver-neutral. |
| `nv2a: nsprof counts Vulkan draws of guest triangle fans` | nsprof `vk_fan_draw`. |
| `nv2a/vk: copy-present for drivers without VK_EXT_metal_objects` | Each frame, the compositor image is copied into a buffer whose memory is exported as an MTLHeap (`VK_EXT_external_memory_metal`). Metal (MetalFX, the UI and screendumps) reads it as a linear BGRA8 texture. A helper thread waits on each compositor timeline value and signals an xemu-owned `MTLSharedEvent`, so compositor submits stay async. `XEMU_VK_COPY_PRESENT=1` forces this path on a driver that does have `metal_objects`, for A/B runs. |
| `docs: KosmicKrisp migration status and plan` | This document and `scripts/build-kosmickrisp.sh`. |

With `XEMU_VK_DRIVER` unset, the branch behaves like master: MoltenVK
through `volkInitialize()`, the null goldens are unchanged, and fans,
present and workarounds work as before.

## Building KosmicKrisp

The dev layout is a `kk` directory beside the checkout (`../kk`). Build
it with:

```
scripts/build-kosmickrisp.sh            # or: scripts/build-kosmickrisp.sh /path/to/kk
```

It installs nothing system-wide. It needs Xcode 26 or later (for the
Metal 4 headers), macOS 26 on Apple Silicon, cmake, ninja and pkg-config
on `PATH`, and network access on the first run. A scratch build takes
about 14 minutes on an M2 Ultra under load, and LLVM takes 7.6 GB. The
script's steps are:

1. **Python venv** in `kk/venv` with meson, mako, pyyaml and packaging.
2. **Mesa source** at the pin, `MESA_REF=335d5698dc` (main,
   27.0.0-devel; the commit the prototype was measured on). The newest
   release tag at the time was `mesa-26.2.5`.
3. **LLVM 22.1.8**, the official macOS-ARM64 release tarball. The script
   checks the pinned sha256, plus the sigstore attestation when `gh` is
   installed. Only the build-time CL compiler (`mesa_clc`) links LLVM;
   the driver doesn't. Two fixes are applied:
   - **LTO bitcode:** the release's static libs carry LLVM 22 bitcode,
     which Apple ld's libLTO can't read. Every link that pulls LLVM uses
     `-Wl,-lto_library,<llvm>/lib/libLTO.dylib`, set through the
     generated meson native file and the translator's cmake linker flags.
   - **libc++ clash:** the release ships `libc++.a` and `libunwind`.
     With `--prefer-static`, `mesa_clc` linked them without libc++abi and
     failed on `___cxa_throw`. They are moved to
     `lib/hidden-runtimes/`.
4. **SPIR-V deps:** SPIRV-Headers and SPIRV-Tools (static) at
   `vulkan-sdk-1.4.363.0`, and SPIRV-LLVM-Translator v22.1.7 (static),
   all installed in `kk/deps/prefix`.
5. **Mesa, KK only:**
   ```
   meson setup build-release src/mesa --native-file native.ini --buildtype=release \
     --prefix=$K/install -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
     -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static -Dllvm=enabled \
     -Dshared-llvm=disabled -Dcpp_rtti=false -Dxmlconfig=disabled -Dexpat=disabled \
     -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled
   ```

The outputs are `kk/install/lib/libvulkan_kosmickrisp.dylib` (14.6 MB,
arm64, macOS 26 or later, system libraries only) and
`kk/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`. The
dylib's install name is absolute; bundling it needs
`install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib`.

Other sources, not used:
- **LunarG Vulkan SDK 1.4.363.0:** a prebuilt, LunarG-signed KK
  (Mesa 26.3.0-devel) inside the installer's 7z payload. It works for a
  quick try, but it isn't a pinned Mesa tag.
- **Homebrew `mesa`:** its KK links Homebrew's libxcb, X11-xcb, zstd and
  SPIRV-Tools, so it can't be bundled.
- **The `VK_EXT_metal_objects` patch:** about 120 lines exporting
  MTLTexture, MTLSharedEvent, MTLBuffer and MTLDevice from KK. It is
  archived at `../kk/patches/0001-kk-minimal-VK_EXT_metal_objects-export-xemu-macos-pr.patch`.
  Copy-present matches it (below), and LunarG won't take it upstream
  (Mesa #14209), so the plan uses stock KK.

## Running xemu on KosmicKrisp

`XEMU_VK_DRIVER` values:

| Value | Driver |
|---|---|
| unset or empty | `volkInitialize()`, the bundled MoltenVK (master's behaviour) |
| `moltenvk`, `mvk` | `@rpath/libMoltenVK.dylib` |
| `kosmickrisp`, `kk` | `@rpath/libvulkan_kosmickrisp.dylib`, a KK dylib copied into `Contents/Libraries/arm64` |
| any other value | a dylib path in `dlopen()` syntax, such as a dev build: `XEMU_VK_DRIVER=$PWD/../kk/install/lib/libvulkan_kosmickrisp.dylib` |

A healthy KK launch on stock KK logs:

```
xemu: Vulkan driver …/libvulkan_kosmickrisp.dylib (vk_icdGetInstanceProcAddr, ICD interface 7)
- Driver: KosmicKrisp, Mesa 27.0.0-devel (git-335d5698dc) (id 28)
nv2a: triangle fans: CPU triangle lists
nv2a: copy-present: compositor frames reach Metal through an MTLHeap-exported buffer (no VK_EXT_metal_objects)
nv2a: async compositor submit enabled (timeline forwarded to an MTLSharedEvent by a CPU thread)
```

Notes for test runs:
- **Pipeline caches can't be shared.** `pipeline_cache.bin` is keyed by
  the driver's UUID, and whichever driver runs last overwrites it. Give
  each driver its own `XEMU_BASE_PATH`, or pass `--isolate-caches` to
  `bench-abx.sh`. KK's blob is about 4× bigger (7.7-8.2 MB vs 2 MB). The
  SPIR-V cache is driver-independent. KK has no Mesa disk cache, so
  `MESA_SHADER_CACHE_DIR` does nothing.
- **Warm up first.** The first launch of a new bundle path costs KK
  about 1.6-2.2 s of extra device creation (Metal's cache) plus about
  3 s over the first 60 det frames. Do one throwaway launch per bundle
  copy and configuration before measuring.
- **Guest RAM hashes depend on the driver in offscreen (Vulkan) det
  runs,** because surface downloads write rendered pixels into guest
  RAM. They are deterministic per driver except heavy-w2 (C1). The null
  goldens don't depend on the driver. Take timeline markers from a run
  on the same driver.
- **A/B:** `bench-abx.sh <tag> N S OUT --b-app A --e-app A --b-env
  "XEMU_VK_DRIVER=mvk" --e-env "XEMU_VK_DRIVER=kk" --isolate-caches`.
  The identity lines say "MoltenVK 1.4.3" for both arms, so read
  `- Driver:` from stderr. `bench-det.py`'s A/B mode runs the null
  renderer only, so it never touches Vulkan.
- **Validation layers:** set `XEMU_VK_DRIVER=/opt/homebrew/lib/libvulkan.1.dylib`
  (the Homebrew loader), `VK_DRIVER_FILES=<kk icd json>`,
  `VK_LAYER_PATH=<layer manifests>` and `display.vulkan.validation_layers = true`.
  With sync validation on, KK reports zero messages on the anchors.
- **Gaps on KK:** no `fillModeNonSolid`, so wireframe draws come out
  filled. Custom border colors need `MESA_KK_EXPERIMENTAL=custom_border`
  and change no pixels on the anchors. Device loss is fatal.

## 1. Verdict: KK is better, not much better

**The bar.** On 2026-10-16 the owner asked to "fully transition if KK has
better performance". On 2026-10-17 they tightened that: "Make KK the
default only if it is much better than MoltenVK. Otherwise MoltenVK stays
the default and the KK work is banked on a `kosmickrisp` branch, pushed to
origin." KK is judged on macOS 26.7.1 only. At runtime KK ties or wins
everywhere, but at the 60 fps cap the gain is CPU and memory, not frames.
It also brings one correctness regression, one hitching regression and a
macOS 26 / arm64-only floor. Even under the looser "better" bar, the
determinism and hitch regressions would each block a release on their
own.

### Decisive numbers

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
| heavy-w2 det RAM hash stability | 14 / 14 | **15 / 19 (3 different deviant hashes)** | **KK nondeterministic** |
| Screendump PSNR vs MoltenVK | — | 46-55 dB | OK: KK applies the NV2A LOD bias, MoltenVK drops it |
| Validation (sync) | portability notes only | 0 messages | KK clean |

Other measurements:
- **Encoders:** KK issues about 78% fewer Metal encoders (about 1100/s
  vs 4700-5100/s). Part of that is the CPU-fan path, which removes
  MoltenVK's "convert indirect buffers" compute encoder (0.29-0.37
  ms/frame).
- **Per-thread CPU, windowed f1:** PFIFO is +4.3 points of a core on KK,
  because KK encodes Metal inline on PFIFO. The driver, Metal and GCD
  threads are −12.6 points.
- **Copy-present vs the patched driver:** PFIFO idle 12.3 vs 12.1 ms per
  flip; windowed +0.25%, 2/0; offscreen screendumps bit-identical on w2.
- **Robustness:** 8 loadvm cycles across 6 anchors with no device loss.
  Footprint stayed flat over 3 minutes windowed. The meson xbox suite
  passes 7/7.

Facts settled by reading source:
- **No disk cache:** KK never creates a Mesa disk cache
  (`kk_physical_device.c` only frees one).
- **Command buffers:** all three xemu command-buffer begins already use
  ONE_TIME_SUBMIT (`command.c:123,175`, `draw.c:2125`), so KK's
  re-record-at-submit penalty never applies.
- **Query readback:** xemu reads query results on the CPU
  (`reports.c:511`, `vkGetQueryPoolResults` with WAIT). Its pool holds
  4096 entries (`reports.c:41`), well under KK's device-wide limit of
  32768.
- **Z16 depth:** Z16 maps to `D16_UNORM` and takes the `vkCmdBlitImage`
  path at `surface_scale` > 1 (`surface.c:276-282, 366, 1895`). KK has no
  BLIT_DST on D16, so this is a new risk (R8).

**Why not GO:**
- **Performance:**
  - At the cap, users see no fps change.
  - The windowed fps margin is noise.
  - The only consistent wins are CPU and footprint, about 10% each.
  - The offscreen det lead is real, but it measures submit latency in an
    unpaced mode.
- **Correctness:**
  - heavy-w2 RAM results are nondeterministic on KK, and nobody knows
    why.
  - KK's own GPU fan unroll has a race, which xemu now avoids with CPU
    fans.
  - On KK, any device loss is fatal; MoltenVK could resume.
  - Wireframe draws come out filled.
- **First encounter:** compiling new content costs 1.35-2× as much, and
  streaming hitches are worse.
- **Platform:** KK needs macOS 26 and Apple Silicon. Intel Macs and
  macOS 14/15 users would be dropped.
- **Present path:** it needs either a local Mesa patch or a per-frame
  copy (copy-present).

**Why not NO-GO:**
- Khronos deprecated MoltenVK on 2026-10-13.
- KK is Vulkan 1.4 conformant, gets fixes daily, and already runs every
  anchor: Metal present, MetalFX spatial and temporal, interpolation,
  validation clean, 8 loadvm cycles with no device loss, xbox suite 7/7.
- The migration is coming either way. The prototype shows it is about
  750 lines, and most of it is driver-neutral.

### Conditions to make KK the default

C1-C4 must all pass; C5 or the owner's acceptance then decides.
- **C1, determinism:**
  - heavy-w2 RAM hashes at f300 and f1200 are identical in 20 of 20 KK
    offscreen det runs.
  - heavy-f1 (20 runs) and stream-e2 (11 runs) stay stable.
  - The root cause is understood and fixed, in xemu or upstream.
- **C2, first-encounter cost:**
  - `movement-probe.sh --isolate-caches`, 3 cold and 3 warm runs per
    driver.
  - KK's count of intervals with a >50 ms frame must not exceed
    MoltenVK's.
  - No warm KK frame may exceed 50 ms.
- **C3, pinned stable driver:** the whole gate (§2.3) passes on a Mesa
  release tag. The candidate is `mesa-26.3.0`, due about 2026-11-04.
  `main` is not acceptable.
- **C4, coverage:**
  - Stock KK with copy-present passes at `surface_scale` 3 and 4, with
    temporal MetalFX and 120 Hz interpolation. So far only f1 at scale 2
    was measured.
  - A 30-minute soak shows no device loss and a flat footprint.
  - Scaled Z16 surfaces are fixed (§4, R8).
- **C5, "much better":** a windowed win of at least 5% fps, 3/3, in a
  scene below the cap. Candidates: scale 4 with 4x interpolation, or a
  loaded box, where the det lead grew with load. The alternative is that
  the owner accepts CPU and footprint savings plus MoltenVK's deprecation
  as enough.

**Recommendation for that re-decision:** if C1-C4 pass, flip even
without C5. Every runtime number either ties or favours KK, and MoltenVK
will now decay. That call belongs to the owner under the 2026-10-17
rule, so v0.16 ships on MoltenVK.

**Candidates for master now** (driver-neutral, useful on MoltenVK too;
§2.1):
- the demote-feature fix;
- the optional portability subset;
- the driver identity in the log.

---

## 2. Migration plan

### 2.1 Phase 0: master, v0.16, MoltenVK stays default

| # | Commit | Files | Verification |
|---|---|---|---|
| M1 | `nv2a/vk: enable shaderDemoteToHelperInvocation` (cherry-pick from this branch). SPIR-V 1.6 lowers `discard` to demote, and validation flagged every fragment shader on both drivers. | `hw/xbox/nv2a/pgraph/vk/instance.c` | Clean compile. A MoltenVK validation run on heavy-f1 has no demote messages. Offscreen det screendumps for f1, w2 and e2 are bit-identical to the v0.15 MoltenVK baseline, with the same RAM digest. Null goldens. |
| M2 | `nv2a/vk: enable VK_KHR_portability_subset only when advertised, and log the driver`. Splits the spec-correct part out of the `XEMU_VK_DRIVER` commit, and prints `VkPhysicalDeviceDriverProperties` (name, info, ID) beside "Selected physical device". Scripts read driver identity from that line instead of the bundled dylib's MoltenVK version. | `instance.c`; `scripts/bench-abx.sh`, `bench-savestate-ab.sh`, `bench-receipt.py`, `bench-savestate-ab-win.py` | Compile. One MoltenVK headless run logs `MoltenVK 1.4.3, driverID 14`. `bench-abx --dry-run` shows the new identity field. |

The rest of the branch (loader, gating, CPU fans, copy-present) is dead
code while MoltenVK is the only driver, so it stays here, not on master.

### 2.2 Phase 1: the `kosmickrisp` branch, the migration in order

The branch is rebased whenever master moves and is pushed to origin
(owner-approved 2026-10-17). Flipping the default means merging this
branch with one merge commit (rollback in §4).

**K1. `nv2a/vk: load the Vulkan driver as an ICD`** (from the
`XEMU_VK_DRIVER` and MoltenVK-only-workarounds commits)
- Files: `hw/xbox/nv2a/pgraph/vk/instance.c`, `renderer.h`.
- On `__APPLE__`:
  - `dlopen("@executable_path/../Libraries/arm64/libvulkan_kosmickrisp.dylib")`,
    falling back to `XEMU_KK_PREFIX/lib` for dev trees;
  - `dlsym("vk_icdGetInstanceProcAddr")`, negotiate ICD interface v7,
    `volkInitializeCustom`;
  - xemu keeps the handle, since `volkFinalize` won't close it;
  - `volkLoadDevice` runs on Apple too, because KK hands back device
    trampolines.
- No Khronos loader is bundled. That gives deterministic driver choice,
  no interference from a Homebrew `MoltenVK_icd.json`, and no dispatch
  hop.
- `XEMU_VK_DRIVER=<path>` stays as a dev override for loading another KK
  build. Under the owner's rule it is a diagnostic, not a legacy hatch,
  and it is documented as "KK builds only". Validation runs use
  Homebrew's loader with `VK_DRIVER_FILES`, as above.
- Linux and Windows keep `volkInitialize()` unchanged (`#ifdef
  __APPLE__`).
- Verify:
  - compile on all three OSes (CI);
  - a headless KK run logs `KosmicKrisp … (id 28)`;
  - null goldens f300 `6a553d8c7727c6c6` and f1200 `037271ebadb297c5`,
    e2 f600 `1e98a85ee122f87c`, cold boot f900/f1800;
  - `git diff` shows no non-Apple change outside `#ifdef`.

**K2. `nv2a/vk: present through an MTLHeap copy`** (from the
copy-present commit, which becomes the only Apple path)
- Files: `display.c`, `image.c`, `metalfx_upscale.{h,m}`,
  `renderer.{c,h}`, `ui/xemu.c`.
- Delete these `VK_EXT_metal_objects` paths: texture-export mode,
  IOSurface import (the GL presentation backend under the Vulkan
  renderer), `SHARED_EVENT` export, the `metal_objects`-gated zeta
  snapshot, the dark `XEMU_MFX_REAL_DEPTH=2`, and `XEMU_VK_COPY_PRESENT`.
- With the Vulkan renderer on macOS, presentation is Metal only.
- **Why stock KK plus copy-present, not the 120-line Mesa patch:**
  - Measured parity: PFIFO idle 12.3 vs 12.1 ms per flip; windowed
    +0.25%, 2/0; offscreen screendumps bit-identical on w2.
  - LunarG says it won't take `metal_objects` (Mesa #14209), and a
    private patch on a driver that changes daily is a standing rebase
    cost.
  - A pristine pin lets CI build Mesa straight from a release tag.
  - The patch stays archived at `../kk/patches/` in case LunarG ever
    accepts it.
- Verify:
  - offscreen screendumps for f1, w2 and e2 are bit-identical to the
    prototype's patched-KK dumps;
  - windowed `bench-savestate-ab.sh` exits 0 with `XEMU_INTERP_GATE=0`,
    MetalFX spatial, and temporal at scale 4 (C4);
  - a push-ring refute run (`XEMU_PUSH_PRESENT_REFUTE`);
  - the stderr lines "copy-present" and "async compositor submit
    enabled".

**K3. `nv2a/vk: triangle fans and filled polygons become CPU triangle
lists`** (from the CPU-fan and nsprof commits)
- Files: `draw.c`, `nsprof.{c,h}`.
- Unconditional on Apple. `XEMU_VK_CPU_FANS` is removed, because the
  GPU-fan alternative is broken on KK (HUD spikes in offscreen det w2
  and e2) and splits the render pass on Mesa 26.3 and later. Linux and
  Windows keep native fans. This is the idea closed on 2026-10-15,
  reopened because KK requires it.
- Verify:
  - nsprof `vk_fan_draw` = 0 on KK;
  - offscreen det w2 and e2 screendumps free of HUD spikes
    (`score_frames` clean);
  - PSNR vs the MoltenVK baseline ≥ 45 dB.

**K4. `nv2a/vk: drop the MoltenVK workarounds`**
- Files: `draw.c`, `surface.c`, `ui/xemu.c`, `Info.plist`, six scripts,
  docs.
- Remove the visibility primer query (`draw.c:2149-2172`).
- Remove the Apple surface-upload fast path (`surface.c:1486-1516`). It
  relies on MoltenVK's hazard tracking, and KK heaps are untracked, so
  take the generic finish path.
- Remove the six `MVK_CONFIG_*` `setenv` calls (`ui/xemu.c:2727-2752`),
  the `Info.plist` `LSEnvironment` block (`:35-58`), and the
  `MVK_CONFIG_*` exports in `bench-abx.sh`, `bench-savestate-ab.sh`,
  `boot-smoke-corpus.sh`, `movement-probe.sh`, `probe-run.sh` and
  `soak-cycles.sh`.
- Keep:
  - the 4096 query guard (generic, and below KK's 32768);
  - the poisoned CB-scoped dynamic-state cache;
  - the split UBO writes and 16-byte staging alignment (spec fixes);
  - the GS-less paths (KK has no geometry shaders either);
  - the `fillModeNonSolid` and `wideLines` fallbacks.
- Verify:
  - KK offscreen det RAM digests for f1 and e2 unchanged vs K3 (the
    branch already gates these off on KK);
  - `soak-cycles.sh` for 30 minutes plus an 8-anchor loadvm cycle with
    no device loss (fatal on KK);
  - meson xbox suite.

**K5. `nv2a/vk: scaled Z16 surfaces convert through compute`**
- Files: `surface.c:276-282, 1602-1607`.
- KK drops BLIT_DST on D16, D32 and D32S8. D24S8 and D32S8 already go
  through compute; D16 still blits at scale > 1.
- Verify: a Z16 title or synthetic test at scale 2 and 3 under validation
  (no format-feature errors), and screendump parity with MoltenVK.
- This fix is driver-neutral. If it lands early, put it on master.

**K6. `build: bundle KosmicKrisp instead of MoltenVK`**
- Files: `build.sh`, `meson.build:2408-2435`, `Info.plist`,
  `scripts/build-kosmickrisp.sh`, `scripts/build-moltenvk.sh` (deleted),
  `scripts/gen-license.py`, README, the xemu-operate skill.
- `build.sh`:
  - `resolve_kk` looks in `macos-libs/arm64/opt/local/lib`, then
    `$XEMU_KK_PREFIX`, then downloads a pinned fork release asset
    `kosmickrisp-<mesa-tag>-arm64.tar.zst` with its sha256 in
    `build.sh`.
  - It never looks in `/usr/local` or `/opt/homebrew`.
  - Bundling: copy to `Contents/Libraries/arm64`,
    `install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib` (the
    built id is absolute), and codesign it like the other dylibs
    (`build.sh:438-440, 474-500`; `sign-macos-release.sh` applies the
    hardened runtime).
  - Provenance line: `Bundling KosmicKrisp from … (Mesa <ver>
    git-<sha>)`.
  - `macos_min_ver` becomes 26.0.
- `meson.build` on Darwin: a header-only `declare_dependency(compile_args:
  ['-DVK_USE_PLATFORM_METAL_EXT', '-I<kk prefix>/include'])`, like the
  Windows branch. The headers are Mesa's own `include/vulkan`, installed
  by the build script, so they match the driver. `libMoltenVK` is no
  longer looked up.
- `scripts/build-kosmickrisp.sh` (on the branch now, tarball mode only)
  gains:
  - `KK_DEPS=brew` for CI;
  - the pinned Mesa release tag (C3).
- `Info.plist`: `LSMinimumSystemVersion` becomes 26.0.
- License: add Mesa/KK (MIT).
- Verify:
  - `./build.sh` from a fresh tree;
  - `otool -L` on the KK dylib shows system libraries only, and on xemu
    shows no MoltenVK;
  - `codesign --verify --deep`;
  - a bundle clone runs headless KK f1 (after one warm-up launch);
  - null goldens.

**K7. `ci: build KosmicKrisp on macos-26 runners; ship an arm64-only
macOS zip`**
- Files: `.github/workflows/build-macos.yml`, `release.yml:71`,
  `docs/RELEASING-macos.md:68-69, 153-160`.
- Runners move from `macos-15` to `macos-26`. That is required for the
  Xcode 26 SDK (MTL4 headers) and for Metal 4 in the arm64 test step.
- New `kosmickrisp` job:
  - `actions/cache` key = Mesa tag + hash of `build-kosmickrisp.sh` +
    Xcode version;
  - on a miss: `brew install llvm spirv-llvm-translator spirv-tools` (CI
    may use brew; LLVM must be ≥ 20.1.8), pip-install meson, mako,
    pyyaml and packaging, then build;
  - on a hit: about 15 MB restored;
  - the artifact is passed to the build job;
  - the same tarball is attached to the fork's release so `build.sh`'s
    download fallback works.
- Replace the `MVK_EXPECTED` derivation and assert (`:83-131`) with a KK
  provenance assert: `strings` must show `Mesa <pinned ver> (git-<pinned
  sha>)`, and `otool -L` must list system libraries only.
- **x86_64:** drop it from the matrix and remove the `build_universal`
  job. KK is Apple Silicon only: it needs `MTLGPUFamilyMetal4`, and
  LunarG says it isn't visible to the loader on Intel.
  - Keeping MoltenVK for Intel contradicts "get rid of MoltenVK" and the
    owner's no-legacy-path rule.
  - A Vulkan-less x86_64 build with the GL renderer would be a different
    product that nobody tests.
  - Shared code still compiles for x86_64 in the Windows and Linux legs.
  - Intel and macOS 14/15 users stay on v0.15, a published, immutable
    tag. The release notes say so.
  - The asset becomes `xemu-<ver>-macos-arm64.zip`.
- Windows and Linux workflows are untouched.
- Verify:
  - a branch push runs CI (`ci.yml` `branches: ["**"]`) and it goes
    green;
  - a deliberately wrong pin makes the provenance assert fail (one
    throwaway run);
  - the release asset contains the KK dylib and no `libMoltenVK`.

**K8. `pgo: retrain on the KosmicKrisp tree`**
- Copy-present, CPU fans and KK's inline encoding on PFIFO all change
  the hot paths.
- Windowed training, muted, on fixture clones, KK bundle.
- Verify: goldens; a det cross-binary A/B against the pre-PGO KK build.

**Docs ride in their commits; no docs-only commits:**
- README: line 4 changes from macOS 14+ to macOS 26+ on Apple Silicon.
  Also the loading section, the env table and the offscreen determinism
  note ("same Mac, macOS and driver").
- `docs/optimizations.md` config table.
- The skills: xemu-operate; xemu-internals (new invariant: never rely on
  implicit Metal hazard tracking, since KK heaps are untracked; present
  goes through copy-present plus the forwarder); xemu-history (MoltenVK
  rows are marked "MoltenVK era" and kept).
- Owner-only: the `CLAUDE.md` rule "`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS`
  stays 0" becomes moot after the flip. It's the owner's file to edit.

### 2.3 Gate before the flip merge, and for every Mesa pin bump

1. **Null goldens** (all five). The driver can't move them; they guard
   everything else.
2. **KK offscreen det RAM digests**, 5 identical runs per anchor (f1, w2
   per C1, e2), recorded as per-driver references. Branch values
   (stock KK, copy-present, Mesa 335d5698dc): heavy-f1 f60/f300
   `82fc0c62b26f7a68`/`a41904dae60247a6`; prototype w2 f60/f300/f600
   `07b33a5e…`/`16a12437…`/`850e7b90…`. Take timeline markers from a KK
   run, never from MoltenVK.
3. **Screendumps vs a v0.15 MoltenVK bundle:** PSNR ≥ 45 dB, and ≤ 0.01%
   of pixels off by more than 32, on f1, w2 and e2.
4. **Validation** with sync on all three anchors: 0 messages.
5. **Windowed cross-binary A/B**, PGO on both sides: `bench-abx.sh` v0.15
   vs the KK bundle, heavy-f1 and w2, 3×45 s each, `--isolate-caches`,
   one warm-up launch per bundle.
   - Pass: fps not 0/3 worse; process CPU ≤ v0.15; screenshot verdicts
     PASS; `bench-savestate-ab.sh` exit 0.
   - Also scale 3 and 4 with interpolation (C4).
6. **`movement-probe.sh --isolate-caches`**, 3 cold and 3 warm runs (C2).
7. **Soak:** `soak-cycles.sh` for 30 minutes, the 8-anchor loadvm cycle,
   a 30-minute windowed footprint trace (flat), and
   `boot-smoke-corpus.sh`.
8. **meson xbox suite and CI green** on all OSes.
9. Quote the 1-minute load for every number.

---

## 3. Optimization opportunities KK changes or enables

| # | Opportunity | KK status | Expected gain | How to measure | Risk | Call |
|---|---|---|---|---|---|---|
| 1 | **CPU triangle-fan conversion** (closed on MoltenVK 2026-10-15) | Required. Avoids KK's encoder split and its fan race; also removes MoltenVK's 0.29-0.37 ms/frame "convert indirect" GPU encoder | Part of KK's 78% fewer encoders; fps ~0 at the cap | nsprof `vk_fan_draw`; xctrace encoders per second | Low (pixel-identical on MoltenVK) | **K3** |
| 2 | **Merging draws with `VK_EXT_multi_draw`.** The old premise ("~0 PFIFO cost per call, MoltenVK encodes on its own thread") no longer holds: KK encodes each `vkCmd*` inline on PFIFO (+4.3 points of a core). KK flushes state once, then loops | Newly worth it | ~1.6 ms/frame of driver CPU over ~576 draws/flip is ~2.8 µs per draw. With 10.6% candidates plus same-state runs, ≈0.1-0.3 ms/frame of PFIFO | Det offscreen per-thread PFIFO ms/frame (`proc_pidinfo`), Mcyc/f, nsprof per-draw | Low-medium. Compare inline attribute values; never merge across a report or zpass boundary | Prototype after the flip |
| 3 | **Saving the pipeline cache more often** (at loadvm, and every N new pipelines) | Driver-neutral, but KK's cold path costs 1.35-2× more | Avoids a cold relaunch after a crash or kill (and det runs never save today) | Kill-and-relaunch `pipeline_gen` totals | Low (atomic write) | Master |
| 4 | **Upstream KK: cache Metal binaries in the `VkPipelineCache` blob** (MTL4 archive or binary functions keyed by the MSL hash), or precompile libkk to a metallib | KK stores MSL and recompiles every load | Brings "xemu cold, Metal warm" (227 ms) to parity or better; −0.2-0.3 s at device creation | Same `pipeline_gen` matrix as §1 | Upstream work; we can file and prototype it | File upstream; addresses C2 |
| 5 | **Pipeline prewarm** (parked: "≥16 ms compile that survives a warm rerun") | KK had a 146 ms pipeline on the second cold run, and its warm probe still had 2 intervals with a >50 ms frame | Only cache-loss cases; truly new shaders can't be prewarmed | Movement probe, warm, with `pipeline_gen` per interval | Medium (memory, thread safety) | Reopen only if C2 fails after #3 and #4 |
| 6 | **`VK_EXT_host_image_copy` for texture uploads** (every KK memory type is shared and host-visible) | Both drivers have it; it pays most on KK | Removes the staging buffer and GPU copy per upload; stream-e2 benefits. Size unknown | stream-e2 det Mcyc/f, nsprof upload ms, movement-probe hitches | Medium. The image must be idle (see the pink-flash and torn-snapshot history); layout rules | Prototype; measure first |
| 7 | **Surface downloads through `vkCopyImageToMemoryEXT`** | Same | One less blit and buffer per download; `fence_wait` unchanged | Det offscreen download ms and `fence_wait` | Medium. Also touches the RAM path, so it is useful for C1 triage | After C1 |
| 8 | **`logicOp`** (KK yes, MoltenVK no); xemu hard-codes it off at `draw.c:1025, 1449` | New | Fidelity (NV2A XOR/INVERT), no perf | A title that uses it; screendump | Low (feature-gated) | After the flip |
| 9 | **NV2A LOD bias** | KK applies `mipLodBias`; MoltenVK drops it (`samplerMipLodBias=false`) | Fidelity, free on KK. Explains the PSNR delta | Screendump diff | — | Free. On MoltenVK master, shader-side bias is a possible fix (watch the explicit-LOD lookups) |
| 10 | **MTLResidencySet** (parked) | KK uses it internally | Done for free | — | — | Close |
| 11 | **ONE_TIME_SUBMIT everywhere** | Already true, so KK's re-record-at-submit penalty never applies | — | — | — | Keep. Any new command buffer must use it |
| 12 | **Dynamic rendering** (fenced) | Native on KK | Minor CPU (no framebuffer or render-pass churn); no measured upside so far | Det PFIFO ms | Medium (explicit barriers) | Skip unless profiling shows framebuffer cost |
| 13 | **`VK_KHR_global_priority` HIGH** | KK grants only MEDIUM (`NOT_PERMITTED` above it) | None on KK | — | — | Dead on KK; still open on MoltenVK master (expect little) |
| 14 | **Geometry shaders** | Absent on both | — | — | — | GS-less paths stay |
| 15 | **Cheaper queries** | Global visibility table, always bound: no AGX crash, primer removed (K4). Readback is CPU `vkGetQueryPoolResults` either way | ~0 | — | — | K4 only |
| 16 | **`VK_EXT_external_memory_host`** | Buffer imports only; the fence reason (torn reads, <0.5 ms per 5 s) is unchanged | — | — | — | Stays fenced |
| 17 | **Provoking vertex** | `provokingVertexLast` unrolls and splits on KK, so never request LAST. Whether xemu's rotation is right is a separate, driver-neutral question (both drivers use the first vertex natively) | Correctness | A flat-shaded test scene | Low | Master, driver-neutral |
| 18 | **Custom border color** (`MESA_KK_EXPERIMENTAL=custom_border`) | Shader-lowered; "may hurt perf"; changes no pixels on the anchors | Fidelity | — | Perf | Off for now; if enabled later, set it in `main()` and `Info.plist` |
| 19 | **List restart, `primitive_restart_index`** | Enabling list restart triggers KK splits | Negative | — | — | Avoid |
| 20 | **Wireframe** (no `fillModeNonSolid`) | Filled on KK | Fidelity | — | Low | Optional CPU line-list emulation in the existing emulated-primitive path |

---

## 4. Risks, unknowns and rollback

| # | Risk or unknown | Detail | Mitigation |
|---|---|---|---|
| R1 | **heavy-w2 RAM nondeterminism (C1)** | 4/19 KK runs, 3 different deviant hashes, diverging before f300. Screendumps are bit-identical, so the difference is in GPU results written back to guest RAM. Prime suspect: KK maps occlusion queries into one device-global table (`kk_query_pool.c:165-195`), reset by GPU zeroing (`kk_CmdResetQueryPool`, `:281`) in untracked-hazard mode, while xemu reuses each flight's query range and reads it back with WAIT. Second suspect: surface-download ordering without implicit hazard tracking | Dump every report value (`write_reports_from_queue`) and hash every surface download per frame; diff deviant vs normal runs; try `XEMU_MAX_QUERIES=64`; find the first divergent frame with KK-taken timeline markers. If the fault is in KK, file it with a repro |
| R2 | **KK's GPU-helper race** | The fan HUD spikes appeared only when xemu ran ahead of the GPU. Suspect: `kk_heap()` resets per command buffer (unconfirmed). Other GPU helpers may share the mechanism (uint8 indices, restart unrolls, query copies) | CPU fans (K3); avoid the triggers (§3 #19); file upstream |
| R3 | **Device loss is fatal on KK** | No `RESUME_LOST_DEVICE`; a freed-resource GPU fault that MoltenVK survived now aborts | Soak gate; the existing `pgraph_vk_fatal_error` path |
| R4 | **First-encounter compile and hitches (C2)** | 1.35-2× `pipeline_gen`; a 7.7-8.2 MB cache blob (MoltenVK ~2 MB); +0.3 s ready time | §3 #3, #4 and #5 |
| R5 | **macOS 26.x workaround path** | Conformance ran on macOS 27. On 26.7.1, KK turns on workarounds 1-8, 11, 12, 17 and 18. Workaround 13 (16-bit index out-of-bounds) is still open upstream. The owner stays on 26.7.1, so that is where KK is judged | The gate runs on 26.7.1; re-gate on every macOS point update |
| R6 | **Driver churn** | Mesa main changes daily. `pipelineCacheUUID` is a hash of the build sha, so every pin bump re-warms every user's pipeline cache once | Pin release tags only; bump only through the §2.3 gate; note the one-time cold session in release notes |
| R7 | **Platform floor** | macOS 26 on arm64 only; Intel and macOS 14/15 users are dropped | v0.15 stays available for them (immutable tag); release-notes callout |
| R8 | **Limits and formats** | `maxPushConstantsSize` 256 is exactly xemu's need (no headroom). Also: `maxUniformBufferRange` 64 KiB; `maxMemoryAllocationCount` 4096 (dedicated VMA allocations at `buffer.c:66-70`); D16 has no BLIT_DST (scaled Z16 path, K5); SNORM formats can't blend; several 16-bit and sRGB formats have no buffer features. xemu queries only linear-filter support, and an unsupported format aborts in `vkCreateImage` | K5; an init-time assert over every `kelvin_color_format_vk_map` and zeta format (including the Y16 entries the upstream merge moved to `R8G8_UNORM`); count allocations during the soak; any push-constant growth must check the limit |
| R9 | **Shared-storage render targets** (no lossless compression) | GPU bandwidth at scale 3-4: one batch −0.9% (0/3), another flat; GPU ms equal at scale 2 | C4 at scale 3 and 4 |
| R10 | **Contradictory `fence_wait` readings** | Det: KK lower (50 vs 79 ms per flip). Windowed at scale 3 with a loaded GPU: KK higher (2.26 vs 0.72 ms). Unresolved; it decides behaviour below the cap | Part of C5's below-cap run |
| R11 | **Long-session memory** | KK #15998 (render-pass memory never reclaimed); only 3 minutes were measured (flat) | 30-minute footprint trace in the gate |
| R12 | **Measurement confounds** | Non-PGO builds; only the KK arm had CPU fans; the display switched from 60 to 120 Hz mid-session; GPU contention of 85-99%; small samples (2 traces, 1-2 probes per arm) | The §2.3 gate re-runs on PGO builds with fixed display settings |
| R13 | **Startup** | dlopen takes 170-370 ms (unexplained, about 1.5G instructions). First device creation takes 1.8 s (libkk kernels plus the Metal cache), then 9 ms warm. The first launch of each bundle path costs +1.6-2.2 s | Accept; upstream metallib precompile (§3 #4) |
| R14 | **Signing and supply chain** | The KK dylib is built in CI from a pinned tag and re-signed by `sign-macos-release.sh` with the hardened runtime | Provenance assert (K7) |
| R15 | **No validation layers in shipped builds** | Direct ICD loading has no loader | Dev-only loader recipe (Homebrew loader plus `VK_DRIVER_FILES`), above |

### Rollback

- **Before the flip (v0.16 as planned):** nothing to roll back. Master
  stays on MoltenVK, and KK lives on `kosmickrisp`, rebased whenever
  master moves.
- **After the flip:**
  - The flip lands as one merge commit of `kosmickrisp`, so `git revert
    -m 1 <merge>` restores MoltenVK in one commit: bundling, the CI
    asserts, the `MVK_CONFIG_*` settings, the workarounds and
    `scripts/build-moltenvk.sh`. K5 (Z16) and the master-side items
    stay, because they are driver-neutral.
  - Ship the revert as a new point release; published tags are
    immutable.
  - v0.15 remains the MoltenVK download in the meantime.
  - A rollback costs users one cold pipeline-cache session, since the
    UUIDs differ. Savestates (guest state) and the SPIR-V cache are
    driver-independent; both drivers loaded the same anchor savestates
    in the prototype.
