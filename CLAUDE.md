# xemu-macos working agreement

The fork's main branch is `master`; commit straight to it.
`AGENTS.md` applies only to pull requests aimed at upstream
xemu-project/xemu.

On a Windows test machine, `docs/windows-test-plan.md` maps the rules
below to Windows paths and tools. The `windows` branch is kept frozen
as the 2026-10-18 comparison point.

## Keep the loop fast

- Build with `ninja -C build qemu-system-i386`. Use `./build.sh` only for
  a fresh tree or a packaged bundle.
- Test on a bundle clone, headless by default (`XEMU_HEADLESS=1` or
  `null`, frames via QMP `screendump`). Recipes and the change-type map
  are in the `xemu-operate` skill (Test).
- Per change, about 1 min. Before editing, snapshot the base:
  `scripts/refresh-clone.sh /tmp/b.app build/qemu-system-i386`. Then:
  - every change: a clean compile. Docs, skills and CI stop there; a
    script gets its selftest or one run;
  - hw/xbox, libfatx and eeprom: the meson xbox suite;
  - anything outside `ui/xui`, docs, `scripts`, `.github` and the
    skills: `scripts/bench-det.py --check goldens,d98
    new=build/qemu-system-i386` (~15-20 s) on every step of a batch,
    plus the refuter arm that xemu-internals names for an invariant the
    change touches;
  - renderer and shader generators: also the offscreen cold boot and
    heavy-f1 f300 against `/tmp/b.app`, judged by pixel-diff counts;
  - paths only normal mode runs: one muted windowed run. PFIFO
    threading or locking is risky: it adds the debug-build normal-mode
    runs.

  Only the windowed and offscreen rows wait for the end of a batch.
- A perf claim is one A/B of about 5 min: `scripts/bench-det.py -n 4`
  (deterministic host cycles per frame, which shrugs off host load) for
  CPU, TCG and APU work, or the windowed `scripts/bench-abx.sh` for the
  renderer and real-time pacing, judged by its headroom lines when the
  receipt says CAPPED. Not both. Never wait for a cold box; quote the
  load. Skip soaks, extra pairs and multi-agent reviews unless the
  change is risky or the number will be published.
- Pre-release takes about 15 min on a clone of the CI universal zip
  (`docs/RELEASING-macos.md`). A new feature's heavy first validation
  is one-time and adds no standing gate.
- Commit one logical change as `<subsystem>: <short description>` plus a
  short body. Docs ride in the same commit: README for user-visible
  features only (harness interfaces go in docs/automation.md). No
  separate docs or skills bookkeeping commits, and no measurement logs
  in skills.
- Add an `XEMU_*` switch only for a normal-mode change that the
  deterministic goldens can't verify. No other legacy-path or opt-out
  hatches (owner decision 2026-10-15).

## Hard rules

- Never touch the personal setup (`~/Documents/Xemu`,
  `~/Library/Application Support/xemu`) or the `../fixtures` baselines.
  Never launch `dist/xemu.app` itself for tests; launch a clone.
- Never execute an xemu binary without `-config_path <scratch toml>`,
  not even `--version`, `-help` or a bare path left in a compound
  command: it boots the owner's config and HDD and re-saves the toml.
  Inspect a binary with `strings`/`otool`; launch it through
  `scripts/xemu_lease.py` or the bench scripts.
- Mute every test run (`[audio] volume_limit = 0`), or run it headless.
- `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` stays `0`. Published tags
  are immutable.

## Skills (load on demand)

`xemu-operate` (build, run, test, ship), `xemu-internals` (invariants,
triage), `xemu-history` (dead ends, open work).
