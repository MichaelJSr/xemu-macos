# 05 · bink-movie: death.bik in game, from one level request

This pack plays one in-game Bink movie, `death.bik`. It is the movie d1's `D2_movie_levelSwitch` plays on the
way to d2. The test starts from a plain `loadvm` and makes one level request with the exit node's own strings,
and then the game does the rest. It needs no pad input, no touch and no placement. So far the fork has seen only
`prophecy.bik`, at boot, and its fixture disc ends that one at once. This is also the "fixture title's FMV
moment" the fork's open BINK/VideoToolbox item asks for: its first step is to confirm that such a moment is
decode-bound on an M-series core (`docs/roadmap.md`).

## What it exercises

- **The game's movie path:** `load_level_from_path` (`0x53750`) gets `bink:death.bik`. The movie is
  `movies/Death.bik` on the disc:
  - 50,295,624 B, sha256[:12] `0a67fb91691a`;
  - its Bink header: 3,006 frames at 30/1, 640×480, 1 audio track, about 100 s.
- **What runs during it:** about 6,000 vblanks (3,006 frames at 30/s) of software Bink decoding on the CPU, the
  file streaming from the DVD, and the movie's audio on the APU [inferred: Bink's decoder and audio path weren't
  traced].
- **Afterwards:** a level load (`levels/death/d2`, spot `D1_D2_Fall`), and a falling arrival that takes about
  170 player ticks to stand.
- **Unchecked:** whether Bink presents through PVIDEO.

## Contents

| File | What |
|---|---|
| `movie.json` | The start tag and its start RAM, the movie's facts, the exit node's strings, the two hooks as `xemu-hooks-set` takes them, and the oracle's definition. |
| `bink_movie.py` | Our side: job 05-a. It checks `movie.json` against the disc (`--dry-run`), runs the test on v0.15 and writes `results-v0.15.json`. |

Job 05-a (ours) writes `results-v0.15.json` here: F1, F2, the RAM at F2 + 900 and the DVD bytes. It hasn't run, so that file isn't in the pack; the fork's run below needs nothing from it.

## Start recipe (the "nodereq")

1. Clone the D98 fixtures (paths in `LOCAL/05-bink-movie.md`). Launch headless null with
   `-S -icount shift=2,align=off,sleep=off`.
2. Send `stop`, then `loadvm vanilla-beach-shield5`. The start RAM must hash to `c637e9851b64`.
3. Send `xemu-hooks-set` with `movie.json`'s `hooks`:
   - **The write hook at `0x8CCC0`** (the player's tick). Its one entry posts R7, the request a levelSwitch
     fires. The entry writes, each with its NUL:
     - at the request buffer `0x38D320`: `bink:death.bik` at +0, `levels/death/d2` at +0x104 and `D1_D2_Fall` at
       +0x208;
     - the buffer's address into the request block's level slot, `[[[0x1BF63C]+0x2C]+0xD8]+0x10`.

     These are d1's `D2_movie_levelSwitch` node's own movie, destination and spot strings (record
     `d1/g0/r2271`, node +0x18/+0x1C/+0x20). The game takes the request at its next update.
   - **The collect hook at `0x53750`** reads `[esp+8]` (the path) and `[esp+0xc]` (the spot, or NULL).
4. Run with no input.

## How the fork runs it

Use QMP on the launch above. After `xemu-hooks-set`:

- `xemu-run-until {"until": [{"pc": 341840}], "frames": 12000}` twice. These stop at F1 (the movie) and F2
  (d2). Read `query-blockstats` at each.
- Then `xemu-frame-advance` to F2 + 900 and hash RAM with `xemu-dump-ram` (`gpu-sync` false).

A straight `xemu-frame-advance` of F2 + 900 from the load must end on the same RAM; under icount, a run cut at
hits is the uncut run. bench-det can't set hooks, so it doesn't fit here. For pictures, run the same thing
offscreen (`XEMU_HEADLESS=1`). That is deterministic too, but it hashes different RAM, so don't compare it with
the null hash.

## How we run it

`bink_movie.py` (job 05-a) uses `Sitting(app=<v0.15>)` and the same QMP calls:

- **Run A** cuts the run at each `0x53750` hit, as above.
- **Run B** goes in a relaunched xemu: the same load and hooks, then one straight advance to the same frame.

It checks the two paths and spots in order, then compares A and B: the start RAM, the end RAM, the paths and
their frames, the level and the HP.

## The oracle

The `0x53750` path sequence, the frame gap and the end RAM:

- `bink:death.bik` with a NULL spot at F1;
- then `levels/death/d2` / `D1_D2_Fall` at F2;
- the RAM sha256[:12] at F2 + 900;
- plus the DVD bytes read between F1 and F2. We expect at least the file's 50,295,624 B [inferred: the movie
  streams the whole file].

No build has these values yet: the fork's run or our job 05-a gives the first.

## What we measured, and on which build

- **The nodereq on this exit**: sitting probes, 2026-10-11, v0.13.3, real time, posted in d1 after an R7 hop
  there. `0x53750` got `bink:death.bik` (spot NULL) one tick after the post. Then it got `levels/death/d2` /
  `D1_D2_Fall`, the arrival record `d2/g0/r185` matched by its spot, and the player stood after 170 ticks with
  HP 185/400.
  - That run pressed A on a stall 5.03 s in, and d2 loaded 0.04 s later. So it likely skipped the movie
    [inferred]. This pack presses nothing.
- **The same movie request from w1** (to e5; sitting rz-b2, Q246, `vanilla-rz-w1-fw`, 2026-10-11, v0.13.3-37,
  real time): `0x53750` got `bink:death.bik` 0.1 s after the post, and no load followed in the next 45 s, where
  that run stopped watching. Its rerun of the same request arrived in e5. So the request needs no d1 start.
- **The whole movie unskipped, under icount, and on v0.15:** not measured on any build yet; that is job 05-a.
- **The disc's file:** `bink_movie.py --dry-run` (2026-10-16) read `movies/Death.bik` from the vanilla USA
  image: 50,295,624 B, sha256[:12] `0a67fb91691a`, 3,006 frames at 30/1, 640×480, 1 audio track.

## Report back on v0.15

From the paused `vanilla-beach-shield5` load with `movie.json`'s hooks, under icount, send:
- the start RAM sha256[:12] (`c637e9851b64` on v0.14-36 and v0.15);
- F1 and F2 (frames from the load) and the two `0x53750` records (path and spot);
- `query-blockstats` at the load, F1 and F2: DVD bytes read load→F1 and F1→F2 (we expect at least the file's
  50,295,624 B between them [inferred]) and HDD bytes written;
- the RAM sha256[:12] at F2 + 900, from the cut run and from one straight advance in another process;
- wall seconds from the load to F2 and the host load (the movie is about 6,000 vblanks of Bink decoding);
- if run offscreen too, its F1, F2 and F2 + 900 RAM, kept apart from the null ones.
