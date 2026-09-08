# Review of the 2026-09-07 Windows-side work (main + windows-wave1-wip) — 2026-09-08

Scope: everything pushed from the Windows box over the weekend. Main
(`macos-optimizations`) moved `ea9633798d` → `add066354f` (upstream merge,
MSYS2 configure fixes, DXGI lock fix, docs, harness scripts). Branch
`windows-wave1-wip` (`67a5d56133`, merge-base `e707ccf335`) holds wave 1 of
the audit plan and crashes intermittently on Windows. This was a review and
a recommendation, not a benchmark: no fps number below is citable.

Method: 11 per-commit Fable reviewers → two-lens adversarial verification of
every non-trivial finding (38 survived, 1 contested) → six crash hunters with
distinct lenses → two-lens verification of their top hypotheses (5 survived,
0 refuted) → two Opus planners (risk-first, value-first). The critic and
refiner stages hit the session limit; this document is that synthesis. Per-agent
evidence: session workflow `wf_fabb80f4-432`. Alongside the static review, on
this Mac: main and the wave-1 head were both built (arm64 release), both pass
`meson test --suite xbox` 6/6, and both ran the in-game savestate harness.

## 1. State of main (`add066354f`) — safe to keep as head, NOT ready to tag

Evidence produced this session:

| Check | Result |
|---|---|
| `./build.sh` arm64 release, glslang **16.5.0** (after `meson subprojects update --reset glslang`; the local checkout was still 16.2.0) | clean |
| `meson test --suite xbox` | 6/6 |
| CI run 34178988918 | 21/21 legs green incl. both win64-cross |
| In-game smoke, F5 savestate `vm-20260704032357` (July, pre-vmstate-v4), 2 harness invocations = 6 runs | loads, renders, 0 magenta/white/stuck frames, 0 asserts, fork DSP JIT banner present |

Merge mediation `2e0aad3e18` is correct: `git merge-tree` vs the commit differ
only in the four `ui/xemu.c` conflict hunks, each resolved fork-side; the Metal
present path, push-present ring and `nv2a_get_present_frame` are untouched;
DXGI is confined to `#ifdef _WIN32` inside the non-Metal GL branch; no fork
change was dropped in the eight co-modified files; nv2a vmstate 3→4 keeps
`minimum_version_id 1` (old snapshots load — confirmed live); PTIMER alarms run
under the BQL and never touch `pfifo_kick`; CI workflow files only got action
bumps.

Verified defects on main:

1. **DSP engine default silently changed on every platform, including macOS
   (MUM-1, high).** `config_spec.yml:344-351`: fork `dsp_jit.enabled` defaults
   false and upstream's `use_dsp_jit` now defaults false, so a stock config runs
   the plain C interpreter where pre-merge it ran upstream's dsp56300 JIT. The
   merge body and three docs say "non-Apple hosts only" — false. The owner's own
   xemu.toml sets `[audio.dsp_jit] enabled = true`, so the dev box never shows
   it. Owner decision required (see §5).
2. **Docs on main describe code that is not on main (DOCS-1..6, high).**
   `docs/optimizations.md:24-31` lists eight escape hatches
   (`XEMU_WIN32_DXGI`, `XEMU_SPIRV_CACHE*`, `XEMU_PVIDEO_UPLOAD_ALWAYS`,
   `XEMU_DISPLAY_SKIP_STRICT`, `XEMU_APU_RAM_DIRTY`, `XEMU_VK_VOLK_DEVICE`,
   `XEMU_WIN_O3`) with no `getenv` on main; `README.md:48-50` claims native
   MSYS2 builds are `-O3` with GCC PGO (wave-1 only); `docs/optimizations.md:1088`
   still says `use_dsp_jit` default on; `docs/windows-port-2026-09.md` §9 tells
   the next Mac session wave 1 "was landed" and to bisect with hatches that do
   not exist on main.
3. **SPIR-V cache key reads Homebrew's glslang header, not the compiled
   subproject (new, verified).** `glsl.c` gets `-I/opt/homebrew/include` before
   `-I…/subprojects/glslang/__CMake_build/include`; the user's cache dir
   `spirv_cache_v0.13.2-glslang16.5.0` existed while the subproject was 16.2.0.
   Release (CI) builds have no Homebrew glslang so are unaffected; local builds
   can serve stale SPIR-V after a pin bump. Fix: order the subproject include
   first or key on a runtime version query.
