# Native Windows port, 2026-09 to 2026-10-18

> **Finished 2026-10-18.** The second hardware session's branch
> `windows` was merged into master as `8c5f3851bc` (shipped in v0.15.2)
> and frozen as the comparison point. Open Windows debt:
> [../windows-gating-audit.md](../windows-gating-audit.md) §T and §W;
> the runbook: [../windows-test-plan.md](../windows-test-plan.md). This
> record folds the port's status doc and the 2026-09 audit summary. The
> raw audit (findings with evidence, verifier notes and the 22-batch
> plan) and its ranked summary left the tree with them:
> `git show 45c5c70bbb:docs/Campaigns/windows-audit-2026-09.json` and
> `git show 45c5c70bbb:docs/Campaigns/windows-audit-2026-09-summary.md`.
> They record 2026-09-07 and are stale in these places:
>
> - Finding 37 (docs claim `use_dsp_jit` defaults on) is void (§4).
> - Batch 10 item (1)'s "do not add a second call in gl/renderer.c" is
>   wrong: one renderer ops table is installed at a time, so the GL and
>   Vulkan flip hooks never both run; `eb971f860d` added the GL hook.
> - Batch 3's dead-assert step is covered by `nv2a_vk_assert` in debug
>   builds since `45d4a2d656`.
> - Batch 12's `XEMU_HARDENING` hoist (and plausible finding 6) is void:
>   it landed in `5dfde3356d`, then the knob left `build.sh`, and since
>   `baf5e0c100` no host builds with `-fzero-call-used-regs`.
> - `XEMU_ZETA_SHAPE_READBACK`, `XEMU_PFIFO_KICK_STATS` and
>   `XEMU_SURFACE_CB_STATS` are gone. Finding 44 reads the
>   `XEMU_PFIFO_HEARTBEAT` `iters` rate; batch 4 reads nsprof's
>   `finish_*` counts; batch 4 item (2) needs `skip_readback` forced
>   false (§6).
> - The exit-139 crash was SDL's, not the flags (§7).

## 1. Sessions

| Date | Where | What |
|---|---|---|
| 2026-09-07 | Windows 10 Pro 22H2, i7-4770K (Haswell 4C/8T, AVX2), NVIDIA TITAN Xp (561.09, Vulkan 1.3.280), 32 GB; MSYS2 MINGW64 with GCC 16.2, meson 1.9.0, python 3.14.7, cmake 4.4.3 | first native build and run, the upstream merge `2e0aad3e18`, the static audit, wave 1 of fixes on `windows-wave1-wip` |
| 2026-09-08 | the Mac | the wave-1 review ([windows-wave1-review-2026-09-08.md](windows-wave1-review-2026-09-08.md)): arm64 build clean, `meson test --suite xbox` 6/6, CI run 34178988918 21/21 green, the F5 smoke clean; the `use_dsp_jit` default restored |
| 2026-10-17/18 | the same PC | the test plan's first run, from branch `windows`; results in the gating audit §S-§W |

## 2. Native build traps

The MSYS2 recipe and package list are in the test plan's 7.1 (the
2026-09 list also had `mingw-w64-x86_64-libsamplerate`). A cold build
took about 10 min. Traps a fresh MSYS2 hit:

| Trap | Cause | Fix |
|---|---|---|
| `bash: ./build.sh: \r: command not found` | a `core.autocrlf=true` checkout | `git config core.autocrlf false && git rm --cached -r . && git reset --hard` |
| `prefix value '/qemu' must be an absolute path` | Python 3.13's `ntpath.isabs` against QEMU's `configs/meson/windows.txt` | `build.sh` passes `--prefix=$(cygpath -m /qemu)` (`ac28e5a5bd`) |
| meson "Unhandled python exception" in the CMake probe | `Path.home()` needs `USERPROFILE`, missing when another tool spawns the login shell | export `USERPROFILE`, `HOMEDRIVE`, `HOMEPATH` before `source /etc/profile` |
| dsp56300 `tar: Cannot connect to C:` | MSYS tar reads `C:/x` as host:path | `--force-local` on Windows (`ac28e5a5bd`) |
| the Vulkan SDK winget install cancelled | the installer needs UAC | MSYS2's validation layer with `VK_LAYER_PATH=C:\msys64\mingw64\bin` |

## 3. First results (2026-09-07, Azurik, Vulkan)

