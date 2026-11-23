"""Live multi-bot soak test + human-likeness scorer on the isolated test server.

Supports both open-world questing and rotated 5-man Tank/Healer/3DPS dungeon
runs across varied classes, factions, levels, and zones, with real
`out/common/harness run` + real `llama-server` (Gemma 4) and unified behavior
tracing (`var/soak_traces/`).

Suites (`--suite`):
  - `varied` (default): 2 varied runs per evaluation —
      1. `soak_horde_rfc`: 5 Horde questing classes across 5 zones +
         5-man Horde Tank/Healer/3DPS in Ragefire Chasm (L15)
      2. `soak_alliance_dm`: 5 Alliance questing classes across 5 zones +
         5-man Alliance Tank/Healer/3DPS in Deadmines (L20)
  - `heldout`: generalization check (never used for hill-climb tuning) —
      1. `soak_northrend_hol`: 5 mid/high-level Horde questing bots +
         5-man Halls of Lightning (L80)
      2. `soak_heldout_uk`: 5 unseen Alliance questing classes +
         5-man Utgarde Keep (L70)
  - `quest`: legacy 10-bot level-1 Priest Valley of Trials soak (`soak`)
  - Or any single scenario name above.
"""

import json
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

from wlblib import build, server
from wlblib.common import OUT, ROOT, TEST, VAR, say
from wlblib.testing import LlamaServer, Stack, expect

sys.path.insert(0, str(ROOT / "tools" / "bench"))
import soak_scorer  # noqa: E402

NAME = "soak"
DESCRIPTION = "varied questing + 5-man Tank/Healer/3DPS dungeon soak + scorer"
TEST_CONFIG = "soak"
BOT = "Loada"
ONLINE = 10

DUNGEON_PARTY = ["Loadf", "Loadg", "Loadh", "Loadi", "Loadj"]

# Each scenario specifies its test config override plus optional per-bot gear/tele
# and 5-man dungeon party setup run via `llmtest` right after login.
SCENARIOS = {
    "soak": {
        "config": "soak",
        "desc": "10x L1 Priest questing (Valley of Trials)",
        "quest_setup": [],
        "dungeon": None,
    },
    "soak_horde_rfc": {
        "config": "soak_horde_rfc",
        "desc": "5 Horde questing (War/Dru/Mag/Pal/Hun) + 5-man RFC L15 (War/Pri/Rog/Mag/Sha)",
        "quest_setup": [
            # Loade: L15 Troll Hunter at The Sepulcher (Silverpine)
            ("Loade", 15, (0, 504.534, 1539.08, 129.502)),
        ],
        "dungeon": {
            "name": "Ragefire Chasm",
            "level": 15,
            "map": 389,
            "pos": (3.81, -14.82, -17.84),
        },
    },
    "soak_alliance_dm": {
        "config": "soak_alliance_dm",
        "desc": "5 Alliance questing (Pal/Hun/War/Loc/Sha) + 5-man Deadmines L20 (Pal/Dru/Rog/Mag/Hun)",
        "quest_setup": [
            # Loade: L15 Draenei Shaman at Sentinel Hill (Westfall)
            ("Loade", 15, (0, -10624.5, 1096.66, 33.7641)),
        ],
        "dungeon": {
            "name": "Deadmines",
            "level": 20,
            "map": 36,
            "pos": (-16.4, -383.07, 61.78),
        },
    },
    "soak_northrend_hol": {
        "config": "soak_northrend_hol",
        "desc": "5 mid/high Horde questing (L15-L70) + 5-man Halls of Lightning L80",
        "quest_setup": [
            ("Loada", 15, (0, 504.534, 1539.08, 129.502)),
            ("Loadb", 35, (0, -12388.9, 172.578, 2.83358)),
            ("Loadc", 35, (0, -12388.9, 172.578, 2.83358)),
            ("Loadd", 70, (571, 2741.29, 6097.16, 76.9055)),
            ("Loade", 70, (571, 2741.29, 6097.16, 76.9055)),
        ],
        "dungeon": {
            "name": "Halls of Lightning",
            "level": 80,
            "map": 602,
            "pos": (1331.47, 259.619, 53.398),
        },
    },
    "soak_heldout_uk": {
        "config": "soak_heldout_uk",
        "desc": "HELD-OUT: 5 unseen Alliance questing + 5-man Utgarde Keep L70",
        "quest_setup": [
            ("Loadd", 15, (0, -10624.5, 1096.66, 33.7641)),
        ],
        "dungeon": {
            "name": "Utgarde Keep",
            "level": 70,
            "map": 574,
            "pos": (153.789, -86.548, 12.551),
        },
    },
}

