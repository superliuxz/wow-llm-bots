"""Does a smaller model choose like the big one? (agreement with a reference model's plans)

`collect` runs captured wake-ups through whatever llama-server is running (greedy: TEMP=0) and
saves the plans; `compare` measures how often a model's plans agree with a reference's, on the
same screens. A rough stand-in for a quality check: it says whether the small model picks what
the big one picks, not whether either is right.

  python3 -I tools/bench/agreement.py collect <snapshot_dir> <wakes.jsonl> <out.json> [limit=200]
  python3 -I tools/bench/agreement.py compare <reference.json> <other.json>

Uses the same env as brain_load.py (MODE=menu MERGED=1 RENDER=tight COUNTS=1 ...).
"""

import json
import os
import pathlib
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import brain_load as b  # noqa: E402
from prompt_render import render_tight  # noqa: E402


def collect(snap_dir, wakes_file, out_file, limit):
    wakes, seen = [], set()
    for line in open(wakes_file):
        w = json.loads(line)
        if (w["bot"], w["seq"]) not in seen:
            seen.add((w["bot"], w["seq"]))
            wakes.append(w)
    wakes = wakes[:limit]
    results, lock = {}, threading.Lock()
    todo = list(enumerate(wakes))
    t0 = time.perf_counter()

    def worker(slot):
        while True:
            with lock:
                if not todo:
                    return
                i, w = todo.pop(0)
            wake = {**w, "source": w["bot"], "copy": slot, "worker": slot}
            plan, usage, _ = b.call(wake, snap_dir, 16)
            _, kinds = render_tight(b.snapshot(snap_dir, w["bot"], w["seq"]), numbered=True)
            with lock:
                results[f"{w['bot']}:{w['seq']}"] = {"plan": plan, "kinds": kinds,
                                                     "tokens": usage["completion_tokens"]}

    threads = [threading.Thread(target=worker, args=(s,)) for s in range(16)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    pathlib.Path(out_file).write_text(json.dumps(results))
    print(f"{len(results)} plans in {time.perf_counter() - t0:.0f}s -> {out_file}")


def first_step(plan):
    do = (plan.get("do") or "").split(",")[0].strip()
    number, _, times = do.partition("x")
    return (int(number), int(times) if times else 1) if number.isdigit() else (None, 1)


def compare(ref_file, other_file):
    ref, other = json.load(open(ref_file)), json.load(open(other_file))
    keys = sorted(set(ref) & set(other))
    same_number = same_kind = both_valid = same_look = say = 0
    for k in keys:
        a, c = ref[k], other[k]
        if not (a["plan"].get("_valid") and c["plan"].get("_valid")):
            continue
        both_valid += 1
        (na, _), (nc, _) = first_step(a["plan"]), first_step(c["plan"])
        same_number += na == nc
        same_kind += a["kinds"][na - 1] == c["kinds"][nc - 1]
        la, lc = float(a["plan"]["look"]), float(c["plan"]["look"])
        same_look += max(la, lc) / max(min(la, lc), 1) <= 2
        say += "say" in c["plan"]
    valid = lambda r: sum(1 for k in keys if r[k]["plan"].get("_valid"))
    n = max(both_valid, 1)
    print(f"{len(keys)} wake-ups | valid plans: reference {valid(ref)}, other {valid(other)}")
    print(f"first step: same option {same_number / n:.0%}, same kind of action {same_kind / n:.0%}")
    print(f"look time within 2x: {same_look / n:.0%} | other model adds a chat line to "
          f"{say / n:.0%} of plans (reference "
          f"{sum('say' in ref[k]['plan'] for k in keys) / len(keys):.0%})")
    print(f"plan tokens: reference {sum(ref[k]['tokens'] for k in keys) / len(keys):.0f}, "
          f"other {sum(other[k]['tokens'] for k in keys) / len(keys):.0f}")


if __name__ == "__main__":
    if sys.argv[1] == "collect":
        collect(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 200)
    else:
        compare(sys.argv[2], sys.argv[3])