- The fork's Vulkan renderer's first native-driver run rendered
  correctly; the 2026-08-09 fixes (gating audit §K-§R) held.
- `XEMU_NV2A_NSPROF=1`: intro and menu about 30 flips/s, the title area
  21-25, below its 30 fps cap, so that CPU had headroom. The first
  interval was pipeline generation (16 ms/flip, max 72 ms).
- Audio picked `dsound`.
- Upstream's DXGI presenter never activated on NVIDIA: interop FBO
  completeness was checked while the WGL_NV_DX_interop object was
  unlocked (`GL_FRAMEBUFFER_UNSUPPORTED`). `2182baec46` locks it around
  the check, 1:1 per frame: "DXGI Presentation Helper successfully
  initialized (mode=flip, tearing=supported)".

## 4. The upstream merge and the DSP default

`2e0aad3e18` merged upstream `429c9972eb` (12 commits: the
DXGI/WGL_NV_DX_interop presenter, PTIMER alarm IRQs with nv2a vmstate
v4, controllerdb from the config path, glslang 16.5.0, and upstream
`fc13b78060` flipping `audio.use_dsp_jit` to false). With
`audio.dsp_jit.enabled` then defaulting to false too, that put every
host, macOS included, on the C interpreter. On 2026-09-08 the fork restored
`use_dsp_jit: default: true`: a stock config runs upstream's dsp56300
engine, and `[audio.dsp_jit] enabled = true` picks the fork's ARM64 JIT.
Audit finding 37 ("docs still claim `use_dsp_jit` defaults on") is
void: don't "fix" the docs back to false. The July F5 savestate still
loads across the vmstate 3-to-4 bump (`minimum_version_id` stayed 1).

## 5. The 2026-09 static audit

13 finders, a two-lens adversarial verification and a planner: 114 raw
findings, 89 after dedupe, 60 confirmed, 19 plausible, 10 refuted. The
confirmed classes:

- Platform-neutral renderer defects hidden by MoltenVK: the
  `REPORTS_FULL` guard running after `begin_pre_draw` built slot state
  (`framebuffers[-1]`), a `create_pipeline` early return leaving the
  clear pipeline bound, surface uploads staging at offset 0 across
  flight slots, dynamic-state caches not invalidated by static
  pipelines, `pgraph_vk_finish` called inside the display aux CB, the
  PVIDEO cache keyed on registers only.
- x86_64 host FP: the 2026-08-09 FIST/FRNDINT directed-rounding fallback
  called the hard helper and nothing programmed the host x87 control
  word, so it was a no-op.
- APU: a guest voice handle indexed `filters[]` out of bounds (`vp.c`).
- Windows perf: no i386 sub-page store prefilter, FIST through x87
  round-trips, the host-SSE path dark, a `-O2` release, no thread
  priorities, the DSP interpreted, `volkLoadDevice` never called.
- Harness: the whole bench protocol was macOS-only (zsh, AF_UNIX,
  `screencapture`, APFS clones, `dist/xemu.app`).

Wave 1 (one commit per batch on `windows-wave1-wip`) landed on master
except the PVIDEO fix `285779ce83`; the branch survives as tag
`archive/windows-wave1-wip`, and none of its knobs has a `getenv` on
master.

## 6. Batch status

