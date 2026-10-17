#!/usr/bin/env python3
"""Pack 02-physics: Azurik's player control numbers from per-tick rows, judged against physics.json (stdlib only).

  physics_check.py hook                 the xemu-hooks-set hook: one record per player tick at 0x8CCC0
  physics_check.py check CASE RECORDS   a case's numbers from that hook's records (a JSON list of records, or of
                                        their "data" lists), judged against the case's "expect"

A row is the player at the entry of the player tick 0x8CCC0 (entity = [esp+4]); row k is the k-th tick from the
first 0x8CCC0 after the load. Our harness's Sandbox.capture rows have the same field names. x, y, z and bz are
positions: only differences (a distance, a rise, an apex) leave the machine, never a row.
"""
import argparse, json, math, struct, sys
from pathlib import Path

TICK = 0x8CCC0
E, CR, SLOT = "[esp+4]", "[[esp+4]+0x20]", "[[[[[esp+4]+0x20]+0x154]+0xCC]+0x20]"   # entity, critter, armor slot
FIELDS = [("vx vy vz", E + "+0x24", "<3f"), ("x y z", E + "+0x38", "<3f"), ("state", E + "+0x94", "<I"),
          ("t", E + "+0x9C", "<f"), ("flaps_used", E + "+0xD8", "<i"), ("h", E + "+0x144", "<f"),
          ("bz", E + "+0x1A4", "<f"), ("hp hp_max", CR + "+0x210", "<2i"),
          ("fuel", "[[[[esp+4]+0x20]+0x154]+0xCC]+0x24", "<f"), ("armor_level", SLOT + "+0x8", "<i"),
          ("flaps", SLOT + "+0x38", "<i")]


def hook() -> dict:
    return {"pcs": [TICK], "key": {"addr": E + "+0x18", "size": 8},
            "collect": [{"addr": a, "size": struct.calcsize(f)} for _, a, f in FIELDS]}


def decode(records: list) -> list[dict]:
    rows = []
    for r in records:
        row = {"k": len(rows)}
        for (names, _, f), h in zip(FIELDS, r["data"] if isinstance(r, dict) else r):
            row.update(zip(names.split(), struct.unpack(f, bytes.fromhex(h)) if h else [None] * len(names.split())))
        rows.append(row)
    return rows


def hs(r: dict) -> float:
    return math.hypot(r["vx"], r["vy"])


def fin(v) -> bool:
    return isinstance(v, (int, float)) and math.isfinite(v)


def jump(rows: list[dict], start: int = 1) -> dict:
    """The first takeoff at or after row ``start`` (state 2 from not 2): v0, the airborne rows, the per-tick
    change of vz, the apex (the highest +0x1A4 over the airborne rows less the row before takeoff's)."""
    i = next((j for j in range(max(start, 1), len(rows)) if rows[j - 1]["state"] != 2 and rows[j]["state"] == 2), None)
    if i is None:
        return {"takeoff_k": None}
    j = i
    while j < len(rows) and rows[j]["state"] == 2:
        j += 1
    air, bz = rows[i:j], [r["bz"] for r in rows[i - 1:j]]
    return {"takeoff_k": i, "v0": round(rows[i]["vz"], 9), "h": round(rows[i]["h"], 6), "airborne_ticks": len(air),
            "dvz_set": sorted({round(b["vz"] - a["vz"], 6) for a, b in zip(air, air[1:])}),
            "apex_m": round(max(bz[1:]) - bz[0], 4) if all(map(fin, bz)) else None,
            "land_k": j if j < len(rows) else None, "land_state": rows[j]["state"] if j < len(rows) else None}


def runs(states: list[int]) -> list:
    out = []
    for s in states:
        if out and out[-1][0] == hex(s):
            out[-1][1] += 1
        else:
            out.append([hex(s), 1])
    return out


