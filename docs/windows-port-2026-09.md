# Native Windows port — status and handoff (2026-09-07, revised 2026-09-08)

First session of this fork on real Windows hardware. Everything below was
done on a Windows 10 Pro 22H2 box: Intel i7-4770K (Haswell, 4C/8T, AVX2),
NVIDIA TITAN Xp (driver 561.09, Vulkan 1.3.280, two TITAN Xp devices
enumerated), 32 GB RAM, MSYS2 MINGW64 (gcc 16.2, meson 1.9.0 from
`python/wheels`, mingw python 3.14.7, cmake 4.4.3). macOS remains the
first-priority platform; every change here is Windows-gated or a
platform-neutral bug fix whose Apple path is argued unchanged in its
commit body. Written before any of it had been built on macOS; the
macOS build, unit suite, CI matrix and in-game smoke were all run on
2026-09-08 and are recorded in §8, and §2/§5/§9 carry that session's
corrections.

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
`audio.use_dsp_jit` default → **false** (upstream `fc13b78060`). Only
`ui/xemu.c` conflicted; the DXGI hooks live inside the fork's non-Metal
GL branch. Mediation is in the merge body.

**Correction (2026-09-08).** This doc, the merge body and the ledger all
said the `use_dsp_jit` flip only affected non-Apple hosts. That was
wrong: `audio.dsp_jit.enabled` also defaults `false`, so with
`use_dsp_jit` off *no* platform's stock config selected a JIT engine —
macOS included — and every host quietly moved to the plain C
interpreter. The fork owns its defaults, so `config_spec.yml` is back to
`use_dsp_jit: default: true`; a stock config runs upstream's dsp56300
engine everywhere and `[audio.dsp_jit] enabled = true` selects the
fork's inline ARM64 JIT instead. Precedence in
`dsp_want_external_jit_engine()` (`hw/xbox/mcpx/apu/dsp/dsp.c:120`) is
unchanged. See `docs/windows-wave1-review-2026-09-08.md` §1 item 1 and
§5 decision (a).

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

On `macos-optimizations`: the upstream merge `2e0aad3e18`, the MSYS2
configure fixes `ac28e5a5bd`, the DXGI presenter lock fix `2182baec46`,
these docs, and — as of `add066354f` — the Windows benchmark harness
(`scripts/bench-savestate-ab-win.py`, `scripts/win/quiet_check.py`,
`scripts/win/capture_screen.ps1`, `scripts/win/score_shot_win.py`,
`scripts/win/inject_input.py`), which
is pure tooling compiled into nothing. All of it was validated on the
Windows box on 2026-09-07 and on the Mac on 2026-09-08 (§8).

On branch **`windows-wave1-wip`** (NOT merged): wave 1 of the audit plan
as one commit per batch (the present-hatch + ui-warning batch rode along
inside the harness commit `ec31facd7d`) — SPIR-V cache
hardening, PVIDEO/display reorder, APU/XID bounds, vk hygiene
(real bounds checks, volkLoadDevice, LRU filter), Windows/GL
diagnostics, build.sh GCC PGO + `-O3` parity, and the Windows benchmark
harness + `XEMU_INPUT_PIPE` named-pipe transport. **STATUS (end of session, 19:03): the wave-1 head crashes on Windows
INTERMITTENTLY, and the crash is not yet isolated.** The two commits on
the branch that claim a bisect result (`58b385c9aa`, `3495cd28c0`) were
based on single runs; a later sweep of the very same binary (wave 1
minus the pitch clamp) failed 5 of 5 (Vulkan+DXGI, Vulkan without DXGI,
OpenGL renderer, and twice with `XEMU_NV2A_NSPROF` unset), while a
control run of the main-branch binary (`2182baec46` state) passed in
the same machine state. Facts that survive: the crash is exit 139 about
6–10 s after launch (intro start), in BOTH renderers, with or without
the profiler, with `XEMU_VK_VOLK_DEVICE=0`, and with
`XEMU_APU_RAM_DIRTY=0`. Single passes were observed once each for
"all apu/xid files reverted", "only vp.c reverted" and "vp.c minus the
pitch clamp", so the apu/xid commit (`971292ed48`) remains the leading
suspect, but every variant must be re-run ≥5 times before believing a
pass. The pitch clamp stays reverted (harmless either way). gdb did not
catch it (run-under and attach both missed).

