# Windows gating audit

The register of fork changes that Windows compiles, and of what still
needs real Windows hardware. Verdicts: **upstream-on-non-Apple** (a
compile-time gate keeps upstream's path), **equivalence-argued** (the
reasoning is in the code comments) and **needs-real-HW** (never run on
a real Windows Vulkan driver). Audited 2026-07-04 (`a045dfc780`) and
2026-07-11 (`ed2568277b`); hardware reports 2026-08-09 and 2026-10-18.
Section letters match older citations; step ids are those of
[windows-test-plan.md](windows-test-plan.md); history is in
[Finished/windows-port-2026-09.md](Finished/windows-port-2026-09.md).
The roadmap's real-hardware item mirrors §T and §W; a new hardware run
updates both and adds a dated section after §W.

CI proves compiles only on Windows: `build-windows.yml` (win64-cross)
runs no tests. The xbox suite (`meson test --suite xbox`) runs on the
macOS arm64 and Linux legs. Its AArch64 DSP JIT arms are gated
`host_machine.cpu_family() == 'aarch64' and host_os != 'windows'`
(`tests/xbox/dsp/meson.build`), the corpus JIT arm also where
`dsp_llvm_jit` is built. Anything below not marked settled rests on
compiles and static review.

## A. Platform gates and Windows-only code

