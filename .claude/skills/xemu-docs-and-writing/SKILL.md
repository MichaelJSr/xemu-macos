---
name: xemu-docs-and-writing
description: Documentation maintenance and house style for this fork — use when writing or updating README.md (Changes bullets, Failed/reverted-experiments rows, Future-vectors entries, knob tables, Troubleshooting), docs/dsp-jit-design.md or a new design doc, docs/RELEASING-macos.md, release notes, or commit messages; when deciding WHERE a fact belongs (README vs docs/ vs .claude/skills/); when asked to "document this", "write up", "update the README", "draft release notes", or "fix stale docs"; and when editing any skill in this .claude/skills/ library so its conventions stay consistent. Also holds the dated list of currently-known documentation drift (e.g. the README PREFILL=2 table row).
---

# xemu docs of record — where knowledge goes and how to write it

This skill is the maintenance manual for this fork's documentation:
the map of which document owns which kind of knowledge, the house
style each artifact follows (derived from the shipped artifacts, with
real quotes), copy-paste templates, and the currently-known drift
between docs and code. All file references are relative to the repo
root `/Users/michaelsrouji/Documents/Xemu/tools/xemu-macos` (branch
`macos-optimizations`). Facts verified 2026-07-04 unless noted.

## When NOT to use this skill

- **Deciding what evidence a change needs before it can land (and
  whether the doc update is sufficient)** → `xemu-change-control`.
  This skill tells you how to WRITE the documentation a change owes;
  the change classes and required evidence live there.
- **Executing a release** (tagging, CI flow, publishing the draft,
  packaging fallback) → `xemu-run-and-operate`. This skill covers
  only the release-notes FORMAT and the staleness of the release doc.
- **Running benchmarks to get the numbers your prose will cite** →
  `xemu-testing` (method) and `xemu-validation-and-qa` (thresholds).
- **The full story behind a Failed-experiments row** →
  `xemu-failure-archaeology`.
- **The authoritative catalog of every config/env knob** (you are
  only documenting one) → `xemu-config-and-flags`.

---

## 1. The docs-of-record map

"Doc of record" means: the one place a fact lives; everything else
points at it. Four homes, one rule each:

| Home | Owns | Audience |
|---|---|---|
| `README.md` (~935 lines as of 2026-07-04) | THE manifest of the fork: what changed vs upstream, what failed and why, what's next, how to build/run/troubleshoot. Every landed change, reverted attempt, and settled not-worth-it ends up here. | Users + future contributors |
| `docs/dsp-jit-design.md` | The design-doc exemplar: one subsystem's architecture + phased roadmap, updated as phases land (status column). New multi-session designs get a sibling file in `docs/` shaped like it. | Whoever implements the next phase |
| `docs/RELEASING-macos.md` | The release runbook. Its notes FORMAT section is still canon; its FLOW sections are stale (see §4). | Release operator (human or agent) |
| `.claude/skills/` | Operational knowledge: proven methods, playbooks, this library. Skills cross-reference each other by name and never duplicate a fact that has a home above — they point at it. | Agents working on the repo |

Everything else under `docs/` is inherited upstream QEMU
documentation — do not treat it as fork doc of record. The only two
fork-authored files there are `dsp-jit-design.md` and
`RELEASING-macos.md` (verify: `ls docs/*.md`).

### 1.1 README section anatomy

As of 2026-07-04 (re-derive anchors with `grep -n '^#' README.md`):

