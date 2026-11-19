#!/usr/bin/env python3
"""Unit tests for varied and held-out soak suite definitions."""

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from wlblib.cases import soak as soak_case  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
