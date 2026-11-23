#!/usr/bin/env python3
"""Unit tests for varied and held-out soak suite definitions."""

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "bench"))
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

    def test_aggregate_results_handles_real_score_soak_with_none_metrics(self):
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
            self.assertIsNone(r1["rating"])
            agg = soak_case._aggregate_results([r1, r2])
            self.assertIsNone(agg["rating"])
            self.assertTrue(agg["guardrails_ok"])

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
                {"run_stats": {"scenario": "a", "bots": 1, "elapsed_min": 1.0, "decisions": 5, "do_p50_ms": 50.0, "do_p95_ms": 100.0}},
                {"run_stats": {"scenario": "b", "bots": 1, "elapsed_min": 1.0, "decisions": 7, "do_p50_ms": 70.0, "do_p95_ms": 120.0}},
            ])
            self.assertEqual(stats["bots"], 2)
            self.assertEqual(stats["decisions"], 12)
            res = soak_scorer.score_soak(merged, run_stats=stats)
            self.assertEqual(res["progress"]["traced_bots"], 2)
            self.assertEqual(res["progress"]["stuck_bots"], 2)
            self.assertEqual(res["progress"]["moved_bots"], 0)
            self.assertFalse(res["guardrails_ok"])


if __name__ == "__main__":
    unittest.main()
