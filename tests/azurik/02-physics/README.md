# 02-physics: Azurik's player control numbers

**Why:** a moved RAM hash doesn't say whether gameplay changed. v0.15 moved the 120-vblank end hashes of `vanilla-rz-w1-dark`, `vanilla-rz-e7-keylord` and `vanilla-rz-e7-dark` (unchanged since v0.14-93, fork note, part 7 F):
- each by two 4-byte words in physical page `0x274000` (VA `0x1C5000`): w1-dark's at `0x274db4`/`0x274db8`, e7-keylord's at `0x2746b4`/`0x2746b8`, e7-dark's at `0x274a34`/`0x274a38`;
- the words are layer 0's animation values (+0x164/+0x168) in records 2, 0 and 1 of the game's shader-instance cache at VA `0x1C5550`, computed from a NaN rate. v0.15 keeps the NaN's payload (`0x3FFF8C`), as x87 does; v0.14-36 wrote the default NaN. So the cause is `0ff9a1eaf7` (x87 NaN payloads in inline x87 code), and the game reads a NaN either way (Step X.32, sitting x32, 2026-10-16, v0.15 against v0.14-36).
- That case was settled by reading the bytes. These numbers settle the next one without that work.

These numbers are the game's own physics, measured on the vanilla disc. A build that keeps them all has not changed how the player walks, rolls, jumps, flaps, swims, takes hits or climbs a ledge. The fork has no physics number today: `git grep` finds no `4.643`, `0x8CCC0` or `6.687`.

**Exercises:**
- the player state machine (`0x8CCC0`, once per tick);
- x87 float math (sqrt for the jump, `0.1^dt` for the swim blend, Euler steps of gravity);
- the move and collision code (ground friction, the step-up onto a ledge, a slope state);
- the armor fuel code (the shield drop);
- an enemy's AI timing and fixed damage;
- the cold-boot path (`prophecy.bik`, the menus, `levels/training_room`).

## Files

| File | Bytes | What it is |
|---|---|---|
| `physics.json` | 12,759 | 12 cases (8 rows): start, poke, tick count, input schedule, and the expected numbers with their source and builds (`enemy-hits` and `ledge-j14`: v0.15's icount numbers, s6's beside them as `s6_expect`) |
| `physics_check.py` | 7,314 | stdlib only. Prints the collect hook (`hook`), and turns the hook's records into rows and judges them against a case's numbers (`check CASE RECORDS.json`) |
| `ours/j02_physics_v015.py` | 2,603 | Our measurement driver for v0.15 (job J02); it writes `v015-physics.json` here |
| `v015-physics.json` | 4,341 | J02's v0.15 numbers and verdicts for all 12 cases (logged in our sitting x38-j01; `LOCAL/02-physics.md`) |

## The rows

All rows are from D98 states on the vanilla USA disc, except the cold boot. They were measured in sitting s6 (2026-10-11, v0.13.3-37-gd5ad94f4bf, normal mode), and all but two hold under icount. `enemy-hits` and `ledge-j14` expect the fork's v0.15 icount numbers, with the schedule tagged `@F+1` (the fork's PROMPT-9 answer, 2026-10-17).

