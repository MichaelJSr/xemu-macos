# 10 · audio-e6-song: e6's crystal song, replayed under the voice log and the WAV

This pack plays e6's giant-crystal song: seven staff hits, each a tone, in the order e6's data asks for. Then the
song's reward plays: the crystal door's timer runs. The pad input is recorded once under icount and replayed
with no debugger, with the fork's `XEMU_VOICE_LOG` and `XEMU_AUDIO_WAV` on. The fork's audio checks get a game
scene with known, repeatable audio events at volume 0.

## What it exercises

- **The APU:** voices started and stopped by game events. These are the staff selections (LT combos), the
  swings, the crystal's tones and the solve. Both DSPs run (`audio.use_dsp`), on the fork's DSP JIT by default.
  The mixbins/HRTF path is in play too [inferred: the tones are 3D voices].
- **Input timing:** an LT combo counts only the buttons pressed while LT is held. A wrong frame gives a wrong
  staff and so a wrong tone; after seven tones with a wrong one, every latch resets.
- **The scene:** e6, the slowest scene we have measured in real time (see `03-heavy-scenes`).

## Contents

| File | What |
|---|---|
| `song.json` | The tag and its start RAM, the set-up as one `xemu-hooks-set` write hook, the latch record indices it uses, the song order, and where the logs start. |
| `audio_song.py` | Our side: job 10-a. It checks `song.json` against the disc (`--dry-run`), records the song on v0.15, replays it twice and writes `results-v0.15.json`. |
| `results-v0.15.json` | Job 10-a's output (2026-10-17, the 01:59 rerun): the Modkit code it ran (`modkit`), the recorded lines, `frames`, the RAM, the song's result, and per replay the WAV's sha256[:12], samples and bytes and the voice log's counts and hash from the load. No WAV, no positions. |

**Ready (2026-10-17).** Job 10-a ran: `results-v0.15.json` holds the recorded lines, so the fork can replay the song
with its own tools. Recording it takes our harness (the walk to the crystal steers by a target read from the disc's
collision mesh, kept in memory); `audio_song.py` run from a Modkit checkout does it again (it takes one of the
Modkit's sandbox slots):

    uv run --locked python <pack>/10-audio-e6-song/audio_song.py --app <fixtures>/xemu-v0.15.app

with `AZURKIT_ISO` naming the vanilla USA image. It imports that checkout's `scripts/` (the main checkout's when the
working directory is no Modkit) and records which in the results' `modkit` (the checkout's name, HEAD, `xemu_oracle.py`'s and
`play_recording`'s sha256[:12]); `--out` and `--sitting` name the results file and the sitting. `--dry-run` checks `song.json` against the disc and launches nothing.

## Start recipe

1. Clone the D98 fixtures (paths in `LOCAL/10-audio-e6-song.md`). Use a scratch copy of their `xemu.toml`: the
   recorded scancodes use its keyboard map.
2. Launch headless null with `-S -icount shift=2,align=off,sleep=off` and the keyboard pad on port 2.
3. Send `stop`, then `loadvm vanilla-rz-e6-song`. The start RAM must hash to `d1925e02d35e`.
4. Send `xemu-hooks-set` with `song.json`'s `hooks`: one write hook at `0x8CCC0`, the first player tick. This is
   sitting r18f's set-up. It writes, with no positions:
   - the four element levels: 1 each, i32, at the inventory `[[[0x1BF63C]+0x2C]+0xE0]` +0x04..+0x10;
   - for each of e6's four crystal latches (records 762, 764, 768 and 772 of e6's main graph), 1.0 into the
     `input` and `update` attributes and all ones into the node's +0x10. This stands in for the crystals hit in
     e2, e5 and e7. Without it a hit is a bad tone (index 5), measured for whirlwind and inferred for the
     other three, and the song never solves.
5. Start the logs before any frame runs: `xemu-voice-log-start {"filename": ...}` and
   `xemu-audio-wav-start {"filename": ...}`.
