# Azurik checks (the Azurik_Modkit harness's packs, pre-release tier)

These are the Azurik_Modkit harness's correctness checks for this fork, from the PUBLIC side of its part-9 test
pack (2026-10-16). `scripts/azurik-pack.py` runs them in one command before a release, on a clone of the release
candidate's CI zip:

```sh
AZURIK_FX=<the Modkit's D98 fixtures folder> python3 -I scripts/azurik-pack.py rc=/tmp/rc.app
```

Outside a release, a change to an interface the Modkit drives (hooks, run-until, the input pipe, checkpoints, the
block or voice log, the WAV) runs the case that drives it, e.g. `--cases 01-w2`, `--cases 10` or `--cases ckpt`.
Pack 03's RAM goldens run per change in `bench-det.py --check goldens,d98`.

Each pack's `README.md` is the harness's own, copied unchanged, with the full recipe: hooks, addresses and start
steps. In those READMEs, "we" and "our side" mean the Modkit harness. Its drivers (`ours/`, `heavy_scenes.py`,
`bink_movie.py`), its LOCAL path notes and its result files are not copied here.

**Never commit:** LOCAL paths, drives (qcow2) or their states, ISOs, EEPROMs, the owner's recordings, WAVs,
screenshots, RAM dumps, per-tick reads, or anything positional. Record results as counts, tags, addresses and
sha256[:12] only. The driver keeps none of them: each case's sandbox goes when it ends.

## Cases

Each case is one fresh headless xemu (null renderer, `-S -icount shift=2,align=off,sleep=off`), launched and
cloned as bench-det's runs are, paused at its state and driven over QMP. Each case's expected seconds live in the
driver's case table, which `--list` prints; they also set the case's kill limit.

**Core (the default): 40 cases, ~175 s of xemu time by `--list`. Not yet timed at `-j 3` with this driver.**

| Case | What it drives | Oracle |
|---|---|---|
| `01-w2` | `vanilla-rz-new`; gdbstub writes at run-until halts (the R7 level request to w2, the popup bits, the pace cvar); the three hooks (pad and camera written per tick, CORE read at the RETs); one 6,000-frame advance, stopped by the empty pad queue | the start check, 1,067 CORE digests (`replays.py check`), 3,202 records with 0 dropped, RAM at the start halt and the end |
| `02-<case>` (11) | The D98 cases (walk, roll, jump, swim, beach-jump, beach-flaps, enemy-hits, ledge-j14/j16/j28/j30): the per-tick hook, run-until row by row, the schedule's lines on the pipe tagged `@F+1` | `physics_check.py`'s numbers, one record a tick, start and end RAM |
| `03-e2`, `03-e7` | Pack 03's collect hooks (`scenes.json`), with the scene's input to +600 | e2: one 0x53750 path, `levels/earth/e5` at +245. e7: `apply_damage` 0 times on the player and 12 on other critters, the first at +207. The start RAM, and the +300/+600 RAM still `det-goldens.json`'s |
| `04-<level>` (22) | Each level's recorded warp from `vanilla-beach-shield5`, under the block log | RAM, block-log rows and bytes per drive and op (all `ok`), the `query-blockstats` deltas |
| `05` | `death.bik`: `movie.json`'s request and hooks, run-until 0x53750 twice with blockstats at each, then F2 + 900 | F1 = 4 (`bink:death.bik`, NULL spot), F2 = 6046 (`levels/death/d2`, `D1_D2_Fall`); DVD 8,192 B to F1 and 50,298,880 B from F1 to F2 (50,307,072 B in all); HDD 65,024 B written, all before F1; RAM `9ab9c0158f03` |
| `10-fork` | e6's song: `song.json`'s hook, the 213 recorded lines, the voice log and the WAV; two replays in one process, the second loadvm into a warm TB cache | per replay: RAM `3e23917e16ee`; WAV `14bf0e233fe9`, 2,531,328 samples; voice log 395 rows (stop 123: 72 `guest`, 51 `end`), 47 start rates, `3e701e5179ff` with frames from the load; the engine named in the log |
| `10-upstream` | One replay on dsp56300's JIT: the shipped default where the fork's JIT isn't built (Windows, Linux x86_64, the universal zip's Intel slice), which no CI leg runs | as `10-fork` |
| `ckpt` | `vanilla-rz-e6-song` with pack 03's x24 lines: `xemu-checkpoint-save`, 300 frames, a save against that base, two incremental restores (the second through the dirty log), a restore of the second checkpoint, `xemu-run` from the first, a second loadvm | every 300-frame RAM is `det-goldens.json`'s e6 f300; `pages-missed` 0 on every verified save and restore |

**On demand (`--all`, or by id):**

| Case | What it drives | Oracle |
|---|---|---|
| `01-w1` | The hooks plus a mid-advance pipe press: tick 415's popup stalls the updates, and A goes in after 120 vblanks with no entry record | the start check, 475 CORE digests, one press at tick 415 (its end RAM follows the press: no oracle) |
| `02-cold-jump` | Power-on to `levels/training_room` by bench-det `--timeline`'s menu route, through run-until: START at `prophecy.bik` and at the title, DPAD_LEFT, A, then A on a stall until 20 s of arena play; one jump | the jump's numbers; no save preview on the D98 EEPROM; the level; default.xbe's `.text` in RAM, 979,408 B with 0 differing |
| `05-straight` | One 6,946-frame advance instead of the cut | the cut run's paths, totals and RAM |
| `10-interp` | The DSP interpreter (neither JIT) | as `10-fork` |

## Flags