4. **Local subproject drift.** imgui, SPIRV-Reflect, volk and
   VulkanMemoryAllocator still emit meson's "revision may be out of date"
   warning in this tree; local binaries ≠ CI binaries until
   `meson subprojects update --reset` is run for them too. build.sh should warn.
5. `hw/xbox/nv2a/ptimer.c:88` divides by `ptimer.denominator` /
   `core_clock_freq`, both zero at reset; a guest ALARM_0 write before
   NUMERATOR/DENOMINATOR now traps (SIGFPE, not the Windows 139). Low; guard it.
6. `subprojects/dsp56300/meson.build:32` keys tar `--force-local` on
   `host_machine`, so the Linux win64-cross legs also get it (harmless with GNU
   tar, wrong). fb42bce22e already re-keys on `build_machine`; land that hunk.
7. Windows harness `scripts/bench-savestate-ab-win.py`: `loadvm` monitor call
   at :538 is unguarded (a xemu that dies at 6-10 s — the exact failure being
   chased — kills the batch with a traceback instead of a DEAD run); renderer
   not pinned/recorded (:763); misleading `os.name` refusal (:907); `shlex`
   POSIX mode eats backslashes (:146). Fix before the first real Windows A/B.

## 2. Wave-1 branch, per commit

Both branches built and smoked clean on macOS (wave-1: 2 invocations, 6 runs,
0 artifacts, 0 asserts, isolated caches so the new SPIR-V writer was exercised
cold and warm). One run per binary showed a late-run fps collapse; both
coincided with two other sessions saturating the host (load avg 18-25) and did
not recur, and the same-binary A/B design makes them non-signals.

| Commit | Verdict | Why |
|---|---|---|
| `ec31facd7d` ui hunks (Win32 named pipe, `XEMU_WIN32_DXGI`, gl-helpers) | **land** after CI green | POSIX FIFO branch byte-identical; MVK setenv block untouched; scripts already on main (`git diff HEAD origin/windows-wave1-wip -- scripts/` is empty). Minor: re-arm pipe after `DisconnectNamedPipe`; knob row still says "FIFO path". |
| `818fe27828` SPIR-V cache hardening | **land after fix** | Strictly safer on every platform, but the "regenerate instead of aborting" claim is false: SPIRV-Reflect's bounds check is an `assert` compiled into every build, so a word-aligned truncated `.spv` still aborts every launch. Add a ~10-line instruction-header walk to `spirv_blob_is_valid`; clean orphaned `.tmp`; README Changes row. |
| `285779ce83` display.c PVIDEO/finish reorder | **hold** | The reorder is right (removes a live plain-assert abort reachable in release). But `XEMU_DISPLAY_SKIP_STRICT` widening applies on macOS too (commit + docs label it non-Apple); descriptor cache not invalidated on overlay resize (1-line fix); needs a title that actually enables PVIDEO — Azurik never does, so every existing smoke proves nothing here. Pre-existing, not wave-1: `surface.c:1939` writes staging at absolute offset 0 regardless of flight slot. |
| `971292ed48` + `58b385c9aa` + `3495cd28c0` apu/xid | **rework into three** | (a) xid.c/xid-gamepad.c index bounds: land. (b) vp.c bounds (voice_snapshot NULL path, unsigned FECV, list-walk stop): land, squashed so the retracted bisect never enters history; make the list-walk truncation a rate-limited `LOG_GUEST_ERROR`, not a compiled-out DPRINTF. (c) APU RAM dirty marking is **default-ON on Apple** (commit claims hatch-gated): two full barriers + up to four contended atomic ORs per voice-register store from every voice worker, unmeasured. Default it off on `__APPLE__` (mirror `0aeb1998c3`) or pre-check with `physical_memory_range_includes_clean`. BQL engine switch is dormant. Exonerated as the Windows crash cause on four independent grounds. |
| `736533709e` vk/tcg hygiene | **land after macOS soak** | Exonerated as crash cause. Ungated Apple change: `nv2a_vk_bounds_check` now `abort()`s at 28 release sites where it was `__builtin_unreachable`; six are table-index checks whose register field is wider than the table, so a title that previously read a neighbouring `.rodata` word now terminates. Soak ≥3 corpus titles to gameplay, or clamp-and-warn those six. lru filter: prefer `assert` over silent skip; delete the false "release strips lru_evict_one's assert" claim in texture.c and the archaeology skill. |
| `eb971f860d` diagnostics | **land as-is** | Apple release path unchanged (`NV2A_STRIP_PROFILE_COUNTERS` stays 1; hooks gated first). Two doc-line fixes (plan text vs the GL hook; `XEMU_INV_TIMING` no longer cntvct-only). |
| `fb42bce22e` build.sh | **split** | Keep: PGO compiler-family probe + GCC arm (fix: `cygpath -m` the pgo_dir or `.gcda` discovery never finds the files), `XEMU_HARDENING` on every arm, `build_machine` tar rekey, imgui.wrap comment. Darwin arm proven byte-identical. **Invert** the native-Windows `-O3 -Dstack_protector=disabled` default to opt-in (`XEMU_WIN_O3=1`): it is the crash confound (§3) and no CI leg covers it. Contested and to be settled on the box: does MSYS2 GCC 16.2 actually get `-fstack-protector-strong` by default (grep `compile_commands.json`)? |
| docs commits `c1804ab839 a2dc6464f1 2c00a0c0bd 67a5d56133` | **drop** | Retracted bisect narrative; main already carries the corrected text (branch differs by 4 lines). |