| Section (line) | Contains |
|---|---|
| `## Quick start` (13) | Prereqs, clone/build/run commands, clean-rebuild line |
| `### Building for Windows` (29) | Which optimizations are portable vs macOS-only; native MSYS2 + docker cross recipes |
| `### Build knobs` (71) | Table `Env var / Default / Purpose` of build-time `XEMU_*` vars |
| `### How Vulkan is provisioned (all platforms)` (82) | volk + MoltenVK/vulkan-1.dll/libvulkan per platform |
| `### Runtime debug / escape-hatch knobs` (109) | Table `Env var / Purpose` of runtime `XEMU_*` vars ("escape hatch" = env var restoring pre-change behavior for bisects) |
| `### Recommended xemu.toml` (129) | The recommended user config block, with inline comments |
| `## Changes` (163) | The landed-change manifest, one `###` per subsystem |
| — `### CPU / JIT (ARM64)` (165) | x87/TCG work |
| — `### Vulkan renderer (pgraph/vk)` (197) | NV2A Vulkan renderer work |
| — `### MetalFX + presentation` (453) | Present chain, MetalFX, window backends |
| — `### MCPX APU` (563) | Voice processor + DSP JIT summary |
| — `### Input` (667) | Pads, remap UI, input pipe |
| — `### Threads + runtime` (682) | BQL/threading/timer changes |
| — `### Build + packaging` (692) | build.sh, bundling, CI |
| — `### MoltenVK runtime config (Info.plist LSEnvironment)` (718) | The `MVK_CONFIG_*` table — mirrors `Info.plist` + `ui/xemu.c` `setenv` calls (currently drifted, §4) |
| `## Failed / reverted experiments` (730) | Two-column table `Attempt / Reason`; intro: "Lessons worth preserving so they aren't re-attempted." |
| `## Future vectors` (766) | Candidate work; intro: "Not attempted, or scope/risk too high for a one-shot change." |
| `## Troubleshooting` (856) | `**Symptom.**` + remedy entries |
| `## Architecture at a glance` (909) | ASCII dataflow diagram |

### 1.2 Which section a given change updates

| You changed... | Update |
|---|---|
| TCG / x87 / tcg/aarch64 | `### CPU / JIT (ARM64)` bullet |
| `hw/xbox/nv2a/pgraph/vk/` | `### Vulkan renderer (pgraph/vk)` bullet |
| Present path / MetalFX / `ui/xemu-metal.m` | `### MetalFX + presentation` bullet |
| APU / DSP (`hw/xbox/mcpx/apu/`) | `### MCPX APU` bullet; if it's a JIT phase/round, ALSO a `docs/dsp-jit-design.md` roadmap row (see §1.3) |
| Input / pads / remap UI | `### Input` bullet |
| Threading / BQL / timers | `### Threads + runtime` bullet |
| `build.sh` / CI / packaging / MoltenVK provisioning | `### Build + packaging` bullet; provisioning-model changes also touch `### How Vulkan is provisioned` |
| An `MVK_CONFIG_*` value | THREE homes must agree: `Info.plist` `LSEnvironment`, `ui/xemu.c` `setenv(...)` (~line 1610), and the README MoltenVK table. Drift here caused a multi-week bug hunt — see the pink-tile entry in `xemu-failure-archaeology` |
| New build-time `XEMU_*` var | `### Build knobs` table row |
| New runtime `XEMU_*` escape hatch | `### Runtime debug / escape-hatch knobs` table row (2 columns — keep it 2 cells per row, see §4 item 5) |
| Changed user-facing recommendation | `### Recommended xemu.toml` block |
| New user-visible failure mode with a workaround | `## Troubleshooting` entry |
| Big-picture dataflow | `## Architecture at a glance` diagram |

### 1.3 THE RULE: every outcome gets recorded

This is the fork's core documentation discipline, and it is enforced
practice, not aspiration (commits like `README: record
streamed-vertex stall experiments (both ~neutral)` and `Document
reverted per-flight vertex mirror experiment` exist solely to pay
this debt):

1. **Every landed change gets a Changes bullet** in its subsystem
   subsection (template §3.1).
2. **Every reverted attempt gets a Failed / reverted experiments
   row** with enough mechanism to prevent a re-attempt (template
   §3.2). ~27 rows exist as of 2026-07-04.
