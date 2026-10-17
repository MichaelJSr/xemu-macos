# 01-replays: two per-tick input replays with a CORE digest per tick

**What it exercises:** whole game steps under `-icount` from a level load:
- the CPU and JIT, and x87 float math in the player, the enemies and the camera;
- `loadvm` and a level request posted in RAM;
- per-tick input written by set hooks: the pad at the player update's entry and the camera direction on the stack.

| Replay | Ticks | What happens | String sha256[:12] |
|---|---|---|---|
| `w2-walk-pickup` | 1,067 (250 + 817, about 36 s at 30 ticks/s) | a walk through w2, three enemy hits (HP 200 → 185 → 170 → 155), the r1364 pickup | `332c62540d14` |
| `w1-seg0` | 475 (about 16 s) | the w1 opening after the new game's scripted walk, with 78 ticks of Y; one popup stops the game at tick 415 | `693816c62974` |

Each tick's oracle is a CORE digest: a sha12 over the body centre, the state and the HP. Our simulator (L3: the game's own XBE code run in Unicorn from a RAM snapshot of the start) produces the same digest on every tick. So a fork build that changes any of the three on any tick shows up at that tick, and nothing coordinate-like leaves this machine.

## Files

| File | Bytes | What it is |
|---|---|---|
| `w2-segments.json` | 44,168 | The w2 string in the format `test_b3.py:38` reads (`/tmp/xt/hooks/b3/w2-segments.json`, lost from `/tmp/xt`): run-length segments `{"n", "stick": [x, y], "buttons": [...], "cam": [x, y]}`. The file's own sha256[:12] is the string's: `332c62540d14`. |
| `w1-seg0-segments.json` | 38,005 | The w1 string, the same format; sha256[:12] `693816c62974`. |
| `replays.json` | 35,011 | Per replay: the start recipe, the start check, and the per-tick CORE digests (`oracle.core`, one per tick). |
| `replays.py` | 5,907 | The exporter (`export`, our side) and the checker (`check`, the fork's side; stdlib only). |
| `ours/j01_replays_v015.py` | 5,334 | Our measurement driver for v0.15 (job J01); it writes `v015-oracle.json` here. |
| `v015-oracle.json` | 1,893 | J01's v0.15 results: start checks, CORE equality, start and end RAM hashes (logged in our sitting x38-j01; `LOCAL/01-replays.md`). |

**The string format.** Each segment runs `n` ticks with:
- the left stick `[x, y]`;
- the held buttons, named from `a b x y black white lt rt lstick rstick start back` (the key is left out when none is held: 397 of w2's 403 segments, 277 of w1's 338);
- the camera direction the game's input builder read, as an exact f32 pair.

Per tick, the pad is `struct.pack('<18f', sx, sy, 0, 0, 0, 0, a, b, x, y, black, white, lt, rt, lstick, rstick, start, back)`, with 1.0 for each held button: 72 B, which is `l3.pad_floats`. The camera is `struct.pack('<2f', *cam)`. No segment writes RAM: no placement, no position.

## Start (both replays: D98's `vanilla-rz-new`, the vanilla USA disc)

The tag is on D98's drive, which the fork's fcheck already loads from clones (32/32 states on v0.15). Clone the drive; never use it in place. Addresses are USA vanilla. "Halted" means stopped before that instruction, as `xemu-run-until` stops.

1. `loadvm vanilla-rz-new` (paused), then run to the first `0x8CCC0` (the player tick; the entity is `[esp+4]`).
2. **w2 only:** at that halt, post the game's own level request (R7):
   - at `0x38D320`, a 00 byte (no movie);
   - at `0x38D424`, `levels/water/w2` and a NUL;
   - at `0x38D528`, `W1_W2_Fall` and a NUL;
   - then u32 `0x0038D320` at `[[[0x1BF63C]+0x2C]+0xD8]+0x10`.

   Then run to the level load `0x53750`. After the last load, run to the 20th hit of `0x8CCC0` whose u32 `[esp+4]+0x94` (the state) is 0, 3 or 6, and halt there. With `xemu-run-until`, that is three `until` entries at `0x8CCC0` (state `eq` 0, 3 and 6) with `hits` 20.
3. Write u32 `0xFFFFFFFF` at `[[[0x1BF63C]+0x2C]+0xE0]+0x14`. These are the inventory's first-pickup popup bits.
4. If u32 `[[0x38C058]+4]` isn't 1, write 1 there. This is the game's slow-motion cvar, which holds the game to one scene tick a frame (`pin_pace`).
5. Run to the next `0x8CCC0` and let E = `[esp+4]`. Then run to the next `0x8C230` with ECX == E: this is tick 0's update entry, the start.
6. **Start check:** `sha256(E[0:0x1C0] + [E+0x20][0:0x240])[:12]` must equal `start_check.start_sha12`: w2 `7548494a88eb` (E was `0x162c1d0`), w1 `c6af3ec882b3` (E `0x17e8280`). Both were measured on v0.14-12. v0.15 kept all 32 D98 start hashes, but this start comes after steps 2–5, so J01 re-measures it.

Writes at a halt can go through the gdbstub, or a one-entry write hook at that PC. Either lands before the instruction. Save an `xemu-checkpoint-save` at the start so later runs restore it. Hooks keep their hit numbers, keys and queues across a restore, so clear and set them again for each run.

## How the fork runs it (set hooks, one advance)

This is the three-hook recipe from the fork README's "Write at guest PCs". E and PAD are resolved once at the start: PAD = `0x37BE98 + 0x54*u32[0x1A7AE4]` (an index of 4 or more reads slot 0). The key is the entity's event clock.

```json
{"hooks": [
 {"pcs": [574000], "cond": [{"reg": "ecx", "op": "eq", "value": E}], "key": {"addr": "E+0x18", "size": 8},
  "write": [{"addr": "PAD", "size": 72}], "set": ["<72 B pad hex per tick>"], "stop-empty": true},
 {"pcs": [545398], "key": {"addr": "E+0x18", "size": 8},
  "write": [{"addr": "esp+0x14", "size": 8}], "set": ["<8 B camera hex per tick>"]},
 {"pcs": [574449, 574465], "key": {"addr": "E+0x18", "size": 8},
  "collect": [{"addr": "[E+0x20]+0x15c", "size": 12}, {"addr": "E+0x94", "size": 4},
              {"addr": "[E+0x20]+0x210", "size": 4}]}]}
```

- The PCs, in decimal for JSON: `0x8C230` = 574000 is the update entry, `0x85276` = 545398 the camera read, and `0x8C3F1` = 574449 and `0x8C401` = 574465 the update's two RETs.
- Then `xemu-frame-advance {"frames": 6000}`. The entry hook's empty queue stops it at the entry after the last tick.
- `test_b3.py`'s `hook_specs` builds the same hooks. It reads `/tmp/xt/hooks/b3/w2-segments.json`; copy `w2-segments.json` there (the fork's copy step, not ours).
- **Start state:** `test_b3.py` starts from the fork's own `azurik-heavy-w2` on `Azurik_qol.iso`. That start doesn't give these digests; the start above does.
- **w1's popup** (v0.14-12: the player updates stop after tick 415). Advance in chunks. When a chunk ends with entries still queued and no new entry record for about 120 vblanks:
  1. send `down` and then `up` of A through `XEMU_INPUT_PIPE`, about 9 vblanks apart;
  2. advance again.

  Our replayer cleared it the same way, by A through the pad FIFO after a 4 s wall-clock stall. The CORE digests don't depend on when A lands, because L3 has no popup and still matches every tick. The end RAM does depend on it, so w1's end RAM is not an oracle.

