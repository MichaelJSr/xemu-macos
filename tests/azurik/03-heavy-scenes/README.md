# 03 · heavy-scenes: four vanilla scenes, from a plain load, with pad input only

The fork's two heavy anchors (`azurik-heavy-f1`, `-w2`) are on its QoL disc and usually sit at the 60 fps cap.
These four are on the vanilla disc. Each starts from a plain `loadvm` of one of the Modkit's D98 states, with
pad input only and no RAM write.

- **Why these four:** on our null, deterministic harness (v0.14-36, the slower of two 120-vblank runs), three are
  among the six slowest of the 22 D98 field states:
  - `vanilla-rz-new` is 1st, at 32.9 fps even with no rendering;
  - `vanilla-rz-e2-cart` is 2nd, at 37.0;
  - `vanilla-rz-e7-keylord` is 5th, at 70.7.
- **e6 is here for real time:** it ranks 11th, at 86.2 fps on null, but in a real-time window it ran at 26
  player ticks/s against the game's 30.

## What it exercises

| Scene | Tag | Input | What runs |
|---|---|---|---|
| new game | `vanilla-rz-new` | `x24` | w1 just after the new game's scripted walk. The level is still streaming: 36,130,816 B from the DVD and 7,297,024 B from the HDD in the first 120 vblanks. |
| e6 | `vanilla-rz-e6-song` | `x24` | e6 at the crystal song puzzle. This is the slowest scene we have measured in real time. |
| e7 fight | `vanilla-rz-e7-keylord` | `fight` | A walk toward the live red keylord in view, then a staff attack every 16 vblanks. Whether a walk from this load reaches it is unmeasured: the `fight` search finds out. |
| e2 ride | `vanilla-rz-e2-cart` | `ride` | A walk into the minecart's bucket. The bucket's level switch then loads e5 (`levels/earth/e5`, spot `E2a_E5a`). |

Each run exercises the CPU/JIT, x87 and the APU. The new game and the ride's e5 load also stream from the DVD
and the HDD. Under a GPU renderer each run exercises PGRAPH too; that is what the fork's draws/flip numbers
measure.

## Contents

| File | What |
|---|---|
| `scenes.json` | The tags, the input strings and the `fight` candidates, the start-RAM check per tag, each scene's check and its collect hook, and our v0.14-36 and v0.15 numbers. |
| `heavy_scenes.py` | Our side: job 03-a. It runs the scenes on v0.15 through the Modkit harness and writes `results-v0.15.json` here. Job 03-a hasn't run, so that file isn't in the pack. |

## Inputs (pipe lines, `+N` = N vblanks after the paused load)

- **`x24`**: X.24's string, the one the fork's `fcheck` already runs for 120 vblanks:
  `+2 clear`, `+10 axis ly 1.000000`, `+30 down 44`, `+36 up 44` (Y: a jump), `+60 axis lx -0.500000`,
  `+90 axis ly 0.000000`, `+90 axis lx 0.000000`.
- **`ride`**: `+2 axis ly 1.000000`, `+182 axis ly 0.000000`. This is the stick-up walk of our 2026-10-11 ride:
  90 player ticks, touching the bucket at tick 29, with e5 loading at tick 119 (real time, v0.13.3).
- **`fight`**: a search over 32 candidates (`scenes.json`'s `fight_candidates`), kept at the first whose 600
  vblanks land an `apply_damage` on a critter other than the player. Not chosen yet: whether a pad-only string
  from this load reaches the keylord has never been measured.
  - A candidate is a heading (lx, ly) and a walk W. The lines: `+2 axis lx <lx>` and `+2 axis ly <ly>` (each
    only if not 0), the same axes back to `0.000000` at `+(2+W)`, then `+k down 81` and `+(k+4) up 81` (A) for
    k = 4+W, 20+W, ... while k+4 < 600. Values print with 6 decimals, as `1.000000`.
  - The order: heading first, then W. Headings (0, 1) forward, (−0.707107, 0.707107), (0.707107, 0.707107),
    (−1, 0), (1, 0), (−0.707107, −0.707107), (0.707107, −0.707107), (0, −1); W 60, 120, 180, 240.
    `heavy_scenes.py --dry-run` prints the first candidate's lines.

The scancodes come from the D98 config's keyboard map (`a` 81, `y` 44). Use that config, as below.

## Start recipe

1. Clone the fixtures' HDD and EEPROM. Use a scratch copy of their `xemu.toml`. `$FX` below is the fixtures
   folder; the paths are in `LOCAL/03-heavy-scenes.md`.
2. Launch headless null with `-S -icount shift=2,align=off,sleep=off` and the keyboard pad on port 2.
3. Send QMP `stop`, then HMP `loadvm <tag>`. The VM stays paused.
4. Before running a frame, check the start RAM: sha256[:12] of `xemu-dump-ram` must equal `start12` in
   `scenes.json`. These are the same start hashes as `fcheck`'s.
5. Write the input's lines to `XEMU_INPUT_PIPE`, then `xemu-frame-advance` to +300 and to +600.

## How the fork runs it

The oracle comes from `bench-det.py` on a scratch config built from the D98 one. It needs the D98 scancodes and
a cloned disc:

```sh
S=$(mktemp -d /tmp/xt-heavy.XXXXXX)
cp -c "$FX/vanilla.iso" "$S/dvd.iso"
sed "s#^dvd_path = .*#dvd_path = '$S/dvd.iso'#" "$FX/xemu.toml" > "$S/xemu.toml"
export XEMU_TEST_CONFIG=$S/xemu.toml
python3 scripts/bench-det.py -n 1 -t vanilla-rz-e2-cart --hash-at 300,600 \
    -i "+2 axis ly 1.000000" -i "+182 axis ly 0.000000" v015=dist/xemu.app
```

Do the same for each tag, passing that tag's lines with `-i`. bench-det clones the HDD and EEPROM, stubs the
pads with `harness-config.py` and mutes the run. Its `f300=`/`f600=` values are the same sha256's first 16
digits.

For draws per flip, run each tag with the profiler under a GPU renderer. This is an idle dwell, so it shows the
scene with no input:

```sh
scripts/probe-run.sh dist/xemu.app $OUT/<tag> <tag> 40 XEMU_HEADLESS=1   # probe-run sets XEMU_NV2A_NSPROF=1
python3 scripts/nsprof_summarize.py $OUT/<tag>/run.log
```

Those numbers are the fork's to record. We have no draws/flip figures for any scene.

## How we run it

`heavy_scenes.py` (job 03-a) uses `scripts/xemu_sitting.py`'s `Sitting` with `app=` set to the fixtures' v0.15
copy. It runs each scene twice: once in one xemu, and once more after a relaunch. It records:

- the RAM at +300 and +600 (from `xemu-dump-ram` with gpu-sync off, as bench-det);
- wall fps;
- blockstats deltas;
- each scene's check, through collect hooks (they don't change RAM).