SUITES = {
    "varied": ["soak_horde_rfc", "soak_alliance_dm"],
    "heldout": ["soak_northrend_hol", "soak_heldout_uk"],
    "quest": ["soak"],
}


def _apply_scenario_setup(scenario: dict) -> None:
    """Runs `llmtest gear` / `group` / `tele` for mid/high quest bots and the 5-man party."""
    cmds = []
    for bot_name, level, tele in scenario.get("quest_setup", []):
        cmds.append(f"llmtest gear {bot_name} {level}")
        if tele:
            m, x, y, z = tele
            cmds.append(f"llmtest tele {bot_name} {m} {x} {y} {z} 0")

    dungeon = scenario.get("dungeon")
    if dungeon:
        leader = DUNGEON_PARTY[0]
        for member in DUNGEON_PARTY[1:]:
            cmds.append(f"llmtest group {leader} {member}")
        lvl = dungeon["level"]
        for member in DUNGEON_PARTY:
            cmds.append(f"llmtest gear {member} {lvl}")

    if not cmds:
        return

    for cmd in cmds:
        server.console(cmd, inst=TEST)

    if dungeon:
        # Let gear/spell setup settle briefly before teleporting the 5-man party
        # into the shared dungeon instance.
        time.sleep(2.0)
        m = dungeon["map"]
        x, y, z = dungeon["pos"]
        for member in DUNGEON_PARTY:
            server.console(f"llmtest tele {member} {m} {x} {y} {z} 0", inst=TEST)
        time.sleep(3.0)


def _run_once(
    scenario_name: str,
    minutes: float,
    human_dir: pathlib.Path | None,
    use_judge: bool,
    baseline_trace_dir: pathlib.Path | None = None,
    blind_ratings_path: pathlib.Path | None = None,
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
) -> dict:
    scenario = SCENARIOS[scenario_name]
    trace_dir = VAR / "soak_traces"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)

    say(f"soak scenario [{scenario_name}]: {scenario['desc']}")
    with Stack(scenario["config"], BOT, online=ONLINE) as stack:
        _apply_scenario_setup(scenario)
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
                say(
                    f"soak [{scenario_name}]: running {ONLINE} bots for {duration_s:.0f} s "
                    "with real llama-server..."
                )
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
            max_repeated_refusals = 0
            for bot_dir in sorted(bots_dir.iterdir()):
                ev_dir = bot_dir / "events"
                if not ev_dir.is_dir():
                    continue
                decs = []
                refusal_counts: dict[str, int] = {}
                for f in sorted(ev_dir.glob("*.jsonl")):
                    for line in f.read_text().splitlines():
                        if not line.strip():
                            continue
                        d = json.loads(line)
                        decs.append(d)
                        if d.get("error"):
                            llm_errors += 1
                        elif d.get("plan") is None:
                            invalid_plans += 1
                        if d.get("first_line_ms", 0) > 0:
                            do_ms.append(float(d["first_line_ms"]))
                        for w in d.get("wake_reasons", []):
                            if isinstance(w, str) and w.startswith("error:"):
                                refusal_counts[w] = refusal_counts.get(w, 0) + 1
                if refusal_counts:
                    max_repeated_refusals = max(
                        max_repeated_refusals, max(refusal_counts.values())
                    )
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
                "scenario": scenario_name,
                "bots": ONLINE,
                "elapsed_min": elapsed_min,
                "decisions": total_decs,
                "max_wakes_per_min": max_wakes_per_min,
                "do_p50_ms": p50_do,
                "do_p95_ms": p95_do,
                "llm_errors": llm_errors,
                "invalid_plans": invalid_plans,
                "max_repeated_refusals": max_repeated_refusals,
            }
            res = soak_scorer.score_soak(
                trace_dir,
                run_stats=run_stats,
                human_dir=human_dir,
                baseline_trace_dir=baseline_trace_dir,
                blind_ratings_path=blind_ratings_path,
                use_judge=use_judge,
                min_eval_passed=min_eval_passed,
                eval_passed=eval_passed,
            )
            res["scenario"] = scenario_name
            return res


