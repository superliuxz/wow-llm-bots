"""What the stepped mmap walk (Explore / Go to) costs on the world thread: 50 bots walking at once
in Orgrimmar, measured from the module's own `walk paths` summary line (docs/plans/options-and-panels.md).
"""

import json
import re
import subprocess

from wlblib import build
from wlblib.common import OUT, ROOT, Failed
from wlblib.testing import expect

NAME = "walk"
DESCRIPTION = "stepped mmap PathGenerator cost on the world thread, 50 bots walking at once"
TEST_CONFIG = "pressure"
BOT = "Loada"
ONLINE = 50
WALK_S = 68
BOTS = 50

WALK_PATHS = re.compile(r"walk paths n=(\d+) \(path_us p50=(\d+) max=(\d+)\)")


def run(perf):
    if not (OUT / "common" / "libwlb_wire_py.so").exists():
        build.common("default", tests=False)
    result = subprocess.run(
        ["python3", "-I", ROOT / "tools/snapshot/fake_harness.py", "--walk", "--json",
         str(WALK_S)], capture_output=True, text=True)
    if result.returncode != 0:
        raise Failed(f"the walk harness failed:\n{result.stderr.strip()[-600:]}")
    r = json.loads(result.stdout)
    expect(r["bots_walked"] == BOTS,
           f"walked {r['bots_walked']} bots, expected {BOTS}")

    log = perf.stack.server_log()
    matches = list(WALK_PATHS.finditer(log))
    expect(matches, f"the module logged no `walk paths` summary in {WALK_S} s")
    last = matches[-1]
    n_paths = int(last.group(1))
    p50_us = int(last.group(2))
    max_us = int(last.group(3))
    expect(n_paths >= BOTS, f"only {n_paths} walk paths computed across {BOTS} bots")

    perf.measure("path_p50_us", p50_us, " us", slack=300, limit=1000)
    perf.measure("path_max_us", max_us, " us", factor=3, slack=1000, limit=5000)
