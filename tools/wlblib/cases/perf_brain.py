"""What the brain costs: llama-server with the 35B model, driven by recorded wake-ups.

Needs the replay input, 50 bots' live snapshots (`wlb test perf --capture`, into
out/bench/snapshots/quest), and the model. Measures, as docs/phase2-results.md describes:
  capacity   wake-ups per second with 16 calls in flight, back to back (tools/bench/throughput.py)
  latency    from a wake-up being due to its plan read back, for 50 bots on 30 s plans (a normal
             load) and 100 bots on 15 s plans (about the capacity limit), replayed in real time
             (tools/bench/brain_load.py)
Wake to plan only: the module, the socket and the harness are not in it.
"""

import os
import re
import subprocess

from wlblib.common import OUT, ROOT, Failed, say
from wlblib.testing import LlamaServer, expect

NAME = "brain"
DESCRIPTION = "35B brain: capacity and wake-to-plan latency from recorded wake-ups"

SNAPSHOTS = OUT / "bench" / "snapshots" / "quest"
# The prompt the brain really gets (docs/phase2-results.md): numbered screen, grammar, counts.
ENV = {"MODE": "menu", "RENDER": "tight", "MERGED": "1", "COUNTS": "1", "SLOTS": "16",
       "PYTHONDONTWRITEBYTECODE": "1"}
CAPACITY_S = 30
WARMUP_S = 20
# (bots, mean plan length in s, seconds replayed)
CELLS = ((50, 30, 90), (100, 15, 90))

THROUGHPUT = re.compile(r"(\d+) workers: ([\d.]+) wake-ups/s \| one call p50 ([\d.]+)s "
                        r"p95 ([\d.]+)s \| valid (\d+)/(\d+)")
LOAD = re.compile(r"wake->plan p50 ([\d.]+)s p95 ([\d.]+)s max ([\d.]+)s .*? valid (\d+)/(\d+)")


def bench(script, *args):
    """Runs a script of tools/bench with the brain's prompt settings; returns its output."""
    result = subprocess.run(["python3", "-I", ROOT / "tools/bench" / script, *map(str, args)],
                            capture_output=True, text=True, env={**os.environ, **ENV})
    if result.returncode != 0:
        raise Failed(f"{script} failed:\n{(result.stderr or result.stdout).strip()[-600:]}")
    return result.stdout


def wakes_file(plan_s):
    """When bots would wake with plans of about `plan_s` seconds (tools/bench/trigger_sim.py on
    the captured snapshots, cached beside them)."""
    path = SNAPSHOTS.parent / f"wakes-{plan_s}.jsonl"
    if not path.exists():
        path.write_text(bench("trigger_sim.py", SNAPSHOTS, plan_s))
    return path


def run(perf):
    expect(SNAPSHOTS.is_dir(), "no replay input: record it with `tools/wlb test perf --capture`")
    expect(sum(1 for _ in SNAPSHOTS.iterdir()) >= 50, "fewer than 50 bots in the replay input")
    with LlamaServer():
        # A run straight after the model loads reads low (cold caches: 6.5 against 7.8 wake-ups/s
        # on the same input a few minutes later): one short run first, discarded.
        say(f"perf brain: warm-up ({WARMUP_S} s)")
        bench("throughput.py", SNAPSHOTS, wakes_file(30), 16, WARMUP_S)
        # Capacity: closed loop, 16 calls always in flight.
        say(f"perf brain: capacity ({CAPACITY_S} s, 16 in flight)")
        cap_out = bench("throughput.py", SNAPSHOTS, wakes_file(30), 16, CAPACITY_S)
        say("  " + cap_out.strip())
        m = THROUGHPUT.search(cap_out)
        expect(m, "throughput.py printed nothing readable")
        perf.measure("capacity_wakes_per_s", float(m.group(2)), "/s", lower_is_better=False,
                     factor=1.2)
        perf.measure("call_p50_s", float(m.group(3)), " s", factor=1.3, slack=0.2)
        perf.measure("capacity_valid", int(m.group(5)) / int(m.group(6)), "", lower_is_better=False,
                     factor=1, limit=1.0)

        # Latency under a replayed load.
        for bots, plan_s, seconds in CELLS:
            say(f"perf brain: replay ({bots} bots, {plan_s} s plans, {seconds} s)")
            load_out = bench("brain_load.py", SNAPSHOTS, wakes_file(plan_s), bots, seconds)
            for line in load_out.strip().splitlines():
                say("  " + line)
            m = LOAD.search(load_out)
            expect(m, f"brain_load.py printed nothing readable ({bots} bots, {plan_s} s plans)")
            cell = f"{bots}bots_{plan_s}s"
            perf.measure(f"p50_s[{cell}]", float(m.group(1)), " s", factor=1.5, slack=0.3)
            perf.measure(f"p95_s[{cell}]", float(m.group(2)), " s", factor=1.5, slack=0.5)
            perf.measure(f"valid[{cell}]", int(m.group(4)) / int(m.group(5)),
                         "", lower_is_better=False, factor=1, limit=1.0)