def _resolve_scenarios(suite: str, runs: int) -> list[str]:
    if suite in SUITES:
        base = SUITES[suite]
        if runs <= 0:
            return list(base)
        # Repeat/cycle through the suite so `runs` matches requested run count,
        # or if `runs == 1` and suite has multiple scenarios, run the full suite
        # only when runs >= len(base).
        if suite in ("varied", "heldout") and runs == 1:
            return list(base)
        return [base[i % len(base)] for i in range(runs)]
    if suite in SCENARIOS:
        return [suite] * max(1, runs)
    raise ValueError(f"unknown soak suite/scenario: {suite}")


def _merge_trace_dirs(src_dirs: list[pathlib.Path], dest_dir: pathlib.Path) -> None:
    """Merges per-run `<src>/<role>/<name>/*.jsonl` into `dest_dir`.

    Bot character directories are namespaced per run (`<name>_r00`, `<name>_r01`)
    so distinct scenarios reusing `Loada..Loadj` across different maps/levels
    never concatenate into one trajectory (preserving per-run duration, stuck
    distance, XP/death deltas, and population stats).
    """
    shutil.rmtree(dest_dir, ignore_errors=True)
    dest_dir.mkdir(parents=True, exist_ok=True)
    for run_idx, src in enumerate(src_dirs):
        if not src.exists():
            continue
        for role in ("human", "bot"):
            rdir = src / role
            if not rdir.is_dir():
                continue
            for cdir in sorted(rdir.iterdir()):
                if not cdir.is_dir():
                    continue
                char_name = f"{cdir.name}_r{run_idx:02d}" if role == "bot" else cdir.name
                out_cdir = dest_dir / role / char_name
                out_cdir.mkdir(parents=True, exist_ok=True)
                for f in sorted(cdir.glob("*.jsonl")):
                    target = out_cdir / f"{f.stem}_run{run_idx:02d}{f.suffix}"
                    shutil.copy2(f, target)


def _aggregate_run_stats(results: list[dict]) -> dict:
    stats_list = [r.get("run_stats", {}) for r in results if r.get("run_stats")]
    if not stats_list:
        return {}
    p50s = [float(s["do_p50_ms"]) for s in stats_list if s.get("do_p50_ms", 0) > 0]
    p95s = [float(s["do_p95_ms"]) for s in stats_list if s.get("do_p95_ms", 0) > 0]
    return {
        "scenario": "+".join(str(s.get("scenario", "soak")) for s in stats_list),
        # Multi-run merged traces namespace bots per run (`Loada_r00`, `Loada_r01`),
        # so total traced bot-runs equals the sum across runs.
        "bots": sum(int(s.get("bots", 0)) for s in stats_list),
        "elapsed_min": sum(float(s.get("elapsed_min", 0.0)) for s in stats_list),
        "decisions": sum(int(s.get("decisions", 0)) for s in stats_list),
        "max_wakes_per_min": max(float(s.get("max_wakes_per_min", 0.0)) for s in stats_list),
        "do_p50_ms": statistics.median(p50s) if p50s else 0.0,
        "do_p95_ms": max(p95s) if p95s else 0.0,
        "llm_errors": sum(int(s.get("llm_errors", 0)) for s in stats_list),
        "invalid_plans": sum(int(s.get("invalid_plans", 0)) for s in stats_list),
        "max_repeated_refusals": max(
            int(s.get("max_repeated_refusals", 0)) for s in stats_list
        ),
    }


def _aggregate_results(results: list[dict]) -> dict:
    if len(results) == 1:
        return results[0]
    agg = dict(results[-1])
    ratings = [r["rating"] for r in results if r.get("rating") is not None]
    ci_lows = [r["ci_low"] for r in results if r.get("ci_low") is not None]
    ci_highs = [r["ci_high"] for r in results if r.get("ci_high") is not None]
    agg["rating"] = round(statistics.mean(ratings), 1) if ratings else None
    agg["ci_low"] = round(min(ci_lows), 1) if ci_lows else None
    agg["ci_high"] = round(max(ci_highs), 1) if ci_highs else None
    agg["guardrails_ok"] = all(r["guardrails_ok"] for r in results)
    agg["run_ratings"] = [r.get("rating") for r in results]
    agg["scenarios"] = [r.get("scenario", "soak") for r in results]
    agg["run_stats"] = _aggregate_run_stats(results)
    if "ranking" in agg:
        agg["ranking"] = dict(agg["ranking"])
        agg["ranking"]["beats_current_outside_ci"] = all(
            bool(r.get("ranking", {}).get("beats_current_outside_ci"))
            for r in results
        )

    # Aggregate progress across runs.
    agg["progress"] = {
        "total_xp": sum(r["progress"]["total_xp"] for r in results),
        "xp_per_hour": round(
            statistics.mean(r["progress"]["xp_per_hour"] for r in results), 1
        ),
        "quests_completed": sum(r["progress"].get("quests_completed", 0) for r in results),
        "quests_per_hour": round(
            statistics.mean(r["progress"].get("quests_per_hour", 0.0) for r in results), 2
        ),
        "deaths": sum(r["progress"]["deaths"] for r in results),
        "moved_bots": sum(r["progress"]["moved_bots"] for r in results),
        "stuck_bots": sum(r["progress"]["stuck_bots"] for r in results),
        "traced_bots": sum(r["progress"]["traced_bots"] for r in results),
    }
    agg["tells"] = sorted({t for r in results for t in r.get("tells", [])})
    return agg


