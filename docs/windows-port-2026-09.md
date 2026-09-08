# Native Windows port — status and handoff (2026-09-07)

First session of this fork on real Windows hardware. Everything below was
done on a Windows 10 Pro 22H2 box: Intel i7-4770K (Haswell, 4C/8T, AVX2),
NVIDIA TITAN Xp (driver 561.09, Vulkan 1.3.280, two TITAN Xp devices
enumerated), 32 GB RAM, MSYS2 MINGW64 (gcc 16.2, meson 1.9.0 from
`python/wheels`, mingw python 3.14.7, cmake 4.4.3). macOS remains the
first-priority platform; every change here is Windows-gated or a
platform-neutral bug fix whose Apple path is argued unchanged in its
commit body — **none of it has been built or run on macOS yet** (see
"Owed on macOS").

## 1. Environment recipe (MSYS2 MINGW64)

```
winget install MSYS2.MSYS2            # -> C:\msys64
# in C:\msys64\usr\bin\bash -lc:  pacman --noconfirm -Syuu  (twice)
pacman -S --needed --disable-download-timeout git make patch diffutils \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-clang mingw-w64-x86_64-lld \
  mingw-w64-x86_64-compiler-rt mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python mingw-w64-x86_64-python-yaml \
  mingw-w64-x86_64-python-requests mingw-w64-x86_64-ccache mingw-w64-x86_64-glib2 \
  mingw-w64-x86_64-pixman mingw-w64-x86_64-libepoxy mingw-w64-x86_64-libsamplerate \
  mingw-w64-x86_64-sdl3 mingw-w64-x86_64-libslirp mingw-w64-x86_64-vulkan-headers \
  mingw-w64-x86_64-vulkan-loader mingw-w64-x86_64-vulkan-validation-layers \
  mingw-w64-x86_64-zlib mingw-w64-x86_64-zstd mingw-w64-x86_64-libusb \
  mingw-w64-x86_64-curl mingw-w64-x86_64-openssl mingw-w64-x86_64-gdb
```

Then, from a MINGW64 shell in the repo: `./build.sh -j8` → `dist/xemu.exe`
plus bundled DLLs (`get_deps.py`). Traps hit on the way, all fixed in-tree
or documented:

| Trap | Cause | Fix |
|---|---|---|
| `bash: ./build.sh: \r: command not found` | checkout had `core.autocrlf=true` | `git config core.autocrlf false && git rm --cached -r . && git reset --hard` |
| `prefix value '/qemu' must be an absolute path` | Python ≥ 3.13 `ntpath.isabs` change vs QEMU's `configs/meson/windows.txt` | `build.sh` passes `--prefix=$(cygpath -m /qemu)` (commit `ac28e5a5bd`) |
| meson "Unhandled python exception" in the CMake probe | `Path.home()` needs `USERPROFILE`; a login shell spawned from a foreign parent lacks it | export `USERPROFILE`/`HOMEDRIVE`/`HOMEPATH` before `source /etc/profile` (only when driving bash from another tool; a normal MSYS2 terminal has them) |
| dsp56300 `tar: Cannot connect to C:` | MSYS tar parses `C:/x` as host:path | `--force-local` on Windows (commit `ac28e5a5bd`) |
| Vulkan SDK via winget cancelled | installer needs UAC; no elevation from the agent shell | validation layer from MSYS2, loaded with `VK_LAYER_PATH=C:\msys64\mingw64\bin` |

Build time: ~10 min cold on this box. Warnings on the Windows leg are
listed in `build.log` (103; the fork-relevant ones are `-Wmaybe-uninitialized`
in `nv2a_regs.h` users, `-Wnested-externs`/`-Wmissing-prototypes` in
`accel/tcg`, and an unused-variable pair in `ui/` fixed in wave 1).

## 2. Upstream merge

`2e0aad3e18` merges xemu-project/xemu master `429c9972eb` (12 commits):
DXGI/WGL_NV_DX_interop presenter, PTIMER alarm IRQs (vmstate v4),
controllerdb from config path, joystick logging, glslang 16.5.0, and
`audio.use_dsp_jit` default → **false**. Only `ui/xemu.c` conflicted; the
DXGI hooks live inside the fork's non-Metal GL branch. Mediation is in the
merge body. Non-Apple hosts now default to the DSP **interpreter** (fork
precedence unchanged: `audio.dsp_jit.enabled` wins where supported).

