"""Live multi-bot soak test on the isolated test server (Gate 1).

Runs 10 questing bots (`config/tests/soak.llm-brain.overrides`) with real
`out/common/harness run` + real `llama-server` (Gemma 4) and unified behavior
tracing enabled (`var/soak_traces/`). Checks:
  - Both worldserver and harness stay alive (no crashes)
  - No wake loops (bounded wakes/min/bot)
  - No latency blowups (`do:` p95 < 2000 ms, 0 LLM/parse errors)
  - No stuck bots (bots make decisions and move across the soak)
  - Prints the unified `human_metrics.py` behavior report at the end
"""

import json
import math
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

from wlblib import build
from wlblib.common import OUT, ROOT, VAR, say
from wlblib.testing import LlamaServer, Stack, expect

sys.path.insert(0, str(ROOT / "tools" / "bench"))
import human_metrics  # noqa: E402

NAME = "soak"
DESCRIPTION = "10-bot live questing soak with real harness + llama-server + behavior traces"
TEST_CONFIG = "soak"
BOT = "Loada"
ONLINE = 10


def run_soak(minutes: float = 2.0):
    build.common("default", tests=False)
    trace_dir = VAR / "soak_traces"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)

    with LlamaServer(slots=16):
        with Stack(TEST_CONFIG, BOT, online=ONLINE) as stack:
            with tempfile.TemporaryDirectory(prefix="wlb-soak-") as tmp:
                tmp_dir = pathlib.Path(tmp)
                bots_dir = tmp_dir / "bots"
                bots_dir.mkdir(parents=True)
                harness_log = tmp_dir / "harness.log"
                proc = subprocess.Popen(
                    [
                        str(OUT / "common" / "harness"),
                        "run",
                        f"--socket={stack.socket}",
                        f"--rules={ROOT / 'content' / 'RULES.md'}",
                        f"--bots-dir={bots_dir}",
                        "--host=127.0.0.1",
                        "--port=8090",
                        "--slots=16",
                    ],
                    cwd=ROOT,
                    stdout=harness_log.open("w"),
                    stderr=subprocess.STDOUT,
                )
                duration_s = max(30.0, minutes * 60.0)
                t0 = time.monotonic()
                try:
                    say(f"soak: running {ONLINE} bots for {duration_s:.0f} s with real llama-server...")
                    while time.monotonic() - t0 < duration_s:
                        if proc.poll() is not None:
                            tail = "\n".join(harness_log.read_text().splitlines()[-20:])
                            raise AssertionError(
                                f"harness exited early (rc={proc.returncode}):\n{tail}"
                            )
                        time.sleep(2.0)
                finally:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()

                elapsed_min = max((time.monotonic() - t0) / 60.0, 1e-6)
                # Parse decision logs written by `harness run`.
                decisions_by_bot = {}
                do_ms = []
                total_ms = []
                invalid_plans = 0
                llm_errors = 0
                for bot_dir in sorted(bots_dir.iterdir()):
                    ev_dir = bot_dir / "events"
                    if not ev_dir.is_dir():
                        continue
                    decs = []
                    for f in sorted(ev_dir.glob("*.jsonl")):
                        for line in f.read_text().splitlines():
                            if not line.strip():
                                continue
                            d = json.loads(line)
                            decs.append(d)
                            if d.get("error"):
                                llm_errors += 1
                            elif not d.get("valid", True):
                                invalid_plans += 1
                            if d.get("first_line_ms", 0) > 0:
                                do_ms.append(float(d["first_line_ms"]))
                            if d.get("round_trip_ms", 0) > 0:
                                total_ms.append(float(d["round_trip_ms"]))
                    decisions_by_bot[bot_dir.name] = decs

                total_decs = sum(len(v) for v in decisions_by_bot.values())
                max_wakes_per_min = max(
                    (len(v) / elapsed_min for v in decisions_by_bot.values()),
                    default=0.0,
                )
                p50_do = statistics.median(do_ms) if do_ms else 0.0
                p95_do = (
                    sorted(do_ms)[min(len(do_ms) - 1, int(0.95 * (len(do_ms) - 1)))]
                    if do_ms
                    else 0.0
                )

                # Analyze movement/stuckness from unified behavior traces.
                traces = human_metrics.load_traces(trace_dir)
                bot_traces = traces["bot"]
                moved_bots = 0
                for name, rows in bot_traces.items():
                    samples = [r for r in rows if r.get("kind") == "sample"]
                    if len(samples) >= 2:
                        x0, y0 = samples[0]["x"], samples[0]["y"]
                        max_d = max(
                            math.hypot(s["x"] - x0, s["y"] - y0) for s in samples
                        )
                        if max_d >= 3.0:
                            moved_bots += 1

                say(
                    f"soak summary ({elapsed_min:.1f} min, {ONLINE} bots): "
                    f"decisions={total_decs}, max_wakes/min/bot={max_wakes_per_min:.1f}, "
                    f"do_p50={p50_do:.0f}ms, do_p95={p95_do:.0f}ms, "
                    f"errors={llm_errors}, invalid={invalid_plans}, "
                    f"traced_bots={len(bot_traces)}, moved_bots={moved_bots}/{len(bot_traces)}"
                )
                say(human_metrics.format_report(trace_dir))

                expect(total_decs >= ONLINE, f"too few decisions ({total_decs}) for {ONLINE} bots")
                expect(llm_errors == 0, f"LLM errors during soak: {llm_errors}")
                expect(invalid_plans == 0, f"invalid plans during soak: {invalid_plans}")
                expect(
                    max_wakes_per_min <= 20.0,
                    f"wake loop detected: {max_wakes_per_min:.1f} wakes/min/bot",
                )
                expect(
                    p95_do <= 2000.0,
                    f"latency blowup: do p95 = {p95_do:.0f} ms (> 2000 ms)",
                )
                expect(
                    len(bot_traces) >= ONLINE,
                    f"expected {ONLINE} traced bots in {trace_dir}, got {len(bot_traces)}",
                )
                expect(
                    moved_bots >= max(1, ONLINE // 2),
                    f"too many stuck bots: only {moved_bots}/{len(bot_traces)} moved >= 3 yd",
                )