| Case | Start | Input (row: pipe line) | Control numbers |
|---|---|---|---|
| `walk` | `vanilla-flat` | 10 down lstick_up, 55 up | Speed (hypot of +0x24/+0x28) at rows 12-14: 3.26691, 6.53610, 7.00009 m/s. Plateau 7.00009. Stop at rows 56-59: 7.00009 → 3.73227 → 0.46353 → 0. |
| `roll` | `vanilla-flat` | 10 down black, 13 up | State 0x100 for 21 ticks from row 12. The body (+0x38/+0x3C) moves 3.37521 m from row 11 to row 33. |
| `jump` | `vanilla-wall` | 5 down y, 6 up | Takeoff row 7. v0 4.643274784, h 1.1. Δvz −0.326667 on every airborne tick. 28 airborne ticks. Apex 1.0235 m on +0x1A4 (as a change). Lands in state 0. |
| `swim` | `vanilla-ocean` | 10 down lstick_up, 100 up | From row 12, v_h(n) = 10·(1 − 0.1^(n/30)) within 1.2e-6 over n = 1-90 (0.73881, 9.0, 9.9, 9.99 at n 1, 30, 60, 90). From row 102, ×0.926119 per tick. State 6 throughout. |
| `beach-jump` | `vanilla-beach-shield5` | 40 down y, 46 up | With air power: takeoff row 42, v0 4.849742413, h 1.2, 29 airborne ticks, apex 1.1199 m. |
| `beach-flaps` | `vanilla-beach-shield5` | Y tapped (2 rows) at 4, 12, …, 60, then 200, 208, 216 | 5 flaps at rows 14, 22, 30, 38, 46: vz 4.849742, 3.825263, 1.764722, 0.992530, 1.056628. Fuel 0.8, 0.6, 0.4, 0.2, 0.0. At row 46 the armor goes Level 3 → 1, Flaps 5 → 0: **the 5th flap drops the shield**. The later presses don't flap. 64 airborne ticks, landing at row 70. The next jump (row 202) is plain: v0 4.643274784, h 1.1, no flap. |
| `enemy-hits` | `vanilla-enemy`, HP poked to 400 at row 0 | 1 down lstick_up, 90 up | A blaze sentinel hits twice: HP −25 at rows 188 and 425 (v0.15, icount, `@F+1`, in three processes). s6 had 186 and 421 from real-time untagged input. After each: state 0x102 for 13 rows, 2 for 6, then 0 (on every build). The hit rows follow input timing, not the CPU: one vblank later (`@F+2`) gives 187 and 361. |
| `ledge-j14`, `-j16`, `-j28`, `-j30` | `vanilla-wall` | 5 down lstick_up, J down y, J+1 up y, 85 up lstick_up | A running jump at row 16 or 28 (3.69 and 0.89 m before the blocked spot) ends 1.2578 m up, on the ledge, with no other state (0 → 2 → 0). At row 14 (4.39 m), under icount, it ends on the ledge too: rise 1.2578 m, states 2 at 16, 3 at 38, 0 at 40, 0x100 at 41, 0 at 46 (v0.15, `@F+1` in two processes; `@F+2` moves each row by +1). s6's floor ending (0 → 2 → 0) was a real-time run; its cause is most likely real-time tick timing [inferred, the fork's], not the input: moving the input one vblank (`@F+2`) leaves it on the ledge. At row 30 (0.42 m) it ends on the floor and passes through state 3 (slope). |
| `cold-jump` | power on, vanilla disc | The menu route, then one Y | v0 4.643274784, h 1.1, Δvz −0.326667, 28 airborne ticks, landed. `.text` 979,408 B, 0 bytes differ. |

Each row's exact schedule, tick count and expected values are in `physics.json`.
- `physics_check.py` reproduced every s6 row from its original capture (11 of 11 cases). The same 11 also pass round-tripped through the hook-record decoder.
- The cold jump's capture wasn't kept. Its numbers are X.30 §3's.

Corrections to the inventory's rows:
- The walk's stop sequence is from a 45-tick hold (`04-walk_lstick_up_45.json`). The 60-tick hold meets an obstacle (7.00009 → 5.82354 → 1.45881 → 0), and 30-tick holds stop as 3.73684 → 0.46811.
- The beach row needs two cases: a plain jump for the 29 ticks, and the flap jump (64 airborne ticks).
- The plain jump's apex 1.0235 m is also on `vanilla-flat` (s6 `07-jump.json`). It is kept here on `vanilla-wall`, as s6 §5 has it.

## The hook and read recipe (at `0x8CCC0`, entity `[esp+4]`)

`python3 -I physics_check.py hook` prints the hook: `{"pcs": [576704], "key": {"addr": "[esp+4]+0x18", "size": 8}, "collect": [...]}`. The key is the entity's event clock (f64), so there is one record per tick. The collect regions, in order:

| Field | Address | Type |
|---|---|---|
| vx vy vz | `[esp+4]+0x24` | 3 f32 |
| x y z | `[esp+4]+0x38` | 3 f32, a position: differences only |
| state | `[esp+4]+0x94` | u32 |
| t (air timer) | `[esp+4]+0x9C` | f32 |
| flaps used | `[esp+4]+0xD8` | i32 |
| h (jump height) | `[esp+4]+0x144` | f32 |
| bz (airborne height) | `[esp+4]+0x1A4` | f32, a position: as a change only. It can repeat the previous tick (8 of 28 rows in s6's tap run): take the arc's maximum. |
| hp, hp max | `[[esp+4]+0x20]+0x210` | 2 i32 |
| fuel | `[[[[esp+4]+0x20]+0x154]+0xCC]+0x24` | f32 |
| armor Level | `[[[[[esp+4]+0x20]+0x154]+0xCC]+0x20]+0x8` | i32 |
| Flaps | `[[[[[esp+4]+0x20]+0x154]+0xCC]+0x20]+0x38` | i32 |

If the fork's `key` takes no `[ ]` expression, resolve E = `[esp+4]` at row 0 and key on `E+0x18`.

## How the fork runs a case (no gdbstub needed)

1. `loadvm <start>` paused. Use a clone of D98's drive, which the fork's fcheck already loads (32/32 on v0.15).
2. `xemu-hooks-set` with the hook above.
3. `xemu-run-until {"until": [{"pc": 576704}], "frames": 600}`. The hit is row 0.
4. **The poke** (`enemy-hits` only): at row 0, i32 400 at `[[esp+4]+0x20]+0x210`. Use a one-entry write hook at `0x8CCC0`, or the gdbstub.
5. **The input:** for each row k in the schedule, in order:
   1. run-until `0x8CCC0` with `hits` = k minus the current row;
   2. at that halt, write the row's lines to `XEMU_INPUT_PIPE`: `down`/`up` with the scancode the fork's keyboard map gives the named pad input (`lstick_up`, `y`, `black`), for the keyboard pad on port 2.

   A line written at the halt of row k reaches ControllerState at row k+1 and moves the player at row k+2, as in s6. Tag it `@F+1`, where F is `query-xemu-frame`'s `frame` at the halt: `physics.json`'s numbers are `@F+1`'s. It equals our untagged gdbstub capture (J02) in 11 of 12 cases, not in `enemy-hits` (J02: rows 175 and 348), whose hits follow input timing.
6. Run on to the case's tick count. Pass the records the replies carry (`hooks.records`) to `physics_check.py check <case> records.json`.

**The cold jump:** the fork's own cold boot to `levels/training_room`, by the route in the `cold-jump` case's `start`. Then the same hook, with Y down on the first row ≥ 4 in state 0 and up 6 rows later.

## How we run it (our scripts)

- **Per D98 case:** `Sitting.loadvm` and `Sitting.set_hp`, then `Sandbox.capture` (`scripts/xemu_oracle.py`). Capture is tick-locked on `0x8CCC0` over the gdbstub, and sends each schedule line at its row.
- **The cold boot:** `skip_prophecy`, `walk_to_level` (`TITLE`), `to_player_tick` (`ARENA_STEADY_S` 20 s), `code_identity`, then `capture` with `_press_jump` (X.30's `d05_jump.py`).
- **Standalone:** `uv run --locked python scripts/xemu_oracle.py jump --ticks 45 --app <the xemu.app>`.
- **v0.15:** `ours/j02_physics_v015.py` runs all 12 cases in one sitting (job J02).

## Measured on which builds

- **Every D98 row:** s6, v0.13.3-37-gd5ad94f4bf, normal mode (not icount).
- **Under icount:**
  - v0.14-12, vanilla-flat (X.24 d05): v0 4.643274784.
  - v0.14-36, offscreen deterministic (2.75b S1-01): roll 21 ticks, flat jumps 28 ticks, apex 1.024 m. Those are from that sitting's own starts.
  - v0.15: every case, by job J02 (below).
- **The cold jump:** v0.13.3-37 (the region study's usa-na2 boot), v0.14-12 (X.24 §2, deterministic) and v0.14-36 (X.30 §3, deterministic, 27.4 s from the relaunch). All gave v0 4.643274784, 28 ticks. The region study's four Europe boots belong to pack 06.
- **v0.15 (job J02, 2026-10-16 22:33-22:34, `v015-physics.json`):** the fixtures' v0.15 copy, null renderer, `-S -icount shift=2,align=off,sleep=off`, our gdbstub capture (`Sandbox.capture`), lines untagged; the first time the walk, swim, flap, hit and ledge rows ran under icount. 10 of 12 cases equal s6. `enemy-hits` hit at rows 175 and 348; `ledge-j14` ended on the ledge (the rows `physics.json` now expects).
- **v0.15 (the fork, 2026-10-17, its PROMPT-9 answer part 02):** binary `e1869fccc44a`, all 12 cases by hooks, icount, null, schedule lines tagged `@F+1`; every case gave exactly its tick count of records. 10 of 12 equal s6, with J02's numbers (the swim's model error 1.1398707e-06; the cold jump's route: one START at the movie, one at the title, A to `levels/training_room`, three A for the welcome dialog). `enemy-hits`: rows 188 and 425, end RAM `75b7170edc8f` in three processes; `@F+2`: 187 and 361 (`389493182913`). `ledge-j14`: on the ledge, J02's states, `ff2d63cd224a` in two processes (one ran the cases in reverse); `@F+2`: on the ledge, each row +1 (`66b5a70573ae`). So `enemy-hits` follows input timing (s6's 186/421, J02's 175/348, the fork's 188/425), not a CPU or x87 change, and `ledge-j14` is stable under icount; s6's floor ending is most likely real-time tick timing [inferred, the fork's]. `physics.json` took the `@F+1` numbers for both (s6's stay as `s6_expect`).

## Report back on v0.15

Received 2026-10-17 (the fork's PROMPT-9 answer, part 02; above): all 12 cases pass against `physics.json` as it is now, and both changed cases repeat across processes. A later build reports per case `physics_check.py check`'s JSON line (its `numbers`, `pass` and `bad`), with the schedule tagged `@F+1`; `enemy-hits` moves with any change in input timing, so judge it by its tagging first.

## Sharing

Public: the numbers, the tags, the addresses and the schedules.

Never public: s6's captures (`captures/*.json` hold positions per tick), the D98 drive and the disc. The paths are in `LOCAL/02-physics.md`.