## 3. First real-hardware results (Azurik, Vulkan renderer, TITAN Xp)

- Renders correctly: no black screen, no pink tiles, HUD fine. This is the
  first execution of the fork's Vulkan renderer on a native ICD; the
  2026-08-09 native-driver fixes (split UBO write, dynamic-state poisoning,
  bufferOffset alignment) hold.
- `XEMU_NV2A_NSPROF=1`: intro/menu ≈ 30 flips/s; the title area sits at
  21–25 flips/s, i.e. **below the title's 30 fps cap** — there is CPU-side
  headroom to chase on this host. Cold first interval is shader/pipeline
  generation (pipeline_gen 16 ms/flip, max 72 ms); the on-disk caches make
  later launches warm.
- Audio backend chosen at runtime: `dsound` (one "Voice is not playing"
  warning at quit).
- Upstream's DXGI presenter **never activated** on NVIDIA
  (`GL_FRAMEBUFFER_UNSUPPORTED` at FBO check) — fixed in `2182baec46`
  (lock the interop object around the completeness check; 1:1 lock/unlock
  per frame). After the fix: "DXGI Presentation Helper successfully
  initialized (mode=flip, tearing=supported)", output upright and correct.
  Candidate for upstream.

## 4. Static audit (13 finders → 2-lens adversarial verification → planner)

Workflow result, 2026-09-07: 114 raw → 89 deduped → **60 confirmed, 19
plausible, 10 refuted**. Ranked table: `docs/windows-audit-2026-09-summary.md`.
Full JSON with evidence, verifier notes and the 22-batch plan:
`docs/windows-audit-2026-09.json` (per-batch splits were used to drive the
implementers). Headline confirmed items:

- **Platform-neutral renderer defects** (affect macOS too, hidden by
  MoltenVK leniency): REPORTS_FULL capacity guard in `begin_draw()` runs
  after `begin_pre_draw()` built slot state → `framebuffers[-1]` /
  `descriptor_sets[-1]` (draw.c); `create_pipeline()` early-return can leave
  the CLEAR pipeline bound (draw.c); surface upload writes staging at
  absolute offset 0 ignoring the flight-slot partition (surface.c:1939);
  dynamic-state skip caches not invalidated when a pipeline sets the same
  state statically (draw.c:2388); PVIDEO upload calls `pgraph_vk_finish()`
  with the display aux CB open (display.c:548, blocker); PVIDEO overlay
  cache keyed on registers only (display.c:1633).
- **x86_64 host FP**: the 2026-08-09 FIST/FRNDINT directed-rounding
  fallback calls the *hard* helper and x86_64 never programs the host x87
  control word, so the fallback is a no-op (translate.c:2103,
  fpu_helper.c:1186); Windows `longjmp` restores MXCSR leaving
  `cached_fpuc_rc` stale (translate.c:1785).
- **APU**: guest voice handle indexes `filters[]` out of bounds (vp.c:329).
- **Perf opportunities on Windows**: sub-page store pre-filter has no i386
  emitter (forfeits the measured +12%); FIST/FRNDINT via x87 round-trip
  where CVTSD2SI would do; host-SSE fast path ships dark; release Windows
  build is `-O2` not `-O3`; no thread-priority equivalent of the Apple QoS
  bumps; DSP fully interpreted by default; volkLoadDevice never called.
- **Harness**: the whole benchmark/testing protocol is macOS-only (zsh,
  AF_UNIX monitor, screencapture, APFS clones, `dist/xemu.app`).

## 5. What landed in this session

On `macos-optimizations` (all validated on this box, none on macOS yet):
the upstream merge `2e0aad3e18`, the MSYS2 configure fixes `ac28e5a5bd`,
the DXGI presenter lock fix `2182baec46`, and these docs.

