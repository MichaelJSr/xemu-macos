# Azurik checks (the Azurik_Modkit harness's pack, fast tier)

These are the Azurik_Modkit harness's correctness checks for this fork. They come from the PUBLIC side of
its part-9 test pack (2026-10-16). v0.15's results, measured on 2026-10-17, are below. Run them before a
release, or after CPU/JIT, x87, GPU or timing work. They are not part of the per-change loop:
`bench-det.py -n 4` and `bench-savestate-ab.sh` stay as they are.

| Pack | What it checks | Files here | v0.15 run time |
|---|---|---|---|
| `01-replays` | Two per-tick input replays by set hooks, with a CORE digest per tick (sha12 of the body centre, state and HP) and w2's end RAM | `replays.py check`, `replays.json` (oracle), the w2 and w1 strings | ~25 s each |
| `02-physics` | 12 player-control cases: walk, roll, jump, swim, flaps and the shield drop, enemy hits, ledges, cold-boot jump | `physics_check.py`, `physics.json` | ~30 s for the 11 D98 cases in one process, plus ~17 s for cold-jump |
| `03-heavy-scenes` | Four vanilla scenes from a plain load with pad input only: RAM at +300/+600, plus two hook checks | `scenes.json` | ~5-10 s per scene in bench-det |
| `05-bink-movie` | `death.bik` in game (6,042 vblanks of Bink), then the d2 load | `movie.json` | ~40 s to F2 |

Each pack's `README.md` is the harness's own, copied unchanged, and holds the full recipe: hooks, addresses and
start steps. In those READMEs, "we" and "our side" mean the Modkit harness. Its drivers (`ours/`,
`heavy_scenes.py`, `bink_movie.py`), its LOCAL path notes and its result files are not copied here. Times were
measured at 1-minute load 45-70.

**Never commit:** LOCAL paths, drives (qcow2) or their states, ISOs, EEPROMs, the owner's recordings, WAVs,
screenshots, RAM dumps, per-tick reads, or anything positional. Record results as counts, tags, addresses and
sha256[:12] only.

## Setup

The D98 fixtures (the drive, `vanilla.iso`, `eeprom.bin` and `xemu.toml`) stay on the owner's machine and are
read-only. Clone them with `cp -c` for every run. Never open a drive in place, and never `savevm`.

```sh
FX=${AZURIK_FX:?}                      # the Modkit's D98 fixtures folder
APP=/tmp/<scratch>/clone.app           # a bundle clone, never dist/xemu.app
S=$(mktemp -d /tmp/xt-azurik.XXXXXX)
cp -c "$FX/vanilla.iso" "$S/dvd.iso"
sed "s#^dvd_path = .*#dvd_path = '$S/dvd.iso'#" "$FX/xemu.toml" > "$S/xemu.toml"
export XEMU_TEST_CONFIG=$S/xemu.toml   # bench-det clones the HDD and EEPROM, mutes, stubs the pads
```

The keyboard pad is on port 2, with the D98 map: A is 81, Y is 44.

Packs 01, 02 and 05, and 03's checks, run over QMP. The repo has no driver for them yet. Use one xemu per
run, set up the way `bench-det.py`'s `scratch()` and `run_once()` do it: a per-run clone and config, a
`scripts/xemu_lease.py` launch, `XEMU_HEADLESS=null`, `-S -icount shift=2,align=off,sleep=off`, then `stop`
and HMP `loadvm <tag>` (the VM stays paused). After that, apply the pack's hooks with the main README's
"Headless mode (automation)" commands: `xemu-hooks-set`, `xemu-run-until`, `xemu-frame-advance` and
`xemu-ram-digest`.

## Expected results on v0.15

### 01. replays

- **Start:** `vanilla-rz-new`, then the README's start steps 1-6.
- **Hooks:** the three hooks, then one `xemu-frame-advance {"frames": 6000}`.
- **Check:** `python3 -I 01-replays/replays.py check <name> run.json`.

| Replay | Start check | `check` | RAM |
|---|---|---|---|
| `w2-walk-pickup` | `7548494a88eb` | 1067 of 1067 ticks, `first_diff` null | `aad6cc27fbb1` at the start halt, `7e4e74cc76a6` after the last tick |
| `w1-seg0` | `c6af3ec882b3` | 475 of 475 ticks, `first_diff` null | end RAM is not an oracle |

- **w2:** the run stops on the empty pad queue at hit 1067 (pc `0x8C230`). It gives 3,202 records with 0
  dropped.
- **w1:** a popup holds tick 415's update. Send A (`@F+1 down 81`, `@F+10 up 81`) through `XEMU_INPUT_PIPE`
  after 120 vblanks with no new entry record. One press clears it.

**The hooks suite** (`test_b3.py`, outside the repo) reads the w2 string from a fixed path. Place it first:

```sh
mkdir -p /tmp/xt/hooks/b3 && cp tests/azurik/01-replays/w2-segments.json /tmp/xt/hooks/b3/   # 332c62540d14
```

On v0.15 it gives 19 PASS and 0 FAIL.

### 02. physics

- **Recipe:** per case, the README's run-until-row recipe, with the schedule lines tagged `@F+1`.
- **Check:** `python3 -I 02-physics/physics_check.py check <case> records.json`.

10 of the 12 cases pass. `enemy-hits` and `ledge-j14` fail against `physics.json`, which holds real-time
numbers from sitting s6. Both v0.15 results repeated exactly in three processes, so they are the icount values
to expect:
- **`enemy-hits`:** hits `[[188, 25], [425, 25]]`, against s6's `[[186, 25], [421, 25]]`. After each hit:
  0x102 for 13 rows, then 0x2 for 6. It follows input timing: tagging the lines `@F+2` moves the hits to 187
  and 361. Always tag them `@F+1`.
