# 04 · level-tour: every level the game's own debug menu lists, by pad input

The fork's runs load only the few levels its anchors and its cold boot reach. This pack loads all 22 levels that
Azurik's own "Change level..." debug menu lists, each from the same plain `loadvm`. It uses pad input only: the
game's cheat code opens the menu, the D-pad picks the row and START warps.

## What it exercises

- 22 level loads from one state: XBR parsing and decompression, disc streaming, the HDD cache and the level
  setup code. Several of these levels the fork has never loaded.
- Input timing: the cheat code is matched once per game update, each step within 1.0 s of the last. The menu
  then needs exact presses and releases.
- What the oracle catches: each level's RAM at its first steady tick, and its block-log totals. Together they
  separate a change in what the game reads from a change in what it computes.

The levels are a1, a3, a5, a6, airship, d1, d2, e2, e5, e6, e7, f1, f2, f3, f4, f6, life, town, w1, w2, w3 and
w4: every `levels/...` path in `gamedata/index/index.xbr` except training_room and selector. The menu also lists
9 movie and scene rows; the tour doesn't use them.

## Contents

| File | What |
|---|---|
| `tour.json` | The start tag and its start RAM, the 22 level paths, and how the input was recorded. |
| `level_tour.py` | Our side: job 04-a. It records the 22 warps on v0.15, replays each in a fresh xemu and writes `results-v0.15.json`. |
| `results-v0.15.json` | Job 04-a's output (2026-10-17, the 02:02 rerun): the Modkit code it ran (`modkit`), and per level the recorded pipe lines, `frames`, `ram12`, the start RAM, the block-log totals and `query-blockstats` deltas, and the warp's summary (taps, the request posted). Pad lines, hashes and counts only. |

**Ready (2026-10-17).** Job 04-a ran: `results-v0.15.json` holds each level's recorded lines, so the fork can replay all 22 with its own tools. Recording it takes our harness (the warp walks the menu by reading its rows from RAM over the gdbstub); `level_tour.py` run from a Modkit checkout does it again (it takes one of the Modkit's sandbox slots).

## Start recipe (per level)

1. Clone the D98 fixtures' HDD and EEPROM. Use a scratch copy of their `xemu.toml`, which has the keyboard map
   the recorded scancodes use; the paths are in `LOCAL/04-level-tour.md`.
2. Launch headless null with `-S -icount shift=2,align=off,sleep=off` and the keyboard pad on port 2.
3. Send `stop`, then `loadvm vanilla-beach-shield5`. The start RAM must hash to `c637e9851b64`, the same as
   `fcheck`'s start for this tag.
4. Write the level's `lines` from `results-v0.15.json` to `XEMU_INPUT_PIPE`, each `+N body` as `@<base+N> body`,
   base = `query-xemu-frame`'s `frame` right after the paused load (on the pipe a `+N` counts from the frame the
   line is read at: pack 07's lesson, the fork's PROMPT-9 answer). QMP `xemu-run {"input": [...]}` takes them in
   one call too.
5. Run `xemu-frame-advance <frames>`, then hash RAM.

## How the fork runs it

- **The RAM oracle:** `bench-det.py`, one run per level, with the level's lines as `-i` arguments. Use the same
  scratch config recipe as `03-heavy-scenes`:
  `XEMU_TEST_CONFIG=$S/xemu.toml python3 scripts/bench-det.py -n 1 -t vanilla-beach-shield5 -f <frames> -i <line> ... v015=dist/xemu.app`.
  Its `f<frames>=` value must start with the level's `ram12`.
- **The block-log totals:** add the log to the arm, as in `v015=dist/xemu.app:XEMU_BLOCK_LOG=$S/<level>.tsv`
  (an arm's own `K=V` is passed through). Then sum `bytes` and count rows per `drive` and `op`, and compare with
  `block`. The rows add up to the `query-blockstats` deltas (`blockstats`).
  - Compare totals, not the file's hash: its `frame` column is the process's frame counter, which only starts
    from the same value in a fresh process.

## How we run it

`level_tour.py` (job 04-a) uses `Sitting(app=<v0.15>, record=2)`, the kit's input recorder. From a Modkit
checkout, with `AZURKIT_ISO` naming the vanilla USA image:

    uv run --locked python <pack>/04-level-tour/level_tour.py --app <fixtures>/xemu-v0.15.app

It imports that checkout's `scripts/` (the main checkout's when the working directory is no Modkit) and records
which in the results' `modkit` (the checkout's name, HEAD, `xemu_oracle.py`'s and `play_recording`'s sha256[:12]); `--out` and
`--sitting` name the results file and the sitting. `--dry-run` checks `tour.json`'s 22 levels against the disc's `index.xbr` and launches nothing. It runs in two
phases:

- **A, the recording.** Each level gets a paused load and then `Sitting.warp`. The warp is driven through the
  pipe, from the cheat handler `0x15DA0`; the menu row's action is `0x12A50`. It runs until 20 player ticks on
  the ground or in water in the new level, pressing A when a dialog stalls the tick, and then 1 vblank more.
- **B, the replay.** xemu is quit, which closes the recording, and relaunched. Each level's section is replayed
  with no debugger, under `xemu-block-log-start` (`play_recording`, which sends each `+N` as `@<base+N>` since
  Step X.39, AZ-510). The RAM must equal phase A's.
- **No RAM writes.** With the default spot (`startspot`) the warp writes no RAM. The recording holds exactly
  what the pad gave the guest.

## The oracle

- **Per level:** `ram12` at `frames`, equal in the recording and in a replay in another xemu; plus the
  block-log totals (rows and bytes per drive and op).
- **The v0.15 values:** `results-v0.15.json` (job 04-a, below).

## What we measured, and on which build

- **Step X.17** (2026-10-14, v0.14-8, offscreen, real time; sitting x17-cheats) measured `Sitting.warp` on six
  levels. It took 7.5–15.1 s a level: about 15 updates for the code, about 0.2 s a menu row, then the load.

  | Level | From | Time | Taps |
  |---|---|---|---|
  | f6 | `vanilla-beach-shield5` | 15.1 s | 27 |
  | e2 | `vanilla-beach-shield5` | 8.6 s | 18 |
  | a1 | `vanilla-beach-shield5` | 7.5 s | 4 |
  | w4 | `vanilla-beach-shield5` | 12.3 s | 33 |
  | life | `vanilla-beach-shield5` | 9.8 s | — |
  | town | `vanilla-rz-new` | 11.4 s | — |

- **A warp against an R7 hop** to f6's startspot: they arrived 0.0 m apart, with the same HP and the same
  inventory except the day clock.
- **Recording replays (Step X.30 §9, v0.14-36):** a pipe-driven string recorded under icount replayed
  bit-identically in another process (`7c2e0f8363b2`).
- **Job 04-a, v0.15 (2026-10-17 01:38-01:41, sitting `x38-tour`, Step X.39):** the fixtures' v0.15 copy (binary
  `e1869fccc44a`), null, icount, the recorder on port 2. All 22 levels arrived (each loaded only its own level,
  the request's spot `startspot`, no RAM written) and each replay, in the relaunched xemu with no debugger,
  ended on the recording's RAM: 22 of 22.
  - The start RAM `c637e9851b64` at every load; 194-664 frames a level (10,642 in all); 22-80 pipe lines a level
    (1,198: 533 down, 533 up, 132 axis); 4-33 menu taps; 2.3-7.1 s a warp (97 s in all, host load about 23-55).
  - The block logs: no status but `ok`. Ten levels write about what they read from the disc to the HDD (f2 115 MB,
    f4 96 MB, e6 85 MB, e7 52 MB, ...): copies into the title's HDD cache, which the tag's state doesn't hold
    and each `loadvm` restores [inferred]; the other twelve wrote 65-212 KB.
  - The replay's code: the 01:38 run's logs don't name it. A rerun (02:02-02:05, sitting `x39-tour`) records it
    in `results-v0.15.json`'s `modkit`: the Modkit lane X.39 at `290556fab3b8` (since amended to `92af286ed15f`, the same `xemu_oracle.py`), `xemu_oracle.py` `6192e4054d6c`
    unchanged, `play_recording` `8ba2f1af5f85` (each `+N` sent as `@<base+N>`, Step X.39, AZ-510). It gave every
    number above again; only the warps' wall seconds differ.
  - A control with the old `play_recording` (raw `+N` lines, sitting `x39-tour-raw`) gave the same lines, frames,
    RAM and block totals, 22 of 22. Each section here (at most 80 lines, 1,094 B) is written at once while the
    load is paused, and raw `+N` lands on time too [inferred: xemu reads them all at the load's frame]. So this job
    doesn't tell `@<base+N>` from raw `+N`; the fork's 07 runs, lines written a window ahead, do.

## Report back on v0.15

`results-v0.15.json` is here (job 04-a). Replay each level's lines on v0.15 in a fresh process, each `+N` sent as
`@<base+N>` (start recipe step 4), and send, per level:
- the RAM at `frames` (its first 12 digits must be the level's `ram12`), from two processes;
- the block-log totals: rows and bytes per drive (`ide0-cd1`, `ide0-hd0`) and op (`r`, `w`, `f`, `d`), and any
  status but `ok` (compare with `block`);
- wall fps and the host load.

No earlier build has these numbers. The ones that exist are X.17's warp times (v0.14-8, real time, above).