| # | Batch | Status (2026-10-18; batch 2 updated 2026-11-24) |
|---|---|---|
| 1 | DXGI presenter, non-Apple compile warnings | landed (`2182baec46`, wave 1) |
| 2 | display and PVIDEO: the finish inside the display aux CB, the register-only overlay cache, the `draw_time` early-out, `create_pvideo_image` sizes, the YUV compute descriptor guard | `285779ce83` never landed. willemabc's `632b7fd705` (2026-11-24) re-uploads the overlay every frame while on, keeps the redraw while it's on and flushes a full compute descriptor pool before converting; the aux-CB finish was not rechecked |
| 3 | `draw.c` pipeline, dynamic state, query capacity | mostly landed: the guard move `a825c1ff07` (reachable since `54671ac359`), the clear pipeline `a3f158b495`, one dynamic-state list `848c45ba58`. Open: strip/fan primitive restart |
| 4 | surface and texture upload | flight-slot staging `1182827158`. Open: the swizzled Z24S8 zeta upload (to exercise it, force `skip_readback` false in `update_surface_part`, `vk/surface.c`), the compute-set reservation loop |
| 5 | Vulkan hygiene | wave 1: bounds checks, `volkLoadDevice`, the LRU filter. Open: `prune_invalid_surfaces` order, `texture.c` barrier ranges, the `draw.c` query-reset range |
| 6 | shader key and uniform dirty tracking | open: `use_hw_depth = !need_geometry_shader` in `vk/shaders.c`; `SET_MATERIAL_ALPHA`, `SET_SPECULAR_PARAMS`, `SET_POINT_PARAMS` and the four light setters don't mark uniforms dirty |
| 7 | SPIR-V cache: UTF-8 paths, atomic writes, validation, key | landed (wave 1) |
| 8 | x87 and FP on x86_64 | landed: `deb98ee93f`, `cfcc8e68ab`; the longjmp item is moot (`cached_fpuc_rc` deleted in `238e5d8114`) |
| 9 | APU and host-array bounds | landed (wave 1) without the pitch clamp |
| 10 | diagnostics on Windows | landed (wave 1; nsprof under GL `eb971f860d`) |
| 11 | the Windows bench harness, named-pipe input | landed (`add066354f`, `164fd4c876`) |
| 12 | build and CI | GCC PGO wiring landed (first trained 2026-10-18); no native MSYS2 CI job |
| 13-16 | perf: `-O3` parity, FIST via CVTSD2SI, host SSE, the i386 sub-page prefilter | 13 landed (`6a58566a54`); 14 superseded by the inline host x87 ops (`aae214c27a`, `67e48b43b2`); 15 landed (`57cfe4fb51`); 16 open |
| 17 | Windows scheduling, UI frame cap | priorities `f96f00ba9d`, the waitable timer `e8e260186d`, the minimized sleep `9778d93ad5`; vblank stays normal. Open: a GL/DXGI UI frame cap, now new code (the Metal `XEMU_UI_FRAME_CAP_NS` knob `78c4573dfb` was removed in v0.16.1; read it at `858e2af19f:ui/xemu.c`) |
| 18-20 | renderer micro-costs, 1 GiB of staging, a GL texture thread pool, an async VK-to-GL handoff | the unused `VK_KHR_external_semaphore_win32` requirement dropped (`6e2accff54`); the rest open (measure `aux_fence` per present first; kill below 0.3 ms) |
| 21 | the DSP engine on Windows | open: det hashes match across engines, no perf A/B |
| 22 | docs reconciliation | partial |

## 7. The 2026-09 startup crash

Native wave-1 builds died 6-10 s after launch (exit 139) in about 5 runs
of 6, in both renderers, with or without the profiler. Single-run
bisects (`58b385c9aa`, `3495cd28c0`, and the `vp.c` pitch clamp,
retracted in `40f0a01ad3`) were noise at that rate. The 2026-09-08
review blamed the native `-O2` to `-O3 -Dstack_protector=disabled` flip
(`fb42bce22e`), the one change in every crashing binary, and wrote the
A/B protocol (test plan 7.3). On 2026-10-18 the -dbg dumps showed the
real cause: SDL 3.4.18's `WIN_CreateWindow` pumps messages before it
sets the window's `internal`, so an activation sent `WIN_OpenClipboard`
through NULL (the GL renderer's hidden gloffscreen windows exist in
every renderer mode; upstream v0.8.137 crashed 0 of 5). `058431f83b`
and `d1b99ddb42` fixed it; 7.3 then ran 6 of 6 clean at `-O2` and `-O3`
natively and on the CI `-O3` build, and x86_64 builds default to `-O3`
(`6a58566a54`; `XEMU_WIN_O3` removed). Lesson: at a 5-in-6 failure
rate, re-run every variant five or more times.

## 8. 2026-10-18 optimization A/Bs

`det_golden.py`, cross-binary unless noted, interleaved pairs,
instructions and core cycles per frame from ETW counters
(`c783af41ca`), det heavy-f1 to frame 1200. The x87 rows took det
heavy-f1 from 38.8 to about 76 fps; a final native `-O2` build measured
88 on a quiet box and 91 with GCC PGO. Mechanisms:
`docs/optimizations.md` (CPU / JIT, x86_64 hosts). Windowed headline:
gating audit §V.