- `--cases ID[,ID]`: case ids, or a pack prefix (`02`, `04`, `10`) for that pack's core cases (all of them with
  `--all`). `--list` prints the selection with each case's expected seconds and launches nothing.
- `--all`: adds the on-demand cases.
- `--renderer offscreen`: pack 05 through Vulkan (MoltenVK on a portable clone with cold caches), for PVIDEO and
  surface work; its RAM at F2 + 900 is `10b5a35aa66b`. The other packs stay null.
- `--dsp ENGINE[,ENGINE]`: pack 10's engines, `fork`, `upstream` and `interp` (default `fork,upstream`), for DSP
  JIT work or an upstream DSP merge: `--cases 10 --dsp interp,upstream`.
- `-j N` (default 3) cases at once, longest first. `--timeout-scale X` (default 1; 4 for a debug build): a case's
  xemu is killed at 10x its expected seconds plus 60 s, times X. `--out FILE` appends one JSON line per case and
  arm.
- Arms are bench-det's: `NAME=APP`, a bundle clone or a bare binary (`new=build/qemu-system-i386` runs in a
  `refresh-clone.sh` clone of `--bundle`, default `dist/xemu.app`, which is only ever cloned); `K=V` goes to
  xemu's environment and `TABLE.KEY=V` into its config (a key xemu refuses fails the case). An arm's
  `sys.mem_limit` picks its `det-goldens.json` table; under any other `sys.*` key, 03's RAM is checked only for
  agreement between arms and `ckpt`'s against its own first run. Add a `base=` arm only to attribute a moved
  oracle.

**Output:** one line per case and arm: OK with its end RAM, MOVED with each oracle that moved and its expected
value, or FAIL with why; a failed case keeps its xemu log as `/tmp/xbd-fail.<pid>.<arm>-<case>.log`. Then the
counts and the load, and exit 0 (all OK), 2 (arms disagree), 4 (an oracle moved) or 1 (a case failed), as
`bench-det.py --check`.

## Where the oracles live

- **The packs' JSON:** 01's start checks and per-tick CORE digests (`replays.json`), 02's numbers
  (`physics.json`), 03's start RAM and check hooks (`scenes.json`), 04's lines, RAM and block totals and 10's
  lines, RAM, WAV and voice log (each `results-v0.15.json`), 05's request and hooks (`movie.json`).
- **`scripts/det-goldens.json`'s `d98` table:** pack 03's RAM at +300 and +600 per scene, for `bench-det.py
  --check d98`, `03-e2`, `03-e7` and `ckpt`.
- **The table at the top of `scripts/azurik-pack.py`:** what the packs give only in prose: 01's RAM at the start
  halt and the end, 02's start and end RAM per case, cold-jump's route facts and `.text`, 03's hook results, 05's
  frames, bytes and RAM, 10's stop reasons.

An intentional move edits those values in the same commit, with the reason, and the release note names them for
the Modkit.

## Notes

- **Setup:** the D98 fixtures (the drive, `vanilla.iso`, `eeprom.bin` and `xemu.toml`) stay on the owner's
  machine, read-only. The driver clones the drive and EEPROM for every case and the disc once per batch; never
  open a drive in place, and never `savevm`.
- **Keyboard maps:** the keyboard pad is on port 2, with the D98 map (A 81, Y 44). Packs 04 and 10 were
  recorded in the Modkit's sandbox, whose scratch config binds xemu's 25 keyboard-pad inputs, in
  `config_spec.yml` order, to scancodes 232-256; the driver writes that map into their runs' configs.
- **02's `@F+1`:** `physics.json` expects v0.15's icount numbers for `enemy-hits` (hits at rows 188 and 425) and
  `ledge-j14` (it ends on the ledge); s6's real-time numbers stay as `s6_expect`. `enemy-hits` follows input
  timing: lines tagged `@F+2` move its hits to 187 and 361.
- **The hooks suite** (`test_b3.py`, outside the repo) reads the w2 string from a fixed path: `mkdir -p
  /tmp/xt/hooks/b3 && cp tests/azurik/01-replays/w2-segments.json /tmp/xt/hooks/b3/` (`332c62540d14`). On v0.15
  it gave 19 PASS and 0 FAIL.
- **03's scenes aren't bench anchors:** vanilla Azurik presents at 30 fps, and all four scenes hold 30.0-30.2
  fps windowed and offscreen, so a windowed A/B can't show a speedup on them; `azurik-heavy-f1` and `-w2` stay
  the windowed anchors. e7's f600 covers combat and is x87-sensitive. e6 and e7 read nothing from the DVD; the
  new game and e2 each stream about 36-41 MB from it.

## Not adopted

| Pack | Why it isn't here | v0.15 headline |
|---|---|---|
| 06 europe-pal50 | It needs the Europe disc (bench-det picks Europe's VAs itself and `--route dpad_left,a` now moves on when no save preview appears). | PAL50 EEPROM: F = 25. Fixture EEPROM: F = 30. ~30.06 player ticks per guest second at 60 Hz. |
| 09 grown-iso-io | It needs a local 2.8 GB ISO built by the Modkit. | +75,238,912 B of HDD writes, all in the logos phase. Repeatable block logs. |
| 07 owner-recording | Local-only soak. | Sections 50 and 81 end on `3edb08f3c2ea` and `648e70df76c4` in two processes, with lines sent as `@<frame>`. |
| 08 v22-boot-hang | Soak. | 0 of 10 cold boots hung. All reached `0x5F748` at frame 1350, with identical block logs. |

The harness keeps all ten packs under its own evidence tree.