- **`ledge-j14`:** ends on the ledge: rise 1.2578, states 2@16, 3@38, 0@40, 0x100@41, 0@46. s6 ended it on the
  floor.

End RAM per case on v0.15. These are reference values, not oracles:

| Case | End RAM | Case | End RAM |
|---|---|---|---|
| walk | `132499b74127` | enemy-hits | `75b7170edc8f` |
| roll | `c2ed673c127e` | ledge-j14 | `ff2d63cd224a` |
| jump | `73bc237f6888` | ledge-j16 | `81a3f3917563` |
| swim | `e577eeb299db` | ledge-j28 | `a105d33c0908` |
| beach-jump | `e8f55d06b95d` | ledge-j30 | `47dd2d11f4c5` |
| beach-flaps | `551b98fbd735` | cold-jump | `85490f088fc7` |

- **cold-jump:** on the D98 EEPROM, DPAD_LEFT shows no save preview and A starts `levels/training_room`
  directly. Three A presses clear the welcome dialog. `.text` is 979,408 B, with 0 bytes differing.

### 03. heavy-scenes

`python3 scripts/bench-det.py -n 2 -t <tag> --hash-at 300,600 -i <line> ... v015=$APP`, with these inputs:
- **`x24` and `ride`:** from `scenes.json`.
- **`fight`:** the first candidate (heading (0, 1), W 60), which is 70 lines:

  ```sh
  F=(-i "+2 axis ly 1.000000" -i "+62 axis ly 0.000000")
  for k in $(seq 64 16 592); do F+=(-i "+$k down 81" -i "+$((k+4)) up 81"); done
  python3 scripts/bench-det.py -n 2 -t vanilla-rz-e7-keylord --hash-at 300,600 "${F[@]}" v015=$APP
  ```

| Tag (input) | start12 | f300 | f600 | Minsns/frame |
|---|---|---|---|---|
| `vanilla-rz-new` (x24) | `4fbb36cc0967` | `6279ea3c0b21c696` | `846baeb5cb4fa7a1` | 47.2 |
| `vanilla-rz-e6-song` (x24) | `d1925e02d35e` | `7a3f20f0ba7d0671` | `6835875d7dd0ed2e` | 20.5 |
| `vanilla-rz-e7-keylord` (fight) | `d9e66aad9afc` | `6f17e002086d441b` | `b9cd6a0527ced223` | 34.4 |
| `vanilla-rz-e2-cart` (ride) | `5d322a1f90fb` | `2b6adc99d21bf19c` | `7f0156a86e5f6454` | 33.3 |

**Checks** (run over QMP with `check_hooks`, which only collect):
- **e2:** `0x53750` gets one path, `levels/earth/e5`, at +245.
- **e7:** `apply_damage` is called 0 times on the player and 12 times on other critters. The first is at +207.

**Disc I/O:** e6 and e7 read nothing from the DVD. The new game and e2 each stream about 36-41 MB from it.

**Draws per flip** (offscreen `probe-run.sh`, 40 s idle dwell, intervals up to the loadvm dropped):

| Tag | Draws per flip |
|---|---|
| `vanilla-rz-new` | 296.7 |
| `vanilla-rz-e6-song` | 109.0 |
| `vanilla-rz-e7-keylord` | 251.3 |
| `vanilla-rz-e2-cart` | 110.8 |

**No bench anchor was adopted.** Vanilla Azurik presents at 30 fps. All four scenes hold 30.0-30.2 fps,
windowed and offscreen, so windowed A/Bs can't show a speedup on them. `azurik-heavy-f1` and `-w2` remain the
windowed anchors. e7's `f600` is this pass's deterministic golden: it covers combat and is x87-sensitive. It
takes about 7 s a process, with no disc I/O.

### 05. bink-movie

The recipe is the README's: `vanilla-beach-shield5` with `movie.json`'s hooks, then `xemu-run-until` 0x53750
twice, then advance to F2 + 900.

- **Start RAM:** `c637e9851b64`.
- **F1 = 4:** `bink:death.bik`, with a NULL spot.
- **F2 = 6046:** `levels/death/d2`, with the spot `D1_D2_Fall`.
- **DVD read:** 8,192 B from the load to F1, and 50,298,880 B from F1 to F2.
- **HDD written:** 65,024 B, all before F1.
- **RAM at F2 + 900:** `9ab9c0158f03` in the cut run and in a straight run.
- **Offscreen:** keep these numbers apart from the null ones. The RAM at F2 + 900 is `10b5a35aa66b`.

## Not adopted

| Pack | Why it isn't here | v0.15 headline |
|---|---|---|
| 04, 10 | They wait on recordings from the harness (jobs 04-a and 10-a). | - |
| 06 europe-pal50 | It needs the Europe disc. `bench-det --route dpad_left,a` stalls at the save list. | PAL50 EEPROM: F = 25. Fixture EEPROM: F = 30. ~30.06 player ticks per guest second at 60 Hz. |
| 09 grown-iso-io | It needs a local 2.8 GB ISO built by the Modkit. | +75,238,912 B of HDD writes, all in the logos phase. Repeatable block logs. |
| 07 owner-recording | Local-only soak. | Sections 50 and 81 end on `3edb08f3c2ea` and `648e70df76c4` in two processes, with lines sent as `@<frame>`. |
| 08 v22-boot-hang | Soak. | 0 of 10 cold boots hung. All reached `0x5F748` at frame 1350, with identical block logs. |

The harness keeps all ten packs under its own evidence tree.
