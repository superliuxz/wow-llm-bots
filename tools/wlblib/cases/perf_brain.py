"""What the brain costs: llama-server driven by C++ `harness replay` over captured snapshots.

Needs the replay input, 50 bots' live snapshots (`wlb test perf --capture`, into
out/bench/snapshots/quest), and the model. Measures:
  capacity   wake-ups per second with 16 calls in flight, back to back
  latency    wake-to-plan wait for 50 bots (30 s plans) and 100 bots (15 s plans)
"""

import re
import subprocess

from wlblib import build
from wlblib.common import OUT, ROOT, Failed, say
from wlblib.testing import LlamaServer, expect

NAME = "brain"
DESCRIPTION = "brain capacity and wake-to-plan latency via C++ harness replay"

SNAPSHOTS = OUT / "bench" / "snapshots" / "quest"
RULES = ROOT / "content" / "RULES.md"
CAPACITY_S = 30
WARMUP_S = 20
CELLS = ((50, 30, 90), (100, 15, 90))

THROUGHPUT = re.compile(
    r"(\d+) workers: ([\d.]+) wake-ups/s \| one call p50 ([\d.]+)s "
    r"p95 ([\d.]+)s \| valid (\d+)/(\d+)"
)
FAILED = re.compile(r"(\d+) calls FAILED")
LOAD = re.compile(
    r"wake->plan p50 ([\d.]+)s p95 ([\d.]+)s max ([\d.]+)s \| valid (\d+)/(\d+)"
)


def _replay(*args):
    result = subprocess.run(
        [str(OUT / "common" / "harness"), "replay", str(SNAPSHOTS),
         f"--rules={RULES}", "--slots=16", *map(str, args)],
        capture_output=True, text=True, cwd=ROOT,
    )
    if result.returncode != 0:
        raise Failed(f"harness replay failed:\n{(result.stderr or result.stdout).strip()[-600:]}")
    if m := FAILED.search(result.stdout):
        raise Failed(f"harness replay: {m.group(1)} brain calls failed:\n{result.stdout.strip()[-600:]}")
    return result.stdout


def run(perf):
    build.common("default", tests=False)
    expect(SNAPSHOTS.is_dir(), "no replay input: record it with `tools/wlb test perf --capture`")
    expect(sum(1 for _ in SNAPSHOTS.iterdir()) >= 50, "fewer than 50 bots in the replay input")
    with LlamaServer():
        say(f"perf brain: warm-up ({WARMUP_S} s)")
        _replay("--mode=capacity", f"--seconds={WARMUP_S}", "--warmup=0", "--plan-s=30")

        say(f"perf brain: capacity ({CAPACITY_S} s, 16 in flight)")
        cap_out = _replay("--mode=capacity", f"--seconds={CAPACITY_S}", "--warmup=5", "--plan-s=30")
        say("  " + cap_out.strip())
        m = THROUGHPUT.search(cap_out)
        expect(m, "harness replay capacity printed nothing readable")
        perf.measure("capacity_wakes_per_s", float(m.group(2)), "/s", lower_is_better=False, factor=1.2)
        perf.measure("call_p50_s", float(m.group(3)), " s", factor=1.3, slack=0.2)
        perf.measure("capacity_valid", int(m.group(5)) / int(m.group(6)), "", lower_is_better=False,
                     factor=1, limit=1.0)

        for bots, plan_s, seconds in CELLS:
            say(f"perf brain: replay ({bots} bots, {plan_s} s plans, {seconds} s)")
            load_out = _replay("--mode=load", f"--bots={bots}", f"--plan-s={plan_s}",
                               f"--seconds={seconds}")
            for line in load_out.strip().splitlines():
                say("  " + line)
            m = LOAD.search(load_out)
            expect(m, f"harness replay load printed nothing readable ({bots} bots, {plan_s} s plans)")
            cell = f"{bots}bots_{plan_s}s"
            perf.measure(f"p50_s[{cell}]", float(m.group(1)), " s", factor=1.5, slack=0.3)
            perf.measure(f"p95_s[{cell}]", float(m.group(2)), " s", factor=1.5, slack=0.5)
            perf.measure(f"valid[{cell}]", int(m.group(4)) / int(m.group(5)), "",
                         lower_is_better=False, factor=1, limit=1.0)