3. **Every settled "not worth it" gets a Future-vectors update or
   removal** — a vector that was investigated and measured below the
   bar does not silently disappear; it is converted into a recorded
   negative result or struck through with the reason. Two shipped
   examples of the two forms:
   - *Strikethrough with reason* (the idea was based on a wrong
     premise): "~~Shader specialization constants~~ — stale:
     alpha-test and fog-enable are *already* compile-time GLSL
     variants (baked into the generated shader via
     `PshState`/`VshState`; the LRU shader cache keys on them). ...
     A useful reframing would target shader-compile stutter (variant
     count) rather than per-draw branching."
   - *Negative result recorded in place* (the idea was tried and
     measured neutral): the entry beginning "**Streamed-vertex stall
     reduction: attempted twice, both ~neutral — heavy scenes are
     GPU-bound.**" keeps the measurements (75-85% of
     `finish_vtx_dirty` eliminated, flips/s unmoved, ~2.5-3.5 s
     fence wait per 5 s constant) and the redirect ("Future work
     here must reduce GPU work per frame ... not CPU
     synchronization").
4. **A design doc supersedes its Future-vectors seed.** When a
   vector graduates into a design doc, say so in the doc
   (`docs/dsp-jit-design.md`: "It supersedes the 'MCPX APU DSP
   dynarec' entry in the README 'Future vectors' section.") and
   remove/replace the README entry. One home per fact applies inside
   the repo docs too.
5. **Design-doc roadmaps are living tables.** `dsp-jit-design.md`'s
   roadmap has a `Status` column (`**landed**`, `**landed
   (partial — via Phase 4)**`, `**deferred**`) updated as phases
   land, and grew appended rows (`round 3` ... `round 8`,
   `retro-chaining`) as the work outran the original phase plan.
   Landing a phase without flipping its status row is doc debt.

Whether a change may land WITHOUT its doc updates is a change-control
question — see `xemu-change-control`. The default answer is no.

---

## 2. House style, derived from the shipped artifacts

Do not invent style; imitate the corpus. Quotes below are verbatim
from the repo as of 2026-07-04.

### 2.1 README Changes bullets

Shape: **bold short title ending in a period**, then why-first prose
with code names in backticks and measured numbers instead of
adjectives. A representative shipped bullet (README `### Vulkan
renderer (pgraph/vk)`, trimmed):

> - **VkPipelineCache persisted across runs.** The cache was only
>   saved in `pgraph_destroy()`, which a normal app quit never
>   reaches — `pipeline_cache.bin` was never written, so every launch
>   recompiled all MSL pipelines (measured 164 ms worst single
>   `vkCreateGraphicsPipelines` on a cold cache). The PFIFO thread now
>   flushes the cache at flip boundaries ... Warm-cache worst case
>   measured ≤ 4 ms — a ~40x hitch reduction ...

Rules embodied there:

- Title is the WHAT in ≤ ~6 words, bolded, period inside the bold.
  No colon after the bold title.
- First sentence after the title is the WHY (symptom, root cause, or
  contended constraint) — the fix comes second. (PFIFO = the NV2A
  command-FIFO thread, i.e. the renderer thread.)
- Code identifiers, env vars, file names in backticks.
- Numbers over adjectives: "164 ms → ≤ 4 ms", "~40x", "46.3 vs
  48.5 fps" — never "significantly faster". If the change is
  perf-relevant, the bullet cites the measurement (protocol per
  `xemu-testing`).
- Parenthetical asides carry the caveats ("(FISTTP inline is
  AArch64-only: ...)"), keeping the main sentence clean.

### 2.2 Failed / reverted experiments rows

Two-column table `| Attempt | Reason |`. The Reason column carries
*mechanism*, not just verdict — enough that a future engineer
recognizes the same idea and knows exactly why it died. Range of
shipped granularity:

- Minimal (mechanism is one fact):
  `| Separate compute queue | MoltenVK only exposes queueCount=1 |`
- Full post-mortem (the per-flight-mirror row): names the design,
  the measured result ("Measured ~**neutral** ... an initial '-30%'
  read traced to an invalid baseline run parked on a 3-draws/flip
  static screen"), the root-cause analysis, the revert rationale
  ("+128 MiB, swap/delta complexity, no measured win"), and the
  redirect ("The real target this exposed: ... see Future vectors").

Write the row at whichever length prevents the re-attempt. If the
saga deserves more than a row, the row stays the summary and the
full story goes to `xemu-failure-archaeology` (skills) — the README
row is still mandatory.

### 2.3 Future-vectors entries

Each entry is a candidate with its decision inputs on the page:
problem, what has been measured, and — for entries already probed —
the explicit bar that killed or defers it. Shipped example of a
bar-to-attempt: "Measured: the snapshot memcpy it would eliminate
totals < 0.5 ms per 5 s in-game — far below the ≥ 1 ms/frame bar
for attempting this." Unproven ideas stay labeled as candidates;
never write a vector as if it were planned work.

### 2.4 Release notes (the format of record)

The FORMAT lives in `docs/RELEASING-macos.md`, which was rewritten
2026-07-05 around the current flow (CI builds a draft on tag push,
`release.yml` seeds the body from `.github/scripts/gen-changelog.py`,
the owner rewrites the notes into this format and publishes —
execution details in `xemu-run-and-operate`).

- **Title**: `vX.Y.Z — <headline>` with an em-dash (`—`, not
  hyphen), headline ≤ ~70 chars. Shipped examples from the doc:
  - `v0.8.150 — Fix single-frame pink-tile flashes on 2/4-bpp textures`
  - `v0.8.151 — VRAM snapshot closes texture-upload tear race; zero-copy GPU unswizzle`
  If one release bundles unrelated changes, separate the two most
  prominent with `;` — don't list all of them.
- **Body**: opening line stating what the artifact is, a
  `## Highlights` 1-3 sentence summary, then one `###` subsection
  per subsystem touched ("Skip subsystems that have no changes"),
  each with bold-title bullets in the same shape as README Changes
  bullets. Skeleton in §3.4.
- Style rules, verbatim from `docs/RELEASING-macos.md`:
  - "Each bullet starts with a bold short title followed by a
    period, then the prose. Don't use a colon after the bold title."
  - "Describe the **why** first (symptom / root cause / contended
    constraint) and the fix second."
  - "Prefer concrete numbers over adjectives for perf work. Avoid
    'significantly' / 'much faster' / 'greatly improved'."
  - "No emojis. No GitHub-Flavoured-Markdown callouts (`> [!NOTE]`).
    No screenshots".

### 2.5 Commit messages

Verified against `git log -8 --format=full` and
`git log --first-parent --format='%s' -40` (2026-07-04):

- **Title**: `<prefix>: imperative title`, prefix naming the touched
  area or file. Casing tracks what the prefix names: lowercase for a
  code area or filename (`input:`, `vk:`, `macos:`, `scripts:`,
  `nsprof:`, `build.sh:`, `xemu-version.sh:`), capitalized for a
  named system or doc (`README:`, `CI:`, `DSP JIT:`, `Docs:`). Many
  commits — cross-cutting ones especially — drop the colon prefix
  entirely and open straight with an imperative verb (`Fix
  pink-tile corruption: MoltenVK prefill off; snapshot-load fixes`;
  `Merge upstream/master: DSP engine abstraction + dsp56300 JIT
  engine`). There is no single enforced rule here — match the
  nearest analogous commit (`git log --oneline -20 -- <path>`) rather
  than inventing a new prefix. Titles routinely carry the measured
  outcome in parentheses:
  - `vk: in-pass occlusion queries + deferred zpass reports (2.4x fps)`
  - `vk: byte-exact vertex-conflict refinement (+5.4% fps in-game)`
  - `input: guard XEMU_INPUT_PIPE to POSIX (fix Windows build)`
- **Body**: why-first prose wrapped at ~72 columns — same discipline
  as Changes bullets: symptom/root cause, then the fix, with
  measured numbers ("0 artifact frames in 388 captures at 48.5 fps
  (prefill=0) vs 22 at 46.3 (prefill=2)" — from `531122e8aa`).
  Doc-only commits state what sections now cover (`8597422308`).
- **Footer (current)**:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
  Older commits carry `Made-with: Cursor` — that footer is
  historical; do not use it for new commits.

### 2.6 Troubleshooting entries

Shape: `**Symptom sentence.**` then the remedy, quoting the exact
knob and its cost, and — for escape hatches — a request to report:
"**Depth artifacts after a render-target switch.** Set
`XEMU_ZETA_SHAPE_READBACK=1` to restore the full GPU→CPU round trip
on zeta shape switches (costs ~2 finishes + multi-MB copies per
frame on affected titles) and report the title." (zeta = the
depth/stencil buffer.) Every new escape-hatch knob whose symptom a
user could hit deserves one of these.

---

## 3. Copy-paste templates

### 3.1 README Changes bullet

```md
- **<What changed, ≤6 words>.** <Symptom / root cause / constraint
  that motivated it — the why.> <The fix or mechanism, naming
  functions, buffers, and flags in backticks (`pgraph_vk_finish`,
  `XEMU_VTX_EXACT`).> <Measured result with protocol-grade numbers:
  "<before> → <after> <unit> (<scene/protocol>)". Caveats in
  parentheses.>
```

### 3.2 Failed / reverted experiments row

```md
| <The attempt, one line, code names in backticks> | <Why it died, with mechanism: what broke or what was measured (numbers), why that is inherent rather than incidental, and — if applicable — what a viable retry would have to change ("see Future vectors") or which harness/knob was kept in-tree> |
```

### 3.3 Future-vector entry

```md
- **<Problem statement>.** <What is known: the measured bound and
  where it came from ("Measured (`XEMU_NV2A_NSPROF`): <X> per 5 s
  in-game").> <First steps if attempted: the concrete entry point,
  files, and the experiment that would confirm/kill it.> <Bar to
  attempt: the threshold the measurement must clear before this is
  worth doing — e.g. "far below the ≥ 1 ms/frame bar", or "not
  worth the complexity at current frame rates".>
```

If the vector has been tried and settled negative, rewrite the entry
as a recorded result (lead with "attempted <N> times, <verdict>" —
mirror the streamed-vertex entry) or strike it through with the
reason (mirror the shader-specialization entry). Do not delete
without leaving the lesson somewhere (usually a Failed row).

### 3.4 Release notes skeleton

```md
macOS (Apple Silicon) release. Attached build is an ad-hoc-signed `xemu.app` for macOS 14+ (macOS 26 Tahoe recommended for MetalFX + frame interpolation).

## Highlights

<1-3 sentence summary of what's in this release and what problem it
solves. If this is a small patch on top of the previous tag, say so
explicitly, e.g. "Single bug-fix commit on top of v0.8.XYZ.">

### Vulkan renderer (pgraph/vk)

- **<Short bold title of change #1>.** <Why first (2-4 sentences:
  root cause / motivation), then the fix. Code names in backticks.
  Concrete before/after numbers for perf work.>

### <Next subsystem with changes — e.g. APU, TCG / JIT, MetalFX + presentation>

- **<...>.** <...>
```

Title it `vX.Y.Z — <headline>` (em-dash, ≤ ~70 chars). One `###`
per subsystem touched; skip untouched subsystems; no emojis,
callouts, or screenshots.

### 3.5 Design-doc skeleton (mirrors `docs/dsp-jit-design.md`)

New multi-session designs go in `docs/<topic>-design.md` shaped like
the exemplar (verify its live structure:
`grep -n '^#' docs/dsp-jit-design.md`):

```md
# <Subsystem> — design and phased roadmap

<One paragraph: what this documents. If it graduates a README
Future-vectors entry, say "It supersedes the '<entry>' entry in the
README 'Future vectors' section." and update the README.>

## 1. Current state

| Piece | File | Size | Notes |
|---|---|---|---|
<inventory of the code being changed, with real paths as links>

### 1.1 <Invariants the new design must preserve>

## 2. Architecture

<The design. Include rejected alternatives WITH reasons (the
exemplar: "TCG was considered and rejected. TCG is tightly coupled
to `CPUState` ..."). Subsections per mechanism.>

## 3. Phased roadmap

<Phase granularity and gating flag statement.>

| Phase | Status | Scope | Target <metric> delta |
|---|---|---|---|
| 0 | **landed** / **deferred** / (blank until attempted) | <what lands, in enough detail to review against> | <predicted number — commit to it before measuring> |

**Aggregate target after Phase <N>**: <the end-state claim>.

## 4. Risks / open questions

## 5. Failed-experiment traps explicitly sidestepped

<Name the README Failed-experiments rows this design must not
re-trip, and how it avoids each.>

## 6. Test strategy

## 7. Runtime knobs

| Env var | Default | Purpose |
|---|---|---|
```

Maintenance contract for a design doc: flip `Status` cells as phases
land (append rows for unplanned follow-up rounds rather than
rewriting history), and keep §5 in sync with new Failed rows the
work produces. Predicted-numbers-before-run discipline is covered in
`xemu-research-methodology`.

---

## 4. Known documentation drift (as of 2026-07-05)

This section records the debt; it does not fix it. Fixing README or
RELEASING is a normal doc change through change control
(`xemu-change-control`) — the evidence for a doc fix is the
ground-truth grep quoted in the commit body, nothing more. When you
fix ANY item below, also run the §4.1 drift sweep and update this
section (and its date) in the same change.

**2026-07-05 doc-cleanup pass resolved the previous list**: the
README PREFILL table row (now `0` with the why), the 3-cell rows in
the 2-column runtime-knob table, `XEMU_COREAUDIO_FRAMES` re-filed as
a runtime knob (and the `coreaudio.m` comment corrected),
`XEMU_VIS`/`XEMU_STRIP`/`XEMU_DSP_JIT`/dev-subflag rows added, the
"`XEMU_DSP_JIT=1` enables the JIT" overclaim corrected to
config-only + kill-switch, `docs/RELEASING-macos.md` rewritten
around the CI draft flow (annotated fork-versioned tags, current
commit footer, manual packaging kept as fallback, MoltenVK
release-parity note added), and the frozen `xemu-testing` topology
formula fixed to include `port_map = {3,4,1,2}`. The same pass also
re-filed the four misplaced CPU/threading/build Changes bullets out
of the Vulkan section and re-derived §1.1's line anchors stale.

Open items:

1. **README recommends `audio.dsp_jit.enabled = true`; the config
   default is `false`.** Not a contradiction — a recommendation is
   not a default — but any edit near either spot must keep both
   facts explicit: `config_spec.yml` has `audio.dsp_jit.enabled`
   default `false` (the fork ARM64 JIT, recommended `true` on Apple
   Silicon) and `audio.use_dsp_jit` default `true` (the upstream
   dsp56300 engine, ignored when the fork JIT is supported+enabled —
   precedence in `dsp_want_external_jit_engine()`,
   `hw/xbox/mcpx/apu/dsp/dsp.c:120`).
   Full engine-mediation rules: `xemu-change-control`; knob catalog:
   `xemu-config-and-flags`.
2. **§1.1's README line anchors predate the 2026-07-05 restructure**
   (README went 1217 → ~1106 lines; section names unchanged).
   Re-derive with `grep -n '^#' README.md` before citing.

The flag-item mirror in `xemu-config-and-flags` ("Known
README-vs-code drift — flag items") was updated to resolved in the
same pass; keep the two sections in sync.

### 4.1 Drift sweep — run after ANY doc fix

Fixing one stale line while its siblings rot is how drift survives.
After fixing an item above (or any doc), re-grep for the rest:

```bash
cd /Users/michaelsrouji/Documents/Xemu/tools/xemu-macos
# 1. MVK config: the three homes must agree
grep -n 'PREFILL\|ARGUMENT_BUFFERS\|FAST_MATH\|SYNCHRONOUS_QUEUE\|RESUME_LOST' \
  README.md Info.plist ui/xemu.c
# 2. RELEASING staleness markers (post-2026-07-05 rewrite these must
#    NOT reappear; 'tag -a' and '--draft=false' are now deliberate)
grep -n 'Committing\|Cursor\|track upstream\|Made-with' docs/RELEASING-macos.md
# 3. Live release reality to compare against
grep -n 'draft:' .github/workflows/release.yml
git tag -l | tail -3 && git cat-file -t "$(git tag -l | tail -1)"
# 4. Current commit footer convention
git log -3 --format='%(trailers)'
# 5. Knob tables vs code: every README-documented env var must exist
for v in $(grep -o 'XEMU_[A-Z_]*' README.md | sort -u); do
  git grep -lq "$v" -- '*.c' '*.h' '*.m' '*.mm' '*.cc' '*.sh' '*.yml' '*.py' \
    || echo "README documents $v but code doesn't reference it"
done
# 6. Config recommendations vs defaults
grep -n -A3 'dsp_jit:' config_spec.yml
```

Anything the sweep surfaces that you don't fix in the same change
gets added to §4 with today's date.

---

## 5. Writing rules for THIS skills library

`.claude/skills/` is a doc of record too. Every skill here (and
every future edit to one) follows these rules — they are what keeps
sixteen concurrently-authored files coherent:

1. **Frontmatter description = trigger conditions.** The
   `description:` answers "when should a model load this?" — name
   the symptoms, tasks, and keywords, not a topic summary. (Compare
   `xemu-testing`'s: "...reproducible savestate benchmarks,
   background input injection, visual artifact detection, and the
   environment-parity rules that make results trustworthy.")
2. **Ground-truth verification duty.** Verify every command, flag,
   path, function name, and code-level claim against the repo
   (Read/grep/read-only git) before writing it. If you cannot
   verify, omit it or label it `unverified`. A wrong runbook is
   worse than none.
3. **Date-stamp volatile facts** ("as of 2026-07-04"): line numbers,
   defaults, tag names, drift lists, historical measurements.
   Historical numbers are cited WITH their date and context, never
   as current.
4. **End with `## Provenance and maintenance`** — one line per
   drift-prone fact stating the re-verification command that
   refreshes it.
5. **One home per fact + cross-reference by skill name.** Each fact
   lives in exactly one skill; siblings say "see `xemu-<name>`"
   rather than restating it. Before adding a fact, ask which skill
   owns it (method → `xemu-testing`; evidence bar →
   `xemu-change-control` / `xemu-validation-and-qa`; history →
   `xemu-failure-archaeology`; knobs → `xemu-config-and-flags`;
   docs/style → this skill). Duplicated facts drift independently —
   that is the failure mode this rule exists to prevent.
6. **House basics carry over**: imperative runbook voice,
   copy-pasteable commands, define jargon at first use, tables for
   scannable content, a "When NOT to use this skill" section, no
   overselling (unproven = labeled open/candidate), and never advise
   routing around change control.
7. **Frontmatter `description:` uses a `>-` block scalar.** A
   single-line unquoted scalar breaks two ways: an interior `: ` is
   a YAML error to spec-compliant parsers, and a ` #` silently
   starts a comment that truncates everything after it (this
   truncated `xemu-architecture-contract`'s entire keyword list
   until the 2026-07-04 review). Block scalars are immune to both.
   Validate after ANY frontmatter edit:
   `python3 -c "import yaml,glob; [yaml.safe_load(open(f).read().split('---')[1]) for f in glob.glob('.claude/skills/*/SKILL.md')]"`.

## Provenance and maintenance

Facts here were verified against the repo on 2026-07-04. Re-verify
before trusting the volatile ones:

- README section anatomy + line anchors: `grep -n '^#' README.md`
- PREFILL agreement across the three homes: `grep -n PREFILL README.md Info.plist ui/xemu.c`
- RELEASING staleness set (expect no hits): `grep -n 'Committing\|Cursor\|track upstream\|Made-with' docs/RELEASING-macos.md`
- Release-notes format rules: read `docs/RELEASING-macos.md` "Notes body format" + "Style rules"
- CI draft flow: `grep -n 'draft:\|gen-changelog' .github/workflows/release.yml` and the owner guard in `.github/workflows/release-on-tag.yml`
- Commit conventions + footer: `git log -8 --format=full`
- Tag scheme + annotated-vs-lightweight: `git tag -l | tail -3; git cat-file -t v0.9`
- Design-doc exemplar structure: `grep -n '^#' docs/dsp-jit-design.md`; roadmap header: `grep -n '| Phase | Status |' docs/dsp-jit-design.md`
- dsp_jit defaults vs recommendation: `grep -n -A3 'dsp_jit:' config_spec.yml` and README `### Recommended xemu.toml`
- Fork-authored docs inventory: `ls docs/dsp-jit-design.md docs/RELEASING-macos.md`
- Strikethrough / negative-result exemplars still present: `grep -n 'Shader specialization\|attempted twice' README.md`
