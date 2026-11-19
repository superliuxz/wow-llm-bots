"""Live multi-bot soak test + human-likeness scorer on the isolated test server.

Runs 10 questing bots (`config/tests/soak.llm-brain.overrides`) with real
`out/common/harness run` + real `llama-server` (Gemma 4) and unified behavior
tracing enabled (`var/soak_traces/`). Supports multi-run averaging (`--runs N`)
and composite human-likeness + guardrail scoring (`--score`,
`tools/bench/soak_scorer.py`).
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
import soak_scorer  # noqa: E402

NAME = "soak"
DESCRIPTION = "10-bot live questing soak with real harness + llama-server + scorer"
TEST_CONFIG = "soak"
BOT = "Loada"
ONLINE = 10


def _run_once(
    minutes: float,
    human_dir: pathlib.Path | None,
    use_judge: bool,
) -> dict:
    trace_dir = VAR / "soak_traces"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)

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
                    "--temperature=0",
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
            decisions_by_bot = {}
            do_ms = []
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
            run_stats = {
                "bots": ONLINE,
                "elapsed_min": elapsed_min,
                "decisions": total_decs,
                "max_wakes_per_min": max_wakes_per_min,
                "do_p50_ms": p50_do,
                "do_p95_ms": p95_do,
                "llm_errors": llm_errors,
                "invalid_plans": invalid_plans,
            }
            return soak_scorer.score_soak(
                trace_dir,
                run_stats=run_stats,
                human_dir=human_dir,
                use_judge=use_judge,
            )


def run_soak(
    minutes: float = 2.0,
    runs: int = 1,
    score: bool = False,
    human_dir: pathlib.Path | None = None,
    use_judge: bool = False,
    verify_hash: pathlib.Path | None = None,
) -> dict:
    if verify_hash:
        ok, msg = soak_scorer.verify_scorer_hash(verify_hash)
        expect(ok, f"scorer integrity check failed: {msg}")

    build.common("default", tests=False)
    results = []
    with LlamaServer(slots=16):
        for r in range(max(1, runs)):
            if runs > 1:
                say(f"== soak run {r + 1}/{runs} ==")
            res = _run_once(minutes=minutes, human_dir=human_dir, use_judge=use_judge)
            results.append(res)
            st = res["run_stats"]
            p = res["progress"]
            say(
                f"soak summary ({st['elapsed_min']:.1f} min, {ONLINE} bots): "
                f"decisions={st['decisions']}, max_wakes/min/bot={st['max_wakes_per_min']:.1f}, "
                f"do_p50={st['do_p50_ms']:.0f}ms, do_p95={st['do_p95_ms']:.0f}ms, "
                f"errors={st['llm_errors']}, invalid={st['invalid_plans']}, "
                f"xp/h={p['xp_per_hour']:.0f}, moved={p['moved_bots']}/{p['traced_bots']}"
            )

    # Aggregate across runs (average score; guardrails must hold on every run).
    if len(results) == 1:
        agg = results[0]
    else:
        agg = dict(results[-1])
        agg["score"] = round(statistics.mean(r["score"] for r in results), 2)
        agg["dist_score"] = round(statistics.mean(r["dist_score"] for r in results), 2)
        agg["guardrails_ok"] = all(r["guardrails_ok"] for r in results)
        agg["run_scores"] = [r["score"] for r in results]

    if score:
        say(soak_scorer.format_score_report(agg))
    else:
        say(human_metrics.format_report(VAR / "soak_traces"))

    expect(agg["guardrails_ok"], "one or more soak guardrails failed (see above)")
    return agg