## 3. The Windows exit-139: verdict and the first experiment

Five of six hunters, each starting from a different lens, converged on the
same mechanism, and all five survived code and fact-fit refutation
(confidence 0.4-0.5 each):

**The `-O2 → -O3 + stack protector disabled` flip on the native MSYS2 arm
(`fb42bce22e`, GCC 16.2, x86-64-v3) is the differential, acting as an exposer
of latent UB, not any wave-1 source hunk.** It is the only change that
reaches every translation unit in both renderers with every hatch unset. Every
crashing binary — including every single-run "bisect" variant — was produced by
`rebuild-quick.sh`/ninja, which keeps the configure-time flags, while the only
passing binary (the control) was configured by main's build.sh at `-O2`. The
runtime hatches that were toggled (`XEMU_VK_VOLK_DEVICE`, `XEMU_APU_RAM_DIRTY`,
profiler, DXGI, renderer) do not change codegen, and a timing/ASLR-dependent
latent bug explains 1-pass-then-5-fails and gdb missing it. What it does not
do is name the faulting line; if confirmed, a real bug still exists in the tree
and must be hunted with a dump.

Eliminated with evidence: all Vulkan-only hunks (OpenGL crashes too), all
profiler-gated code, the named-pipe transport (inert when unset), the include
hoist, lru filter (no-op while the split-list invariant holds), RCU
registration (correct, once per thread), the BQL engine swap (dormant), every
vp.c bounds hunk (bit-identical for handles < 256), the restored pitch wrap
(always in bounds).

First Windows session, in order (pre-commit the interpretation before running):

1. Freeze the control: copy the 2182baec46-state `dist/xemu.exe` + DLLs to
   `dist-control/`, record sha256 and `grep -c -- ' -O3 ' build/compile_commands.json`
   / `grep -c fstack-protector …` for both trees. Also `env | grep XEMU_` from
   `run-test.sh`'s shell (a stray value would re-open the named-pipe lens).
2. Enable post-mortem dumps (`WER LocalDumps DumpType=2` or
   `procdump -e -ma -w xemu.exe`), run the existing wave-1 binary until it
   faults, open the `.dmp` in gdb: faulting instruction + stack. A
   `vmovaps/vmovdqa` on an unaligned operand = the `-O3` vectoriser class.
3. **The experiment.** Full reconfigure, not `rebuild-quick.sh`:
   (A) wave-1 head `XEMU_WIN_O3=0 ./build.sh -j8` (`-O2`, SSP probe), ≥6 runs;
   (B) main `add066354f` with `-Doptimization=3 -Dstack_protector=disabled`,
   ≥6 runs. At the observed ~5/6 failure rate, 6 clean runs give P<1e-4.
   A clean and B crashing convicts the flags (then find the latent bug from the
   dump; keep `-O3` opt-in). A still crashing exonerates the flags: the culprit
   is a source hunk — revert whole batches with ≥5 runs each, never single runs.
4. Cheap side-variable while there: `[audio] use_dsp_jit = true` (restores the
   pre-merge DSP engine; the C-interpreter path at audio start is new on
   x86_64 since the merge, though the control passes with it too).

## 4. Recommended order of work

Mac, next session (nothing here needs the Windows verdict):

1. Docs-correction commit on main (class 1): items 1-2 of §1, plus the
   config-and-flags skill's `use_dsp_jit default true` row, plus a rewritten
   §8/§9 in `docs/windows-port-2026-09.md` that describes main's real delta and
   carries the ≥5-runs rule and the frozen-control SHA.
