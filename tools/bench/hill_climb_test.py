#!/usr/bin/env python3
"""Unit tests for hill_climb.py safeguards (locks, human-only, scope, floors)."""

import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import hill_climb  # noqa: E402


class HillClimbSafeguardsTest(unittest.TestCase):
    def test_locked_files_include_trace_and_call_sites(self):
        rels = {p.relative_to(ROOT).as_posix() for p in hill_climb._locked_files()}
        for required in (
            "tools/bench/soak_scorer.py",
            "tools/bench/human_metrics.py",
            "tools/wlblib/cases/soak.py",
            "server_module/src/snapshot/behavior_trace.h",
            "server_module/src/snapshot/behavior_trace.cc",
            "server_module/src/llm_brain_world_script.cc",
            "config/tests/soak_horde_rfc.llm-brain.overrides",
            "config/tests/soak_alliance_dm.llm-brain.overrides",
            "config/tests/soak_northrend_hol.llm-brain.overrides",
            "config/tests/soak_heldout_uk.llm-brain.overrides",
        ):
            self.assertIn(required, rels)

    def test_has_human_traces_requires_jsonl(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            self.assertFalse(hill_climb._has_human_traces(d))
            (d / "human" / "Alice").mkdir(parents=True)
            self.assertFalse(hill_climb._has_human_traces(d))
            (d / "human" / "Alice" / "2026-11-19.jsonl").write_text("{}\n")
            self.assertTrue(hill_climb._has_human_traces(d))

    def test_edit_scope_allows_humanization_and_rejects_game_logic(self):
        ok, bad = hill_climb._check_edit_scope([
            "server_module/src/options/human_pacing.cc",
            "content/RULES.md",
            "content/identities/default.md",
        ])
        self.assertTrue(ok)
        self.assertEqual(bad, [])

        ok, bad = hill_climb._check_edit_scope([
            "content/RULES.md",
            "server_module/src/options/option_executor.cc",
            "server_module/src/llm_brain_world_script.cc",
        ])
        self.assertFalse(ok)
        self.assertEqual(
            bad,
            [
                "server_module/src/options/option_executor.cc",
                "server_module/src/llm_brain_world_script.cc",
            ],
        )

    def test_fixed_guardrails_enforce_xp_floor_and_death_cap(self):
        ok, fails = hill_climb._check_fixed_guardrails(
            {"guardrails_ok": True, "xp_per_hour": 5000, "deaths": 2},
            min_xp_per_hour=3000.0,
            max_deaths=4,
        )
        self.assertTrue(ok)
        self.assertEqual(fails, [])

        ok, fails = hill_climb._check_fixed_guardrails(
            {"guardrails_ok": True, "xp_per_hour": 2500, "deaths": 5},
            min_xp_per_hour=3000.0,
            max_deaths=4,
        )
        self.assertFalse(ok)
        self.assertEqual(len(fails), 2)


if __name__ == "__main__":
    unittest.main()
