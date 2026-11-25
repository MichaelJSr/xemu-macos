# Fork documentation

Most of `docs/` is upstream QEMU's Sphinx tree (`*.rst`, `*.txt`,
`conf.py`, `about/`, `devel/`, `system/`, ...); the fork leaves it alone
so upstream merges stay clean. The fork's own docs are the Markdown
files below. Agent operating knowledge lives in the skills under
[`.claude/skills/`](../.claude/skills/) (xemu-operate, xemu-internals,
xemu-history).

## User docs

- [`../README.md`](../README.md): what the fork changes, download,
  requirements, the recommended `xemu.toml`, credits.
- [`troubleshooting.md`](troubleshooting.md): problems players hit, and
  the settings that trade quality for speed.
- [`kosmickrisp.md`](kosmickrisp.md): the opt-in KosmicKrisp Vulkan
  driver: selection, strike marker, `XEMU_VK_DRIVER`, the Auto-back gate.

## Developer and release docs

- [`building.md`](building.md): building on macOS, Windows and Linux,
  build knobs, how Vulkan is loaded at runtime, build failures.
- [`automation.md`](automation.md): the harness interface: headless
  modes, QMP commands and events, input pipe, deterministic mode,
  gdbstub, `XEMU_*` variables, PAL 50 Hz, guest RAM.
- [`optimizations.md`](optimizations.md): the ledger: every landed
  change with its receipt, the `XEMU_*` knob catalog, failed and
  settled experiments. Every landing updates it.
- [`roadmap.md`](roadmap.md): open candidates with their bars; closed
  ones move to the ledger and `Finished/`.
- [`RELEASING-macos.md`](RELEASING-macos.md): the release runbook and
  its pre-release tier.
- [`windows-gating-audit.md`](windows-gating-audit.md): fork changes
  that Windows compiles, and what still needs real Windows hardware.
- [`windows-test-plan.md`](windows-test-plan.md): the runbook for
  real-Windows-hardware sessions; CLAUDE.md sends Windows sessions here.

## Records of finished work (`Finished/`)

A closed record keeps its body as written; a status line under the
title gives the outcome, date, landing commit and where any follow-up
lives.

- [`dsp-jit-design.md`](Finished/dsp-jit-design.md): the ARM64 DSP56300
  JIT design; every phase landed. Template for new design docs.
- [`fork-optimization-audit-2026-08.md`](Finished/fork-optimization-audit-2026-08.md):
  the fork-wide optimization audit, closed 2026-08-04.
- [`lockless-mmio-verdict.md`](Finished/lockless-mmio-verdict.md):
  BQL-free dispatch for the remaining MMIO blocks; closed, do not build.
- [`pgraph-lockless-audit.md`](Finished/pgraph-lockless-audit.md):
  BQL-free PGRAPH MMIO dispatch, shipped 2026-07-11.
- [`roadmap-closed-items.md`](Finished/roadmap-closed-items.md): write-ups
  of closed and parked roadmap candidates.
- [`subpage-gate0-prediction.md`](Finished/subpage-gate0-prediction.md):
  sub-page dirty tracking, from sizing to results; shipped 2026-07-11
  and 2026-07-18.
- [`windows-port-2026-09.md`](Finished/windows-port-2026-09.md): the
  native Windows port to its 2026-10-18 merge, with the audit summary.
- [`windows-wave1-review-2026-09-08.md`](Finished/windows-wave1-review-2026-09-08.md):
  review of the Windows wave-1 work, closed with v0.13.3.
- [`xpage-design.md`](Finished/xpage-design.md): cross-page TB chaining
  soundness design, shipped 2026-07-11.

## Proposals (`Future/`)

Proposals with a real write-up that nobody is working on; a
one-paragraph candidate goes in `roadmap.md` instead.

- [`moltenvk-optimization-experiments.md`](Future/moltenvk-optimization-experiments.md):
  the unbuilt MoltenVK private-API build, and the pipeline prewarm
  experiment (shipped 2026-10-24).

## Formal models (`spin/`)

- [`spin/`](spin/): upstream QEMU's SPIN (Promela) models of
  `aio_notify`, TCG exclusive sections and the Win32 `QemuEvent`. The
  fork does not edit them.
