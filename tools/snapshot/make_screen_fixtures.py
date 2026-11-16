"""Makes the golden test data for harness/src/screen_renderer: real snapshots and the text the
Python prototype (tools/bench/prompt_render.py) renders for them.

Picks, from snapshots captured with tools/bench/capture_snapshots.py, one snapshot for each
feature below, and writes per feature in harness/testdata/screens/:
  NAME.pb        the Snapshot, binary
  NAME.txt       the screen text the prototype renders
  NAME.options   the option kinds it numbers, one per line

The C++ renderer is a port of the prototype and is tested against this text. To change how
screens read, change the C++ and the prototype's expected text together, then regenerate
(or edit the .txt by hand, deliberately).

Usage: out/tools-venv/bin/python -I tools/snapshot/make_screen_fixtures.py <capture_dir>...
"""

import glob
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "out" / "proto"))
sys.path.insert(0, str(ROOT / "tools" / "bench"))
from google.protobuf import json_format  # noqa: E402
from proto import snapshot_pb2  # noqa: E402
from prompt_render import render_tight  # noqa: E402

OUT = ROOT / "harness" / "testdata" / "screens"


def units(d):
    return d["base"].get("nearby", {}).get("units", [])


def objects(d):
    return d["base"].get("nearby", {}).get("objects", [])


# name -> predicate over the snapshot as JSON.
FEATURES = {
    "crowd": lambda d: len(units(d)) >= 45,
    "town_trainers": lambda d: sum(1 for u in units(d) if u.get("subtitle")) >= 3,
    "fight": lambda d: any(u.get("targeting_me") for u in units(d)) and d["base"]["self"].get("in_combat"),
    "corpse_to_loot": lambda d: any(u.get("lootable") for u in units(d)),
    "herbs_and_objects": lambda d: any(o.get("kind") == "HERB" for o in objects(d)),
    "pack_of_mobs": lambda d: any(u.get("pack") for u in units(d)),
    "status_effect": lambda d: any(u.get("visual_state") for u in units(d)),
    "indoors": lambda d: d["base"].get("where", {}).get("indoors"),
    "hurt_enemy": lambda d: any(0 < u.get("health_pct", 100) < 100 and u.get("reaction") != "FRIENDLY"
                                for u in units(d)),
    "casting_unit": lambda d: any((u.get("activity") or {}).get("state") in ("CASTING", "CHANNELING")
                                  for u in units(d)),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    files = sorted(f for root in sys.argv[1:] for f in glob.glob(f"{root}/*/*.json"))
    wanted = dict(FEATURES)
    used = set()  # One snapshot per feature.
    for f in files:
        if not wanted:
            break
        d = json.load(open(f))
        for name in list(wanted):
            if f in used or not wanted[name](d):
                continue
            used.add(f)
            text, kinds = render_tight(d, numbered=True)
            message = json_format.ParseDict(d, snapshot_pb2.Snapshot())
            (OUT / f"{name}.pb").write_bytes(message.SerializeToString())
            (OUT / f"{name}.txt").write_text(text + "\n")
            (OUT / f"{name}.options").write_text("\n".join(kinds) + "\n")
            print(f"{name}: {pathlib.Path(f).parent.name}/{pathlib.Path(f).name} "
                  f"({len(text)} chars, {len(kinds)} options)")
            del wanted[name]
    for name in wanted:
        print(f"no snapshot with feature: {name}")


if __name__ == "__main__":
    main()