| # | Site | Windows behaviour | Test |
|---|---|---|---|
| A1, A2 | `vk/surface.c` | Apple narrows the upload-surface full GPU sync (`target_is_active`) and skips the scratch image at `surface_scale` 1; elsewhere upstream's full finish and an always-allocated scratch image | |
| A3 | `vk/instance.c` | Apple relaxes device features; non-Apple requires `geometryShader`, `occlusionQueryPrecise`, `fillModeNonSolid`, `depthClamp`, `shaderClipDistance` (`shaderTessellationAndGeometryPointSize` optional since v0.13.2). Device: the GPU driving the display, unless `display.vulkan.force_physical_device` names one (exact name; xemu never writes it; upstream v0.8.138-23) | 2.1, 2.4 |
| A5 | `pgraph.c`, `config_spec.yml` | `display.renderer` defaults to VULKAN everywhere (upstream: OPENGL). When the configured renderer fails to initialize for any reason, the Vulkan feature rejection included, `init_renderer` tries `get_default_renderer()` (OpenGL first off Apple; notifies `Switched to default renderer: ...`), then any other renderer; only if none starts does it print `Fatal error: cannot initialize any renderer. Renderer preference reset to OPENGL for next launch.` and exit (upstream `25afb8603d`, fork `c0d7bb5412`) | 2.4 |
| A6 | vCPU, `pfifo.c`, `apu.c`, `ui/xemu.c` | Apple QoS bumps compiled out. The normal-mode vCPU (mttcg), PFIFO and APU frame threads run at `THREAD_PRIORITY_ABOVE_NORMAL` (`f96f00ba9d`; `XEMU_VCPU_QOS=0` leaves them normal); vblank, the main loop and the icount RR vCPU at normal | 3.13 |
| A8 | Metal, MetalFX, IOSurface sources | darwin-only in meson; Windows presents through GL (DXGI flip-model presenter, Win32 external-memory VK-to-GL import) | 2.1 |
| A9 | `apu/vp/vp.c`, `fpconv.h` | vDSP on Apple, portable C elsewhere; mix kernels SSE2 on x86_64, NEON on aarch64 | 3.7 |
| A10 | DSP JIT | `DSP56K_JIT_SUPPORTED = __aarch64__ && !_WIN32` (the AArch64 emitter; its menu toggle hidden elsewhere). Windows runs upstream's dsp56300 JIT (`audio.use_dsp_jit`) or the interpreter. The portable LLVM JIT (`dsp56k_jit_llvm.c`, 2026-11-24) is opt-in: meson feature `dsp_llvm_jit` (`-Ddsp_llvm_jit=enabled`, `meson_options.txt`), LLVM 21 or newer, never on aarch64 POSIX; a cross build names `llvm-config` in its cross file. It wraps the interpreter (Phase 1) and runs only with `audio.dsp_jit.enabled = true` and `use_dsp_jit = false` | 3.7, 3.9, 4.5 |
| A11 | `tcg/aarch64/tcg-target-has.h` | `TCG_TARGET_HAS_fpu 0` under `_WIN32` (llvm-mingw can't fold `qemu_build_not_reached`); since v0.13.2 inline FP is gated on it, so Windows ARM64 runs the helper hard FPU. Pinned registers, the TLB memo and hot/cold TBs stay active; the batched icache flush is off | 3.9 |
| A12 | `ui/xemu-input.c` | `XEMU_INPUT_PIPE` is a named pipe (`CreateNamedPipeA`, `PIPE_NOWAIT`, one instance recycled per writer; `\\.\pipe\<name>` or the value's last path component; `164fd4c876`) | 3.4 |
| A4, A7, A13, A14 | `vk/instance.c`, `ui/xemu.c`, `fpng.cpp`, `util/` | no MoltenVK portability extensions; the `MVK_CONFIG` setenv block is Apple-only; CRC32 intrinsics gated on `__ARM_FEATURE_CRC32`; `util/cutils.c`, `util/miniz` and `osdep.h` carry no fork-changed `__APPLE__` lines | |
| A15 | `ui/xemu.c` console | stderr goes to the parent console or the file it was redirected to, else `xemu.log`; reports go out in one write (`c25a93771b`) | 3.1 |
| A16, A17 | headless; `ui/xemu-ram-digest.c` | `SDL_WINDOW_HIDDEN`, no host audio; the digest uses a thread pool plus qcrypto (Apple: GCD, CommonCrypto) | 3.2, 3.3 |
| A18 | `hw/xbox/xbox.c` | `XEMU_RAM_SHM` is a named file mapping, no lock file; a held name keeps guest RAM private | 3.5 |
| A19 | icount block I/O | file-win32 coroutine I/O inline on the vCPU (`4e50b583d4`); normal mode keeps worker threads | 3.7 |
| A20 | `fpu_helper.c` | x86_64 hard FPU on host `long double` with the guest's PC and RC in the host x87 control word (`deb98ee93f`); hot ops inline (`aae214c27a`); the SSE host path on with `hard_fpu` (`57cfe4fb51`) | 3.7, 3.8 |
| A21 | `system/cpus.c`, `misc_helper.c` | BQL spin and KiIdleLoop halt paced on `get_clock()` (QueryPerformanceCounter; `cntvct_el0` on ARM64) | 4.5 |
| A22 | `util/qemu-timer.c` | the main loop sleeps on a high-resolution waitable timer (`e8e260186d`); vblank uses `SDL_DelayPrecise` | 3.13 |
| A23 | `accel/tcg/tb-maint.c` | `XEMU_SUBPAGE_FAST=1` is inert on x86_64 (the store prefilter is aarch64-only) | 3.12 |
| A24 | `ui/xemu-disc-fingerprint.c` | no inode change time, so snapshots without a fingerprint always ask | 3.14 |
| A25 | `util/qemu-sockets.c`, `io/channel-socket.c`, `migration/savevm.c` | the unix-listener guard is POSIX-only; Windows keeps upstream's behaviour | 3.15 |
| A26 | `ui/xui/main-menu.cc` | the hard-FPU toggle shows on x86_64 and aarch64; the DSP JIT (ARM64) toggle is hidden (A10) | 2.1 |
| A27 | `gdbstub/xemu-gdb.c` | portable; first Windows run 2026-10-18 | 3.6 |
| A28 | `ui/xui/xbox-drive-explorer.cc`, `xbox-drive.cc`, `ui/thirdparty/libfatx` (2026-11-24) | the merged fork's only `_WIN32` branches: host files open with `_wfopen` on UTF-8 paths widened through `fs::u8path`, seek with `_fseeki64`/`_ftelli64`; `localtime_s`; libfatx formats with a fixed volume id instead of `gettimeofday` | 2.1 |

## B. Shared renderer and audio changes (active on Windows)

| # | Change | Verdict |
|---|---|---|
| B1 | in-pass occlusion queries, bulk reset at CB begin, a per-CB primer query, 4096-query pool, `REPORTS_FULL` and `QUERIES_FULL` guards (top of `begin_pre_draw` since 2026-10-10) | equivalence-argued (core-spec-valid); needs-real-HW for report values (D1, D2) |
| B2 | deferred zpass report delivery, 300 µs FIFO-idle budget, non-blocking `REPORTS_SUBMIT` | platform-neutral; needs-real-HW for guest-poll timing under WHPX (D1) |
| B3 | byte-exact vertex-conflict refinement (`vertex.c`) | equivalence-argued: additive over page tracking, unproven conflicts still finish (D4) |
| B4 | redundant-bind and dynamic-state skips (`draw.c`) | equivalence-argued; caches reset at CB begin |
| B5, B6 | PFIFO untimed idle wait; surface-expiry throttle on `QEMU_CLOCK_REALTIME` | platform-neutral (the kick sites' lock discipline; a monotonic clock everywhere) |
| B7 | `vk_wait_for_fence_or_die`: warns at 5 s, aborts at 60 s | deliberate; legitimate waits are about 8 ms (D5) |
| B8 | deferred zeta-shape eviction, quarantine reuse | reuse equivalence-proven (comment at `get_any_compatible_invalid_surface`). §C, the first audit's find: `prune_invalid_surfaces` destroyed images a just-submitted CB could still use, on every platform; `a045dfc780` stamps evictions with a submission index, gates destruction on the retirement watermark and drains all slots in `surface_flush` (D3) |
| F1 | texture and sampler LRU eviction gated on retirement (`submit_time >= retired_submit_count`) | fixes a use-after-free on every driver; needs-real-HW soak (I.9) |
| F2, F3 | x87 FPCR-cache invalidation; DSP JIT chain-unpatch icache flush | F2 historical (inline x87 and `cached_fpuc_rc` removed 2026-10-15); F3 POSIX aarch64 only (I.11) |
| B9, B10, F4, F5 | nsprof and `XEMU_INV_PROF` counters (getenv-gated, default off); APU reset-hold gate `d6da581d4b`, engine-aware mixbuffer copy; push-model present (Apple only, `HAVE_IOSURFACE_SHARING`) | platform-neutral, or no effect on Windows |
| N1 | 2026-11-24: 16-bit surfaces hosted as RGBA8 where the device lacks the native format (Windows GPUs normally have it), fractional `display.quality.surface_scale` (below 1), GPU NV09F blits (`perf.optimizations`), `display.filtering` fsr/bicubic/lanczos (GL helpers), the PVIDEO overlay fix (`632b7fd705`); the HDD explorer's Windows file I/O is A28 | shared paths, not yet audited here or run on Windows. `perf.async_pipeline_compile` is MoltenVK and KosmicKrisp only |

## K-R. 2026-08-09: the v0.13.1 black screen

An AMD Ryzen 5 5500 user saw v0.13.1 boot to black with audio while
stock upstream rendered. Diffs of the non-Apple Vulkan path found these
MoltenVK-tolerated defects, fixed in v0.13.2:

| § | Finding | Fix |
|---|---|---|
| Q | the cause: `pgraph_vk_update_descriptor_sets` wrote the vertex and fragment UBOs (bindings 0 and 1, different `stageFlags`) as one `descriptorCount=2` write (VUID-VkWriteDescriptorSet-descriptorCount-10776, -dstArrayElement-00321); native drivers left `PshUniforms` unset, so every fragment was black | two writes, confirmed by a standalone validation-layer repro |
| Q | the display descriptor skip-cache keyed on a raw `SurfaceBinding*` (use-after-free on address reuse) | Apple-only |
| R | dynamic state reused across CBs (VUID-vkCmdDraw-None-07833/34/35); staging `bufferOffset` unaligned (-bufferOffset-00193); the GS wrote `gl_PointSize` without the feature; sub-page tracking default-on off Apple | poisoned at CB begin; rounded up to 16; dropped without the feature; Apple-only default |
| L, M | a Parallels VM's Vulkan device lacked `shaderTessellationAndGeometryPointSize` (`Error: Device does not support required feature <name>`; init fails and xemu falls back to OpenGL, A5); OpenGL crashed after PFIFO start there (0xc00000ff) | the feature made optional; the GL crash did not reproduce on NVIDIA (§T) |
| R | suspected latent, unfixed (desktop GPUs don't hit them): unclamped compute workgroup size 256, the 256 MiB `BUFFER_COMPUTE` cap, the surface-upload compute-descriptor guard | |

## S-T. 2026-10-18 run: needs-real-HW status

§S: Windows 10 Pro 22H2, i7-4770K (Haswell 4C/8T, no SHA-NI), 32 GB,
2x TITAN Xp (GPU0 drives both monitors), NVIDIA 561.09; v0.15.1
(`e68e784be6fdbd7f`) and its -dbg, v0.14, upstream v0.8.137, CI and
native GCC 16 builds. Fixes landed on `windows`
(`ad7188d177..18638975a3`), merged as `8c5f3851bc` (v0.15.2).
"Settled" means on this one CPU and NVIDIA Pascal driver.

| Item | Status | Evidence |
|---|---|---|
| D1 occlusion report values (B1, B2) | partly | anchors render (2.3), soak clean (3.11); values not compared; B2 under TCG only |
| D2 primer cost, validation (B1) | partly | 0 VUIDs after `699bbd0d38` (3.10a); `QUERIES_FULL` guard ran (about 18 trips/flip at 64, 3.10b); primer cost not measured |
| D3 zeta quarantine soak (B8) | settled (NVIDIA) | 3.11, 3.10c, 0 VUIDs |
| D4 vertex refinement (B3) | partly | 3.10c and 3.11 clean under TCG; 3.10d frames await the Mac |
| D5 fence margin under load (B7) | partly | no `vkWaitForFences` warning; fence_wait 0.1-0.6 ms/flip at v0.15.1, 1-5 ms at 30-40 fps; slow or GPU-bound systems open |
| D6 Windows baselines | settled (this box) | §V |
| D7 Windows PGO | settled natively (x86_64) | GCC `.gcda` flow (7.4); not in CI (its cross GCC can't read native profiles). arm64 uses llvm-mingw: the profile format fits, but there is no committed Windows profile and no win64-cross PGO plumbing |
| D8, I.11 DSP JIT on non-Apple aarch64 (F3) | open | there it uses a plain RWX mmap and falls back to the interpreter when refused (Apple: `MAP_JIT`). CI's ubuntu-22.04-arm leg runs the corpus JIT arms; the chain-unpatch path still needs an in-game `XEMU_DSP_JIT_DIFF=1` + `_STATS=1` run (`checked>0, failures=0`) |
| I.9 texture retirement gate (F1) | settled (NVIDIA) | 3.11, 0 VUIDs |
| I.10 FPCR cache on Windows ARM64 | historical (F2) | 3.9 still needs ARM64 hardware |
| B5, B6; lockless MMIO and the DMA_PUT doorbell | settled (Windows x86_64, NVIDIA) | 3.11, 30 of 30 cycles with heartbeat `iters` rising to the end; walk-ins (3.10c); Linux open |
| `XEMU_SSE_HOST=2` on x86_64 | settled | 3.8 clean, its hashes equal to the v0.15.1 (softfloat) default; the host path is now the x86_64 default (`57cfe4fb51`) |
| §K/§L Vulkan default, strict features; §N/§Q Win32 external-memory present; §M OpenGL | settled (NVIDIA) | Vulkan picks GPU0 (2.3); "External memory sharing: enabled", DXGI flip mode with tearing; OpenGL renders heavy-f1, exit 0 (2.4); AMD unchecked |
| Windows-only paths since v0.14 | settled | 3.1-3.7, 3.12 (0 violations), 3.15; 0 of 100 pipe writer failures |
| 2026-09 startup crash | settled | SDL's race (§U), not `-O3`: 7.3 gave 6 of 6 clean at `-O2` and `-O3`; x86_64 builds default to `-O3` (`6a58566a54`) |
| LLVM DSP JIT, the N1 changes, A28 (2026-11-24) | open | never run on Windows |

## U. Defects the 2026-10-18 run fixed

| Defect | Fix |
|---|---|
| P0: startup crash 7-9 s in, 3 of about 25 launches, either renderer: SDL 3.4.18's `WIN_CreateWindow` pumps messages before setting `internal`, so `WIN_OpenClipboard` went through NULL | `058431f83b` (wrap patch), `d1b99ddb42` (static SDL natively) |
| P1: every `XEMU_HEADLESS=null` quit crashed (DXGI cleanup through an unresolved epoxy pointer) | `e40df3fae7` |
| x86_64 hard FPU: `frndint`, `fist`, `fistp` rounded to nearest under any RC (§R's fallback never ran under `hard_fpu`); every Windows det hash moved | `cfcc8e68ab` |
| the vCPU thread's x87 control word was never set (Windows starts threads at `0x27f`, 53-bit) | `deb98ee93f` |
| the main loop busy-waited sub-millisecond timer deadlines (95% of a CPU; heavy-f1 once 3.9 fps under load); minimized, flip-model Present returned at once and the UI loop spun | `e8e260186d`; `9778d93ad5` |
| `xemu.log` fully buffered; a redirected stderr replaced by `xemu.log` | `a150d08571`, `7d5a2e6cc6`, `06d375729f` |
| `VUID-VkDeviceCreateInfo-enabledLayerCount-12384` (validation layers passed as device layers) | `699bbd0d38` |
| `XEMU_MAX_QUERIES` also shrank the report pool; `XEMU_TB_REVAL_REFUTE` armed only on incremental restores | `54671ac359`; `2568477fd8` |
| `OS_Version` read with WCHAR sizes; reports written a character at a time; the SPIR-V cache ignored Settings > Cache shaders to disk | `47b506ad18`; `c25a93771b`; `a922602c75` |

## V. Baselines and goldens (2026-10-18)

| Measure | Value |
|---|---|
| v0.15.1, windowed nsprof flips/s | heavy-f1 8.3-10.1, W2 7.9-8.1, stream-e2 23.9-25.3, F5 19.9-21.1; fence_wait 0.1-0.56 ms/flip; CPU-bound on the vCPU (about 47% in x87 helpers) |
| headline | heavy-f1 11.1 to 38.7 fps (3.5×), W2 8.5 to 40.7 (4.8×): v0.15.1 against the final CI build `18638975a3`, 3 interleaved 60 s pairs, 3/3, load 1-3%; per-commit A/Bs in the history record |
| other windowed | A/A floor +0.06%; v0.15.1 heavy-f1 10.7 fps on a quiet box (4.3); F5 14.4 fps against v0.14's 12.4 (+15.9%, 2/2); on v0.15.1 `XEMU_BQL_SPIN` -2.0%, `XEMU_KI_IDLE_HALT` +6.1%, `XEMU_PFIFO_STALL_DOORBELL` -1.3% (1-2 pairs each), no default changed |

Windows det is deterministic in every config (r1 = r2; dsp56300 JIT =
interpreter) and matches no macOS hash (x87 long double vs double, SSE2
vs NEON kernels). The axes of a Windows-vs-macOS mismatch: the DSP
engine (dsp56300 JIT against the fork's DSP JIT; `audio.use_dsp_jit =
false` is closest), x87 `long double` against `double` plus the ARM and
x86 NaN rules, the SSE host path, GCC x86-64-v3 FMA contraction with
SSE2 kernels against clang arm64 with NEON, and the cold boot's block
I/O. `perf.hard_fpu = false` removes the x87 axis, but the macOS goldens
use `hard_fpu = true`, so that config needs a fresh macOS reference.
Windows ARM64 (`double` helpers, NEON SSE, no AArch64 DSP JIT) is the
host closest to macOS.

Among Windows builds, GCC's FMA contraction moves heavy-f1 and
stream-e2; the debug build's hashes are reproduced by a macOS x86_64
clang `-O3` build under Rosetta (x87 refuter 0 violations), so the
split is GCC-optimizer-specific. Cold boot is equal in every build. As
of the merge:

| Build | heavy-f1 f300 | f1200 | stream-e2 f600 | cold f900 | cold f1800 |
|---|---|---|---|---|---|
| GCC x86-64-v3 release (since `cfcc8e68ab`) | `37fe3d839c4be757` | `ec2e9596cc9dfa9c` | `38b50b4c6f35ac02` | `d6381be67430024d` | `1daeb1149976062c` |
| `-ffp-contract=off` or x86-64-v1 | `0d82336084feb76d` | `599aceace7f41529` | `3808ce768eeb4123` | same | same |
| debug; Rosetta x86_64 clang | `654eee348901d236` | `6938665a5124a399` | `48aeac8dabb00a8e` | same | same |

## W. Still open

| Area | Open item |
|---|---|
| hardware | AMD and Intel GPUs (D1-D5, I.9, §Q), Windows ARM64 (3.9, I.10), Linux (its lockless soak, D8, I.11), everything merged after v0.15.2 |
| x86_64 JIT | landed after the run, Windows numbers owed (`det_golden.py --pmc`, 6 pairs per commit): shared softmmu TLB walks `1f973eea41`, deferred x87 stack and FDP `3e2db13946`, guest-page x87 operands `f64b5a28cc`. Still backend ports: pinned guest registers (aarch64 -3.9% cycles; x86_64 has half the registers), inline packed-single SSE (exact MXCSR flags need a design), the double-based x87 write-through path |
| x87 precision | the `__hard` internals of `f2xm1`, `fyl2x`, `fyl2xp1`, `fpatan` run at the guest's precision control on x86_64; the fix (a `0x37f` bracket, as `XEMU_LIBM` does) moves the Windows goldens, so it waits for a Windows session to re-baseline. Intel macOS runs this path, checked only under Rosetta |
| needs a person | 2.1 audio, pad, fullscreen, UI scale, thumbnails; 3.14 menu loads; 3.15 live pads; 4.7 |
| findings | `xemu-ram-digest` sha256 655 ms for 128 MiB without SHA-NI (xxh3-128 29-41 ms); the disc fingerprint takes 7.3 s at the first save after a disc insert; an empty tray on the play HDD shows "Please insert an Xbox disc..." instead of a dashboard; pause Windows Update before an unattended run |

## Re-verification

```sh
grep -n "ifdef _WIN32" tcg/aarch64/tcg-target-has.h                      # A11
grep -n "CreateNamedPipeA" ui/xemu-input.c                               # A12
grep -n "DSP56K_JIT_SUPPORTED\|DSP56K_JIT_LLVM" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h   # A10
grep -n "force_physical_device" hw/xbox/nv2a/pgraph/vk/instance.c        # A3
grep -n "get_default_renderer()\|cannot initialize any renderer" hw/xbox/nv2a/pgraph/pgraph.c   # A5
grep -n "_WIN32" ui/xui/xbox-drive*.cc ui/thirdparty/libfatx/*.c         # A28
grep -rn "THREAD_PRIORITY_ABOVE_NORMAL" accel/tcg hw/xbox                # A6
grep -n "evict_submit_seq\|retired_submit_count" hw/xbox/nv2a/pgraph/vk/*.c hw/xbox/nv2a/pgraph/vk/renderer.h   # B8, F1
grep -c "__APPLE__\|IOSurface\|Metal" hw/xbox/nv2a/pgraph/vk/texture.c   # F1: 0
git diff upstream/master...HEAD -- util/cutils.c util/miniz include/qemu/osdep.h | grep -c "__APPLE__"   # A14: 0
```
