"""What snapshots cost: 50 bots standing in Orgrimmar's Valley of Strength (hundreds of units in
view), one harness connected for a few minutes.

Two parts, from one run: how the snapshots arrive (age when received, how evenly, bunching:
tools/snapshot/fake_harness.py --stats) and what the module spent building and sending them (its
own once-a-minute lines in the server log). The limits are the criteria set before the first
pressure test (docs/results.md): under 5 ms added per world update on average, no single
build over 2 ms, no dropped frames.
"""

import json
import re
import subprocess

from wlblib import build
from wlblib.common import OUT, ROOT, Failed
from wlblib.testing import expect

NAME = "snapshots"
DESCRIPTION = "snapshot build cost and delivery, 50 bots in a crowd, one harness"
TEST_CONFIG = "pressure"  # config/tests/pressure.*.overrides: 50 bots standing in Orgrimmar.
BOT = "Loada"
ONLINE = 50
# Long enough for one full minute of the module's stats line with the harness connected.
RECEIVE_S = 135
BOTS = 50

BUILD = re.compile(r"snapshots in the last minute: n=(\d+) build p50=(\d+)us p99=(\d+)us "
                   r"max=(\d+)us total=(\d+)ms bytes avg=(\d+) max=(\d+)")
TICK = re.compile(r"one tick built at most (\d+) snapshots, in at most (\d+) us")
SENT = re.compile(r"sent n=(\d+) \(built to socket p50=(\d+)us p99=(\d+)us max=(\d+)us\), "
                  r"(\d+) replaced by a newer snapshot, (\d+) dropped")


def last_full_minute(log):
    """The module's last stats lines that cover a whole minute with every bot (n about 60 x
    bots): (build, tick, sent) match groups."""
    full = [m for m in BUILD.finditer(log) if int(m.group(1)) >= BOTS * 45]
    expect(full, f"the module logged no full minute of snapshots (n >= {BOTS * 45}) in "
                 f"{RECEIVE_S} s: {[m.group(1) for m in BUILD.finditer(log)]}")
    build = full[-1]
    tick = TICK.search(log, build.end())
    sent = SENT.search(log, build.end())
    expect(tick and sent, "the build line has no tick and sent lines after it")
    return build, tick, sent


def run(perf):
    if not (OUT / "common" / "libwlb_wire_py.so").exists():
        build.common("default", tests=False)
    result = subprocess.run(
        ["python3", "-I", ROOT / "tools/snapshot/fake_harness.py", "--stats", "--json",
         f"--socket={perf.stack.socket}",
         str(RECEIVE_S)], capture_output=True, text=True)
    if result.returncode != 0:
        raise Failed(f"the harness stand-in failed:\n{result.stderr.strip()[-600:]}")
    r = json.loads(result.stdout)
    expect("steady" in r, f"no steady state: {r}")
    st = r["steady"]
    expect(r["bots"] == BOTS, f"snapshots from {r['bots']} bots, expected {BOTS}")

    # Arrival: the age is how old a snapshot is when the harness has it (built to received).
    perf.measure("age_p50_ms", st["age_p50_ms"], " ms", slack=0.5)
    perf.measure("age_p95_ms", st["age_p95_ms"], " ms", slack=1.0)
    perf.measure("age_max_ms", st["age_max_ms"], " ms", factor=3, slack=5.0)
    # How evenly: the bots are spread over the interval, so a tick never sends them all.
    perf.measure("bunching_10ms", st["bunching"], " frames", factor=2, slack=1, limit=6)
    perf.measure("frames_per_s", r["per_s"], "/s", lower_is_better=False, factor=1.05,
                 limit=BOTS * 0.95)

    # The module's side: what building and sending cost, from its own minute.
    build, tick, sent = last_full_minute(perf.stack.server_log())
    perf.measure("build_p50_us", int(build.group(2)), " us", slack=50)
    perf.measure("build_p99_us", int(build.group(3)), " us", slack=150)
    perf.measure("build_max_us", int(build.group(4)), " us", factor=3, slack=500, limit=2000)
    perf.measure("build_total_ms_per_min", int(build.group(5)), " ms", slack=100)
    perf.measure("snapshot_bytes_avg", int(build.group(6)), " B", factor=1.3)
    perf.measure("tick_max_snapshots", int(tick.group(1)), "", factor=2, slack=1, limit=4)
    perf.measure("tick_max_us", int(tick.group(2)), " us", factor=3, slack=500, limit=2000)
    perf.measure("built_to_socket_p99_us", int(sent.group(3)), " us", slack=500)
    perf.measure("dropped", int(sent.group(6)), "", factor=1, limit=0)
