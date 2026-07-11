---
name: xemu-frontier-and-positioning
description: >-
  Ranked research-frontier catalog and external-positioning guide for the
  xemu-macos fork, current as of 2026-07-04. Nine ranked items (GPU-frame-cost
  campaign pointer, push-model present handoff, Windows real-hardware Vulkan
  validation, frame-interpolation real depth, DSP parmove+ALU fusion,
  BINK/VideoToolbox offload, PAL 50 Hz guest vblank, occlusion STALLED drains,
  MTLResidencySet), each with first in-repo steps and a falsifiable milestone;
  a novelty audit of the fork's headline results; and the public claiming
  standard (full protocol receipt, never "significantly", upstream-baseline
  capture protocol). Load when asked what's the research roadmap / what's next
  / what should we work on next; is X novel or has it been done before; what
  can we claim publicly; how we compare to upstream xemu or other Xbox
  emulators; how to draft a blog post, README claim, or release announcement
  about a win; the standard of proof before publishing a number; or whether a
  README Future-vectors entry is still open, stale, or already resolved.
---

# xemu-frontier-and-positioning

Two jobs: (1) where this fork can advance the state of the art, with
concrete next steps and a numeric definition of "done"; (2) how to talk
about any of it in public without overselling. Every fact below was
checked against the repo on 2026-07-04 (re-check commands in "Provenance
and maintenance"). Repo-state pins (commit count, HEAD, latest tag), the
§2.8/§3.5 resolutions, and the epilogue's vertex-transient status were
refreshed 2026-07-11 at `7e2e6e7256`. Historical fps numbers are dated
session measurements, cited as recorded history, not re-run for this
skill.

> **RANKING ADDENDUM (2026-07-04 evening, campaign session at
> `a045dfc780` — supersedes the item-1 pointer and re-ranks the top of
> the list).** The GPU-frame-cost campaign was EXECUTED and its menu is
> measured exhausted on the current fixtures (see the status header in
> `xemu-gpu-frame-campaign` and archaeology 1.14): the heavy scene is
> CPU-side-bound, GPU has ~14 ms/flip slack. Item 8 (STALLED drains) is
> RESOLVED with its confirming measurement (finish_stalled = 0 in all
> in-game intervals; README row updated). Item 3 (Windows real-HW
> validation) advanced: the static gating audit is committed
> (`docs/windows-gating-audit.md`) with a six-item needs-real-HW list —
> what remains genuinely needs Windows hardware. New top of the ranked
> list, by measured mass × feasibility — WITH SAME-DAY OUTCOMES from
> the evening session (receipts in the v0.10..e0c04ce8b3 commit
> bodies; scene table vs v0.9: snap4-area 24.2→32, F7 ~40→47,
> F5 46.6→50, F6 60 held):
> 1. **Dirty-clear TLB-walk coalescing — PARKED same day**: the only
>    multi-walk caller is cold (ramblock init); the surviving design
>    (cross-call TLB re-arm deferral) widens an archaeology-1.9-class
>    race window and needs its own review; ceiling ~1-1.5 fps at the
>    new ~31 fps floor.
> 2. **PGO — SHIPPED** (`19fca12174`): +9.4% on the heavy scene
>    (25.42±0.34 → 27.81±0.62, 3/3 pairs); profile committed at
>    pgo/default.profdata; CI arm64-release legs build with it.
> 3. **Guest TCG throughput — NOW THE #1 OPEN LEVER**:
>    `helper_lookup_tb_ptr` 15.8% of the vCPU thread post-PGO (~75%
>    of its cost is hit-path — get_tb_cpu_state+hash+probe — so a
>    jump-cache-size bump has a small ceiling; parked). SSE
>    packed-float NEON lowering was BUILT and proven bit-exact via
>    its own differential harness (=2 mode), but measured parity
>    post-PGO (PGO'd softfloat + serializing FPCR bracket under
>    guest FTZ) — ships dark as `XEMU_SSE_NEON=1` (`e015705d21`).
> 4. **Deferred-report idle budget — SHIPPED same day**
>    (`7a9b116680`, 5 ms → 300 µs, `XEMU_REPORTS_BUDGET_US`): guests
>    polling same-frame zpass reports stalled on it; +1.3-1.5 fps on
>    the heavy scene, and it halved finish_vtx_dirty as a side
>    effect. Report-path latency is now near its structural minimum.
> 5. Push-model present (unchanged, still open).
> v0.10.1 EPILOGUE (2026-07-05): the batch shipped and tagged —
> cross-binary gauntlet vs the v0.10 release binary on F8:
> 23.88±1.85 → 27.42±1.41 (+14.8%, 5/6 pairs). Additional outcomes:
> BQL-free MMIO for PFB/USER shipped (fps-parity, jitter win —
> archaeology 7.5); eager report submit killed (archaeology 1.15);
> SSE host path extended to x86_64 dark (same-ISA bit-perfect);
> vertex transient-copy subsequently implemented and KILLED
> 2026-07-05 (`XEMU_VTX_TRANSIENT`: −2.09 fps, 6/6 pairs negative —
> archaeology 1.16, README Failed row, `64a8a6b543`); PGO retrained
> on F8 (+ the build.sh merge-trap fixes, archaeology 8.4). The
> 60 fps wall on the heavy scene remains guest TCG throughput (the
> deferred vertex redesign died as 1.16).
> Caveat that must ride every CPU-side claim: this title busy-polls —
> vCPU utilization is never evidence of guest-boundness on its own
> (F6 shows ~95% vCPU at a flat 60 fps cap); PFIFO-starvation time is
> the valid signal. And RE-BASE PREDICTIONS after every shipped win:
> the SSE parity was a direct consequence of predicting against a
> pre-PGO profile (sequential-optimization interaction).

## When NOT to use this skill

| Need | Use instead |
|---|---|
| Execute the #1-ranked item (draw merging / GPU-frame-cost reduction) | `xemu-gpu-frame-campaign` — full campaign lives there; this skill has only a pointer |
| Evidence-bar mechanics (predict-before-run, one-mechanism-explains-all, idea lifecycle) | `xemu-research-methodology` |
| Acceptance thresholds a number must clear (±0.02 fps noise floor, soak sizing) | `xemu-validation-and-qa` |
| What evidence a change owes before landing/tagging, or upstream-merge naming rules in full | `xemu-change-control` |
| Actually run the interleaved savestate A/B protocol | `xemu-testing` |
| Write the README bullet / release-note prose | `xemu-docs-and-writing` |
| Domain theory (TBDR/zpass/parmove/floatx80 definitions) | `xbox-hardware-reference` |
| Full story behind a referenced settled battle | `xemu-failure-archaeology` |

This skill decides *what's worth doing next* and *what you're allowed to
say about it afterward*. It does not re-derive sibling content.

---

## 1. The bar

Owner decision, 2026-07-04, not yet in README — recorded here first:

> "Beyond SOTA" = **the definitive Apple Silicon Xbox emulator** — MEASURED
> performance leadership in real titles (4K/120-class fluidity via MetalFX)
> over upstream xemu and peers, on the same hardware.

Unpack each clause — it gates what counts as progress:

- **MEASURED** — a number with the protocol receipt from §4, not a code
  comment or an architectural argument.
- **real titles** — this fork's fixture titles (Azurik, Battlefield 2
  Modern Combat, Conker: Live & Reloaded, KOTOR, Vexx, NevolutionX), scene
  identity pinned by draws/flip (Azurik's attract reel alone is bimodal: 3
  draws/flip title screen vs 350-390 draws/flip demo reel — `xemu-testing`
  covers the trap).