It also checks whether `xemu-ram-digest` gives the dump's sha256. On v0.15 it did at all 6 halts of pack 01's job
J01 (null renderer).

## The oracle

- **RAM sha256[:12] at +300 and +600 per scene**, equal in two processes. No build has these yet: job 03-a
  (ours) hasn't run.
- **The checks**, which must hold first:
  - e2: `0x53750` (load_level, stdcall; `[esp+8]` = the path) receives `levels/earth/e5` before +600.
  - e7: at least one `0x44640` call (apply_damage, stdcall(critter, new HP, flag)) whose critter isn't the
    player's (`[[[0x1BF63C]+0x2C]+0xC0]`).
  - bench-det can't set hooks, so run the checks over QMP with `scenes.json`'s `check_hooks` (collect only: they
    don't change RAM, so the run's hashes stay bench-det's).

## What we measured, and on which build

All four with the `x24` string for 120 vblanks (not this pack's +300/+600 runs), null renderer, deterministic:

| Scene | v0.14-36: fps (2 runs) | v0.14-36: end RAM | v0.15: fps (2 runs) | v0.15: end RAM | DVD / HDD read (both) |
|---|---|---|---|---|---|
| `vanilla-rz-new` | 36.5, 32.9 | `602d613deb1f` | 167.7, 164.4 | `602d613deb1f` | 36,130,816 / 7,297,024 B |
| `vanilla-rz-e6-song` | 86.7, 86.2 | `ae74dee21cf1` | 461.1, 463.2 | `ae74dee21cf1` | 0 / 110,592 B |
| `vanilla-rz-e7-keylord` | 74.3, 70.7 | `b0a1ce8b06f6` | 248.8, 283.2 | `23ab6ea774c0` | 0 / 6,144 B |
| `vanilla-rz-e2-cart` | 125.6, 37.0 (the second 3.4x slower; cause not read) | `012cce7d0aca` | 246.3, 289.6 | `012cce7d0aca` | 0 / 501,248 B |

- **v0.14-36:** Step X.30 `d01b`, 2026-10-15. Host load was 20.9–29.7.
- **v0.15:** Step X.32, sitting x32 (a02, both runs in one xemu), 2026-10-16. Host load was 28.5–32.4.
  e7's end moved by two words in the game's shader-instance cache, a NaN's payload that v0.15's x87 keeps:
  no gameplay change (`02-physics`). With these fps, `vanilla-rz-new` is still the slowest of the four.
- **e6 in real time:** an owner session in a v0.14-27 window (Step X.28, 2026-10-15) ran at about 26 player
  ticks/s in e6 over 21,910 ticks: 26.1 with the tick gaps over 1 s (pauses) left out, 25.9 with them. Tick
  intervals were p50 33.4 ms, p95 54.3 ms, p99 104.3–104.6 ms (by the percentile rule). a5, e2, life, town and w1 ran at 29.3–30.0
  ticks/s (gaps over 1 s left out), and training_room at about 26.9–27.2. That session started from placed
  spots, so it isn't this pack's start.
- **This pack's runs (+300/+600, the `ride` and `fight` inputs, the checks):** not measured on any build yet
  (job 03-a).

## Report back on v0.15

Send, per scene (with the input used, and for e7 the `fight` candidate that landed a hit):
- the start RAM sha256[:12] (`start12`: `4fbb36cc0967`, `d1925e02d35e`, `d9e66aad9afc`, `5d322a1f90fb` on
  v0.14-36 and v0.15) and the RAM at +300 and +600, from two processes;
- the check: e2's `0x53750` paths in order (`levels/earth/e5` expected), and e7's `apply_damage` calls on the
  player and on others;
- wall fps and the host load, and the DVD and HDD bytes read and written over the run;
- under a GPU renderer: `nsprof_summarize.py`'s draws per flip, passes per flip and fence ms per flip for each
  tag (we have none).