**Superseded 2026-09-08 by `docs/windows-wave1-review-2026-09-08.md`
§3.** The apu/xid commit is *not* the leading suspect — the review
exonerated it on four independent grounds. Five of six crash hunters,
each starting from a different lens and none refuted, converged instead
on the **build flags**: `fb42bce22e` moves the native MSYS2 release arm
from `-O2` to `-O3 -Dstack_protector=disabled`, and that is the only
change reaching every translation unit in both renderers with every
hatch unset. Every crashing binary — including every single-run "bisect"
variant — came out of `rebuild-quick.sh`/ninja, which keeps the
configure-time flags; the one passing binary (the control) was
configured by main's `build.sh` at `-O2`. The runtime hatches that were
toggled change no codegen, and a timing/ASLR-dependent latent bug
explains both 1-pass-then-5-fails and gdb missing it. This is a review
verdict, **not yet confirmed on the box**, and it names a class (latent
UB exposed by the `-O3` vectoriser), not a line — if it holds, a real
bug still exists and must be hunted from a dump. The protocol that
settles it is §9 below. Do not resume single-run bisects.

The DXGI hatch, SPIR-V cache, PVIDEO/display,
bounds checks, volk dispatch, diagnostics and build changes all ran
clean in the surviving runs. Each batch
commit body carries its gating and intended validation; the escape-hatch
rows now live in `docs/optimizations.md` under "Escape hatches on branch
`windows-wave1-wip` (not on main)" — none of them has a `getenv` on
main, so they do nothing on a main build.

## 6. Remaining plan (batches not yet implemented)

From the planner (`docs/windows-audit-2026-09.json` → `plan.batches`),
in recommended order. Batches marked *savestate* need the Azurik in-game
savestate benchmark (not yet created on this box; the protocol is
`scripts/bench-savestate-ab-win.py`, and the Mac's fixtures are listed
in the `xemu-validation-and-qa` skill).

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
| 22 | docs reconciliation (architecture-contract Invariant 2 text vs the REPORTS_FULL finding, windows-gating-audit rows) | none — the `use_dsp_jit` default claims were corrected 2026-09-08 (§2) |

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
`scripts/bench-savestate-ab-win.py` + `scripts/win/` (cherry-picked from the
wave-1 branch onto main as pure tooling; parse-checked and quiet-check
run, but never driven through a full A/B yet) is the durable replacement.
Never run against `%APPDATA%\xemu\xemu\xemu.toml` — the fixture uses a
copied HDD image and `-config_path`.

## 8. State of main, and what was verified on the Mac (2026-09-08)

Main (`macos-optimizations`) moved `ea9633798d` → `add066354f`: the
upstream merge `2e0aad3e18` (which also carried upstream's
`use_dsp_jit` default flip, restored by the fork on 2026-09-08 — §2),
the MSYS2 configure fixes `ac28e5a5bd`, the DXGI presenter lock fix
`2182baec46`, the docs commits including the pitch-clamp retraction
`40f0a01ad3`, and the Windows benchmark harness `add066354f`.
**Wave 1 is NOT on main** — it is still only on `windows-wave1-wip`,
and none of its escape hatches has a `getenv` on main.

Verified on this Mac on 2026-09-08 (evidence table:
`docs/windows-wave1-review-2026-09-08.md` §1):

| Check | Result |
|---|---|
| `./build.sh` arm64 release at `add066354f`, glslang **16.5.0** (only after `meson subprojects update --reset glslang` — the local checkout was still 16.2.0, which `build.sh` now warns about) | clean — no Apple-side compile fallout |
| `meson test --suite xbox` | 6/6 |
| CI run 34178988918 | 21/21 legs green, both win64-cross included |
| In-game smoke on the F5 savestate `vm-20260704032357` (July, pre-vmstate-v4), 2 harness invocations = 6 runs | loads, renders, 0 magenta / white / stuck frames, 0 asserts, fork DSP JIT banner present |
| The wave-1 head, built and smoked on macOS the same way (2 invocations, 6 runs, isolated caches so the new SPIR-V writer ran cold and warm) | clean — 0 artifacts, 0 asserts |