- **4K/120-class fluidity via MetalFX** — the machinery already exists:
  `MTLFXFrameInterpolator` does 2x (30->60) or 4x (30->120)
  (`README.md` architecture diagram ~line 926;
  `hw/xbox/nv2a/pgraph/vk/metalfx_upscale.m`); exclusive fullscreen already
  picks the highest available refresh; the private-texture output ring
  lifts the old 1920px cap (1280x960 -> 2880x2160 on a 4K panel,
  `README.md` ~line 520-521). **What's missing is a published,
  protocol-backed number showing this fork delivers smoother real-title
  motion than upstream or a peer at the same settings on the same Mac** —
  that number does not exist in this repo today.
- **over upstream xemu and peers, on the same hardware** — "upstream" is
  checkable here (§5, §4's baseline protocol). "Peers" (other Xbox
  emulators/ports) is **not** checkable from this repo — no other
  emulator's source is in this tree. Any "vs peers" claim is a
  **positioning hypothesis**, not a fact, until externally benchmarked.

Every frontier entry below is ranked by how directly it moves this bar.

---

## 2. Research frontier, ranked

Status vocabulary:

| Status | Meaning |
|---|---|
| **open** | Understood gap, no attempt yet, tractable today |
| **candidate** | A mechanism is already sketched in-repo — implement and measure |
| **needs-theory** | Premise unvalidated (no evidence the cost is real, or no hook exists) — investigate before implementing |
| **resolved-verify** | Code suggests this was already fixed by a later change than the one documenting it — needs one confirming measurement, not new design |
| **blocked** | Foreclosed by a third-party constraint — track, don't attempt |

| Rank | Entry | Status | Why ranked here |
|---|---|---|---|
| 1 | GPU-frame-cost reduction / draw merging | open (campaign) | Flagship, biggest plausible fps lever; owner-selected |
| 2 | Push-model present handoff | open | Removes a guaranteed per-UI-frame stall; matters more as target Hz rises |
| 3 | Windows real-hardware Vulkan validation + provoking-vertex closure | open | Gates any same-hardware-class comparison and an unvalidated code path |
| 4 | Frame-interpolation real depth (flip-time zeta snapshot) | candidate | Direct quality lever on the exact feature the bar names |
| 5 | DSP parmove+ALU fusion | needs-theory | Audio headroom; explicitly deferred pending a workload that hits it |
| 6 | BINK video via VideoToolbox | needs-theory | No in-repo hook yet; CPU-bound premise itself unmeasured |
| 7 | PAL 50 Hz guest vblank | open | Compat breadth, not a fluidity lever for NTSC fixture titles |
| 8 | Occlusion-report STALLED drains | resolved | Confirmed dead under shipped defaults (`finish_stalled = 0`); stale README bullet pruned |
| 9 | MTLResidencySet | blocked | Evaluated and rejected in-repo on both fronts it could apply to |

