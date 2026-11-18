"""Brain capacity: wake-ups per second when W workers fire real wake-ups back to back.

Each worker loops: pick a random captured wake-up (tools/bench/trigger_sim.py output), call the
brain exactly as brain_load.py does (MODE / RENDER env), repeat. Reports completions per second
and the latency of one call under that load. A steady closed loop, so it measures what the
server can sustain with W calls in flight, without replay timing.

Usage: python3 -I tools/bench/throughput.py <snapshot_dir> <wakes.jsonl> [workers=25] [seconds=30]
"""

import json
import os
import pathlib
import random
import statistics
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import brain_load as b  # noqa: E402


def main():
    snap_dir, wakes_file = sys.argv[1], sys.argv[2]
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 30
    wakes = [json.loads(line) for line in open(wakes_file)]
    if os.environ.get("PROMPT") == "harness":
        b.prepare_harness_requests(snap_dir, wakes)
    done, lat, first_lat, plans, failed = [], [], [], [], []
    lock = threading.Lock()
    t0 = time.perf_counter()
    warm = 5.0

    def worker(w):
        rng = random.Random(w)
        while time.perf_counter() - t0 < seconds + warm:
            wake = rng.choice(wakes)
            wake = {**wake, "source": wake["bot"], "copy": w, "worker": w}
            start = time.perf_counter()
            try:
                plan = b.call(wake, snap_dir, workers)[0]
            except Exception:  # Left out of the numbers, but counted: a failure isn't a pass.
                with lock:
                    failed.append(time.perf_counter())
                continue
            end = time.perf_counter()
            if start - t0 >= warm:
                with lock:
                    done.append(end)
                    lat.append(end - start)
                    first_lat.append(plan.get("_first_line_s", end - start))
                    plans.append(plan)

    threads = [threading.Thread(target=worker, args=(w,)) for w in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    b.revalidate(plans)
    valid = [bool(p.get("_valid", True)) for p in plans]
    print(f"{workers} workers: {len(done) / seconds:.1f} wake-ups/s | one call p50 "
          f"{statistics.median(lat):.2f}s p95 {sorted(lat)[int(0.95 * len(lat))]:.2f}s | "
          f"valid {sum(valid)}/{len(valid)} | first-line p50 "
          f"{statistics.median(first_lat):.2f}s p95 "
          f"{sorted(first_lat)[int(0.95 * len(first_lat))]:.2f}s"
          + (f" | {len(failed)} calls FAILED (left out)" if failed else ""))


if __name__ == "__main__":
    main()
