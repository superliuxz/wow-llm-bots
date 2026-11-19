#!/usr/bin/env python3
"""Unit tests for hill_climb.py & soak_scorer.py safeguards."""

import json
import os
import pathlib
import stat
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import hill_climb  # noqa: E402
import soak_scorer  # noqa: E402


class HillClimbSafeguardsTest(unittest.TestCase):
    def test_locked_files_include_all_measurement_and_config_files(self):
        rels = {p.relative_to(ROOT).as_posix() for p in hill_climb._locked_files()}
        for required in (
            "tools/bench/soak_scorer.py",
            "tools/bench/human_metrics.py",
            "tools/bench/hill_climb.py",
            "tools/wlblib/cases/soak.py",
            "tools/wlblib/testing.py",
            "server_module/src/snapshot/behavior_trace.h",
            "server_module/src/snapshot/behavior_trace.cc",
            "server_module/src/snapshot/snapshot_builder.cc",
            "server_module/src/snapshot/snapshot_schedule.h",
            "server_module/src/snapshot/snapshot_schedule.cc",
            "server_module/src/llm_brain_world_script.cc",
            "server_module/conf/mod-llm-brain.conf.dist",
            "server_module/src/config/llm_brain_config.cc",
            "config/llm-brain.overrides",
            "config/playerbots.overrides",
            "config/tests/soak.llm-brain.overrides",
            "config/tests/soak_horde_rfc.llm-brain.overrides",
            "config/tests/soak_alliance_dm.llm-brain.overrides",
            "config/tests/soak_northrend_hol.llm-brain.overrides",
            "config/tests/soak_heldout_uk.llm-brain.overrides",
        ):
            self.assertIn(required, rels)

    def test_lock_manifest_is_readonly_and_rejects_deleted_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            lf = pathlib.Path(tmp) / "scorer.sha256"
            ok, _ = hill_climb._write_lock_manifest(lf)
            self.assertTrue(ok)
            self.assertEqual(stat.S_IMODE(lf.stat().st_mode), 0o444)
            # Refuses to overwrite without force.
            ok2, _ = hill_climb._write_lock_manifest(lf, force=False)
            self.assertFalse(ok2)
            # Full manifest verifies.
            ok3, _ = hill_climb._verify_all_locks(lf)
            self.assertTrue(ok3)
            # Deleting a line from the manifest fails verification.
            os.chmod(lf, 0o644)
            lines = lf.read_text().splitlines()
            lf.write_text("\n".join(lines[:-1]) + "\n")
            ok4, msg = hill_climb._verify_all_locks(lf)
            self.assertFalse(ok4)
            self.assertIn("missing required files", msg)

    def test_has_human_traces_rejects_bot_rows_and_requires_min_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            self.assertFalse(hill_climb._has_human_traces(d))
            (d / "human" / "Alice").mkdir(parents=True)
            p = d / "human" / "Alice" / "2026-11-19.jsonl"
            # Bot-role traces copied into human/ must NOT count as human.
            bot_line = json.dumps({"kind": "sample", "role": "bot", "t_ms": 1000})
            p.write_text((bot_line + "\n") * soak_scorer.MIN_HUMAN_SAMPLES)
            self.assertFalse(hill_climb._has_human_traces(d))
            # 300 junk rows with no positions/movement metrics must NOT pass.
            junk_line = json.dumps({"kind": "sample", "role": "human", "t_ms": 1000})
            p.write_text((junk_line + "\n") * soak_scorer.MIN_HUMAN_SAMPLES)
            self.assertFalse(hill_climb._has_human_traces(d))
            # Reaching MIN_HUMAN_SAMPLES real movement rows passes.
            real_rows = [
                json.dumps({
                    "kind": "sample", "role": "human", "name": "Alice",
                    "t_ms": 1000 * i, "map": 1, "x": float(i * 4), "y": 0.0, "z": 0.0,
                    "facing": 0.0, "moving": (i % 5 != 0), "jumping": False,
                    "in_combat": False, "dead": False, "in_town": False,
                })
                for i in range(1, soak_scorer.MIN_HUMAN_SAMPLES + 1)
            ]
            p.write_text("\n".join(real_rows) + "\n")
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

    def test_fixed_guardrails_read_nested_progress_dict(self):
        # Matches real `soak_scorer.score_soak()` / `soak.run_soak()` result shape.
        real_shape = {
            "score": 75.0,
            "guardrails_ok": True,
            "progress": {"xp_per_hour": 5000.0, "deaths": 2, "total_xp": 8000},
        }
        ok, fails = hill_climb._check_fixed_guardrails(
            real_shape, min_xp_per_hour=3000.0, max_deaths=4
        )
        self.assertTrue(ok)
        self.assertEqual(fails, [])

        bad_shape = {
            "score": 75.0,
            "guardrails_ok": True,
            "progress": {"xp_per_hour": 2500.0, "deaths": 5, "total_xp": 4000},
        }
        ok, fails = hill_climb._check_fixed_guardrails(
            bad_shape, min_xp_per_hour=3000.0, max_deaths=4
        )
        self.assertFalse(ok)
        self.assertEqual(len(fails), 2)

    def test_scorer_missing_metrics_score_zero_and_no_silent_human_fallback(self):
        bot_summary = {"path_straightness": 0.82, "stops_per_min": 1.5}
        human_summary = {"characters": 1, "path_straightness": 0.82}
        score, per_metric, mode = soak_scorer._score_distributions(
            bot_summary, human_summary, human_sample_count=soak_scorer.MIN_HUMAN_SAMPLES
        )
        self.assertEqual(mode, "human_baseline")
        self.assertEqual(per_metric["path_straightness"]["score"], 100.0)
        # stops_per_min had no human data -> must score 0, not silently use fallback.
        self.assertEqual(per_metric["stops_per_min"]["score"], 0.0)
        self.assertEqual(per_metric["stops_per_min"]["reason"], "no_human_data")
        # whisper_reply_p50_s was missing -> must score 0, not be dropped.
        self.assertIn("whisper_reply_p50_s", per_metric)
        self.assertEqual(per_metric["whisper_reply_p50_s"]["score"], 0.0)
        self.assertLess(score, 50.0)


if __name__ == "__main__":
    unittest.main()