On branch **`windows-wave1-wip`** (NOT merged): wave 1 of the audit plan
as one commit per batch (the present-hatch + ui-warning batch rode along
inside the harness commit `ec31facd7d`) — SPIR-V cache
hardening, PVIDEO/display reorder, APU/XID bounds, vk hygiene
(real bounds checks, volkLoadDevice, LRU filter), Windows/GL
diagnostics, build.sh GCC PGO + `-O3` parity, and the Windows benchmark
harness + `XEMU_INPUT_PIPE` named-pipe transport. **The combined binary
compiles but segfaults ~10 s into Azurik (after 32 flips, exit 139);
`XEMU_VK_VOLK_DEVICE=0` does not help.** Bisect recipe on this box:
`git checkout windows-wave1-wip`, revert one batch commit at a time
(start with `apu/xid:` then `vk: never finish inside the display aux
CB`), `rebuild-quick.sh`, then `PORT=4456 EXE=../xemu-macos/dist-w1/xemu.exe
run-test.sh 55 <name>` and look for a second nsprof interval. Each batch
commit body carries its gating and intended validation; the escape-hatch
rows are already in `docs/optimizations.md`.

## 6. Remaining plan (batches not yet implemented)

From the planner (`docs/windows-audit-2026-09.json` → `plan.batches`),
in recommended order. Batches marked *savestate* need the Azurik in-game
savestate benchmark (not yet created on this box — see §8).

| # | Batch | Needs |
|---|---|---|
| 3 | draw.c pipeline / dynamic-state / query-capacity correctness (REPORTS_FULL guard move, CLEAR-pipeline early return, static-vs-dynamic state) | validation layer + savestate A/B; re-run `XEMU_MAX_QUERIES=64` on both platforms |
| 4 | Surface/texture upload correctness (flight-slot staging offset, swizzled Z24S8 zeta upload, compute-set reservation loop) | same |
| 5 (rest) | surface.c prune order, texture.c barrier ranges, draw.c query-reset range | validation layer |
| 6 | Shader-key / uniform dirty-tracking (7 unmarked VSH uniform writers; `use_hw_depth` for POINTS on native) | a point-sprite title |
| 8 | x87/FP correctness on x86_64 (program host x87 CW or route directed-rounding FIST/FRNDINT to softfloat; MXCSR-after-longjmp) | `XEMU_X87_REFUTE` harness on Windows |
| 13–16 | perf: `-O3` parity A/B, CVTSD2SI FIST path, host-SSE fast path (`XEMU_SSE_HOST=2` soak first), i386 sub-page store pre-filter | savestate |
| 17 | perf: Windows thread scheduling (pfifo/vblank/APU `SetThreadPriority`/MMCSS), UI frame cap on the GL/DXGI path | savestate |
| 18–20 | perf: renderer micro-costs, 1 GiB persistently-mapped staging, GL texture thread pool, async VK→GL handoff via external semaphores (measure `aux_fence` first; kill if < 0.3 ms/present) | savestate |
| 21 | measure DSP engine choice on Windows (interpreter vs upstream JIT) | savestate + audio soak |
| 22 | docs reconciliation (architecture-contract Invariant 2 text vs the REPORTS_FULL finding, windows-gating-audit rows, `use_dsp_jit` default claims in docs/optimizations.md) | none |

Open policy questions for the maintainer: keep `renderer = VULKAN` as the
first-run default on Windows/Linux (works on this NVIDIA box; upstream
defaults to OpenGL there)? Add a native-MSYS2 CI job (~30–45 min cold)?
Send the DXGI lock fix, `volkLoadDevice`, and the tar `--force-local`
guard upstream?

## 7. Windows test recipe on this box

