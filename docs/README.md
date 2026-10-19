# Fork documentation

Most of `docs/` is upstream QEMU's Sphinx tree (`*.rst`, `*.txt`,
`conf.py`, `devel/`, `system/`, ...). The fork leaves it alone so
upstream merges stay clean. The fork's own documents are the Markdown
files indexed here; Sphinx builds `.rst` only, so they never reach the
QEMU manual.

Agent-facing operating knowledge lives in three lean skills under
[`.claude/skills/`](../.claude/skills/) (xemu-operate, xemu-internals,
xemu-history), not here.

## Where a fork doc goes

| Folder | Holds | Leaves when |
|---|---|---|
| `docs/` (root) | Living documents of record, kept current as the code changes. Each has to say why it is here (next table). | It stops being maintained: move it to `Finished/` |
| [`Campaigns/`](Campaigns/) | Multi-session work in progress: designs with open phases, campaign status, their backlogs. A paused campaign stays here with a dated status line. | Every phase lands or is closed: `Finished/` |
| [`Future/`](Future/) | Proposals and parked experiments with a real write-up that nobody is working on. A one-paragraph candidate belongs in `roadmap.md` instead. | Work starts: `Campaigns/`. Killed: `Finished/`, with the reason |
| [`Finished/`](Finished/) | Closed records: landed designs, audits, verdicts, reviews. Bodies stay as written; a status line under the title gives the outcome, date, landing commit and where any follow-up lives. | Never. A reopened topic gets a new `Campaigns/` doc that links back |

## Root documents

| Doc | Why it is at the root |
|---|---|
| [`optimizations.md`](optimizations.md) | The optimization ledger: every landed change with receipts, the runtime knob catalog, Failed / reverted experiments, Settled vectors. Every landing updates it. |
| [`roadmap.md`](roadmap.md) | The open candidates, each with its decision inputs and bar. Closed entries leave it for the ledger and `Finished/`. |
| [`RELEASING-macos.md`](RELEASING-macos.md) | The release runbook, used every release. |
| [`windows-gating-audit.md`](windows-gating-audit.md) | The cross-platform gating register and the list of changes that still need real-hardware validation. The release runbook and roadmap item 5 depend on it. |
| [`kosmickrisp.md`](kosmickrisp.md) | KosmicKrisp, the default Vulkan driver on macOS 26 with MoltenVK as fallback: how the driver is chosen, the strike marker, `XEMU_VK_DRIVER`, building KK, the gate, open items and the measurements. |
| `README.md` | This index. |

## Campaigns

- [`windows-port-2026-09.md`](Campaigns/windows-port-2026-09.md): the native Windows port. Paused since 2026-09-08.
- [`windows-audit-2026-09-summary.md`](Campaigns/windows-audit-2026-09-summary.md) and its source data [`windows-audit-2026-09.json`](Campaigns/windows-audit-2026-09.json): the port's audit backlog.

## Future

- [`moltenvk-optimization-experiments.md`](Future/moltenvk-optimization-experiments.md): the MoltenVK private-API build and pipeline prewarm experiments.

## Finished

- [`dsp-jit-design.md`](Finished/dsp-jit-design.md): the ARM64 DSP56300 JIT design and phased roadmap; every phase landed. Also the template for new design docs.
- [`fork-optimization-audit-2026-08.md`](Finished/fork-optimization-audit-2026-08.md): the fork-wide optimization audit, closed 2026-08-04.
- [`lockless-mmio-verdict.md`](Finished/lockless-mmio-verdict.md): BQL-free dispatch for the remaining MMIO blocks. Closed, do not build.
- [`pgraph-lockless-audit.md`](Finished/pgraph-lockless-audit.md): BQL-free PGRAPH MMIO dispatch, shipped 2026-07-11.
- [`roadmap-closed-items.md`](Finished/roadmap-closed-items.md): full write-ups of closed and parked roadmap candidates.
- [`subpage-gate0-prediction.md`](Finished/subpage-gate0-prediction.md): sub-page dirty tracking, from sizing to results, with the original design as an appendix. Shipped 2026-07-11 and 2026-07-18.
- [`windows-wave1-review-2026-09-08.md`](Finished/windows-wave1-review-2026-09-08.md): review of the Windows wave-1 work, closed with v0.13.3.
- [`xpage-design.md`](Finished/xpage-design.md): cross-page TB chaining soundness design, shipped 2026-07-11.