**Check a run:** `python3 -I replays.py check w2-walk-pickup run.json`.
- `run.json` is a list with one item per tick, `[pos_hex, state_hex, hp_hex]`: the RET hook's three regions in order.
- `test_b3.py`'s `hooks-out.json` (`{"obs": [...]}` rows) works as is.
- It prints one JSON line: `first_diff`, the first tick whose digest differs (`null` if none), and `pass`.

## How we run it (our scripts)

Our side uses Azurik_Modkit's gdbstub replayer, `Sandbox.replay` (`scripts/xemu_oracle.py:2230`). Inside a sitting (`scripts/xemu_sitting.py serve`, then `exec`):
- `ours/j01_replays_v015.py` runs both replays against L3 on the build under test;
- X.22's `d02_x15_strings.py` and X.30's `d07_w2_walk.py` are the drivers that made the records.

At each of the player's update entries the replayer writes the pad (`write_pad`, `:2219`), at `0x85276` the camera, and at the RET it reads the observation (`azurik_mod/sim/obs.py` `read`), all over the gdbstub. On w1's popup, `xemu_chain.Chain.replay` stops after a 4 s wall-clock stall, presses A through the pad FIFO until the next update, and goes on.

## The oracle

**The CORE digest** (`azurik_mod/sim/obs.py:111` `digest` with `CORE = ("pos", "state", "hp")`; the corpus writes it at `azurik_mod/sim/corpus.py:55-56`). It is computed at the update's RET of the tick (`0x8C3F1` or `0x8C401`). E is the player entity (ECX at the entry).

| Field | Read |
|---|---|
| pos | `[E+0x20]+0x15C`, 3 × f32: the critter's body centre (the feet + 1 m) |
| state | `E+0x94`, u32 |
| hp | `[E+0x20]+0x210`, i32 |

- **The digest:** `sha256(repr((x, y, z, state, hp)).encode('utf-8')).hexdigest()[:12]`.
  - x, y and z are Python `bytes` of 4 bytes each: the f32s as read, little-endian.
  - state and hp are Python `int`s, unsigned and signed.
