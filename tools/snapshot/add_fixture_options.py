"""Writes the module's options (and <case>.facts.json) into the eval snapshots, so each case shows
the screen a live bot sees (review 2026-11-17, finding 5; docs/plans/options.md).

For each case in harness/testdata/eval/cases.json: writes <case>.facts.json (when not --check),
renders the screen before and after (with `harness decide --dry-run`), writes `options` from
`out/common/fixture_options` (the module's AddOptions), and moves the case's option numbers
(`first_options`, `forbid_options`) to the new numbering by the option's text.

Usage: python3 -I tools/snapshot/add_fixture_options.py [--check]
  --check: exit 1 if any eval snapshot's options differ from what the module would offer.
"""

import json
import math
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
EVAL = ROOT / "harness" / "testdata" / "eval"
HARNESS = ROOT / "out" / "common" / "harness"
TOOL = ROOT / "out" / "common" / "fixture_options"
OPTION_RE = re.compile(r"^\s+(\d+)\. ([^:\n]+)", re.M)

# Interactive NPC flags from acore_world.creature_template for Durotar / Orgrimmar NPCs.
NPC_FLAGS_BY_ENTRY = {
    0x0C00: 386,   # 3072 Kawnie Softbreeze (General Goods)
    0x0C51: 51,    # 3153 Frang (Warrior Trainer)
    0x0C52: 51,    # 3154 Jen'shan (Hunter Trainer)
    0x0C55: 51,    # 3157 Shikrik (Shaman Trainer)
    0x0C56: 386,   # 3158 Duokna (General Goods)
    0x0E7B: 51,    # 3707 Ken'jai (Priest Trainer)
    0x0F2A: 642,   # 3882 Zlagk (Butcher)
    0x16FC: 51,    # 5884 Mai'ah (Mage Trainer)
}

# Valley of Trials 50-yd grind spot buckets from acore_world (WorldPlacesCache).
VALLEY_OF_TRIALS_BUCKETS = [
    (-742.9, -4196.4, 34.5, "boars, level 1-2"),
    (-691.7, -4348.4, 46.3, "boars, scorpids, level 1-3"),
    (-698.0, -4302.3, 43.5, "boars, level 1-2"),
    (-698.7, -4148.2, 33.2, "boars, scorpids, level 1-3"),
    (-617.7, -4400.2, 44.5, "boars, level 1-2"),
    (-552.3, -4402.9, 43.8, "boars, level 1-2"),
    (-503.6, -4158.5, 69.9, "boars, scorpids, level 1-3"),
    (-482.6, -4198.1, 50.6, "boars, level 1-2"),
    (-500.4, -4299.3, 41.7, "boars, level 1-2"),
    (-595.1, -4098.5, 74.9, "scorpids, level 3"),
    (-252.9, -4358.2, 62.1, "scorpids, imps, level 3-4"),
]

# Durotar / Orgrimmar service hubs from acore_world (WorldPlacesCache).
DUROTAR_HUBS = [
    ("place:the_den", "The Den",
     "vendor, repair, priest trainer, graveyard", -617.4, -4202.4, 38.3),
    ("place:razor_hill", "Razor Hill",
     "innkeeper, vendor, repair, priest trainer", 340.4, -4686.3, 16.5),
]

ORGRIMMAR_HUBS = [
    ("place:valley_of_strength", "Valley of Strength",
     "innkeeper, vendor, repair", 1633.9, -4439.4, 15.5),
    ("place:valley_of_spirits", "Valley of Spirits",
     "priest trainer", 1453.8, -4178.9, 44.1),
    ("place:The_drag", "The Drag",
     "vendor, repair, flight master", 1676.2, -4315.3, 61.6),
]

SECTORS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]


def compass_dir(dx, dy):
    if math.hypot(dx, dy) < 2.0:
        return "HERE"
    deg = math.degrees(math.atan2(-dy, dx))
    if deg < 0:
        deg += 360.0
    return SECTORS[int(round(deg / 45.0)) % 8]


