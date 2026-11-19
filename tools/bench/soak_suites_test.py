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
            self.assertIsNone(r1["per_metric"]["whisper_reply_p50_s"]["bot"])
            agg = soak_case._aggregate_results([r1, r2])
            self.assertIsNone(agg["per_metric"]["whisper_reply_p50_s"]["bot"])
            self.assertTrue(agg["guardrails_ok"])

    def test_empty_soak_fails_guardrails(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = soak_scorer.score_soak(pathlib.Path(tmp), run_stats={})
            self.assertFalse(res["guardrails_ok"])
            failed = {g["name"] for g in res["guardrails"] if not g["ok"]}
            self.assertIn("has_traced_bots", failed)
            self.assertIn("has_decisions", failed)


if __name__ == "__main__":
    unittest.main()
