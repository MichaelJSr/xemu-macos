---
name: xemu-change-control
description: >-
  Change classification and landing gates for the xemu-macos fork. Load
  BEFORE committing, pushing, tagging, cutting a release, merging upstream,
  reverting an experiment, adding an XEMU_* flag, or claiming a performance
  win. Defines what evidence each class of change owes (interleaved
  savestate A/B numbers, legacy escape hatch, README rows, green Windows CI
  incl. win64-cross), the upstream-merge mediation rules (dsp56k_jit_* vs
  dsp_jit_* namespaces, DSP engine precedence), commit-message conventions,
  and the hard prohibitions (published-tag immutability, MoltenVK
  prefill=0, hands off dist/xemu.app). Triggers: "commit", "push", "land",
  "ship", "tag", "release", "draft release", "release-on-tag", "upstream
  merge", "revert", "failed experiment", "escape hatch", "Windows build",
  "win64-cross", "CI matrix", "change control", "Co-Authored-By".
---

# xemu-change-control — how changes earn the right to land

This fork (`MichaelJSr/xemu-macos`, branch `macos-optimizations`) ships
Apple Silicon optimizations for the xemu original-Xbox emulator while
keeping upstream's Windows/Linux paths intact. Changes land by **direct
push to `macos-optimizations`** — there are no pull requests on the fork
(verified 2026-07-04: zero "Merge pull request" commits in fork history;
the only fork-authored first-parent merges are two upstream merges,
`fd467e02b9` and `c8ca02177b` — older QEMU-version merges on the
first-parent chain are upstream xemu's own history). That model
only works because every class of change owes a specific evidence bundle
BEFORE the push. This skill defines the classes, the gates, and the
incidents that made each gate non-negotiable.

Two facts frame everything:

1. **CI compiles, packages, and (since 2026-07-11) runs the xbox unit
   suite on the macOS arm64 legs AND both Linux legs** —
   `build-macos.yml` runs `meson test --suite xbox` (restored DSP unit
   test, swizzle round-trip, and the three inline-JIT differential
   arms; seconds, no game content), and `build-linux.yml` has an
   additive `test` job (x86_64 + aarch64; plain `build.sh --debug`
   tree because the packaging job's dpkg-buildpackage exposes no meson
   dir). The ubuntu-arm leg is the fork's only automated non-Apple
   aarch64 DSP JIT coverage — first green run 2026-07-11. Windows
   cross legs still run no tests, and no in-game testing is automated
   anywhere: correctness and performance evidence is still produced
   locally, before the push, and recorded in the commit message. CI's
   job beyond the suite is proving the change builds everywhere
   (macOS x86_64/arm64, Windows x86_64/arm64 cross, Linux, debug+release).
2. **README.md is the doc of record.** Shipped behavior changes get a
   `## Changes` entry; reverted attempts get a `## Failed / reverted
   experiments` row (34 rows as of 2026-07-11). A change that is not
   documented is not done.

## The shipping model (verified 2026-07-04)

| Step | Mechanism |
|---|---|
| Land code | `git push origin macos-optimizations` (direct; no PR) |
| CI build check | `ci.yml` fires on every push (`branches-ignore: master`, `tags-ignore: v*`) and runs the full `build.yml` matrix |
| Watch it | `gh run list --repo MichaelJSr/xemu-macos --branch macos-optimizations --limit 5`, then `gh run watch --repo MichaelJSr/xemu-macos <run-id>` |
| Release | Push tag `v*` → `release-on-tag.yml` → **draft** release → owner publishes (see "Releases" below) |

## Change taxonomy — what evidence each class owes

Classify first. A change can be in several classes at once (example:
`XEMU_INPUT_PIPE` was class 2 additive tooling, but it touched a file
Windows compiles → class 5 applied, and its omission broke the Windows
build; fix commit `cf85e96597`). When in doubt, the gates stack.

| # | Class | Gate (summary) |
|---|---|---|
| 1 | Docs-only | Claims verified against code; no perf numbers needed |
| 2 | Additive default-off tooling/diagnostics | Prove default-off + zero cost when unset; README env-knob entry |
| 3 | Performance optimization | Interleaved savestate A/B numbers in the commit body |
| 4 | Rendering/audio/timing behavior change | XEMU_* escape hatch restoring legacy + README Changes entry + artifact validation |
| 5 | Shared / upstream / Windows code paths | Green CI matrix incl. the win64-cross jobs before the change counts as landed |
| 6 | Upstream merge | Mediation rules (namespaces, engine precedence) + full CI + in-game smoke |
| 7 | Release | Full release validation gauntlet + tag-flow invariants |

### Class 1 — docs-only

Verify every command, path, and number you write against the repo before
committing (wrong runbooks are worse than none). Where a doc is known
stale, state ground truth and cite the stale doc rather than silently
matching it. (The two long-standing examples — the README
MoltenVK-table prefill row and `docs/RELEASING-macos.md`'s manual
flow — were fixed in the 2026-07-05 doc-cleanup pass; the open-drift
ledger stays in `xemu-docs-and-writing` §4.) House style and
templates: `xemu-docs-and-writing` skill.

### Class 2 — additive, default-off tooling and diagnostics

Pattern: a `getenv()`-gated facility that is provably inert when the
variable is unset. Verified examples of the pattern:

- `XEMU_INPUT_PIPE` — `ui/xemu-input.c:534`; commit `38e5e42bbd` states
  "Zero cost when unset".
- `XEMU_MAX_QUERIES` — `hw/xbox/nv2a/pgraph/vk/reports.c:38`, a test
  knob for the query-pool capacity guard.
- `XEMU_NV2A_NSPROF` — `hw/xbox/nv2a/nsprof.c:83`, the wall-time
  profiler.

Evidence owed:
1. Show the default path is untouched (the gate check must be the first
   thing the feature does).
2. README env-knob table entry (README.md has a "Runtime debug /
   escape-hatch knobs" section at line ~109).
3. If any touched file also compiles on Windows/Linux → class 5 stacks.
   `XEMU_INPUT_PIPE` is the cautionary tale: it landed POSIX-assuming
   (FIFOs, `O_NONBLOCK`) and broke the Windows build; the fix
   (`cf85e96597`) stubs `test_input_poll()` under `#ifdef _WIN32`
   (`ui/xemu-input.c:522-574`). Do the `#ifdef` in the same commit, not
   the follow-up.

The add-a-flag checklist and full knob catalog live in the
`xemu-config-and-flags` skill; interpreting diagnostic output lives in
`xemu-diagnostics-and-tooling`.

### Class 3 — performance optimization

**Rule (established practice, inferred from the record): a perf claim
ships with interleaved savestate A/B protocol numbers in the commit
body.** "Savestate A/B" = boot once per run, load the same QEMU snapshot
via the monitor socket, alternate baseline/experiment runs of one binary
toggled by an env var (`scripts/bench-savestate-ab.sh`). Method and
environment-parity rules: `xemu-testing` skill. Acceptance thresholds
and fixtures: `xemu-validation-and-qa` skill.

The record backing the rule — recent shipped commits all carry protocol
numbers, e.g. `0adb3b21b9` ("fps parity (50.9±1.0 vs 51.3±5.4
interleaved)") and `531122e8aa` ("48.5 fps (prefill=0) vs 46.3
(prefill=2)"). Static-scene baseline reproducibility is ±0.02 fps
(2026-07 measurement), so real deltas are cleanly resolvable.

The incident behind the rule: the per-flight vertex-mirror experiment
initially read **"-30%"** — traced to an invalid baseline run parked on
a 3-draws-per-flip static screen instead of the heavy scene. The row in
README `## Failed / reverted experiments` (first row, README.md:~735)
preserves it. Corollaries:

- Verify scene identity via draws/flip in the nsprof intervals before
  trusting any number (per `xemu-testing`).
- Predict the number before the run; a result you didn't predict needs a
  mechanism before it needs a commit (`xemu-research-methodology`).
- Neutral or negative result → revert AND add the README
  failed-experiments row in the same push (see Non-negotiables).

### Class 4 — rendering / audio / timing behavior change

Three obligations, all in the landing push:

1. **An `XEMU_*` escape hatch that restores legacy behavior**
   (established practice, inferred from the record — the fork's whole
   escape-hatch table exists as legacy-restore switches). Verified
   pattern instances:
   - `XEMU_VTX_EXACT=0` disables byte-exact vertex-conflict refinement
     (`hw/xbox/nv2a/pgraph/vk/vertex.c:95,118`).
   - `XEMU_REPORTS_SYNC=1` "restores the legacy synchronous behavior
     wholesale" (`hw/xbox/nv2a/pgraph/vk/reports.c:289-295`).
   - Also in the family: `XEMU_ZETA_SHAPE_READBACK`
     (`pgraph/vk/surface.c`), `XEMU_TEX_BIND_RECHECK`
     (`pgraph/vk/texture.c`). Directions and the full catalog:
     `xemu-config-and-flags`.
   Rationale: hatches make field regressions single-variable-bisectable
   without rebuilding — exactly how the pink-tile corruption was pinned
   to one MoltenVK option, and how the occlusion-report rework stayed
   fall-back-able. Bisect technique: `xemu-debugging-playbook`.
2. **README `## Changes` entry** (subsystem `###` subsection, bold-lead
   bullet; template in `xemu-docs-and-writing`).
3. **Artifact validation** for anything that can alter pixels or audio:
   run the window-capture artifact oracle over a live scene and report
   the count, e.g. `8b1b934045` ("569 window captures scored for magenta
   clusters — zero artifacts"). Method: `xemu-testing`.

### Class 5 — shared / upstream / Windows code paths

**Rule (established practice, inferred from the record): never regress
Windows or upstream code paths for a macOS win.** README.md:31 states
the contract: "The upstream Windows build paths are preserved and
CI-tested."

Gate: after pushing, the CI matrix must be green **including the Windows
cross jobs** before the change counts as landed —
`.github/workflows/build-windows.yml` runs `./build.sh -p win64-cross`
(line 79) for `arch: [x86_64, arm64] × configuration: [debug, release]`
inside the `ghcr.io/xemu-project/xemu-win64-toolchain*` containers.
macOS jobs (`build-macos.yml`, macos-15 runners) run the same 2×2 matrix
plus a Universal lipo step. Local cross-build recipe if you need to
iterate before pushing: `xemu-build-and-env` skill.

The canonical gating patterns — copy these, verified 2026-07-04:

- **`#ifdef __APPLE__` for semantics that are only safe on Metal**:
  `hw/xbox/nv2a/pgraph/vk/surface.c:1304` narrows the
  `pgraph_vk_upload_surface_data()` full-GPU-sync to the cases that need
  it on Apple only; the `#else` branch keeps `target_is_active = true`
  (upstream full-finish semantics). The comment explains why: the
  fast-path argument relies on Metal's automatic hazard tracking behind
  MoltenVK's single queue; core Vulkan orders same-queue submissions
  only at execution *start*, and the race is visible under WHPX
  (Windows Hypervisor Platform).
- **`#ifndef _WIN32` for POSIX-only facilities**: `ui/xemu-input.c:522`
  compiles `test_input_poll()` to an empty stub on Windows because FIFOs
  and `O_NONBLOCK` do not exist on MinGW.
- **`#ifdef _WIN32` in TCG target config**: `tcg/aarch64/tcg-target-has.h`
  (~line 65) sets `TCG_TARGET_HAS_fpu 0` on Windows/ARM64 — llvm-mingw
  fails to constant-fold `qemu_build_not_reached()` branches out of the
  generic FP paths, leaving undefined references at link. Windows falls
  back to the bit-equivalent helper-based hard FPU.

History says the Windows matrix finds real bugs that macOS work cannot:
getting the first release matrix green (`c2860668b7` era, 2026) required
five pre-existing portability fixes, including the fpng ARM CRC gate
(`ui/thirdparty/fpng/fpng.cpp`, `__ARM_FEATURE_CRC32`), MetalFX
frame-interpolation compile guards for SDK < 26
(`hw/xbox/nv2a/pgraph/vk/metalfx_upscale.m:1234`,
`__MAC_OS_X_VERSION_MAX_ALLOWED >= 260000` — CI runners are macos-15),
and the RC FILEVERSION sanitizer (`scripts/xemu-version.sh:39-49`) whose
bug **only reproduced on tagged builds** ("macos.1" in the tag broke
windres). Lesson: green push-CI is necessary, not sufficient, for
release confidence — hence class 7.

### Class 6 — upstream merge

Upstream is `xemu-project/xemu` (`git remote -v`). Merges are rare,
deliberate, first-parent merges into `macos-optimizations` (latest:
`fd467e02b9`, 2026-07-03, "Merge upstream/master: DSP engine abstraction
+ dsp56300 JIT engine"). The standing mediation rules, established by
that merge when both sides had independently added a `dsp_jit.c`:

1. **Namespace treaty — upstream owns `dsp_jit_*`.** Upstream's
   `hw/xbox/mcpx/apu/dsp/dsp_jit.c` (upstream commit `67cc79e663`) wraps
   the external dsp56300 (Rust) subproject JIT and keeps the `dsp_jit_*`
   symbol namespace. The fork's ARM64 inline DSP JIT lives **inside the
   interpreter engine** at `hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.[ch]`
   with symbols `dsp56k_jit_*` and macros `DSP56K_JIT_*` (renamed from
   `dsp_jit_*`/`DSP_JIT_*` at the merge). Never reintroduce fork symbols
   into the `dsp_jit_*` namespace; never rename upstream's engine.
2. **Engine precedence** — one line: the interpreter engine with the
   fork's inline JIT wins whenever `DSP56K_JIT_SUPPORTED` and
   `g_config.audio.dsp_jit.enabled` are set; only otherwise does
   `audio.use_dsp_jit` select upstream's external JIT engine
   (`dsp_want_external_jit_engine()` / `dsp_set_engine()`,
   `hw/xbox/mcpx/apu/dsp/dsp.c`). Both config knobs stay through any
   merge, **and so do the fork's defaults for them**. The 2026-09-07
   merge accepted upstream's `fc13b78060` flip of `use_dsp_jit` to
   `false`; because `dsp_jit.enabled` also defaults `false`, that left a
   stock config selecting *no* JIT engine on any platform, macOS
   included — a silent behaviour change nobody's own `xemu.toml`
   revealed. It was restored to `true` on 2026-09-08. Treat a default
   change in `config_spec.yml` as a merge conflict even when git does
   not: diff the audio block after every merge. Full truth table,
   config defaults, and env-var non-interaction: `xemu-config-and-flags`
   (the precedence owner). (The `config_spec.yml` comment above
   `dsp_jit:` correctly names `dsp_want_external_jit_engine()` — its
   earlier stale-selector wording was fixed; re-check after any merge
   that touches the spec.)
3. **Preserve fork behavior through structural churn.** The same merge
   re-homed the mixbuffer handoff as engine-aware
   `dsp_write_mixbuffer_bulk()` (`hw/xbox/mcpx/apu/dsp/gp_ep.c`) instead
   of poking core state that had become a sync snapshot. When upstream
   restructures, port the fork's mechanism to the new seam — do not keep
   writing through interfaces whose semantics changed.

Merge evidence bar (from `fd467e02b9` itself): document the mediation in
the merge-commit body, then full macOS build + in-game smoke run with
the fork engines active, then push and hold for the full CI matrix
(class 5 gate applies — a merge touches everything). DSP JIT design
background: `docs/dsp-jit-design.md`; deeper merge history:
`xemu-failure-archaeology`.

### Class 7 — release

Run the **full release validation gauntlet** before tagging — the
gauntlet contents (what to run, on which fixtures, with which pass
thresholds) are owned by the `xemu-validation-and-qa` skill; the
operational how-to-run-the-app mechanics are in `xemu-run-and-operate`.
This skill owns the flow and its invariants:

**Current flow (verified 2026-07-04):**

1. Push a tag matching `v*` to `origin`.
2. `.github/workflows/release-on-tag.yml` fires. Owner guard (line 16):
   `if: contains(fromJSON('["xemu-project", "MichaelJSr"]'),
   github.repository_owner)` — tag pushes on other forks do nothing.
   Note `concurrency: cancel-in-progress: false` (line 9): a re-fired
   run will NOT auto-cancel.
3. It calls `release.yml`, which runs the full `build.yml` matrix and
   then `softprops/action-gh-release` with `draft: ${{ !inputs.pre-release }}`
   (release.yml:59-66) — the tag path always produces a **draft**
   release with the complete artifact set (v0.9: 17 assets).
4. The owner reviews and publishes the draft manually. (`ci.yml` has
   `tags-ignore: "v*"`, so a tag push does not double-build.)

**Versioning:** fork-versioned tags since `v0.9` (= `cf85e96597`,
published 2026-07-04). The previous scheme was upstream-suffixed
(`v0.8.153-macos.1`, still published). One release per tag; every fix
gets a fresh version.

**Doc of record:** `docs/RELEASING-macos.md` was rewritten 2026-07-05
around this CI flow (fork-versioned annotated tags, draft-then-publish,
MoltenVK release-parity note) and keeps the manual packaging recipe as
the explicit fallback — it is current again; follow it.

## Non-negotiables — and the incidents that made them

These are absolute. No macOS win, deadline, or cleanup justifies
breaking one.

### 1. Published tags are immutable

Never delete, move, force-update, or reuse a tag that has a published
release. Incident (2026): deleting a published release's tag **silently
flipped the release to DRAFT** — users' download links died with no
error anywhere (`v0.8.153-macos.1` was the casualty; it has since been
re-published — `gh release view v0.8.153-macos.1 --repo
MichaelJSr/xemu-macos --json isDraft` shows `false` as of 2026-07-04).
Second half of the trap: force-updating a tag **re-triggers
`release-on-tag.yml`**, and its `cancel-in-progress: false` concurrency
means the re-fired run proceeds to the release step and will replace
published assets unless you cancel it (`gh run cancel`) first. If a
release is wrong: cut a new version. `docs/RELEASING-macos.md`'s
Do-not-do list (line 251+) agrees: "Tags are immutable handles for users
who already pulled the zip."

### 2. MoltenVK prefill stays 0

`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` is `0` in BOTH homes —
`Info.plist` `LSEnvironment` and the `ui/xemu.c` `setenv(..., "0", 0)`
no-overwrite fallback — and any new MVK_CONFIG value must be set in
both, because `LSEnvironment` applies only to Finder/LaunchServices
launches (that launch-path split is what hid the pink-tile bug from
every shell-launched harness). The in-code comment says "Prefill MUST
stay 0" — treat any diff moving it as a regression. Incident record
(prefill=2 corrupted streamed textures, enabled the AGX crash, and
measured slower; fixed `531122e8aa`): `xemu-failure-archaeology` 2.1.
The five canonical MVK_CONFIG_* values with file:line homes:
`xemu-config-and-flags` Axis 3 (the value owner); the README
MoltenVK table agrees since 2026-07-05 (`grep -n PREFILL README.md
Info.plist ui/xemu.c` must show `0` in all three).

### 3. Never point a harness at `dist/xemu.app`

`dist/xemu.app` is the bundle the user actually launches. A planted
portable config (`Contents/Resources/xemu.toml`) hijacks their real
session — config, saves, and hdd path (this happened; it is Cardinal
rule 2 of the committed `xemu-testing` skill). Test from a dedicated
APFS clone: `cp -Rc dist/xemu.app <scratch>/xemu-test.app`, refreshed
after every rebuild (a full `./build.sh` run repackages the bundle and
wipes `Contents/Resources`, so anything planted there silently vanishes
— two failure modes, one rule). Full environment-parity method: `xemu-testing`.

### 4. Every reverted attempt gets a README failed-experiments row

README `## Failed / reverted experiments` (README.md:803, 34 rows as of
2026-07-11) exists so lessons "aren't re-attempted" — several rows
document multi-day investigations (per-flight mirrors, depth export,
host-imported vertex RAM) that would otherwise be re-run from scratch by
the next session. The revert and the row land in the same push. Row
format `| Attempt | Reason |` with mechanism + numbers; template:
`xemu-docs-and-writing`. The full chronicle with evidence:
`xemu-failure-archaeology`.

### Rule provenance

The four rules above are documented invariants (in-repo text and code
comments back each one). Three more rules woven through the taxonomy —
perf claims require the interleaved A/B protocol (class 3), never
regress Windows/upstream paths (class 5), every behavior change ships an
escape hatch (class 4) — are **established practice, inferred from the
record**: the owner was asked to confirm them and the question returned
unanswered, but every shipped change in the log complies. Treat them as
binding until the owner says otherwise.

## Commit conventions (verified against `git log -8 --format=full`, 2026-07-04)

- **Title**: `subsystem: imperative summary` — real examples: `input:
  guard XEMU_INPUT_PIPE to POSIX (fix Windows build)`, `vk: prime
  MoltenVK visibility buffer per CB (crash fix, for real)`, `macos:
  launch-path-independent MoltenVK config; bundle provenance`,
  `scripts:`, `README:`.
- **Body: why-first.** State the root cause / motivation, then the
  mechanism, then a `Validated:` (or equivalent) paragraph with the
  actual numbers — e.g. `531122e8aa` ends "0 artifact frames in 388
  captures at 48.5 fps (prefill=0) vs 22 at 46.3 (prefill=2)". The
  commit body is the durable home of the evidence bundle this skill
  requires; README rows summarize, commits prove.
- **Footer** (current): `Co-Authored-By: Claude Fable 5
  <noreply@anthropic.com>` — on every recent commit (69 of the fork's
  284 commits carry it as of 2026-07-11). The bulk of older fork
  history (189 commits) carries the predecessor footer
  `Made-with: Cursor`, which
  `docs/RELEASING-macos.md:40-41` still prescribes — that doc is stale;
  use the Co-Authored-By footer.
- **No merge commits except upstream merges.** Fork-authored work is
  linear on `macos-optimizations`.

## Before you land anything — checklist

1. **Classify** the change against the taxonomy table; remember classes
   stack.
2. **Evidence in hand before pushing** — CI will not produce it:
   - Perf claim → interleaved A/B numbers (method: `xemu-testing`;
     thresholds: `xemu-validation-and-qa`), scene identity verified via
     draws/flip.
   - Behavior change → escape hatch coded + tested both ways, README
     Changes entry written, artifact-oracle run counted.
   - New tooling → proven inert when unset; README knob entry.
3. **Windows check**: does any touched file compile on Windows/Linux?
   If unsure: `git diff --name-only HEAD~1 | xargs grep -l "_WIN32\|__APPLE__"`
   is a hint, not proof — when in doubt assume class 5 and gate on the
   platform `#ifdef` patterns above.
4. **Docs current**: README Changes / failed-experiments / knob tables
   updated in the same push; ground truth stated where docs were stale.
5. **Commit message**: subsystem-prefixed title, why-first body with
   numbers, `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
6. **Push, then watch the matrix**: `gh run list --repo
   MichaelJSr/xemu-macos --branch macos-optimizations --limit 5`. A
   class-5 change is not landed until Windows x86_64+arm64 jobs are
   green. If red: fix forward or revert same-day — the branch is what
   users build.
7. **Never**, at any step: touch a published tag, plant anything in
   `dist/xemu.app`, flip prefill off 0, or revert silently without the
   README row.
8. **Releases only**: gauntlet per `xemu-validation-and-qa`, fresh `v*`
   tag, wait for the draft, owner publishes. Never re-tag.

## When NOT to use this skill

- **How to run benchmarks, inject input, capture/score frames** — the
  method lives in `xemu-testing` (committed, authoritative).
- **Pass/fail thresholds, golden fixtures, the release gauntlet's
  contents** — `xemu-validation-and-qa`.
- **Building, cross-compiling, CI build internals, MoltenVK
  provisioning** — `xemu-build-and-env`.
- **Running the app, monitor socket, snapshots, day-of release
  operations** — `xemu-run-and-operate`.
- **The XEMU_*/MVK_CONFIG_*/xemu.toml catalog and add-a-flag recipe** —
  `xemu-config-and-flags`.
- **Doc templates and house style for the rows this skill requires** —
  `xemu-docs-and-writing`.
- **Why a past experiment failed, full incident narratives** —
  `xemu-failure-archaeology`.
- **Diagnosing a live bug (using the escape hatches to bisect)** —
  `xemu-debugging-playbook`.
- **Idea lifecycle and evidence philosophy for research work** —
  `xemu-research-methodology`.

## Provenance and maintenance

All facts verified 2026-07-04 against `macos-optimizations` at
`cf85e96597` (= tag v0.9). Repo-state pins refreshed 2026-07-11 at
`7e2e6e7256` (latest tag `v0.10.2`; 284 first-parent commits ahead of
upstream; failed-row count 34; footer counts 69/189; README Failed
anchor 803). Re-verify drift-prone claims:

- Direct-push/no-PR model: `git log --first-parent --oneline upstream/master..HEAD | grep -ci "merge pull request"` (expect 0)
- CI triggers + no tests: `sed -n 1,15p .github/workflows/ci.yml` and `grep -rn "ctest\|make check\|pytest" .github/workflows/` (expect nothing)
- Release flow, owner guard, no-cancel concurrency: `grep -n "tags:\|repository_owner\|cancel-in-progress" .github/workflows/release-on-tag.yml`
- Draft-by-default releases: `grep -n "draft:" .github/workflows/release.yml`
- Windows matrix + win64-cross: `grep -n "matrix:\|arch:\|configuration:\|win64-cross" .github/workflows/build-windows.yml`
- Release states: `gh release view v0.9 --repo MichaelJSr/xemu-macos --json isDraft,assets --jq '{isDraft, n: (.assets|length)}'`
- Prefill invariant (both homes + stale README row): `grep -n "PREFILL" Info.plist ui/xemu.c README.md`
- Apple-only upload-sync narrowing: `grep -n "target_is_active" hw/xbox/nv2a/pgraph/vk/surface.c`
- Input-pipe POSIX guard: `grep -n "_WIN32\|XEMU_INPUT_PIPE" ui/xemu-input.c`
- TCG FPU Windows gate: `grep -n "TCG_TARGET_HAS_fpu" tcg/aarch64/tcg-target-has.h`
- DSP engine precedence + namespaces: `grep -n "dsp_want_external_jit_engine\|dsp_set_engine" hw/xbox/mcpx/apu/dsp/dsp.c` and `ls hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c hw/xbox/mcpx/apu/dsp/dsp_jit.c`
- DSP config defaults (expect `dsp_jit.enabled: false`, `use_dsp_jit: true` — the second is fork-owned, upstream ships `false`): `grep -n -A3 "dsp_jit:\|use_dsp_jit:" config_spec.yml`
- Escape hatches present: `grep -rn "XEMU_VTX_EXACT\|XEMU_REPORTS_SYNC" hw/xbox/nv2a/pgraph/vk/{vertex,reports}.c`
- Failed-experiments row count: `awk '/^## Failed/,/^## Future/' README.md | grep '^| ' | grep -vc '^| Attempt\|^|---'` (34 as of 2026-07-05)
- Current commit footer: `git log -3 --format=%B | grep Co-Authored-By`
- RELEASING doc staleness (expect no hits post-rewrite): `grep -n "Cursor\|Made-with\|track upstream" docs/RELEASING-macos.md`
