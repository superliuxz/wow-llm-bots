#!/usr/bin/env python3
"""Unit tests for varied and held-out soak suite definitions."""

import inspect
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "bench"))
from wlblib import testing as wlb_testing  # noqa: E402
from wlblib.cases import soak as soak_case  # noqa: E402
import soak_scorer  # noqa: E402


class SoakSuitesTest(unittest.TestCase):
    def test_varied_and_heldout_suites_are_disjoint(self):
        varied = set(soak_case.SUITES["varied"])
        heldout = set(soak_case.SUITES["heldout"])
        self.assertTrue(varied)
        self.assertTrue(heldout)
        self.assertEqual(varied & heldout, set())

    def test_all_suite_scenarios_have_configs(self):
        for suite, scenarios in soak_case.SUITES.items():
            for sc in scenarios:
                self.assertIn(sc, soak_case.SCENARIOS, f"missing {sc} in {suite}")
                cfg = ROOT / "config" / "tests" / f"{sc}.llm-brain.overrides"
                if sc != "soak":
                    self.assertTrue(cfg.exists(), f"missing override config: {cfg}")

    def test_score_soak_handles_none_metrics_and_aggregate_run_stats(self):
        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            bot_dir = td / "bot" / "Loada"
            bot_dir.mkdir(parents=True)
            rows = [
                json.dumps({
                    "kind": "sample", "role": "bot", "name": "Loada",
                    "t_ms": 1000 * i, "map": 1, "x": float(i * 3), "y": 0.0, "z": 0.0,
                    "facing": 0.0, "moving": True, "jumping": False, "in_combat": False,
                    "dead": False, "in_town": False, "level": 1, "xp": i * 10,
                    "hp_pct": 100.0, "mana_pct": 100.0, "casting": "", "target": "",
                    "attackers": 0,
                })
                for i in range(1, 20)
            ]
            (bot_dir / "trace.jsonl").write_text("\n".join(rows) + "\n")
            stats = {"bots": 1, "decisions": 5, "elapsed_min": 1.0, "llm_errors": 0,
                     "invalid_plans": 0, "max_wakes_per_min": 5.0, "do_p95_ms": 100.0}
            r1 = soak_scorer.score_soak(td, run_stats=stats)
            r2 = soak_scorer.score_soak(td, run_stats=stats)
            self.assertTrue(r1["guardrails_ok"])
            self.assertEqual(r1["progress"]["stalled_bots"], 0)
            merged_stats = soak_case._aggregate_run_stats([r1, r2])
            self.assertEqual(merged_stats["bots"], 2)
            self.assertEqual(merged_stats["decisions"], 10)

    def test_not_stuck_fails_when_bot_moves_early_then_stalls(self):
        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            # Two bots: Loada moves 50 yd then stands still for 400 s; Loadb does the same.
            for name in ("Loada", "Loadb"):
                bdir = td / "bot" / name
                bdir.mkdir(parents=True)
                rows = [
                    json.dumps({
                        "kind": "sample", "role": "bot", "name": name,
                        "t_ms": 0, "map": 1, "x": 0.0, "y": 0.0, "z": 0.0,
                        "in_combat": False, "dead": False, "level": 1, "xp": 0,
                    }),
                    json.dumps({
                        "kind": "sample", "role": "bot", "name": name,
                        "t_ms": 5000, "map": 1, "x": 50.0, "y": 0.0, "z": 0.0,
                        "in_combat": False, "dead": False, "level": 1, "xp": 0,
                    }),
                ]
                # Stationary at (50, 0) every 10 s for 400 s.
                for step in range(1, 42):
                    rows.append(json.dumps({
                        "kind": "sample", "role": "bot", "name": name,
                        "t_ms": 5000 + step * 10_000, "map": 1, "x": 50.0, "y": 0.0, "z": 0.0,
                        "in_combat": False, "dead": False, "level": 1, "xp": 0,
                    }))
                (bdir / "trace.jsonl").write_text("\n".join(rows) + "\n")
            stats = {"bots": 2, "decisions": 4, "elapsed_min": 7.0, "llm_errors": 0,
                     "invalid_plans": 0, "max_wakes_per_min": 1.0, "do_p95_ms": 100.0}
            res = soak_scorer.score_soak(td, run_stats=stats)
            self.assertEqual(res["progress"]["moved_bots"], 2)
            self.assertEqual(res["progress"]["stalled_bots"], 2)
            self.assertEqual(res["progress"]["stuck_bots"], 2)
            self.assertGreaterEqual(res["progress"]["max_stall_s"], 400.0)
            self.assertFalse(res["guardrails_ok"])
            failed = {g["name"] for g in res["guardrails"] if not g["ok"]}
            self.assertIn("not_stuck", failed)

    def test_xp_per_hour_counts_gains_across_level_ups(self):
        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            # Bot A: sample-only level-up rollover (level 1 xp=350 -> level 2 xp=120).
            b1 = td / "bot" / "Loada"
            b1.mkdir(parents=True)
            (b1 / "trace.jsonl").write_text("\n".join([
                json.dumps({"kind": "sample", "role": "bot", "name": "Loada",
                            "t_ms": 0, "x": 0.0, "y": 0.0, "z": 0.0, "level": 1, "xp": 350}),
                json.dumps({"kind": "sample", "role": "bot", "name": "Loada",
                            "t_ms": 1800_000, "x": 20.0, "y": 0.0, "z": 0.0, "level": 2, "xp": 120}),
            ]) + "\n")
            # Bot B: explicit xp_gain events across level-up.
            b2 = td / "bot" / "Loadb"
            b2.mkdir(parents=True)
            (b2 / "trace.jsonl").write_text("\n".join([
                json.dumps({"kind": "sample", "role": "bot", "name": "Loadb",
                            "t_ms": 0, "x": 0.0, "y": 0.0, "z": 0.0, "level": 1, "xp": 350}),
                json.dumps({"kind": "event", "role": "bot", "name": "Loadb",
                            "t_ms": 900_000, "event": "xp_gain", "detail": "Boar", "value": 250}),
                json.dumps({"kind": "sample", "role": "bot", "name": "Loadb",
                            "t_ms": 1800_000, "x": 20.0, "y": 0.0, "z": 0.0, "level": 2, "xp": 200}),
            ]) + "\n")
            stats = {"bots": 2, "decisions": 4, "elapsed_min": 30.0, "llm_errors": 0,
                     "invalid_plans": 0, "max_wakes_per_min": 1.0, "do_p95_ms": 100.0}
            res = soak_scorer.score_soak(td, run_stats=stats)
            # Loada: 120 (rollover), Loadb: 250 (exact xp_gain event) => 370 total across 1.0 bot-hour.
            self.assertEqual(res["progress"]["total_xp"], 370)
            self.assertEqual(res["progress"]["xp_per_hour"], 370.0)

    def test_empty_soak_fails_guardrails(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = soak_scorer.score_soak(pathlib.Path(tmp), run_stats={})
            self.assertFalse(res["guardrails_ok"])
            failed = {g["name"] for g in res["guardrails"] if not g["ok"]}
            self.assertIn("has_traced_bots", failed)
            self.assertIn("has_decisions", failed)

    def test_merge_trace_dirs_namespaces_bots_per_run_preserving_stuck_detection(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp)
            # Run 0: Loada is stuck at (0, 0) on map 1.
            # Run 1: Loada is stuck at (500, 500) on map 0.
            # Without per-run namespacing, merging would fake 707 yd movement.
            for run_idx, (m_id, xy, t0) in enumerate(((1, 0.0, 1000), (0, 500.0, 60000))):
                bdir = t / f"run{run_idx}" / "bot" / "Loada"
                bdir.mkdir(parents=True)
                rows = [
                    json.dumps({
                        "kind": "sample", "role": "bot", "name": "Loada",
                        "t_ms": t0 + 1000 * i, "map": m_id, "x": xy, "y": xy, "z": 0.0,
                        "facing": 0.0, "moving": False, "jumping": False, "in_combat": False,
                        "dead": False, "in_town": False, "level": 1, "xp": 0,
                        "hp_pct": 100.0, "mana_pct": 100.0, "casting": "", "target": "",
                        "attackers": 0,
                    })
                    for i in range(5)
                ]
                (bdir / "2026-11-23.jsonl").write_text("\n".join(rows) + "\n")
            merged = t / "merged"
            soak_case._merge_trace_dirs([t / "run0", t / "run1"], merged)
            stats = soak_case._aggregate_run_stats([
                {"run_stats": {"scenario": "a", "mode": "llm", "temperature": 0.7, "decisions_dir": "var/soak_decisions/a", "bots": 1, "elapsed_min": 1.0, "decisions": 5, "option_counts": {"Attack": 3, "Keep": 2}, "do_p50_ms": 50.0, "do_p95_ms": 100.0}},
                {"run_stats": {"scenario": "b", "mode": "llm", "temperature": 0.7, "decisions_dir": "var/soak_decisions/b", "bots": 1, "elapsed_min": 1.0, "decisions": 7, "option_counts": {"Attack": 4, "Loot": 3}, "do_p50_ms": 70.0, "do_p95_ms": 120.0}},
            ])
            self.assertEqual(stats["bots"], 2)
            self.assertEqual(stats["decisions"], 12)
            self.assertEqual(stats["option_counts"], {"Attack": 7, "Keep": 2, "Loot": 3})
            self.assertEqual(stats["mode"], "llm")
            self.assertEqual(stats["temperature"], 0.7)
            self.assertEqual(stats["decisions_dirs"], ["var/soak_decisions/a", "var/soak_decisions/b"])
            res = soak_scorer.score_soak(merged, run_stats=stats)
            self.assertEqual(res["progress"]["traced_bots"], 2)
            self.assertEqual(res["progress"]["stuck_bots"], 2)
            self.assertEqual(res["progress"]["moved_bots"], 0)
            self.assertFalse(res["guardrails_ok"])

    def test_runs_zero_defaults_and_profession_tool_command(self):
        # testing.soak() defaults runs=0 (run full suite once).
        self.assertEqual(inspect.signature(wlb_testing.soak).parameters["runs"].default, 0)
        self.assertEqual(
            soak_case._resolve_scenarios("endurance", 0),
            list(soak_case.SUITES["endurance"]),
        )
        self.assertEqual(
            soak_case._resolve_scenarios("varied", 0),
            list(soak_case.SUITES["varied"]),
        )
        self.assertEqual(
            soak_case._resolve_scenarios("soak_classic_ulda", 0),
            ["soak_classic_ulda"],
        )
        # Profession tool seeding must emit `llmtest item` (not `llmtest additem`).
        self.assertEqual(
            soak_case._extract_first_option_kind(
                {
                    "plan": "do: 2x3, 4\nexpect: 2\nwatch: none\nlook: 60",
                    "prompt_user": "also:\n  1. Keep doing what I'm doing\n  2. Attack Mottled Boar\n",
                }
            ),
            "Attack",
        )
        self.assertEqual(
            soak_case._extract_first_option_kind(
                {
                    "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60",
                    "prompt_user": "also:\n  1. Accept quest \"Your Place\" from Kaltunk\n",
                }
            ),
            "Talk",
        )
        self.assertGreater(
            inspect.signature(soak_case.run_soak).parameters["temperature"].default,
            0.0,
        )
        self.assertFalse(
            inspect.signature(soak_case.run_soak).parameters["no_brain"].default
        )
        mining_cmds = soak_case._profession_setup_cmds("Loada", "mining", 150)
        self.assertIn("llmtest item Loada 2901", mining_cmds)
        self.assertFalse(any("additem" in c for c in mining_cmds))


if __name__ == "__main__":
    unittest.main()