2. Decide the DSP default (§5) and implement it in the same push.
3. Small main-side fixes: PTIMER divisor guard; dsp56300 `build_machine`
   rekey; glslang include order / cache-key source; a wrap-drift warning in
   build.sh; Windows harness W1/W3/W2/W6.
4. Land wave-1 batches incrementally, one push each, CI green between:
   `eb971f860d` (+doc fixes) → `ec31facd7d` ui hunks → xid half of
   `971292ed48` → `818fe27828` with the header-walk fix → `736533709e` after
   a ≥3-title macOS soak → vp.c half of `971292ed48` with dirty marking
   default-off on Apple. Note every landing moves the Windows control off
   `add066354f`; freeze it first (§3 step 1).
5. `285779ce83` waits for a PVIDEO-enabling title on macOS at default,
   `XEMU_DISPLAY_SKIP_STRICT=0`, `XEMU_PVIDEO_UPLOAD_ALWAYS=0`, plus an
   artifact-oracle soak over the FMV scene.
6. Re-cut `fb42bce22e` after the Windows verdict with `-O3` opt-in.

Do not tag until: the DSP default is decided and documented; a cross-binary
interleaved fps A/B vs v0.13.2 on the heavy savestate (the merge added a new
guest interrupt source); release notes carry the forward-incompatible vmstate
v4, the DSP default, and the glslang 16.5.0 cache-key roll.

## 5. Decisions for the owner

- **DSP default.** Options: (a) restore the fork's pre-merge
  `use_dsp_jit: default: true` in `config_spec.yml` (fork owns its defaults;
  no behaviour change for anyone; upstream turned it off for their own
  first-release caution); (b) fork-own an Apple default of
  `audio.dsp_jit.enabled = true` where `DSP56K_JIT_SUPPORTED` (needs a runtime
  default; best engine on Apple, unchanged elsewhere); (c) keep upstream's flip
  and document that stock configs run the C interpreter everywhere.
  Recommendation: (a) now, (b) as a follow-up with an APU_PROF receipt.
- **Is an `-O3` native Windows release a goal?** Upstream, the fork's
  win64-cross legs and every CI Windows leg are `-O2`. If not a goal,
  `XEMU_WIN_O3` stays an experiment knob permanently.
- **`736533709e`'s abort-on-bounds-failure as Apple behaviour** (upstream
  parity) vs clamp-and-warn at the six wider-than-table sites.
- Land wave 1 incrementally (recommended) or as one series once the crash is
  closed?
- Is there a corpus title with FMV/PVIDEO for `285779ce83`?

## 6. Environment notes for the next Mac session

- Homebrew Python was upgraded to 3.14.7 on 2026-08-19 and lost `pyyaml`/
  `requests`; `./build.sh` fails at `gen_config.py` until
  `/opt/homebrew/bin/python3 -m pip install --break-system-packages pyyaml requests`.
- After any wrap bump: `build/pyvenv/bin/meson subprojects update --reset <name>`
  or the local build is not what CI ships.
- Smoke/bench receipts from this session: `/tmp/xemu-smoke-main`,
  `/tmp/xemu-smoke-main165` (glslang 16.5 build, sha256 `c79d5fda…`),
  `/tmp/xemu-smoke-w1`, `/tmp/xemu-smoke-w1b`; build logs
  `/tmp/xemu-build-*.log`. The wave-1 worktree at `/tmp/xemu-w1` was removed.

## 7. Landing status (2026-09-08, later the same day)

Phase A (main-side fixes) landed as seven commits ending in `4d899b928d`
(PTIMER guard `3446eccb47`, SPIR-V cache runtime key `ca8cbbc281`,
wrap-drift warning + tar rekey `eeb992dcd2`, harness fixes `c77d8a4b05`,
corpus boot smoke `4d0cec2790`, `use_dsp_jit` default `dc8064d112`, docs
`4d899b928d`); CI 21/21 green. Wave 1 was re-cut one reviewed commit per
batch on top of it, each with its fold-ins from §2: diagnostics
`326bfd7865`, `ui/` hunks `164fd4c876`, XID bounds `f655d3b09f`, SPIR-V
cache hardening `43869f5a0b` (first push, this group), bounds checks /
volk / LRU `51d9dc98da`, APU half with dirty marking off on Apple
`7969bf1982`, build.sh re-cut with `-O3` opt-in `5dfde3356d`, docs
`959cbfc61e`. `285779ce83` (display/PVIDEO) stays on the branch. Every
landed commit: arm64 release build clean, `meson test --suite xbox` 6/6.
The forced-truncation test for the SPIR-V cache passed (a word-aligned
half-length `.spv` was rejected with the new "ignoring corrupt" line,
recompiled byte-identical, and a planted stray `.tmp` was swept).