So the Windows-first work does not break macOS, and the July savestate
still loads across the nv2a vmstate 3→4 bump (`minimum_version_id`
stayed 1). Two caveats worth carrying:

- The smoke ran under the owner's `xemu.toml`, which sets
  `[audio.dsp_jit] enabled = true`. The restored `use_dsp_jit` default
  puts a **stock** config on upstream's dsp56300 engine on every host;
  that path has not been re-smoked since the restoration.
- One run per binary showed a late-run fps collapse. Both coincided
  with other sessions saturating the host (load avg 18-25), neither
  recurred, and the same-binary A/B design makes them non-signals — but
  a quiet machine is still a precondition for any number from this
  fixture (`scripts/win/quiet_check.py` is the Windows equivalent).

Still owed before a tag, per the review: a cross-binary interleaved fps
A/B against `v0.13.2` on the heavy savestate (the merge added a new
guest interrupt source, so parity is not free), and release notes that
name the forward-incompatible vmstate v4, the DSP default, and the
glslang 16.5.0 SPIR-V cache-key roll.

## 9. Next Windows session: the exit-139 protocol

Replaces the wave-1 hatch-bisect recipe that used to sit here. That
recipe assumed the crash was a source hunk isolable by toggling
runtime knobs; the 2026-09-08 review concluded the differential is the
`-O2` → `-O3 -Dstack_protector=disabled` flip on the native MSYS2
release arm (`fb42bce22e`), which no runtime knob can reach. Full
reasoning and the eliminations:
`docs/windows-wave1-review-2026-09-08.md` §3. Run the steps in order,
and **pre-commit the interpretation before running anything**.

1. **Freeze the control.** The control is main built by main's
   `build.sh` (a full configure — never `rebuild-quick.sh`, which keeps
   the previous configure's flags). Copy that `dist/xemu.exe` plus its
   DLLs to `dist-control/` and record: `sha256sum`, `grep -c -- ' -O3 '
   build/compile_commands.json` and `grep -c fstack-protector
   build/compile_commands.json` for **both** trees, and `env | grep
   XEMU_` from the shell `run-test.sh` runs in (a stray value would
   re-open the named-pipe lens). Record `git rev-parse HEAD` next to the
   binary's sha256: main moves with every landing (the 2026-09-08 docs commit
   already moves it past `add066354f`), so "the control" is only meaningful with its commit
   written down, and it must be re-frozen after each wave-1 batch lands.
2. **Get a dump.** Enable WER LocalDumps (`DumpType=2`) or run
   `procdump -e -ma -w xemu.exe`, run the existing wave-1 binary until
   it faults, open the `.dmp` in gdb and read the faulting instruction
   and stack. A `vmovaps`/`vmovdqa` on an unaligned operand is the
   `-O3` vectoriser class and confirms the hypothesis directly.
3. **The experiment.** Full reconfigure on both arms:
   (A) wave-1 head with `XEMU_WIN_O3=0 ./build.sh -j8` (`-O2`, stack
   protector back), ≥6 runs; (B) main with `-Doptimization=3
   -Dstack_protector=disabled`, ≥6 runs. At the observed ~5/6 failure
   rate, 6 clean runs give P < 1e-4. Interpretation, committed now:
   **A clean and B crashing convicts the flags** — then keep `-O3`
   opt-in and hunt the latent bug from the dump, because a real bug
   still exists. **A still crashing exonerates the flags** — the
   culprit is a source hunk, and it is found by reverting whole
   batches with ≥5 runs each, never single runs.
4. **Contested side question while there:** does MSYS2 GCC 16.2
   actually apply `-fstack-protector-strong` by default? Settle it with
   `grep -c fstack-protector build/compile_commands.json` rather than
   argument.
5. **Cheap side variable.** The DSP engine is no longer one: main
   defaults to `use_dsp_jit = true` again, so the stock config runs the
   dsp56300 engine as it did pre-merge. To test the interpreter path
   deliberately, set `[audio] use_dsp_jit = false`.

Use `scripts/bench-savestate-ab-win.py` (on main since `add066354f`)
for anything that produces a number; `run-test.sh` is fine for
crash/no-crash runs. The remaining audit batches are §6, and the
landing order for wave 1 — one push each, CI green between — is
`docs/windows-wave1-review-2026-09-08.md` §4.