| Change | Delta | Signs | Verdict |
|---|---|---|---|
| `66a991d060` x87 hard helpers on the registers in place | core cycles -26.1%, insns -6.2%, det fps 38.8 to 52.8 | 4/4 | kept, hash identical |
| `acecea21c8` x87 helpers `TCG_CALL_NO_WG`, fused memory operands | cycles -1.9%, insns -0.37% | 4/4 | kept |
| `deb98ee93f` host x87 control word = guest PC/RC, no `pack` | insns -2.35%, cycles -2.2% | 4/4 | kept, goldens identical |
| `aae214c27a` hot x87 ops inline on the host x87 unit | cycles -23.5%, insns -15.3%, det fps 54.4 to 72.4 | 4/4 | kept, goldens identical |
| `873ab92ed4` TBs keyed by the x87 TOP | insns -8.3%, cycles -6.4%, det fps 69.2 to 74.1 | 4/4 | kept; refuter 0 of 5.0G |
| `bfdf7572d2` det-mode FP brackets compare and skip | insns -0.58%, cycles -2.0% median | 4/4, 3/4 | kept, det mode only |
| `67e48b43b2` `fist`/`fsqrt`/`frndint` inline | insns -0.25%, cycles in noise | 4/4, 2/4 | kept (exact) |
| `e8a32939e5` `fcmov` as a select | cycles -3.0%, det fps +3.3% | 4/4 | kept |
| `57cfe4fb51` SSE host path (same binary, `XEMU_SSE_HOST=1`) | insns -8.8%, cycles -3.4%, det fps 35.6 to 38.0 | 4/4 | kept, default; hash moves |
| its MXCSR flag mirror | insns +2.0%, cycles -0.3% median | 0/4, 2/4 | kept (exact flags) |
| `baf5e0c100` no `-fzero-call-used-regs` | insns -5.47%, cycles -0.9% median | 4/4, 2/4 | kept |
| `4e50b583d4` file-win32 coroutine I/O | cold boot cycles -6.1%; stream-e2 -1.2% | 3/3 | kept, hashes unchanged |
| `6d0e98c828` hot/cold TB layout on tcg/i386 | cycles -0.85% median | 3/4 | kept |
| native `-O2` against `-O3` | cycles -1.4%, insns -1.1% | 3/4, 4/4 | `-O3` kept (`6a58566a54`) |
| `6a58566a54` CI release, `-O2` against `-O3` without the stack protector | insns -2.31%, cycles +0.26% median | 4/4, 1/4 | kept; matches macOS and Linux; cycles neutral under LTO |
| native GCC 16 `-O2` against CI cross GCC with LTO | CI cycles -2.8%, insns -6.1% | 4/4 | LTO pays |
| GCC PGO, native (test plan 7.4) | cycles -3.08%, insns -3.93%, det fps 88.3 to 91.3 | 4/4 | works natively; not in CI |
| `f96f00ba9d` thread priority, windowed (`XEMU_VCPU_QOS=0` vs `1`) | heavy-f1 under a busy loop on 8 CPUs 6.8 to 9.3 fps (+36%); idle -0.8% | 3/3; mixed | kept |
| `e8e260186d` waitable-timer main loop, windowed | process 245.9% to 147.5% of one core; 16.9 to 29.3 fps with builds on CPUs 4-7 | 3/3 | kept |

Harness fixes: `07fb7a811d`, `a0aff4866d`, `f1595e0c5e`, `7a9589adcb`,
`39592145d7`, `c783af41ca`, `d3ea0ee8af`, `06ebdace13`.

## 9. Questions the campaign left

- Policy, open: keep `display.renderer = VULKAN` as the first-run
  default on Windows and Linux? It works on NVIDIA; upstream defaults to
  OPENGL (still in v0.8.138-23).
- Upstream candidates: the DXGI interop lock (`2182baec46`),
  `volkLoadDevice`, the `sync_staging_buffer` coherency flush, and the
  dsp56300 tar `--force-local` guard (which should test
  `build_machine`, not `host_machine`).
- A native MSYS2 CI job (about 30-45 min cold, non-blocking).
- PVIDEO titles: Midtown Madness 3 plays FMV through the overlay
  (willemabc); Azurik's `azurik-movie-e2` cutscene does not
  (`NV_PVIDEO_BUFFER`, guest physical `0xfd008700`, stayed 0).
- Two identical GPUs (this PC): the old name-only device preference
  could split the Vulkan device from the GL adapter. Since upstream
  v0.8.138-23 xemu matches the display's GPU; `force_physical_device`
  still matches by name.
- Size before building: `XEMU_INV_TIMING` for batch 16, `aux_fence` per
  present for batch 20.