**One unexplained boot stall, recorded so it is not lost.** On the
first-group build (`43869f5a0b`), run E1 of its savestate smoke: the
first nsprof interval was normal (6.5 s, 34 flips, boot animation), then
one 34.7 s interval with 7 flips — `flip_idle` max 34.3 s, i.e. the
guest issued no flip for the whole window, while the UI thread presented
4067 frames in it (~120 Hz) — and the owner heard the boot audio looping.
The harness's `loadvm` at t≈40 s ended the stall; the game then ran at
~41 fps / ~490 draws per flip, but the window stayed white (luma 248,
`ARTIFACT(WHITE_SCREEN)`), so the present chain did not re-attach after
the renderer reset during the stall. No assert, no log line. It did not
reproduce: 5 same-conditions boots on that build, then 6 alternating
boots against 6 on the phase-A build with `XEMU_PFIFO_HEARTBEAT=1`, all
clean, heartbeat alive throughout; tally 1/17 boots on this build, 0/33
on every other build today, another GPU game and two CPU-bound jobs
running on the host the whole time. The PTIMER-guard theory (the new
`timer_del` path dropping the alarm inside a transient unconfigured
window) was tested with instrumented boots and refuted: the kernel writes
NVPLL once with MDIV=1, programs NUMERATOR/DENOMINATOR before
`ALARM_0=0xffffffff`, and never re-enters the window. Classification:
rare, unattributed, not a landing blocker on the evidence. If it recurs:
run with `XEMU_PFIFO_HEARTBEAT=1 XEMU_NV2A_NSPROF=1` (expect
`waiting_flip=1` with `iters` advancing = guest-side stall), take
`sample <pid>` during the stall, and note whether the white present
persists after recovery — that second half is its own defect.

## 8. Next session: release candidate for v0.13.3

Main is at `bd3d56ee8d` (all of §7 landed, CI green on every leg, tree
clean, `windows-wave1-wip` untouched at `67a5d56133` with only
`285779ce83` left on it). The owner asked on 2026-09-08 whether to tag;
the answer was "after a short RC pass", because nothing measured today is
citable (the host ran another GPU game and two CPU-bound jobs throughout)
and nothing has run in the owner's real configuration. In order:

1. **Quiet-machine fps A/B, main vs a v0.13.2 build**, F8 heavy savestate
   (`vm-20260704173701`), interleaved, ≥3 pairs
   (`scripts/bench-savestate-ab.sh --app` for the cross-binary shape per
   the `xemu-testing` skill). Motivation: glslang 16.2→16.5 changed every
   shader's SPIR-V, and the bounds checks are now real branches at 28
   per-draw sites. Accept parity within the ±0.02 fps static noise; a
   regression is bisected with `XEMU_SPIRV_CACHE=0` / the 16.2 wrap pin /
   `-DNV2A_VK_PERF_BUILD` before anything else.
2. **Artifact-oracle soak** on the same run (≥300 captures scored, zero
   magenta/white).
3. **Owner session from Finder** in the real config (fullscreen exclusive,
   MetalFX, 2x interpolation), 20–30 min of play, watching for the §7 boot
   stall; if it recurs, capture with the §7 recipe before anything else.
4. **Release notes** (README + draft body): savestates saved on the new
   build do not load on v0.13.2 (nv2a vmstate v4); DSP engine default back
   to the fork's (`use_dsp_jit = true`); shader cache re-keys on the
   glslang version, first launch is cold; Windows: DXGI presenter now
   activates on NVIDIA, wave-1 hardening on by default, `-O3` opt-in via
   `XEMU_WIN_O3=1`, wave-1 hunks not yet run natively at the shipped `-O2`
   flags; the `285779ce83` display/PVIDEO fix is still branch-only.
5. Tag `v0.13.3` → `release-on-tag.yml` draft → owner publishes (never
   re-tag; see xemu-change-control).

Independently, on the Windows box (unchanged from §3): freeze the control
binary, enable WER LocalDumps, full-reconfigure `-O2` experiment on the
wave-1 head vs main at `-O3`, ≥6 runs per variant.
