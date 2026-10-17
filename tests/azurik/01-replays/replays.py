#!/usr/bin/env python3
"""Pack 01-replays: Azurik per-tick input replays and their CORE digests.

  replays.py export --store DIR --out DIR   our side: from the private L3 store (its conformance.jsonl and two
                                            snapshots' headers); needs Azurik_Modkit's azurik_mod
  replays.py check NAME TICKS.json          the fork's side (stdlib): a run's per-tick reads against the pack

export writes only what may be public: the input strings, the start recipes, the start checks and the per-tick
CORE digests (sha256[:12]). It refuses a record whose string writes RAM (a placement), and writes no position.

check takes one item per player tick, in order: [pos, state, hp] as hex (12 B at [E+0x20]+0x15C, 4 B at E+0x94,
4 or 8 B at [E+0x20]+0x210, read at the update's RET), or test_b3.py's {"obs": [pos, state, hp, ...]} rows.
"""
import argparse, hashlib, json, sys
from pathlib import Path

POPUPS = ("write u32 0xFFFFFFFF at [[[0x1BF63C]+0x2C]+0xE0]+0x14 (the inventory's first-pickup popup bits)")
PACE = "if u32 [[0x38C058]+4] != 1, write 1 there (the slow-motion cvar: one scene tick a frame; pin_pace)"
START = ("run to the next 0x8CCC0 (E = [esp+4]), then to the next 0x8C230 with ECX == E: tick 0's update entry; "
         "check sha256(E[0:0x1C0] + [E+0x20][0:0x240])[:12] == start_check.start_sha12")
HOP = ["at that halt post the level request: 00 at 0x38D320 (no movie), 'levels/water/w2' NUL at 0x38D424, "
       "'W1_W2_Fall' NUL at 0x38D528, then u32 0x0038D320 at [[[0x1BF63C]+0x2C]+0xD8]+0x10",
       "run to the level load 0x53750, then to the 20th hit of 0x8CCC0 with u32 [esp+4]+0x94 in {0, 3, 6} after "
       "the last load; halted there"]
REPLAYS = {   # name: corpus label, segments file, start steps (USA vanilla VAs)
    "w2-walk-pickup": ("x22-w2", "w2-segments.json", HOP),
    "w1-seg0": ("x22-w1-seg0", "w1-seg0-segments.json", []),
}
ALSO = {"w2-walk-pickup": ("x26-w2", "0.14-27-g9aa0b21b78")}   # a second icount record of the same start


def core_digest(pos12: bytes, state: int, hp: int) -> str:
    """azurik_mod.sim.obs.digest(o, CORE): sha256[:12] of repr() of (x, y, z as 4-byte bytes objects, the f32s as
    read; state an unsigned int; hp a signed int), UTF-8."""
    return hashlib.sha256(repr((pos12[0:4], pos12[4:8], pos12[8:12], state, hp)).encode()).hexdigest()[:12]


def export(store: Path, out: Path) -> None:
    from azurik_mod.sim import l3
    recs = {}
    for ln in (store / "conformance.jsonl").read_text(encoding="utf-8").splitlines():
        r = json.loads(ln)
        recs.setdefault(r["label"], r)
    doc = {"format": "fork-pack-01/1", "replays": {}}
    for name, (label, file, hop) in REPLAYS.items():
        r, m = recs[label], l3.read_meta(l3.snapshot_path(label, store))
        segs, core = r["segments"], [c for c, _ in r["xemu"]]
        if r["writes"] or not r["passed"] or any(set(s) - {"n", "stick", "buttons", "cam"} for s in segs):
            raise SystemExit(f"{label}: writes RAM, failed, or holds more than pads and cameras")
        body = json.dumps(segs, sort_keys=True)   # the file's sha256[:12] is the string's
        if hashlib.sha256(body.encode()).hexdigest()[:12] != r["string_sha12"] or core != [c for c, _ in r["l3"]]:
            raise SystemExit(f"{label}: the string or the L3 trace doesn't match the record")
        (out / file).write_text(body, encoding="utf-8")
        also = []
        if name in ALSO:
            a = recs[ALSO[name][0]]
            also = [{"record": a["label"], "xemu": ALSO[name][1], "core_equal": [c for c, _ in a["xemu"]] == core}]
        doc["replays"][name] = {
            "segments": file, "string_sha12": r["string_sha12"], "ticks": len(core), "l3_mode": r["mode"],
            "start": {"disc": f"vanilla USA, sha256[:12] {m['iso_sha12']}", "loadvm": m["tag"],
                      "steps": ["loadvm, paused; run to the first 0x8CCC0", *hop, POPUPS, PACE, START]},
            "start_check": {"start_sha12": m["start_sha12"], "entity": hex(m["entity"]), "level": m["level"],
                            "state": m["state"], "hp": m["hp"], "xemu": m["xemu"]},
            "oracle": {"core": core, "core_list_sha12": hashlib.sha256(json.dumps(core).encode()).hexdigest()[:12],
                       "equals_l3": True, "record": label, "xemu": m["xemu"], "also": also}}
    (out / "replays.json").write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")


def check(name: str, ticks: list, pack: Path) -> bool:
    want = json.loads((pack / "replays.json").read_text(encoding="utf-8"))["replays"][name]["oracle"]["core"]
    rows = [t["obs"][:3] if isinstance(t, dict) else t for t in ticks]
    got = [core_digest(bytes.fromhex(p), int.from_bytes(bytes.fromhex(s), "little"),
                       int.from_bytes(bytes.fromhex(h)[:4], "little", signed=True)) for p, s, h in rows]
    first = next((k for k, (a, b) in enumerate(zip(got, want)) if a != b), None)
    first = min(len(got), len(want)) if first is None and len(got) != len(want) else first
    print(json.dumps({"replay": name, "ticks": len(got), "want": len(want), "first_diff": first,
                      "pass": first is None}))
    return first is None


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("--store", type=Path, required=True)
    e.add_argument("--out", type=Path, required=True)
    c = sub.add_parser("check")
    c.add_argument("name", choices=sorted(REPLAYS))
    c.add_argument("ticks", type=Path)
    c.add_argument("--pack", type=Path, default=Path(__file__).resolve().parent)
    a = ap.parse_args()
    if a.cmd == "export":
        export(a.store.expanduser(), a.out)
    else:
        sys.exit(0 if check(a.name, json.loads(a.ticks.read_text(encoding="utf-8")), a.pack) else 1)