Helper scripts (not in the repo) live in `C:\Users\ms\Documents\xemu-win-test\`:
`build-mingw64.sh` (full build), `rebuild-quick.sh [--install]` (ninja +
copy to `dist/`), `run-test.sh <secs> <name>` (launch with
`-monitor tcp:127.0.0.1:4444,server,nowait`, screenshot via `shot.ps1`,
quit through the monitor; `CFG=test-validation.toml` enables the Khronos
layer with `VK_LAYER_PATH=C:\msys64\mingw64\bin`). The in-repo
`scripts/bench-savestate-ab-win.py` (wave 1) is the durable replacement.
Never run against `%APPDATA%\xemu\xemu\xemu.toml` — the fixture uses a
copied HDD image and `-config_path`.

## 8. Owed on macOS before any of this counts as landed

(For `macos-optimizations` head: the merge, the build fixes and the DXGI
fix. The wave-1 branch additionally needs the Windows bisect above
before it is even a candidate.)

1. Build `./build.sh` on the Mac at the pushed head; fix any Apple-side
   compile fallout (most likely spots: `ui/xemu.c` include reorder,
   `gl-helpers.cc` gating, `nsprof` GL flip hook, `glsl.c` cache rewrite).
2. Boot to the Azurik F5 savestate and run the interleaved A/B
   (`scripts/bench-savestate-ab.sh`) at fps parity vs `ea9633798d` — the
   platform-neutral fixes must be within ±0.02 fps static / noise.
3. Artifact-oracle soak (window captures scored for magenta) — the SPIR-V
   cache rewrite and PVIDEO changes are the ones that could alter pixels.
4. Confirm `git describe`/CI: the push fires the full matrix including
   win64-cross; the fork's Windows CI does not run natively, so this box
   remains the only native-Windows validation.

## 9. Continuation prompt for the Mac (paste into a Claude Code session in the repo)

```
Context: on 2026-09-07 the macos-optimizations branch was merged with
upstream xemu master (2e0aad3e18), built and run natively on Windows for the
first time, statically audited, and a first wave of Windows-first fixes was
landed — all from a Windows box, with zero macOS builds. Read
docs/windows-port-2026-09.md (§5 "what landed", §8 "owed on macOS") and
`git log --stat ea9633798d..HEAD` first. Load the xemu-change-control,
xemu-validation-and-qa and xemu-testing skills.

Do, in order, and stop on the first failure:
1. `./build.sh` (arm64, release). Fix any Apple-side compile fallout in the
   wave-1 files (ui/xemu.c include reorder + XEMU_WIN32_DXGI helper,
   ui/xui/gl-helpers.cc gating, hw/xbox/nv2a/pgraph/vk/glsl.c cache rewrite,
   hw/xbox/nv2a/pgraph/vk/display.c PVIDEO reorder, nsprof GL flip hook,
   accel/tcg/xemu-inv-prof.h rdtsc arm, APU bounds in hw/xbox/mcpx/apu/vp/vp.c).
   Every fix must keep the Windows path identical — do not undo the gating.
2. Launch dist/xemu.app from a scratch APFS clone (never the real bundle),
   boot Azurik, load the F5 savestate, confirm: no crash, no pink tiles,
   MetalFX/interpolation still engage, `XEMU_NV2A_NSPROF=1` prints.
3. Interleaved savestate A/B (scripts/bench-savestate-ab.sh, ≥6 pairs) of
   HEAD vs ea9633798d on the heavy Azurik scene. Acceptance: within noise
   (±0.02 fps static / sub-0.5% suspect). The platform-neutral changes that
   could move the needle: PVIDEO staging reclaim reorder (display.c),
   SPIR-V cache validation (first launch only), REPORTS/display early-out
   widening (XEMU_DISPLAY_SKIP_STRICT=0 is the legacy hatch), volk device
   dispatch if landed. If a regression shows, bisect with the hatches listed
   in docs/optimizations.md "Windows native port (2026-09-07)" before
   touching code.
4. Artifact-oracle soak: ≥300 window captures on the F5 scene scored for
   magenta clusters — must be zero.
5. Run the xbox unit suite (`meson test -C build --suite xbox`).
6. If all green: append the A/B receipt to the ledger entry in
   docs/optimizations.md and push. If CI (incl. win64-cross) is red on the
   Windows-first push, fix forward the same day.
Then continue with the remaining audit batches in docs/windows-port-2026-09.md
§6, starting with batch 3 (draw.c REPORTS_FULL guard move) which the
architecture contract's Invariant 2 must be re-validated against on both
platforms with XEMU_MAX_QUERIES=64.
```