Source: `README.md` "Future vectors" (~line 846 as of 2026-07-11; every
entry below re-checked still present 2026-07-04) plus direct code reading. Entries not
reproduced here (streamed-vertex stall, MetalFX input ring, GL-under-Metal
switch, texture-upload barrier batching, `VK_EXT_external_memory_host`,
`VK_KHR_dynamic_rendering` barriers, shader specialization, GPU S3TC
decode, voice-register writeback batching, `dispatch_semaphore_t` kick) are
either settled-negative with measured bounds already recorded in README,
or out of this skill's scope — do not resurrect them; see README directly
and `xemu-failure-archaeology`.

### 2.1 GPU-frame-cost reduction / draw merging — rank 1, open (campaign)

**Why it falls short.** Render passes/flip already sit at the TBDR
(tile-based deferred renderer — Apple GPUs load/store the whole tile's
memory every pass, so pass count is first-order) floor for this scene
shape: 14 passes/flip against 408-485 draws/flip in heavy in-game scenes
(`README.md` ~line 351). The next ceiling is per-draw CPU cost across those
hundreds of draws — pipeline binds, descriptor updates, vertex-state
checks — not pass count. Owner-identified target (2026-07-04), not yet
attempted.

**Asset.** The occlusion/report rework (`hw/xbox/nv2a/pgraph/vk/reports.c`,
commit `6bfbc22863`) already proved the methodology here: instrument with
`nsprof`, find the dominant cost, redesign around the real Vulkan/MoltenVK
legality constraint, validate with the interleaved A/B protocol. The same
renderer (`draw.c`, `command.c`) is already instrumented
(`XEMU_NV2A_NSPROF` `PIPELINE_BIND`/`DRAW` events) to find the next
bottleneck without guessing.

**Steps.** Full decision-gated plan lives in **`xemu-gpu-frame-campaign`**
— this entry is a pointer, not a duplicate.

**Milestone.** A same-scene interleaved A/B (fixed draws/flip) shows a
measured fps delta attributable to fewer draw calls or fewer
pipeline/descriptor binds per flip — not fewer render passes (that lever
is spent at 14/flip). Detail and expected numbers: `xemu-gpu-frame-campaign`.

### 2.2 Push-model present handoff — rank 2, open

**Why it falls short.** `nv2a_get_present_frame` (`hw/xbox/nv2a/nv2a.h:57`
-> `hw/xbox/nv2a/pgraph/pgraph.c:443` -> Vulkan renderer's
`pgraph_vk_get_present_frame`, `hw/xbox/nv2a/pgraph/vk/renderer.c:283-323`)
**pulls**: resets an event, kicks the PFIFO thread, blocks
(`qemu_event_wait`) until PFIFO answers — every UI frame, called
unconditionally at the top of `metal_render_frame` (`ui/xemu.c:991-1008`)
before the paced-presentation "skip unchanged" dedup even runs. At 120 Hz
that's 120 guaranteed cross-thread round trips/sec regardless of whether
anything changed. README names this directly: "the sync handshake is also
what publishes frames, so a UI-side 'skip when unchanged' pre-check is not
possible in the current pull model" (~line 791-796).

**Asset.** `NV2APresentFrame` already carries a shared event, event value,
and monotonic `frame_seq` (`renderer.c:310-318`) — the fields a lock-free
"last published frame" pointer needs. The async compositor path already
threads an exported `MTLSharedEvent` cross-API (`instance.c` ~line
585-588), proving publish-without-blocking is already solved here.

**Steps.** (1) Add an `nsprof` counter isolating the `qemu_event_wait` in
`pgraph_vk_get_present_frame` specifically — nothing isolates just this
wait today. (2) Read `pgraph.c:443-460` and `renderer.c:283-323` end to
end; find where PFIFO already knows "a new frame is ready" (flip handling
in `draw.c`/`pfifo.c`) as the natural publish point instead of the current
wait point. (3) Prototype a published-pointer handoff (PFIFO publishes
`{texture, event, frame_seq}` atomically at flip; UI reads the last
published value, no lock/wait) behind a new escape-hatch flag, per this
fork's practice of shipping a legacy switch with every behavior change.

**Milestone.** Interleaved A/B at a fixed high-draw scene shows either
measured PFIFO-thread time freed (new wait counter drops) or measured
UI-frame pacing jitter reduced — with zero change in flips/s (guest timing
must be unaffected). Report both; this is a latency/jitter win, not
necessarily an fps one.

### 2.3 Windows real-hardware Vulkan validation + provoking-vertex closure — rank 3, open