def numbers(case: dict, rows: list[dict]) -> dict:
    kind, p = case["kind"], case.get("at", {})
    st = [r["state"] for r in rows]
    if kind == "walk":
        a, b = p["press"], p["release"]
        return {"ramp": [round(hs(r), 5) for r in rows[a + 2:a + 5]],
                "plateau": sorted({round(hs(r), 5) for r in rows[a + 4:b + 2]}),
                "stop": [round(hs(r), 5) for r in rows[b + 1:b + 5]], "states": sorted({hex(s) for s in st})}
    if kind == "roll":
        rl = [k for k, s in enumerate(st) if s == 0x100]
        a, b = rows[rl[0] - 1], rows[rl[-1] + 1]
        return {"roll_first_k": rl[0], "roll_ticks": len(rl), "contiguous": rl == list(range(rl[0], rl[-1] + 1)),
                "roll_m": round(math.hypot(b["x"] - a["x"], b["y"] - a["y"]), 5)}
    if kind in ("jump", "cold-jump"):
        return jump(rows)
    if kind == "swim":
        i0 = next(k for k, r in enumerate(rows) if hs(r) > 0.01)
        n = p["release"] - p["press"]
        err = max(abs(hs(rows[i0 + m - 1]) - 10 * (1 - 0.1 ** (m / 30))) for m in range(1, n + 1))
        return {"first_moving_k": i0, "v_h": [round(hs(rows[i0 + m - 1]), 5) for m in (1, 30, 60, 90)],
                "model_max_err": err, "states": sorted({hex(s) for s in st}),
                "release_ratio": sorted({round(hs(rows[k]) / hs(rows[k - 1]), 6)
                                         for k in range(p["release"] + 2, p["release"] + 11)})}
    if kind == "flaps":
        fu, lv = [r["flaps_used"] for r in rows], [r["armor_level"] for r in rows]
        fk = [k for k in range(1, len(rows)) if fu[k] > fu[k - 1]]
        drop = next(k for k in range(1, len(rows)) if lv[k] != lv[0])
        j1 = jump(rows)
        j2 = jump(rows, j1["land_k"] + 1)
        return {"takeoff_k": j1["takeoff_k"], "v0": j1["v0"], "airborne_ticks": j1["airborne_ticks"],
                "land_k": j1["land_k"], "flap_k": fk, "flap_vz": [round(rows[k]["vz"], 6) for k in fk],
                "fuel": [round(rows[k]["fuel"], 3) for k in fk], "drop_k": drop,
                "after_drop": [lv[drop], rows[drop]["flaps"]], "level_before": lv[0],
                "next_v0": j2["v0"], "next_h": j2["h"], "next_flaps": max(fu[j2["takeoff_k"]:j2["land_k"]])}
    if kind == "hits":
        hp = [r["hp"] for r in rows]
        hits = [k for k in range(1, len(rows)) if hp[k] < hp[k - 1]]
        after = []
        for k in hits:
            end = next((j for j in range(k, len(rows)) if st[j] == 0), len(rows))
            after.append(runs(st[k:end]))
        return {"hp_first": hp[0], "hits": [[k, hp[k - 1] - hp[k]] for k in hits], "after_hit": after}
    if kind == "ledge":
        return {"rise_m": round(rows[-1]["z"] - rows[0]["z"], 4),
                "states": [[k, hex(st[k])] for k in range(1, len(st)) if st[k] != st[k - 1]]}
    raise ValueError(f"unknown kind {kind!r}")


def judge(case: dict, got: dict) -> dict:
    """Every expectation: ``key`` equal, ``key_le`` at most."""
    bad = []
    for k, want in case["expect"].items():
        v = got.get(k[:-3]) if k.endswith("_le") else got.get(k)
        if not (v is not None and v <= want if k.endswith("_le") else v == want):
            bad.append(f"{k}: {v!r}, want {want!r}")
    return {"numbers": got, "pass": not bad, "bad": bad}


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("hook")
    c = sub.add_parser("check")
    c.add_argument("case")
    c.add_argument("records", type=Path)
    c.add_argument("--spec", type=Path, default=Path(__file__).resolve().parent / "physics.json")
    a = ap.parse_args()
    if a.cmd == "hook":
        print(json.dumps({"hooks": [hook()]}, indent=1))
        sys.exit(0)
    case = next(x for x in json.loads(a.spec.read_text(encoding="utf-8"))["cases"] if x["id"] == a.case)
    r = judge(case, numbers(case, decode(json.loads(a.records.read_text(encoding="utf-8")))))
    print(json.dumps({"case": a.case, **r}))
    sys.exit(0 if r["pass"] else 1)