def build_facts(snap):
    base = snap.get("base", {})
    self_ = base.get("self", {})
    where = base.get("where", {})
    pos = where.get("position", {})
    bx = float(pos.get("x", 0.0))
    by = float(pos.get("y", 0.0))
    bz = float(pos.get("z", 0.0))

    npcs = []
    for u in base.get("nearby", {}).get("units", []):
        if u.get("reaction") != "FRIENDLY" or u.get("dead") or not u.get("guid"):
            continue
        g = u["guid"]
        if g.startswith("0xF130") and len(g) == 18:
            entry = int(g[6:12], 16)
            if entry in NPC_FLAGS_BY_ENTRY:
                npcs.append({"guid": g, "npc_flags": NPC_FLAGS_BY_ENTRY[entry]})

    facts = {"has_food": True, "has_drink": True}
    if npcs:
        facts["npcs"] = npcs

    if self_.get("in_combat") or self_.get("state", "ALIVE") != "ALIVE":
        return facts

    zone = where.get("zone", "")
    if zone == "Durotar":
        cand = []
        for sx, sy, sz, label in VALLEY_OF_TRIALS_BUCKETS:
            dist = math.sqrt((sx - bx) ** 2 + (sy - by) ** 2 + (sz - bz) ** 2)
            if 25.0 <= dist <= 2500.0:
                d = compass_dir(sx - bx, sy - by)
                sid = f"spot:1:{int(round(sx))}:{int(round(sy))}"
                cand.append((dist, {"id": sid, "dir": d, "dist_yd": round(dist, 1),
                                    "label": label, "pos": {"x": sx, "y": sy, "z": sz}}))
        cand.sort(key=lambda x: x[0])
        spots, seen_dirs = [], set()
        for _, sp in cand:
            if len(spots) >= 3:
                break
            if sp["dir"] in seen_dirs:
                continue
            seen_dirs.add(sp["dir"])
            spots.append(sp)
        if spots:
            facts["spots"] = spots

        places = []
        for pid, pname, services, px, py, pz in DUROTAR_HUBS:
            dist = math.sqrt((px - bx) ** 2 + (py - by) ** 2 + (pz - bz) ** 2)
            if dist >= 25.0:
                places.append({
                    "id": pid, "name": pname, "services": services,
                    "dir": compass_dir(px - bx, py - by), "dist_yd": round(dist, 1),
                    "pos": {"x": px, "y": py, "z": pz},
                })
        if places:
            facts["places"] = places
    elif zone == "Orgrimmar":
        places = []
        for pid, pname, services, px, py, pz in ORGRIMMAR_HUBS:
            dist = math.sqrt((px - bx) ** 2 + (py - by) ** 2 + (pz - bz) ** 2)
            if dist >= 25.0:
                places.append({
                    "id": pid, "name": pname, "services": services,
                    "dir": compass_dir(px - bx, py - by), "dist_yd": round(dist, 1),
                    "pos": {"x": px, "y": py, "z": pz},
                })
        if places:
            facts["places"] = places

    return facts


def screen_options(path):
    out = subprocess.run([str(HARNESS), "decide", str(path), "--dry-run"],
                         capture_output=True, text=True, check=True).stdout
    return {int(n): text.strip() for n, text in OPTION_RE.findall(out)}


def module_options(path):
    out = subprocess.run([str(TOOL), str(path)], capture_output=True, text=True,
                         check=True).stdout
    return json.loads(out).get("options", [])


def main():
    check = "--check" in sys.argv[1:]
    cases = json.loads((EVAL / "cases.json").read_text())
    stale, rejudge = [], []
    for case in cases:
        path = EVAL / f"{case['name']}.snapshot.json"
        facts_path = EVAL / f"{case['name']}.facts.json"
        snap = json.loads(path.read_text())
        if not check:
            facts = build_facts(snap)
            facts_path.write_text(json.dumps(facts, indent=2, ensure_ascii=False) + "\n")
            case.pop("planned", None)
        want = module_options(path)
        if snap.get("options") == want:
            continue
        if check:
            stale.append(case["name"])
            continue
        before = screen_options(path)
        snap["options"] = want
        path.write_text(json.dumps(snap, indent=2, ensure_ascii=False) + "\n")
        after = {text: n for n, text in screen_options(path).items()}
        for key in ("first_options", "forbid_options"):
            if key not in case:
                continue
            moved = [after[before[n]] for n in case[key] if before.get(n) in after]
            dropped = [before.get(n, f"#{n}") for n in case[key] if before.get(n) not in after]
            case[key] = moved
            if dropped:
                print(f"{case['name']}: {key} dropped {dropped}")
            if key == "first_options" and not moved:
                rejudge.append(case["name"])
    if check:
        if stale:
            print("options out of date (run tools/snapshot/add_fixture_options.py):",
                  ", ".join(stale))
            return 1
        return 0
    (EVAL / "cases.json").write_text(json.dumps(cases, indent=2, ensure_ascii=False) + "\n")
    if rejudge:
        print("judge again (no first option left):", ", ".join(rejudge))
    return 0


if __name__ == "__main__":
    sys.exit(main())
