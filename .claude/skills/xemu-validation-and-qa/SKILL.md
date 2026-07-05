---
name: xemu-validation-and-qa
description: >-
  The evidence bar for xemu-macos — what counts as PROOF before a claim is
  accepted. Defines the acceptance thresholds (±0.02 fps static baseline,
  sub-0.5% = noise-suspect, ±5 fps live-route variance, soak sizing vs
  failure rate), the golden fixture inventory on the dev Mac (hdd qcow2 +
  snapshot F-keys, title corpus, firmware, custom MoltenVK), the validation
  gauntlets (pre-release, MoltenVK pin bump, upstream merge, renderer
  behavior change), the honest state of automated testing (CI runs zero
  tests), and how to add regression nets the project's way. Load BEFORE
  accepting or publishing any performance number, shipping a fix, tagging a
  release, bumping the MoltenVK pin, or merging upstream; when asked "is
  this proven / validated / tested", "what evidence do we need", "what
  tests exist", "how do I add a test", or when reviewing anyone's A/B
  results.
---

# xemu-macos validation and QA — the evidence bar

This skill defines what counts as evidence on this project: the minimum
proof per claim type, the numeric acceptance thresholds, the golden
fixtures those thresholds were measured against, the gauntlets a change
must pass before shipping, and the honest state of automated testing.

The *methods* (how to run a savestate benchmark, inject input, capture and
score frames) belong to the **xemu-testing** skill — this skill tells you
when a result produced by those methods is *believable* and *sufficient*.

## When NOT to use this skill

- **How to run** a benchmark, input injection, window-capture oracle, or
  snapshot topology matching → **xemu-testing** (the method owner).
- **Interpreting** nsprof/APU_PROF/DSP-stat output, helper scripts, healthy
  vs pathological number ranges → **xemu-diagnostics-and-tooling**.
- **Which change class** a diff falls into and the commit/release/merge
  rules attached to it → **xemu-change-control** (it maps change classes to
  the claim types below; this skill defines what satisfying a claim type
  means).
- **Research doctrine** (hypothesis discipline, idea lifecycle, adversarial
  refutation) → **xemu-research-methodology**.
- Building, provisioning, or operating the app → **xemu-build-and-env**,
  **xemu-run-and-operate**.

## Terms used here

- **flip**: guest frame boundary (swap); flips/s ≈ in-game fps (guest-capped
  at 60).
- **draws/flip**: draw calls per guest frame — the scene-identity
  fingerprint. Reported by nsprof.
- **nsprof**: the fork's release-safe wall-time profiler
  (`XEMU_NV2A_NSPROF=1`, 5 s summaries to stderr).
- **savestate/snapshot**: QEMU `savevm` state stored inside the qcow2 hdd
  image; the reproducible-benchmark anchor.
- **zpass report / occlusion query**: guest asks "how many pixels passed
  depth"; games consume the count to drive effects (lens-flare/glow
  intensity and similar visibility-gated rendering).
- **DSP GP/EP**: the two Motorola 56300-family DSP cores in the Xbox audio
  processor; the fork JITs them on ARM64.
- **Interleaved A/B**: alternating baseline/experiment runs (B,E,B,E,…) so
  thermal and drift effects hit both arms equally.

---

## 1. Evidence bar by claim type

| Claim | Minimum evidence | Accepted when | Method owner |
|---|---|---|---|
| "X is faster / not slower" | Interleaved savestate A/B, ≥3 pairs, one binary + env toggle | Delta clears the noise bar (§2); scene identity verified | xemu-testing |
| "Bug is fixed / guard works" | Mechanism explains ALL observations (incl. negatives) + forced-failure validation | Forced failure fires many times with zero faults; no observation left unexplained | §1.2 below |
| "No visual regression" | Window-capture soak with the magenta oracle, user-representative launch path | Flagged-frame-free exposure sized against the known failure rate (§2) | xemu-testing |
| "DSP JIT change is safe" | `XEMU_DSP_JIT_DIFF=1` clean run through the affected workload + STATS review | Validator final line: `checked>0, failures=0`; no new cf_fallback bucket | §1.4 below |