6. Write the recorded `lines`, each `+N body` as `@<base+N> body`, base = `query-xemu-frame`'s `frame` right
   after the paused load (on the pipe a `+N` counts from the frame the line is read at: pack 07's lesson, the
   fork's PROMPT-9 answer), then `xemu-frame-advance <frames>`.
7. Stop both logs. Hash the RAM (`xemu-dump-ram`, gpu-sync false) and the WAV file.

## How the fork runs it

Use QMP as above; bench-det can't set hooks. Either way of opening the logs works:

- the QMP starts right after the paused load, which is what job 10-a does and works for any load;
- `XEMU_VOICE_LOG` and `XEMU_AUDIO_WAV` at launch, in a fresh `-S` process whose first action is this load
  [inferred: nothing runs before the load].

- **What to compare:** the WAV's sha256[:12] and `samples`, and the end RAM.
- **Not the voice log's hash:** its `frame` column is the process's frame counter, so compare its per-event
  counts, or rows with frames counted from the load.
- **To listen to it:** an offscreen icount run (`XEMU_HEADLESS=1`) with the same input plays the same run
  [inferred]. Its WAV is game audio: keep it local.

## How we run it

`audio_song.py` (job 10-a) uses `Sitting(app=<v0.15>, record=2)` and `scripts/xemu_minigames.py`. It runs in
two phases:

- **A, the recording.** The load and the hook are as above. Then it walks to the crystal until the body is
  within 2.6 m of `Detect_smshShape2`; the stick goes through the pipe as `axis` lines, so the recorder holds
  it. Then `Minigames.song()`: each tone's LT combo and an A press, with the tone read back. Its zero-stick RAM
  write is turned off, so every input goes through the pipe. Then 450 player ticks and 1 vblank.
- **B, the replays.** xemu is quit, which closes the recording, and relaunched. It replays twice under the voice
  log and the WAV (`play_recording`, which sends each `+N` as `@<base+N>` since Step X.39, AZ-510). Each replay
  must end on A's RAM, and the two replays' WAVs and voice logs must be the same.
  - Each WAV is hashed in the sandbox and deleted at once.

## The oracle

- The WAV's sha256[:12] and sample count, and the RAM sha256[:12] at `frames`.
- These must be equal in two replays, and the RAM equal to the recording run's.
- The song must be solved (`SongPuzzleSolved_toggle` goal 1).
- The v0.15 values: `results-v0.15.json` (job 10-a, below).

## What we measured, and on which build

- **The set-up and the song:**
  - Sitting r18f (2026-10-13, v0.13.3-37, real time): `Minigames.song` solved 5 of 5 runs, 7 hits each with no
    wrong tone. The order read from the data: smash, lightning, fire, whirlwind, lightning, smash, fire.
    `CrystalDoorTimer` ran after.
  - Sitting r18j (2026-10-14, v0.14-8, offscreen): 7 hits, no wrong tone.
  - Those runs wrote the walk's stick into RAM. This pack's pipe-only walk is new.
- **Without the latches** (sitting rz-b2, 2026-10-11): a whirlwind hit in `vanilla-rz-e6-song` landed on
  `Detect_windShape4` but counted as a bad tone (`Tone01_hit_latch` = 5).
- **Job 10-a, v0.15 (2026-10-17 01:41-01:42, sitting `x38-song`, Step X.39):** the fixtures' v0.15 copy (binary
  `e1869fccc44a`), null, icount, the recorder on port 2, every input through the pipe.
  - A, the recording: start RAM `d1925e02d35e`; the walk ran its 240 ticks (it never came within 2.6 m of the
    crystal's centre: `stop` null), and the song still solved: 7 hits in the data's order (smash, lightning, fire,
    whirlwind, lightning, smash, fire), no wrong tone, `SongPuzzleSolved_toggle` goal 1. 3,202 frames from the
    load to the end, RAM `3e23917e16ee`. 213 lines (65 axis, 38 trigger, 55 down, 55 up), the last at +2239.
  - B, two replays in the relaunched xemu with no debugger (`play_recording`, `@<base+N>` lines, Step X.39):
    both ended on A's RAM `3e23917e16ee`, both solved; the WAVs equal, `14bf0e233fe9`, 2,531,328 samples
    (10,125,356 B; deleted at once); the voice logs equal, 395 rows (start 129, stop 123, pause 14, resume 129),
    47 distinct start rates, `3e701e5179ff` with frames counted from the load.
  - The replay's code: the 01:41 run's logs don't name it. A rerun (01:59-02:00, sitting `x39-song`) records it
    in `results-v0.15.json`'s `modkit`: the Modkit lane X.39 at `290556fab3b8` (since amended to `92af286ed15f`, the same `xemu_oracle.py`), `xemu_oracle.py` `6192e4054d6c`
    unchanged, `play_recording` `8ba2f1af5f85` (`@<base+N>`, AZ-510). It gave every number here again.
  - A control with the old `play_recording` (raw `+N` lines, sitting `x39-song-raw`) gave them all too: the 213
    lines (4,194 B) are written at once while the load is paused, and raw `+N` lands on time
    [inferred: xemu reads them all at the load's frame]. So this job doesn't tell `@<base+N>` from raw `+N`.
  - The first audio numbers for this scene on any build.

## Report back on v0.15

`results-v0.15.json` is here (job 10-a). Replay its lines twice on v0.15 (each `+N` sent as `@<base+N>`, start
recipe step 6, then advance `frames`) under the voice log and the WAV and send, per replay:
- the start RAM (`d1925e02d35e` on v0.14-36 and v0.15) and the RAM sha256[:12] at `frames` (ours `3e23917e16ee`);
- the WAV's sha256[:12] and `samples` (ours `14bf0e233fe9`, 2,531,328), and whether the two replays' WAVs are equal;
- the voice log's rows per `event` and `why`, its distinct `hz`, and its rows' hash with frames counted from the
  load (ours: 395 rows, 47 rates, `3e701e5179ff`; `why` not counted on our side);
- wall fps and the host load; and the same run with the DSP JIT off, if you compare the two DSP paths.

The solve needs no report: a replay that ends on `3e23917e16ee` solved the song as the recording did.