**Why it falls short.** Windows builds are validated only via native MSYS2
builds and a Docker/colima cross-compile (`README.md` "Building for
Windows") — no record in this repo of a run on real (non-VM) Windows GPU
hardware. That matters for one path specifically:
`draw_needs_primitive_emulation` (`hw/xbox/nv2a/pgraph/vk/draw.c:122-157`)
CPU-emulates vertex rotation for `LINE_LOOP`/`QUADS`/`QUAD_STRIP` always,
and first-vertex-provoking `TRIANGLES`/line-mode `POLYGON`, specifically
because "MoltenVK lacks reliable support for `VK_EXT_provoking_vertex`"
(`draw.c:144`). That function is skipped entirely when
`r->supports_geometry_shaders` is true (`draw.c:126-128`) — a genuine
Vulkan capability probe (`enabled_physical_device_features.geometryShader`,
`instance.c:580-581`), true on virtually any real desktop GPU, false on
Metal (`shaders.c:371`). So real Windows/Linux hardware takes a
**different render path** for these primitives than macOS, and nobody has
confirmed the two agree.

**Asset.** `build.sh`, `XEMU_NV2A_NSPROF`, and
`scripts/bench-savestate-ab.sh` are all portable, none macOS-gated
(README's own "portable optimizations" list) — the exact protocol used on
this Mac applies unmodified once a build reaches real hardware.

**Steps.** (1) Get a build onto real (non-VM) Windows hardware — README's
Troubleshooting is explicit that VM Vulkan is unrepresentative or absent
(~line 104-106, 899-905); native MSYS2 `./build.sh` is preferred. (2) Copy
the same hdd qcow2 + a savestate there (matching USB pad topology per
`xemu-testing`) and run the interleaved A/B protocol. (3) Drive a scene
that hits the gated-off branches (`QUADS`, `QUAD_STRIP`, `LINE_LOOP`, or
first-vertex-provoking triangles) and visually/hash-compare against the
same scene on macOS to confirm the geometry-shader path and MoltenVK's
CPU-emulation path agree.

**Milestone.** A real, non-VM Windows machine has a savestate A/B number on
record with a full protocol receipt (§4) — none exists in this repo's
history today — and a primitive-emulation-affected scene is confirmed
visually identical (or a documented divergence is filed) between the two
code paths.

### 2.4 Frame-interpolation real depth (flip-time zeta snapshot) — rank 4, candidate

**Why it falls short.** `XEMU_MFX_REAL_DEPTH=1` feeds the temporal scaler
the live zeta (depth/stencil) texture instead of synthetic luminance
depth, but "by present time the single guest zeta buffer typically holds
the *next* in-progress frame's depth" (`surface.c:978-983`) — a known
limitation, which is exactly why this ships dark (off by default) for A/B
(`README.md` ~line 555-561). This caps the quality ceiling of the exact
feature the bar names (MetalFX temporal upscaling/interpolation).

**Asset.** All the plumbing exists: exportable zeta images and the export
gate (`zeta_export_wanted`, `surface.c:985-995`), the real-depth binding
into the temporal scaler (`metalfx_upscale.m` ~line 686), and a per-flip
hook already wired into the renderer ops table (`ops.flip_stall`,
`hw/xbox/nv2a/pgraph/pgraph.h:120`, called every guest flip). This is "add
one blit at an existing hook," not new infrastructure.

**Steps.** (1) Read `surface.c:976-995` and the real-depth binding in
`metalfx_upscale.m` to find exactly where the *live* zeta texture is
handed to the temporal scaler today. (2) Add a snapshot path invoked from
`flip_stall`: blit the current zeta into a dedicated snapshot image before
the next frame's draws touch it, gated behind a new sub-flag so the
existing live-read opt-in stays available. (3) A/B the two variants on a
fast-camera-pan scene by eye (the magenta artifact oracle doesn't catch
ghosting — see `xemu-testing`), plus `nsprof` for the added blit's ms cost.

**Milestone.** A fast-motion scene shows a described, screenshot-backed
reduction in temporal-upscale ghosting between live-read and
flip-snapshotted real depth, and the added blit's cost is measured in
ms/flip — both on record before proposing the snapshot variant replace the
live-read opt-in as default.

### 2.5 DSP parmove+ALU fusion — rank 5, needs-theory

**Why it falls short.** `docs/dsp-jit-design.md`'s "8 (remaining)" row
defers this explicitly: with A/B/X/Y/SR pinning already landed, "the
remaining fold candidates are narrow," and the closest existing trigger
counters (`alu_ccr_nz_skipped`, `alu_ccr_eu_skipped`) are **zero** on the
Azurik audio workload — the workload this fork's fixtures actually
exercise doesn't hit the shape this would fix.

**Asset — with a register-budget constraint.** The pin infrastructure and
validation scaffolding already exist, but `x28` is NOT free: it now holds
the **SR pin** (`DSP56K_JIT_SR_PIN_REG 28`,
`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c:1452`; prologue comment at
:9401 "x28 = SR pin"). No free callee-saved GPR remains — a fusion emitter
must either share the existing pins or add spill/reload discipline, and
one that clobbers x28 silently corrupts SR semantics.
`docs/dsp-jit-design.md`'s former "x28 remains free for future fusion
work" / "last free callee-saved register" lines were stale
pre-SR-pinning prose — corrected 2026-07-11 to state the SR pin and the
no-free-GPR constraint.
`XEMU_DSP_JIT_STATS`/`XEMU_DSP_JIT_DIFF=N` still provide bit-exact
validation scaffolding at zero new tooling cost.

**Steps.** (1) Read the design doc's "8 (remaining)" row for the
already-scoped narrow case (pm_N families where the parmove's `save_reg_N`
holds the value also written to memory). (2) Run `XEMU_DSP_JIT_STATS=1`
against a DSP-heavy scene that isn't Azurik's audio mix and check whether
a fusion-eligible shape is ever nonzero — the design doc is explicit this
is deferred "until profiling shows a specific shape hot," and that
evidence doesn't exist yet. (3) Only if step 2 finds a hot shape: implement
the narrow fold for that `pm_N` family with an explicit register strategy
decided up front — `x28` is unavailable (it pins SR; see the Asset note),
so the fold must share existing pins or spill/reload — validated with
`XEMU_DSP_JIT_DIFF=1` over a multi-minute run.

**Milestone.** `XEMU_DSP_JIT_STATS` on a real workload shows a nonzero
fusion-eligible count, the fused implementation passes
`XEMU_DSP_JIT_DIFF=1` with zero mismatches over a multi-minute run, and
`XEMU_APU_PROF` shows a measured utilization-% drop. **Stays needs-theory
until step 2 produces a nonzero count** — don't implement speculatively
against a workload that doesn't exercise it.

### 2.6 BINK video via VideoToolbox — rank 6, needs-theory

**Why it falls short.** README's entry is one line: "Xbox BINK decoder is
CPU-bound; offload YUV->RGBA (or full transcode)" (~line 827) — the entire
spec today. There is no BINK-related file or symbol anywhere in this tree
(verified: zero word-boundary matches for "bink" in C sources; a loose
case-insensitive grep hits only incidental substrings under
`include/libdecnumber/`). BINK
decode is *guest* CPU work under TCG emulation, not an xemu-side function
— nothing exists to hook yet.

**Asset.** The fork has precedent for this category of move: CoreAudio/vDSP
audio paths and the existing "compute unswizzle + YUV" zero-copy display
path (`README.md` architecture diagram) show the pattern in production.
VideoToolbox is the same tool family applied to video.

**Steps.** (1) Confirm the premise on this hardware: none of this fork's
fixture titles has been profiled for BINK cost. Catch a known FMV moment
with a CPU sampler or `nsprof` running alongside and confirm it's still
CPU-bound on an M-series core (Xbox-era assumptions may not hold). (2)
Identify the guest decode boundary: offload needs either HLE-style
function detection replacing the guest's decode loop (high correctness
risk), or a narrower mechanical interception point — check
`surface-compute.c` and `blit.c`'s existing YUV handling as the closest
candidate hook before assuming the invasive route is required. (3) Only
after 1-2: prototype the narrowest offload (YUV->RGBA only) behind a flag,
A/B against the confirmed scene from step 1.

**Milestone.** A specific title+scene is confirmed CPU-bound on BINK
decode by measurement on this hardware, a decode-boundary hook is
identified in-repo (none exists today), and a prototype offload measures a
CPU-time reduction with byte-identical or documented-different visual
output. **Stays needs-theory until step 1 produces a number.**

### 2.7 PAL 50 Hz guest vblank — rank 7, open

**Why it falls short.** The vblank (guest frame-boundary interrupt) thread
is fixed at 60 Hz; deriving 50 Hz from guest video mode would serve PAL
titles (`README.md` ~line 840-843). The one prior attempt — aligning
vblank cadence to *host* display refresh — shipped then reverted
(`649dd265a0`) because it retimed the wrong thing: the guest-visible IRQ
titles gate simulation on, not the host present cadence (already fully
decoupled — "the Metal present path never references the vblank timer,"
README Failed-experiments table). Correctness/compat gap, not a fluidity
lever for this fork's NTSC fixtures — hence the rank.

**Asset.** The revert already mapped the negative space: it names the
exact chain that must not be touched — host `vblank_interval_ns` ->
`process_vblank` -> `graphic_hw_update` -> `nv2a_vga_gfx_update` ->
`NV_PCRTC_INTR_0_VBLANK` — and confirms the present path's independence
from it. Ready-made constraint spec for a correct redesign.

**Steps.** (1) Read `git show 649dd265a0` plus the README
Failed-experiments row for the exact chain and break reason (see also
`xemu-failure-archaeology`). (2) Check whether guest video-mode/region
(NTSC vs PAL) is already detected anywhere before assuming new detection
is needed. (3) Prototype a **second, independent** fixed-interval timer
(not a retune of the existing one) driving `NV_PCRTC_INTR_0_VBLANK` at 50
Hz only when guest mode is confirmed PAL, leaving the 60 Hz default fully
untouched — behind an escape hatch.

**Milestone.** A PAL-region title (none currently confirmed in this fork's
fixture set — check first) runs at native 50 Hz with zero regression to
the already-decoupled present path, validated by the same A/B discipline
on an NTSC title to confirm zero behavior change there.

### 2.8 Occlusion-report STALLED drains — rank 8, resolved

**Closed 2026-07-04; body updated 2026-07-11.** The README bullet this
item existed to check ("Report-heavy intervals show ~5 `STALLED` finishes
per flip ... Candidate: satisfy guest report polls from per-slot drains
without finishing") described the pre-rework mechanism. `6bfbc22863`
superseded it — its interleaved bench records **"0 stalled finishes"** in
the default config — the confirming `finish_stalled = 0` nsprof
measurement was taken 2026-07-04 (see the ranking addendum at the top of
this file), and the stale bullet has since been pruned from the README
(verified absent at `7e2e6e7256`).

What remains true and worth keeping: `VK_FINISH_REASON_STALLED` has
exactly **one** caller repo-wide (`hw/xbox/nv2a/pgraph/vk/reports.c:322`),
gated behind `sync_mode` — true only when `XEMU_REPORTS_SYNC=1`, the
legacy escape hatch, off by default. The default path does non-blocking
per-slot fence polling
(`pgraph_vk_process_pending_reports`/`pgraph_vk_drain_slot_reports`).

**Reopen only if** a new `VK_FINISH_REASON_STALLED` caller appears, or
`finish_stalled` goes nonzero under shipped defaults in an
`XEMU_NV2A_NSPROF=1` run — fresh numbers would supersede this record.

### 2.9 MTLResidencySet — rank 9, blocked

**Why this isn't open work.** `metalfx_upscale.m`'s "Phase 5 note"
(~line 33-42) carries its own verdict: evaluated, **not adopted**, for two
independent reasons — (1) each MetalFX subsystem owns only 2-4 long-lived
resources, and residency sets pay off for "argument-buffer-driven
rendering with hundreds of resources"; (2) the one subsystem with enough
resources to matter, the NV2A Vulkan renderer, "runs through MoltenVK and
doesn't expose Metal residency sets" to a Vulkan client — a hard capability
wall, not a design choice. README's Future Vectors still lists this
without the strikethrough other settled-negative entries get, but the
in-code note is unambiguous. **Do not resurrect this as open.**

**Steps (monitor, don't implement).** (1) Re-check the Phase 5 note
whenever the pinned MoltenVK version bumps (`scripts/build-moltenvk.sh`)
for any new Vulkan-visible residency/argument-buffer surface. (2) If one
appears, re-measure the MetalFX-subsystem case too (2-4 resources may
still be too few). (3) Otherwise leave fenced off.

**Milestone.** None today by design — blocked, not open. Reopens only on a
MoltenVK capability change; track via the version-bump checklist
(`xemu-build-and-env`).

---

## 3. Positioning: novelty audit

For each: what's genuinely new *here* vs a known technique applied well,
what's proven in-repo today, what a public claim still needs. House rule:
**no claim of priority or uniqueness against anything outside this repo**
without external verification — this repo has no visibility into anyone
else's source.

### 3.1 MoltenVK / macOS Vulkan support

NV2A's Vulkan renderer runs on MoltenVK (Vulkan-on-Metal), with this
fork's flight-slot, dirty-tracking, and occlusion/report architecture on
top. MoltenVK itself is a known third-party library — not new. What's
fork-specific: upstream's own renderer source has **zero** MoltenVK/Apple
code (`git ls-tree -r upstream/master | grep -i moltenvk` and a full-text
grep of upstream's `renderer.c` for `moltenvk|apple|__APPLE__` both return
nothing) — this fork's specific architecture had to be adapted to
MoltenVK's constraints (single queue, unreliable
`VK_EXT_provoking_vertex`, per-CB visibility-buffer semantics, prefill
corruption) through real debugging — see the pink-tile and
visibility-buffer sagas in `xemu-failure-archaeology`. **Scope the credit**:
README names [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu) as the
"MoltenVK seed" (line 9) — the initial port isn't this fork's origination.
The defensible claim is the optimization/hardening layered on that seed,
not "first to run Vulkan on MoltenVK."
**Evidence today**: the upstream-absence check, the credit line, §3.5's
results. **Still needed**: any comparison against CosmicSnow/xemu itself
(not just upstream) is unverified here — flag as a hypothesis to check
before claiming precedence over the seed project.

### 3.2 Inline ARM64 DSP56300 JIT

A from-scratch basic-block JIT (`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`)
emitting native AArch64 for the two DSP56300 cores, embedded in the C
interpreter (`audio.dsp_jit.enabled`), coexisting with and taking
precedence over upstream's separately merged `dsp56300`-subproject engine
(`audio.use_dsp_jit`) via `dsp_want_external_jit_engine()`
(`hw/xbox/mcpx/apu/dsp/dsp.c:119-127`). Basic-block JITs for DSP-like ISAs
are a known technique in general. What's fork-specific and verified absent
upstream (`git ls-tree -r upstream/master | grep dsp56k_jit` → nothing) is
this exact application: pinning A/B accumulators, X/Y operands, and SR
flags into specific callee-saved AArch64 GPRs (x19-x28), multi-step
lazy-CCR-elimination lookahead, and write-set narrowing for an async
bit-exact diff validator — documented in numbered landed rounds in
`docs/dsp-jit-design.md`. The validation methodology itself (deterministic
translation implies one passing validation suffices; async 16-slot SPSC
ring so validation never stalls audio) is notable engineering in its own
right. **Evidence today**: extensive (design-doc rounds with measured
deltas, `PIN_AUDIT`-caught bugs tied to commit hashes). **Still needed**: a
protocol-documented, interleaved comparison against upstream's engine
specifically. The closest data point is one session's `XEMU_APU_PROF`
reading (JIT-both 25% / interpreter 22% / throttled-JIT 20% / upstream
engine 19% CPU on an attract-reel scene) — real, but one sample, not the
interleaved-with-variance standard §4 requires for a public claim.

### 3.3 MetalFX + MTLFXFrameInterpolator in the present chain

MetalFX spatial/temporal upscaling and macOS 26's `MTLFXFrameInterpolator`
(2x/4x) wired into the NV2A Vulkan renderer's present chain, async via
`MTLSharedEvent`, across a Vulkan-render -> Metal-upscale -> Metal-present
boundary. The frameworks are existing Apple APIs, not invented here.
Fork-specific: wiring them into an emulator's present chain sourced from a
Vulkan renderer via MoltenVK — confirmed absent upstream (no
metalfx/xemu-metal-equivalent paths in `upstream/master`). The iteration
depth in README (1920px-cap lift via a private-texture ring instead of a
shared IOSurface, GPU-paced steps via `presentDrawable:afterMinimumDuration:`,
monotonic-clock fixes, the "ships dark" real-depth opt-in) reads as mature,
multi-pass integration. **Evidence today**: extensive (~110 lines of dated
notes in README). **Still needed**: "rare among emulators" or "first to do
this" is an ecosystem claim this repo cannot check — no other emulator's
source is here. Treat as a hypothesis requiring external survey before any
"first"/"only" language ships publicly.

### 3.4 Savestate A/B benchmarking methodology

An interleaved, QEMU-savestate-anchored A/B protocol with a measured
static-scene noise floor of ±0.02 fps (method owned by `xemu-testing`).
Savestates load via the QEMU monitor socket, not CLI `-loadvm` (fails —
snapshots load before USB controllers attach), giving frame-identical
starts; runs interleave (A/B/A/B...) to cancel drift. A/B benchmarking
itself isn't new; what's specific and transferable is the
savestate-anchored frame-identical start, the interleaving discipline, and
a defended noise floor — a **method**, portable to any QEMU-based fork.
**Evidence today**: used for every shipped perf claim in this fork's
practice; rigor demonstrable by its own outputs (e.g. "3 interleaved
pairs... 38.57 +/- 0.80 fps"). **Still needed**: nothing structural — the
discipline is to always cite the full receipt (§4), not a bare number.

### 3.5 The TBDR render-pass discipline result (376 -> 14 passes/flip)

Bulk query-pool reset at command-buffer begin, in-pass occlusion-query
begin/end, and flip-deferred report delivery collapsed render passes from
376 to 14 per flip and raised a heavy in-game bench scene's fps (commit
`6bfbc22863`: "38.57 +/- 0.80 fps ... i.e. +144%," 3 interleaved pairs,
same binary, `XEMU_REPORTS_SYNC` toggled). "Pass count is first-order on a
TBDR" is known Apple-GPU domain knowledge (`xbox-hardware-reference`); the
fork-specific part is recognizing `vkCmdResetQueryPool`'s in-pass illegality
was forcing a pass teardown per query rotation, and re-architecting around
that specific legality constraint — a narrow interaction a native-Vulkan
(non-Apple-GPU) developer might never trip over.

**A worked lesson found while writing this skill.** The README prose for
this same change (same commit `6bfbc22863`) stated **35.6 fps** and
**"2.25x"** — different from the commit message's own **38.57 +/- 0.80
fps** and **"+144%"** (≈2.44x). Both were written in the same commit and
sat unreconciled until 2026-07-11, when the README (both the Changes
bullet and the failed-experiments row) was corrected to the receipt
numbers. The commit-message number carries the full receipt (3
interleaved pairs, mean ± stdev, explicit legacy-vs-deferred split); the
README figure had none attached. **Cite the commit-message number**
(`38.6 fps`, `+144%`, `git show 6bfbc22863`) — never average two figures
or pick whichever sounds better. This is the
concrete case for why §4 exists: even this fork's own flagship number
needed a primary-source check before it was safe to quote.

---

## 4. Claiming standards

No performance number leaves internal notes for public copy (README
headline, release notes, a blog post, a comparison table) without all of
the following. Stricter than day-to-day dev-loop measurement (see
`xemu-validation-and-qa` for those thresholds) — public claims are
permanent and re-quotable, so the receipt must survive someone else trying
to reproduce it.

**The protocol receipt, every time:**
- **Config**: the specific settings in effect (`display.renderer`,
  `display.metalfx_mode`, `display.frame_interpolation`,
  `audio.dsp_jit.enabled`/`audio.use_dsp_jit`,
  `display.quality.surface_scale`) — state them, never assume defaults
  match on either side of a comparison.
- **Title + scene identity anchor**: draws/flip (or equivalent) at the
  moment of measurement, not just a title name — Azurik's attract reel
  alone spans 3 to 350-390 draws/flip.
- **Build hash**: `git describe`/commit hash for every build compared,
  including any baseline.
- **MoltenVK version + UUID**: `build.sh` provenance-logs this at bundle
  time; MoltenVK version is itself a measured perf variable in this
  project's own history — never omit it.
- **Interleaved run order** (A/B/A/B..., never A-then-B) —
  `xemu-testing` protocol.
- **Variance**: mean ± stdev over N >= 3 runs (this fork's own convention,
  e.g. "3 interleaved pairs"). Static-scene noise floor ±0.02 fps,
  live-route variance ±5 fps — use enough pairs for the scene type per
  `xemu-validation-and-qa`'s thresholds.

**Never "significantly," "much faster," "a lot smoother."** Each phrase is
a number that hasn't been measured yet, or is being hidden because it's
less impressive stated plainly. Say the number and its receipt, or say
"not yet measured."

**Upstream-baseline capture protocol** (for any "vs upstream" claim):
1. Build upstream in a **physically separate** location (a second `git
   worktree`/clone at `upstream/master`) — never this repo's own
   `build/`/`dist/` (`./build.sh` wipes `Contents/Resources` on every
   rebuild, taking a planted portable config with it).
2. Build upstream with **its own defaults** — don't carry this fork's
   `xemu.toml` over wholesale. Key parity isn't given: `use_dsp_jit`
   exists upstream too (merged `dsp56300`-subproject engine), but
   `audio.dsp_jit.enabled`, `display.metalfx_mode`, and
   `display.frame_interpolation` don't exist upstream at all (checked:
   `git show upstream/master:config_spec.yml` has none of those keys as
   of this writing).
3. Use the **same savestate/hdd qcow2** for both (clone with `cp -c` for
   APFS copy-on-write first). **USB-topology caveat**: snapshots embed
   per-pad USB device trees; unplugging/replugging pads between the two
   builds' sessions can make `loadvm` fail or bind the wrong pad — verify
   topology matches (`xemu-testing`) before trusting the comparison.
4. Run the **same interleaved protocol** against both builds, same
   machine, same session (cancels thermal/background-load drift).
5. Report **both builds' full receipts side by side** — never a freshly
   measured fork number next to a remembered or old upstream number.

---

## 5. Upstream relationship

**Merge cadence.** Last merge from `xemu-project/xemu`: `fd467e02b9`,
"Merge upstream/master: DSP engine abstraction + dsp56300 JIT engine"
(2026-07-03). This fork carries 284 commits ahead of that merge base
(`git log --first-parent --oneline upstream/master..HEAD | wc -l`,
re-derived 2026-07-11; current `HEAD` = `7e2e6e7256`, latest tag
`v0.10.2`).

**Plausibly upstreamable** (`README.md` "Building for Windows" ~line
31-44 — these already run, proven, on upstream's own Windows CI paths in
this fork, so they're technically portable *today*): the Vulkan renderer's
cross-platform work (flight slots, dirty hashing + spatial index, zeta
shape-switch fast path, targeted vertex-conflict waits, `VkPipelineCache`
persistence, draw-path dedup, tight barriers, query drain at slot
reclaim), the APU work (LUTs, batched register reads, SVF cache, mono
paths, SSE2 mix kernels), the pfifo/BQL fixes, `XEMU_NV2A_NSPROF`, and the
helper-based hard FPU on x86_64. "Plausibly upstreamable" means "already
proven cross-platform in this fork's own CI," **not** "confirmed accepted
upstream" — no PR history against `xemu-project/xemu` exists in this repo
to check; treat acceptance as a hypothesis, not a fact.

**macOS-only** (same paragraph, won't upstream as-is): MetalFX, the
Metal/IOSurface presentation backends and their async present chain,
CoreAudio, vDSP, the ARM64 DSP JIT, and the AArch64 inline x87 FPU
(Windows uses the bit-equivalent helper-based hard FPU instead). The
MoltenVK CPU-primitive-emulation paths (§2.3) simply don't activate on
native Vulkan drivers — no portability question there.

**Naming/mediation constraints that keep merges cheap.** The fork's inline
JIT lives under the `dsp56k_jit_*` namespace
(`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.[ch]`), structurally
distinct from upstream's own `dsp_jit_*`-prefixed `dsp56300`-subproject
engine (`hw/xbox/mcpx/apu/dsp/dsp_jit.c`, `subprojects/dsp56300/`) — why
`fd467e02b9` merged cleanly despite both forks independently building a
DSP JIT: no symbol collision, and `dsp_want_external_jit_engine()`
(`dsp.c:119-127`) makes precedence explicit in one place instead of
scattered `#ifdef`s. Full rules for what a change like this owes (naming,
evidence, README duty) before landing: `xemu-change-control` — this skill
only explains *why* the convention exists.

---

## Provenance and maintenance

Re-run before trusting a number or status label here after the tree moves:

- Merge cadence/fork size: `git log -1 --format='%h %ad %s' --date=short fd467e02b9` and `git log --first-parent --oneline upstream/master..HEAD | wc -l`
- Future Vectors entries still listed as-is: `grep -n "^## Future vectors" -A 90 README.md`
- STALLED resolved status: `grep -n "VK_FINISH_REASON_STALLED" hw/xbox/nv2a/pgraph/vk/reports.c` (expect exactly one caller — `reports.c:322` as of 2026-07-11 — gated by `sync_mode`/`XEMU_REPORTS_SYNC`)
- README/commit fps figures agree (reconciled 2026-07-11): `grep -n "38.57" README.md; git show --format='%B' -s 6bfbc22863 | grep -i "fps\|144%"` (a reappearing `35.6`/`2.25x` in README is new drift)
- MTLResidencySet still blocked: `grep -n -A6 "Phase 5 note" hw/xbox/nv2a/pgraph/vk/metalfx_upscale.m`
- DSP JIT fusion still deferred: `grep -n "8 (remaining)" docs/dsp-jit-design.md`; x28's real owner is the SR pin (design-doc "x28 remains free" rows corrected 2026-07-11): `grep -n "DSP56K_JIT_SR_PIN_REG" hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- Provoking-vertex gating still capability-based: `grep -n "supports_geometry_shaders =" hw/xbox/nv2a/pgraph/vk/instance.c`
- BINK still has no in-repo hook: `grep -rliE '\bbink\b' --include='*.c' --include='*.h' .` (expect 0 hits; a loose `grep -rli bink` additionally hits `include/libdecnumber/` substring noise)
- Upstream still lacks fork-only config keys: `git show upstream/master:config_spec.yml | grep -n "dsp_jit:\|metalfx_mode:\|frame_interpolation:"` (expect no matches)
- Upstream still has no MoltenVK/Apple-aware renderer code: `git show upstream/master:hw/xbox/nv2a/pgraph/vk/renderer.c | grep -ni "moltenvk\|apple"` (expect no matches)
- CosmicSnow credit line still present: `grep -n "CosmicSnow" README.md`