xemu-change-control defines which change class must satisfy which of these
claim types (e.g. a renderer perf change needs the perf claim + visual
claim). This section defines what "satisfy" means.

### 1.1 Performance claims

Protocol: the interleaved savestate A/B in xemu-testing, implemented by
`scripts/bench-savestate-ab.sh <snapshot> <ENV_VAR> [pairs] [secs] [outdir]`
(defaults: 3 pairs, 75 s per run, `./bench-out`; a discarded warmup run
comes first; B runs `ENV_VAR=0`, E runs `ENV_VAR=1`).

A perf number is admissible only if ALL of these hold:

1. **One binary, env toggle.** Both arms are the same executable; only the
   toggled variable differs. (Cross-binary comparisons: see below.)
2. **Snapshot loaded via QEMU monitor** after boot — CLI `-loadvm` fails on
   USB topology (details in xemu-testing).
3. **Interval hygiene**: use only nsprof intervals with draws/flip > 100
   (in-game content), drop the first post-load interval (load transient),
   discard the warmup run.
4. **Scene identity**: draws/flip must match across all runs and both arms.
   Azurik anchors (2026-07, this machine): menus ~3, attract reel 350–390,
   heavy in-game 408–485 draws/flip. A run parked on a menu is invalid —
   the historical "-30%" per-flight-mirror misread came from exactly that
   (baseline stuck on a 3-draws/flip static screen).
5. **≥3 interleaved pairs**, compared as means with spread stated.

Thresholds (see §2 for the table): static-scene baseline reproducibility is
±0.02 fps (measured 2026-07), but treat any static-scene delta below ~0.5%
as **noise-suspect** — add pairs or decline to claim. Live-movement runs
carry ±5 fps route variance: compare means of ≥3 pairs, or prefer a static
scene for small deltas.

**Cross-binary A/B** (release vs previous release, MoltenVK dylib swap):
the same analysis rules apply, but interleave *launches of the two builds*
instead of an env toggle. Expect wider spread than ±0.02 — the custom-vs-
official MoltenVK adoption test (2026-07) measured 50.9±1.0 vs 51.3±5.4 fps
and was correctly called parity, not a regression. State both arms'
spreads; overlapping spreads = no claim either way.

**Fresh baseline per campaign.** Historical numbers (§3 anchors) are dated
context for sanity checks — never gates. Re-measure the baseline on this
machine at the start of every campaign; thermals, OS updates, and MoltenVK
pins move the floor.

### 1.2 Correctness claims (the forced-failure discipline)

A fix or guard is proven only when BOTH legs hold:

**Leg 1 — the mechanism explains everything.** One mechanism must account
for ALL observations, including the negatives (why it never reproduced in
scenario X, why the first fix didn't help). This bar is owned by
xemu-research-methodology. The cautionary incident: the visibility-buffer
crash "Act 1" fix (query-pool capacity guard) was validated in isolation,
yet the crash persisted in user gameplay — because pool exhaustion never
explained the launch-path asymmetry. The real cause (MoltenVK's
per-command-buffer visibility flag vs in-pass query timing) was found in
Act 2 by reading the driver source. Full saga: xemu-failure-archaeology.

**Leg 2 — force the failure and watch the defense hold.** Make the guarded
condition fire on demand and demonstrate zero faults under fire. The
canonical example (2026-07): the query-pool capacity guard was proven by
running with `XEMU_MAX_QUERIES=64` — the guard fired **3312 times per 5 s
interval with zero crashes or asserts**. That knob exists for exactly this
(`hw/xbox/nv2a/pgraph/vk/reports.c`: default `max_queries_in_flight =
4096`, env override accepted in range 8–65536, code comment: "overrides
for testing the guard path").

A guard that has never been observed firing is an *assumption*, not a
defense. `nv2a_vk_assert` is unconditionally compiled out via the
hardcoded `NV2A_VK_PERF_BUILD 1` (`pgraph/vk/debug.h:66-69`) — in every
build, including `./build.sh --debug` — so an assert there is
documentation, not a guard.

### 1.3 Visual-correctness claims

Method: the window-capture + magenta-scoring oracle in xemu-testing.
This skill sets the bar for calling a run "clean":

1. **Soak in the scenario that failed** (or would fail). Launch path is
   part of the scenario: Finder/LaunchServices launches get `Info.plist`
   `LSEnvironment`, shell launches don't — the pink-tile bug hid behind
   exactly that split. A clean soak in the wrong launch path proves
   nothing.
2. **Size the soak against the observed failure rate.** Commit-recorded
   reference (pink-tile fix, `531122e8aa`): the failing config flagged
   **18-22 frames per 4-minute run** (~5/min); the recorded acceptance
   was **0 artifact frames in 388 captures** in the previously-failing
   scenario. Illustrative sizing derived from that recorded rate (a
   derivation, not a recorded run): at ~5 flagged frames/min pre-fix, a
   40-60 minute clean soak represents roughly 200-300 expected fires.
   Rule of thumb: soak long enough that the old failure rate predicts at
   least a few hundred flagged frames; state the arithmetic in the
   result.
3. **For occlusion/zpass changes, add a zpass-consumer eyeball pass.** The
   magenta oracle only catches gross corruption; wrong-but-plausible
   occlusion results show up as missing or wrongly-bright lens flares and
   glow effects. Visit a scene that uses them and compare against the
   escape-hatch behavior (`XEMU_REPORTS_SYNC=1` restores the legacy
   synchronous report path — verified in `pgraph/vk/reports.c`).

### 1.4 DSP JIT changes

The fork's DSP JIT has a built-in differential validator — use it. All
knobs verified in `hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
(2026-07-04):

1. **Make sure the fork JIT is actually running.** Engine precedence
   (`dsp_want_external_jit_engine()`, `hw/xbox/mcpx/apu/dsp/dsp.c`): the
   fork JIT runs when supported and `audio.dsp_jit.enabled = true`
   (config default is **false**); otherwise the upstream dsp56300 engine
   runs iff `audio.use_dsp_jit` (default true). So a default config does
   NOT exercise the fork JIT — and `XEMU_DSP_JIT` cannot fix that:
   engine selection reads g_config only, and the env var merely toggles
   the inline JIT *within* the interpreter engine (full truth table:
   xemu-config-and-flags, the precedence owner). Select the engine by
   setting `[audio.dsp_jit] enabled = true` in the run's xemu.toml
   (`-config_path` a scratch config to avoid touching the real one).
   Then confirm with BOTH signals:
   - the stderr banner `xemu: DSP JIT enabled…` — it prints only from
     the interpreter engine's paths (`parse_flags_once()`,
     `dsp56k_jit_arm64.c:279`, reachable only via `dsp_c.c`), so its
     ABSENCE means the wrong engine is selected. But the banner alone is
     not proof JIT code executed: it reports flag state at init, and a
     core can still fall back or auto-throttle to the interpreter;
   - the positive execution check: run with `XEMU_DSP_JIT_STATS=1` and
     read the exit dump — nonzero per-core translated/executed block
     counts prove JIT blocks actually ran (the STATS atexit dump is
     registered only inside `dsp56k_jit_init`, so its presence also
     discriminates the engine).
   Note: the `config_spec.yml:339` comment names an engine-selector
   function that no longer exists; the logic lives in
   `dsp_want_external_jit_engine()`.
2. **DIFF clean run through the affected workload.**
   `XEMU_DSP_JIT_DIFF=1` bit-exact-validates every *unique block
   translation* once against the interpreter (a per-translation gate
   bounds the work; `N≥2` samples every Nth block; async by default via
   the `mcpx.dsp_diff` worker; `XEMU_DSP_JIT_DIFF_SYNC=1` for immediate
   aborts during bring-up; `XEMU_DSP_JIT_DIFF_MAX=N` caps total
   validations). "Affected workload" means in-game audio in a DSP-using
   title (music + effects), not a menu. Divergence prints
   `xemu: DSP JIT DIFF FAILURE in block pc_start=0x…` and aborts —
   a clean run has neither.
3. **Accept only positive coverage.** With `XEMU_DSP_JIT_STATS=1`, exit
   (atexit handler) prints per-core validator finals:
   `xemu: DSP JIT validator (GP core) final: enqueued=… checked=…
   dropped=… failures=…`. Accept requires `failures=0` **and**
   `checked>0` — a validator that never ran is not evidence.
4. **Review the cf_fallback buckets.** The STATS dump includes a
   `cf_fallback buckets:` breakdown — counts of ops that bailed from
   inlined JIT code to the C-helper (BLR) fallback, per handler kind. A
   bucket that newly appears or grows sharply after your change is an
   inlining-coverage regression (correct but slower); compare against a
   pre-change STATS run.

Forced-failure harnesses for the JIT (all verified in code):
`XEMU_DSP_JIT_SENTINEL=1` (cur_inst poison — detects code that secretly
reads `cur_inst` at runtime), `XEMU_DSP_JIT_FORCE=1` (re-applies the
deferred round-4 cur_inst skip — a known-bad change kept in-tree as a
harness target), `XEMU_DSP_JIT_PIN_AUDIT=1` (per-op check
that the pinned A/B accumulator registers match memory; it caught the
pm_2_2 pin leak within 30 s in 2026-07).

---

## 2. Acceptance thresholds

All measured values are dated session records from this machine (Apple M2
Ultra, macOS 26.x, Azurik) — see §3 for the fixture list.

| Quantity | Value | Status / source |
|---|---|---|
| Static-scene A/B baseline reproducibility | ±0.02 fps | Measured 2026-07 with the savestate protocol (also stated in `scripts/bench-savestate-ab.sh` header) |
| Noise-suspect bar, static scenes | delta < ~0.5% | Acceptance policy: below this, add pairs or don't claim (repro is tight but thermal/scene-microstate drift exceeds ±0.02) |
| Live-movement route variance | ±5 fps | Measured 2026-07 → means of ≥3 pairs, or use static scenes |
| Smallest shipped perf claim on record | +5.4% fps | Exact-vertex refinement, 2026-07, 408 draws/flip scene |
| Guard-fire proof (reference) | 3312 fires / 5 s interval, 0 crashes | `XEMU_MAX_QUERIES=64` run, 2026-07 |
| Visual clean bar (reference) | pre-fix failure rate 18-22 flagged frames per 4-min run; recorded acceptance 0 artifact frames in 388 captures — size soaks so the old rate predicts a few hundred fires (§1.3) | Pink-tile fix record, `531122e8aa`, 2026-07 |
| Scene-identity bands (Azurik) | menus ~3 · attract reel 350–390 · heavy in-game 408–485 draws/flip | 2026-07 anchors |
| Cross-binary parity example | 50.9±1.0 vs 51.3±5.4 fps ⇒ parity | Custom-vs-official MoltenVK adoption test, 2026-07 |

---

## 3. Golden fixture inventory (this machine, verified 2026-07-04)

Hardware anchor: **Apple M2 Ultra (Mac Studio), macOS 26.5.2** — every
historical number in this library was measured here. Re-verify:
`sysctl -n machdep.cpu.brand_string; sw_vers -productVersion`.

| Fixture | Value (2026-07-04) | Re-verify |
|---|---|---|
| Repo | `/Users/michaelsrouji/Documents/Xemu/tools/xemu-macos`, branch `macos-optimizations`, tag v0.9 @ `cf85e96597` | `git describe --tags --match 'v*'` |
| Game HDD | `/Users/michaelsrouji/Documents/Xemu/xbox_hdd.qcow2` (826 MB); savestates live inside it | `ls -lh /Users/michaelsrouji/Documents/Xemu/xbox_hdd.qcow2` |
| Snapshot shortcuts | As of 2026-07-04 evening: F5=`vm-20260704032357` (471 draws/f), F6=`vm-20260704034933` (settles 192 draws/f @60), F7=`vm-20260704145901` (417 draws/f, live scene — fps drifts), F8=`vm-20260704173701` (**primary heavy anchor**, ~770 draws/f area, death-issue fixed by owner — though dying still shifts fps slightly). The earlier heavy anchor `vm-20260704150046` was REPLACED by F8 — receipts citing it name a snapshot that no longer exists; re-baseline on F8 before any new A/B | `grep -n 'f5 = \|f8 = ' "$HOME/Library/Application Support/xemu/xemu/xemu.toml"` — or list ALL snapshots in the image with the qcow2 snapshot lister (xemu-diagnostics-and-tooling) |
| Title corpus | `/Users/michaelsrouji/Documents/Xemu/games/`: Azurik (retail + `Azurik_dev.iso`), Battlefield 2 MC, Conker L&R, KOTOR, Vexx, NevolutionX (homebrew) | `ls /Users/michaelsrouji/Documents/Xemu/games/` |
| Configured DVD | `dvd_path = …/games/Azurik_dev.iso` | `grep -n 'dvd_path' "$HOME/Library/Application Support/xemu/xemu/xemu.toml"` |
| Firmware | `bootrom_path=…/Boot ROM image/mcpx_1.0.bin`, `flashrom_path=…/BIOS/Complex_4627v1.03.bin`, `eeprom_path=…/xemu/xemu/eeprom.bin` (keys under `[sys.files]`) | `grep -n '_path' "$HOME/Library/Application Support/xemu/xemu/xemu.toml"` |
| Custom MoltenVK | `/usr/local/lib/libMoltenVK.dylib` = the fork's pinned optimized build (pin `096714a2954fc8e9db9daae97c426d7dd7f8a838`, reports 1.4.2) — build.sh prefers it over vendored/Homebrew and logs provenance at bundle time: `Bundling MoltenVK from <src> (version <v>, <arch> UUID <uuid>)` | `ls -l /usr/local/lib/libMoltenVK.dylib` + version check below; check the latest build log for the provenance line |
| MoltenVK checkout | `/Users/michaelsrouji/Documents/Xemu/tools/MoltenVK` (default arg of `scripts/build-moltenvk.sh`) | `ls -d /Users/michaelsrouji/Documents/Xemu/tools/MoltenVK/.git` |

```sh
# Installed custom MoltenVK version string (expect 1.4.2 as of 2026-07-04):
strings /usr/local/lib/libMoltenVK.dylib | grep -m1 -E '^[0-9]+\.[0-9]+\.[0-9]+$'
```

Fixture rules:

- **A running xemu holds the qcow2 write lock.** Clone before any parallel
  use: `cp -c` (APFS copy-on-write, instant, snapshots ride along) — see
  xemu-testing for the parallel-observation workflow.
- **Snapshot names are date-stamped, not scene-descriptive.** Never assume
  which scene F5/F6 hold: load, read draws/flip from nsprof, and match it
  against the §2 bands before treating a run as "the heavy scene".
  Shortcuts are owner-volatile — re-run the grep before every campaign.
- **Validation history**: Azurik is the primary bench title and, as of
  2026-07-04, the ONLY title with recorded validation history in the repo
  docs (every measured result in README names Azurik). The other five are
  on disk but untested on record — treat them as smoke-test material, not
  as titles with known-good baselines.
- If a fixture is missing: build/provisioning → xemu-build-and-env; data
  dir, snapshots, running the app → xemu-run-and-operate.

### Historical anchors (context, never gates)

Dated records (2026-07, Azurik, this machine): savestate bench scene
15.78 → 38.57±0.80 fps (+144%) after the occlusion/report rework, per
commit `6bfbc22863`'s protocol receipt (README prose rounds this change
to 35.6 fps / 2.25x); render passes 376 → 14 per flip; fence wait
31.9 → 2.3 ms/flip (README figures; the commit records 2.2 ms on the new
path); prefill=2 vs 0 measured 46.33 vs 48.50 fps; user-environment
sustained 48.5 fps post-pink-tile fix. Use these to sanity-check that a fresh baseline is in a plausible
range — a fresh baseline MUST still be captured per campaign (§1.1).

---

## 4. Validation gauntlets

A gauntlet is the fixed set of checks a change of the given kind must pass
before it ships. Run methods per xemu-testing; accept per §1–§2.

### 4a. Pre-release (before publishing a tag)

1. **fps A/B vs the previous release build** on a golden savestate —
   cross-binary interleaved protocol (§1.1). Any regression must be
   explained and deliberately accepted, not waved through.
2. **Artifact-oracle soak** (§1.3): flagged-frame-free at the sized bar,
   in the Finder-launch path (the user-representative one).
3. **Boot/play smoke on ≥2 additional corpus titles** beyond Azurik (pick
   from BF2MC / Conker / KOTOR / Vexx / NevolutionX): boots to gameplay,
   several minutes of play, no crash/hang/obvious artifact.
4. **CI matrix green and the draft release complete.** Tag push `v*` fires
   `release-on-tag.yml` (owner-guarded) → `release.yml` → a DRAFT release
   (`draft: ${{ !inputs.pre-release }}`; the tag job is owner-guarded via
   `github.repository_owner`). Compare the asset count against the
   previous release — v0.9 shipped **17 assets** (verified live
   2026-07-04; one-line count command in Provenance below).

Tag/release invariants (immutable published tags, one release per tag)
are owned by xemu-change-control; release mechanics by xemu-run-and-operate.

### 4b. MoltenVK pin bump

Stated in the `scripts/build-moltenvk.sh` header (verified 2026-07-04):
"bump `MVK_PIN` deliberately and re-run the validation gauntlet (savestate
fps A/B + artifact hunt) before releasing with it."

1. fps A/B: old dylib vs new dylib, cross-binary interleaved rules (§1.1)
   — the 2026-07 adoption test (parity at 50.9±1.0 vs 51.3±5.4) is the
   template and the precedent for how to call it.
2. Artifact hunt (§1.3) with the new dylib in the Finder-launch path.
3. Provenance check: the build log's `Bundling MoltenVK from …
   (version …, UUID …)` line shows the intended version — this line exists
   because a silently-substituted local dylib is how a pink-tile-class bug
   escapes testing.

### 4c. Upstream merge

1. Full macOS build completes (both the fork's Vulkan renderer and Metal
   presentation paths compile).
2. In-game smoke with the fork's differentiators actually engaged: DSP
   JIT enabled (`[audio.dsp_jit] enabled = true` in the run's config —
   engine selection is config-only, `XEMU_DSP_JIT` cannot select it;
   banner + STATS confirmation per §1.4 step 1), Vulkan renderer,
   MetalFX active. A merge that only survives default config has not
   been smoked.
3. If the merge touched `hw/xbox/mcpx/apu/` or `dsp/`: a DIFF clean run
   (§1.4).
4. Windows compile via CI (push the branch; the build matrix covers
   Windows). Never regress Windows/upstream paths for a macOS win —
   merge mediation rules and the Windows-preservation gate live in
   xemu-change-control.

### 4d. Renderer-behavior change

1. **Escape-hatch A/B**: every renderer behavior change ships a legacy
   switch (established practice — see xemu-change-control); the A/B
   toggles that very switch, which doubles as the bisect tool if a report
   comes in later. Existing hatches for reference:
   `XEMU_REPORTS_SYNC`, `XEMU_VTX_EXACT=0`, `XEMU_ZETA_SHAPE_READBACK`,
   `XEMU_TEX_BIND_RECHECK` (all verified in `hw/xbox/nv2a/pgraph/vk/`).
2. **zpass-consumer visual check** when occlusion/report paths are
   touched (§1.3 step 3).
3. **Cross-title smoke**: at least one corpus title beyond Azurik —
   renderer changes tuned on one title's draw pattern are the classic
   overfit.

---

## 5. Automated testing: honest state, and how to extend it

### 5.1 What exists (and what does not)

- **CI compiles and packages; it runs NOTHING.** `ci.yml` → `build.yml` →
  `build-{macos,windows,linux}.yml`; the macOS job builds a
  {x86_64,arm64}×{debug,release} matrix and lipo-merges. No workflow
  contains a test step. Verified 2026-07-04:
  `grep -riE 'make check|meson test|ctest|pytest' .github/workflows/`
  → zero hits. "CI green" therefore means *it builds everywhere*, nothing
  more.
- **The upstream QEMU `tests/` tree is present but unused by the fork**
  (`tests/qtest`, `tests/functional`, `tests/qemu-iotests`, `tests/fp`, …).
  It is not wired into any fork workflow and has no recorded fork use; it
  targets generic QEMU devices, not the xbox machine or the fork's
  renderer/JIT code. Treat it as inherited furniture, not a safety net.

### 5.2 The fork's real regression nets

| Net | What it catches | Where |
|---|---|---|
| DSP JIT DIFF validator | Any bit-level JIT/interpreter divergence, per unique translation | in-binary, `XEMU_DSP_JIT_DIFF` (§1.4) |
| Savestate A/B harness | Perf regressions, anchored to nsprof intervals | `scripts/bench-savestate-ab.sh` + xemu-testing protocol |
| Artifact oracle | Gross visual corruption over long soaks | capture method in xemu-testing; scoring half ships as `score_frames.py` in `.claude/skills/xemu-diagnostics-and-tooling/scripts/` (capture loop recreated from the description) |
| Forced-failure knobs | Prove guards/harnesses actually fire | `XEMU_MAX_QUERIES` (capacity guard), `XEMU_DSP_JIT_SENTINEL` / `_FORCE` (cur_inst discipline), `XEMU_DSP_JIT_PIN_AUDIT` (pinned-register integrity) |

These four are what "tested" means on this project today (2026-07-04).

### 5.3 Adding validation the project's way

When you need a new regression net, follow the house pattern:

1. **Prefer an in-binary, env-gated differential/audit knob** (the
   DIFF / PIN_AUDIT / SENTINEL pattern): zero cost when unset (guard the
   whole thing on a `getenv` at init); an unmistakable failure signature
   prefixed `xemu: …` on stderr (grep-able by harnesses, e.g.
   `DSP JIT DIFF FAILURE`); abort on divergence for bring-up, count+report
   for soaks.
2. **Build in a forced-failure mode** so the net can prove itself — a knob
   that makes the guarded condition fire (`XEMU_MAX_QUERIES=64`) or
   re-applies a known-bad change (`XEMU_DSP_JIT_FORCE=1`). **A net that
   has never caught its target failure is unproven**: fire it once and
   watch it catch, before trusting any green result from it.
3. **Wrap it in a harness script under `scripts/`** —
   `bench-savestate-ab.sh` is the template: warmup run, interleaved arms,
   per-run logs to an outdir, monitor-socket loadvm.
4. **Create one golden savestate per scenario**: save in-app, bind an
   F-key under `[general.snapshots.shortcuts]`, load headless via the
   monitor (xemu-testing owns the topology-matching details). Record the
   scene's draws/flip band next to the snapshot name.
5. **Document the knob** per the xemu-config-and-flags add-a-flag
   checklist (README's "Runtime debug / escape-hatch knobs" section is
   the user-facing catalog).

---

## 6. Result-acceptance checklist (run before believing ANY number)

A reviewer (human or model) walks this list against the run logs. Any
"no" invalidates the result — no exceptions for exciting deltas.

1. **Scene identity**: draws/flip reported for every run, matching across
   arms, and in the intended band (§2)? A menu-parked or attract-reel-
   drifted run is not the benchmark scene.
2. **Interleaving**: arms alternated (B,E,B,E,…)? All-baseline-then-all-
   experiment ordering confounds thermals and drift — reject.
3. **Environment parity**: exactly one variable differs between arms; same
   launch path (Finder vs shell — `LSEnvironment` applies only to Finder
   launches); same governing `xemu.toml`; any plist-only env var passed
   explicitly per xemu-testing rule 1?
4. **Binary identity**: window title (`xemu | v<version>`, git-describe-
   derived via `scripts/xemu-version.sh`; debug builds append "Debug")
   matches the intended build in BOTH arms? A running process keeps its
   old binary across rebuilds. For dylib A/Bs, the MoltenVK provenance
   line (§3) is the identity check.
5. **Interval hygiene**: first post-load interval dropped, warmup run
   discarded, only draws/flip>100 intervals aggregated?
6. **Variance stated**: number of pairs, per-arm mean ± spread, per-pair
   deltas available? Static delta under ~0.5% flagged noise-suspect;
   live-movement compared only as means of ≥3 pairs; overlapping spreads
   called parity?
7. **Fresh baseline**: baseline measured this campaign on this machine —
   not inherited from §3 anchors or an older session?
8. **For fix claims**: forced-failure leg run (§1.2)? Mechanism accounts
   for every observation, including why earlier repro attempts failed?
   If either leg is missing, the fix is a hypothesis, not a result.

---

## Provenance and maintenance

Re-verify drift-prone facts with these one-liners (all paths relative to
the repo root unless absolute):

- Env-knob inventory: `grep -rn 'getenv("XEMU_' hw/ ui/ target/ tcg/`
- `XEMU_MAX_QUERIES` default/range + guard-test purpose: `grep -n -B2 -A8 'XEMU_MAX_QUERIES' hw/xbox/nv2a/pgraph/vk/reports.c`
- `XEMU_REPORTS_SYNC` legacy hatch: `grep -n 'XEMU_REPORTS_SYNC' hw/xbox/nv2a/pgraph/vk/reports.c`
- DIFF semantics + sampling + per-translation gate: `grep -n -A8 'XEMU_DSP_JIT_DIFF' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- DIFF failure signature: `grep -n 'DIFF FAILURE' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- Validator final line + STATS buckets: `grep -n 'validator\|cf_fallback buckets' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- Engine precedence (and the stale engine-selector comment at `config_spec.yml:339`): `grep -n -B6 -A8 'dsp_want_external_jit_engine' hw/xbox/mcpx/apu/dsp/dsp.c; sed -n '336,342p' config_spec.yml`
- DSP config defaults: `grep -n -A4 'dsp_jit:\|use_dsp_jit' config_spec.yml`
- CI runs no tests: `grep -riE 'make check|meson test|ctest|pytest' .github/workflows/`
- Pin-bump gauntlet requirement + pin value: `grep -n 'MVK_PIN\|validation gauntlet' scripts/build-moltenvk.sh`
- Bench protocol header (thresholds + usage): `sed -n '1,17p' scripts/bench-savestate-ab.sh`
- MoltenVK provenance line in build.sh: `grep -n 'Bundling MoltenVK' build.sh`
- Snapshot shortcuts: `grep -n 'f5 = ' "$HOME/Library/Application Support/xemu/xemu/xemu.toml"`
- Fixtures on disk: `ls -lh /Users/michaelsrouji/Documents/Xemu/xbox_hdd.qcow2 /usr/local/lib/libMoltenVK.dylib; ls /Users/michaelsrouji/Documents/Xemu/games/`
- v0.9 asset count: `gh release view v0.9 -R MichaelJSr/xemu-macos --json assets --jq '.assets|length'`
- Owner guard on the tag flow: `grep -n 'repository_owner' .github/workflows/release-on-tag.yml`
- `nv2a_vk_assert` unconditionally compiled out (hardcoded `NV2A_VK_PERF_BUILD 1`, every build incl. `--debug`): `grep -n -B3 'nv2a_vk_assert' hw/xbox/nv2a/pgraph/vk/debug.h`
- Installed custom MoltenVK version: `strings /usr/local/lib/libMoltenVK.dylib | grep -m1 -E '^[0-9]+\.[0-9]+\.[0-9]+$'`
- Window-title identity: `grep -nF 'xemu | v%s' ui/xemu.c`
- README validation-title history: `grep -in 'azurik\|conker\|kotor\|battlefield\|vexx\|nevolution' README.md`

All fixture values, thresholds, and release facts in this file were
verified on 2026-07-04 on the dev machine; historical measurements are
dated session records from the 2026-07 campaigns.