def run_soak(
    minutes: float = 5.0,
    runs: int = 2,
    score: bool = False,
    human_dir: pathlib.Path | None = None,
    baseline_trace_dir: pathlib.Path | None = None,
    blind_ratings_path: pathlib.Path | None = None,
    use_judge: bool = False,
    verify_hash: pathlib.Path | None = None,
    suite: str = "varied",
    check_guardrails: bool = True,
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
) -> dict:
    if verify_hash:
        ok, msg = soak_scorer.verify_scorer_hash(verify_hash)
        expect(ok, f"scorer integrity check failed: {msg}")

    scenarios = _resolve_scenarios(suite, runs)
    build.common("default", tests=False)
    results = []
    with tempfile.TemporaryDirectory(prefix="wlb-soak-runs-") as runs_tmp:
        per_run_dirs: list[pathlib.Path] = []
        with LlamaServer(slots=16):
            for idx, sc_name in enumerate(scenarios):
                if len(scenarios) > 1:
                    say(f"== soak run {idx + 1}/{len(scenarios)} ({sc_name}) ==")
                res = _run_once(
                    scenario_name=sc_name,
                    minutes=minutes,
                    human_dir=human_dir,
                    use_judge=(use_judge if len(scenarios) == 1 else False),
                    baseline_trace_dir=baseline_trace_dir,
                    blind_ratings_path=blind_ratings_path,
                    min_eval_passed=(min_eval_passed if len(scenarios) == 1 else None),
                    eval_passed=(eval_passed if len(scenarios) == 1 else None),
                )
                results.append(res)
                if len(scenarios) > 1:
                    run_trace_dst = pathlib.Path(runs_tmp) / f"run_{idx:02d}_{sc_name}"
                    if (VAR / "soak_traces").exists():
                        shutil.copytree(VAR / "soak_traces", run_trace_dst)
                    per_run_dirs.append(run_trace_dst)
                st = res["run_stats"]
                p = res["progress"]
                rat_s = f"{res['rating']:.1f}" if res.get("rating") is not None else "unrated"
                say(
                    f"soak summary [{sc_name}] ({st['elapsed_min']:.1f} min, {ONLINE} bots): "
                    f"rating={rat_s}, decisions={st['decisions']}, "
                    f"max_wakes/min/bot={st['max_wakes_per_min']:.1f}, "
                    f"do_p50={st['do_p50_ms']:.0f}ms, do_p95={st['do_p95_ms']:.0f}ms, "
                    f"errors={st['llm_errors']}, invalid={st['invalid_plans']}, "
                    f"xp/h={p['xp_per_hour']:.0f}, deaths={p['deaths']}, "
                    f"moved={p['moved_bots']}/{p['traced_bots']}"
                )

            if len(scenarios) > 1:
                _merge_trace_dirs(per_run_dirs, VAR / "soak_traces")
                merged_stats = _aggregate_run_stats(results)
                agg = soak_scorer.score_soak(
                    VAR / "soak_traces",
                    run_stats=merged_stats,
                    human_dir=human_dir,
                    baseline_trace_dir=baseline_trace_dir,
                    blind_ratings_path=blind_ratings_path,
                    use_judge=use_judge,
                    min_eval_passed=min_eval_passed,
                    eval_passed=eval_passed,
                )
                agg["run_ratings"] = [r.get("rating") for r in results]
                agg["scenarios"] = [r.get("scenario", "soak") for r in results]
                agg["guardrails_ok"] = bool(agg["guardrails_ok"]) and all(
                    r["guardrails_ok"] for r in results
                )
            else:
                agg = results[0]
    say(soak_scorer.format_score_report(agg))

    if check_guardrails:
        expect(agg["guardrails_ok"], "one or more soak guardrails failed (see above)")
    return agg