- **Python's repr rules:** `b'...'`; printable ASCII as is; `\\` for a backslash; `\t`, `\n` and `\r`; `\xNN` (lowercase) otherwise. Single quotes, unless the bytes hold `'` and no `"`.
- **Reproduce it with Python, not by hand:** `replays.py`'s `core_digest`. It agreed with `obs.digest` on about 20,000 synthetic triples, including bytes that hold quotes and backslashes.
- **Correction to the inventory:** pos is at `[E+0x20]+0x15C` itself. `obs.read` reads from `+0x154` only to start its window; there is no further dereference.

**Why CORE and not the whole observation:** the whole-observation digest adds `inv_sha12` and `flags_sha12`, the latter a walk of the game's latch tree, which hooks can't practically collect. On w1, L3's tick mode differs from xemu on `inv_sha12` from tick 1 (`obs_divergence` 1) while CORE matches.

**Measured:**
- **w2-walk-pickup:**
  - x22-w2, v0.14-12-g1723609ea0, 2026-10-14: every tick equal to L3, exact on every observed field. xemu 43.2 s, 40.5 ms a tick.
  - x26-w2, v0.14-27-g9aa0b21b78: identical per-tick CORE digests (and whole digests) on all 1,067 ticks. So the start recipe reproduces under icount.
  - X.30 d07, v0.14-36-g08fe118fa0: exact against L3 again (27.2 ms a tick). Its digests weren't kept.
  - The real-time record x20e-w2-180208 diverges from them at tick 283: icount is required.
- **w1-seg0:** x22-w1-seg0, v0.14-12. CORE equal to L3 on all 475 ticks; one popup was cleared at tick 415.
  - X.15's real-time original (x15o-s00-0-043007) started from another snapshot, and its digests differ from tick 0.
  - Only one icount record exists.
- **`oracle.core_list_sha12`:** w2 `96a860ff33cb`, w1 `4dc73bf5cb42`. This is sha256[:12] of `json.dumps(core)`: one comparison for a whole run.
- **v0.15 (job J01, 2026-10-16 22:28-22:33, `v015-oracle.json`):** the fixtures' v0.15 copy (`xemu_version` 0.15), null renderer, icount; our gdbstub replayer against L3.
  - **w1-seg0:** start check `c6af3ec882b3` (equal); all 475 CORE digests equal the pack's; L3 exact on every observed field; the popup cleared at tick 415, as on v0.14-12. Its RAM hashes (start `58df07e54b9d`, end `c767b89ab212`) are not an oracle (the popup's A comes on a wall-clock stall).
  - **w2-walk-pickup, twice, the second in a relaunched xemu:** the hop with **no A press** reached w2 in one load (3.4 s and 4.6 s) and gave the recorded start: start check `7548494a88eb` both times. All 1,067 CORE digests equal the pack's in both runs; L3 exact; HP 200 -> 155 and back as recorded. Whole RAM at the snapshot halt `aad6cc27fbb1` and at the end `7e4e74cc76a6`, **equal in both processes**: an oracle for v0.15.
  - `xemu-ram-digest`'s sha256 equals the sha256 of `xemu-dump-ram`'s file at every one of the 6 halts (`dump_equals_digest` true).
- **v0.15 by hooks (Step X.32, 2026-10-16, sitting x32 a06):** the w2 string from an in-memory checkpoint of the same start (the R7 hop to `W1_W2_Fall`), through the three hooks above and one `xemu-frame-advance` until the pad queue ran out: 1,067 ticks, 3,201 records, 0 dropped; every tick's observation equal to the gdbstub replay's; end RAM `7e4e74cc76a6`, the same as J01's; 8.58 ms a tick. So a hook replay from this start should end on `7e4e74cc76a6` too.

## Report back on v0.15

Run both replays by hooks on v0.15 from the start above, and send, per replay:
- the start check: `sha256(E[0:0x1C0] + [E+0x20][0:0x240])[:12]` (w2 `7548494a88eb`, w1 `c6af3ec882b3` on v0.14-12 and v0.15);
- `replays.py check`'s line: `ticks`, `first_diff` and `pass` (1,067 and 475 ticks; `first_diff` null on v0.14-12, v0.14-27 and v0.15);
- for w2 only, the whole-RAM sha256[:12] at the start halt and at the stop after the last tick (v0.15, ours: `aad6cc27fbb1` and `7e4e74cc76a6`);
- for w1, the tick the popup stopped the updates at (415 on v0.14-12 and v0.15) and how A was sent;
- ms a tick and the host load.

## Sharing

Public: the strings, the recipes, the addresses, the start hashes and the digests.

Never public: the D98 drive and states, the disc, the L3 snapshots, the corpus rows and any per-tick read (positions). The paths are in `LOCAL/01-replays.md`.
