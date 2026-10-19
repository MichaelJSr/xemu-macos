# xemu-macos working agreement

The fork's main branch is `master`; commit straight to it.
`AGENTS.md` applies only to pull requests aimed at upstream
xemu-project/xemu.

On the `windows` branch (the Windows test machine): commit to `windows` only. Never commit to, merge into or push `master`, and never tag. `docs/windows-test-plan.md` maps the macOS rules below to Windows.

## Keep the loop fast

- Build with `ninja -C build qemu-system-i386`. Use `./build.sh` only for
  a fresh tree or a packaged bundle.
- Test on a bundle clone, headless by default (`XEMU_HEADLESS=1` or
  `null`, frames via QMP `screendump`). Recipes are in the `xemu-operate`
  skill.
- Verify in proportion to risk. Every change needs a clean compile. A
  behaviour change needs one functional run. A perf claim needs an A/B:
  `scripts/bench-det.py -n 4` (deterministic host cycles per frame,
  which shrugs off host load) for CPU, TCG and APU work, plus the
  windowed `scripts/bench-savestate-ab.sh <tag> <VAR> 2 45` when the
  renderer or real-time pacing is involved. Never wait for a cold box;
  quote the load. Skip soaks, refuters and multi-agent reviews unless
  the change is risky or the number will be published.
- Commit one logical change as `<subsystem>: <short description>` plus a
  short body. Docs ride in the same commit: README for user-visible flags
  or features only. No separate docs or skills bookkeeping commits, and
  no measurement logs in skills.
- Add an `XEMU_*` switch only for a normal-mode change that the
  deterministic goldens can't verify. No other legacy-path or opt-out
  hatches (owner decision 2026-10-15).

## Hard rules

- Never touch the personal setup (`~/Documents/Xemu`,
  `~/Library/Application Support/xemu`) or the `../fixtures` baselines.
  Never launch `dist/xemu.app` itself for tests; launch a clone.
- Mute every test run (`[audio] volume_limit = 0`), or run it headless.
- `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` stays `0`. Published tags
  are immutable.

## Skills (load on demand)

`xemu-operate` (build, run, test, ship), `xemu-internals` (invariants,
triage), `xemu-history` (dead ends, open work).
