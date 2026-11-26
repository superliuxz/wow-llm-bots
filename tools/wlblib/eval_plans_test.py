#!/usr/bin/env python3
"""Unit tests for the eval case split in tools/wlblib/cases/eval_plans.py (4c-rest): plain runs
never see `held_out` cases; only the live check scores them, and reports them apart."""

import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from wlblib.cases import eval_plans  # noqa: E402
from wlblib.common import Failed  # noqa: E402

CASES = [
    {"name": "tuned_a", "reason": "plan done", "forbid_kinds": ["attack"]},
    {"name": "planned_b", "reason": "plan done", "planned": True},
    {"name": "held_c", "reason": "plan done", "held_out": True},
    {"name": "held_planned_d", "reason": "plan done", "held_out": True, "planned": True},
]


class LoadCasesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        path = pathlib.Path(self.tmp.name) / "cases.json"
        path.write_text(json.dumps(CASES))
        patcher = mock.patch.object(eval_plans, "CASES_FILE", path)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.tmp.cleanup)

    def names(self, *args, **kw):
        return [c["name"] for c in eval_plans.load_cases(*args, **kw)]

    def test_plain_run_skips_held_out_and_planned(self):
        self.assertEqual(self.names(), ["tuned_a"])
        self.assertEqual(self.names([eval_plans.NAME]), ["tuned_a"])

    def test_live_run_adds_held_out_but_not_planned(self):
        self.assertEqual(self.names(live=True), ["tuned_a", "held_c"])

    def test_held_out_by_name_needs_live(self):
        with self.assertRaises(Failed):
            eval_plans.load_cases(["held_c"])
        with self.assertRaises(Failed):
            eval_plans.load_cases(["tuned_a", "held_c"])
        with self.assertRaises(Failed):
            eval_plans.load_cases(["held_out"])
        self.assertEqual(self.names(["held_c"], live=True), ["held_c"])
        self.assertEqual(self.names(["held_out"], live=True), ["held_c"])

    def test_tuned_and_planned_by_name_unchanged(self):
        self.assertEqual(self.names(["tuned_a"]), ["tuned_a"])
        self.assertEqual(self.names(["planned"]), ["planned_b", "held_planned_d"])
        with self.assertRaises(Failed):
            eval_plans.load_cases(["nope"])


class RunLiveTest(unittest.TestCase):
    def test_held_out_scored_apart_from_the_gated_score(self):
        cases = [c for c in CASES if not c.get("planned")]
        # tuned_a passes every draw ("keep"); held_c is a plain case that fails no draw either,
        # so give it a check a keep-plan breaks.
        cases = [cases[0], {**cases[1], "forbid_kinds": ["keep doing what I'm doing"]}]
        keep = {"valid": True, "steps": [{"option": 1, "times": 1, "kind":
                                          "keep doing what I'm doing", "text": "Keep"}],
                "look_s": 60, "goal": "", "tell_to": "", "tell": "", "open": [], "hover": "",
                "orders": [], "plan_desc": "keep"}
        with mock.patch.object(eval_plans, "ensure_harness"), \
                mock.patch.object(eval_plans, "can_fail", return_value=True), \
                mock.patch.object(eval_plans, "decide_one", return_value=keep), \
                mock.patch.object(eval_plans, "say"):
            res = eval_plans.run_live(cases, samples=2)
        self.assertEqual((res["passed_draws"], res["draws"]), (2, 2))
        self.assertEqual(res["mean_rate"], 1.0)
        self.assertEqual(res["held_out"]["draws"], 2)
        self.assertEqual(res["held_out"]["passed_draws"], 0)
        self.assertEqual(res["held_out"]["unstable"], ["held_c"])

    def test_no_held_out_cases(self):
        with mock.patch.object(eval_plans, "ensure_harness"), \
                mock.patch.object(eval_plans, "can_fail", return_value=True), \
                mock.patch.object(eval_plans, "decide_one", return_value={"valid": False}), \
                mock.patch.object(eval_plans, "say"):
            res = eval_plans.run_live([CASES[0]], samples=1)
        self.assertIsNone(res["held_out"])
        self.assertEqual(res["passed_draws"], 0)


class RealCasesTest(unittest.TestCase):
    def test_repo_has_held_out_cases_outside_the_plain_run(self):
        plain = {c["name"] for c in eval_plans.load_cases()}
        live = {c["name"] for c in eval_plans.load_cases(live=True)}
        held = live - plain
        self.assertGreaterEqual(len(held), 8)
        self.assertTrue(all(n.startswith("held_") for n in held), held)
        for name in held:
            for suffix in (".snapshot.json", ".facts.json"):
                self.assertTrue((eval_plans.EVAL_DIR / f"{name}{suffix}").exists(), name)


if __name__ == "__main__":
    unittest.main()
