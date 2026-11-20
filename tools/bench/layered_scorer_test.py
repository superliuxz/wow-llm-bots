#!/usr/bin/env python3
"""Comprehensive unit tests for the layered scorer redesign (Steps 1-6)."""

from __future__ import annotations

import json
import math
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))

import adversary_suite  # noqa: E402
import known_bad_bots  # noqa: E402
import pairwise_ranker  # noqa: E402
import population_metrics  # noqa: E402
import scorer_calibration  # noqa: E402
import soak_scorer  # noqa: E402
import trace_parity  # noqa: E402


class LayeredScorerTest(unittest.TestCase):
    def test_step1_recording_parity_hides_packet_cadence_and_type(self):
        """Human 500 ms heartbeats vs bot 50 ms spline on the exact same straight path
        must reconstruct into identical 5 Hz canonical trajectories."""
        t0 = 1_795_000_000_000
        speed = 7.0
        heading = 0.6

        # Human: irregular 400-500 ms MSG_MOVE_HEARTBEAT packets.
        human_rows = []
        for dt_ms in range(0, 10_001, 500):
            dist = speed * (dt_ms / 1000.0)
            human_rows.append({
                "kind": "sample", "t_ms": t0 + dt_ms, "mono_us": dt_ms * 1000,
                "role": "human", "name": "Hele", "map": 1, "zone": "Elwynn",
                "x": round(dist * math.cos(heading), 3),
                "y": round(dist * math.sin(heading), 3),
                "z": 5.0, "o": heading, "moving": True, "jumping": False,
                "in_combat": False, "dead": False, "in_town": False,
            })

        # Bot: dense 50 ms MoveSpline ticks.
        bot_rows = []
        for dt_ms in range(0, 10_001, 50):
            dist = speed * (dt_ms / 1000.0)
            bot_rows.append({
                "kind": "sample", "t_ms": t0 + dt_ms, "mono_us": dt_ms * 1000,
                "role": "bot", "name": "Loada", "map": 1, "zone": "Durotar",
                "x": round(dist * math.cos(heading), 3),
                "y": round(dist * math.sin(heading), 3),
                "z": 5.0, "o": heading, "moving": True, "jumping": False,
                "in_combat": False, "dead": False, "in_town": False,
            })

        h_seg = trace_parity.reconstruct_trajectory(human_rows, step_ms=200)[0]
        b_seg = trace_parity.reconstruct_trajectory(bot_rows, step_ms=200)[0]
        self.assertEqual(len(h_seg["frames"]), len(b_seg["frames"]))
        for hf, bf in zip(h_seg["frames"], b_seg["frames"]):
            self.assertEqual(hf["t_rel_ms"], bf["t_rel_ms"])
            self.assertLess(math.hypot(hf["x"] - bf["x"], hf["y"] - bf["y"]), 0.05)
            self.assertLess(trace_parity.angle_diff(hf["o"], bf["o"]), 0.01)
            self.assertAlmostEqual(hf["speed_yd_s"], bf["speed_yd_s"], delta=0.1)

    def test_step1_situation_labeller_and_clip_anonymization(self):
        t0 = 1_795_000_000_000
        rows = []
        for k in range(300):  # 60 s at 5 Hz
            t = t0 + k * 200
            rows.append({
                "kind": "sample", "t_ms": t, "mono_us": k * 200_000,
                "role": "human", "name": "SecretName", "map": 1, "zone": "SecretZone",
                "level": 42, "x": 1000.0 + k * 1.4, "y": -500.0, "z": 10.0, "o": 1.2,
                "moving": True, "jumping": False, "in_combat": False, "dead": False,
                "in_town": False, "in_group": True, "in_dungeon": True,
            })
        clips = trace_parity.extract_clips(rows, clip_s=40.0)
        self.assertTrue(clips)
        c = clips[0]
        self.assertEqual(c["situation"], "in_group")
        # Origin shifted and metadata stripped.
        self.assertEqual(c["frames"][0]["x"], 0.0)
        self.assertEqual(c["frames"][0]["y"], 0.0)
        self.assertEqual(c["frames"][0]["o"], 0.0)
        dumped = json.dumps(c)
        self.assertNotIn("SecretName", dumped)
        self.assertNotIn("SecretZone", dumped)
        self.assertNotIn("human", dumped)

    def test_step2_bradley_terry_ci_and_keep_revert(self):
        comps = (
            [{"a": "human", "b": "candidate", "winner": "a"}] * 12
            + [{"a": "human", "b": "candidate", "winner": "b"}] * 4
            + [{"a": "candidate", "b": "current", "winner": "a"}] * 22
            + [{"a": "candidate", "b": "current", "winner": "b"}] * 3
            + [{"a": "human", "b": "current", "winner": "a"}] * 18
            + [{"a": "human", "b": "current", "winner": "b"}] * 2
        )
        bt = pairwise_ranker.fit_bradley_terry(comps, anchor_label="human", n_bootstrap=200, seed=7)
        self.assertEqual(bt["ratings"]["human"]["rating"], 1000.0)
        self.assertGreater(bt["ratings"]["candidate"]["rating"], bt["ratings"]["current"]["rating"])

        dec = pairwise_ranker.decide_keep_revert(bt, "candidate", "current", gates_ok=True, confirm_agrees=True)
        self.assertTrue(dec["keep"])
        self.assertTrue(dec["beats_outside_ci"])

        dec_fail_gate = pairwise_ranker.decide_keep_revert(bt, "candidate", "current", gates_ok=False, confirm_agrees=True)
        self.assertFalse(dec_fail_gate["keep"])
        self.assertEqual(dec_fail_gate["reason"], "gates_failed")

        agree = pairwise_ranker.check_judge_agreement(["a"] * 8 + ["b"] * 7, ["a"] * 7 + ["b"] * 8)
        self.assertTrue(agree["valid"])

    def test_step3_scorer_calibration_passes(self):
        res = scorer_calibration.run_calibration(pairs_per_matchup=10, seed=11)
        self.assertTrue(res["passed"], f"calibration failed: {res}")
        self.assertTrue(res["ranking_ok"])
        self.assertTrue(res["repeatable"])
        self.assertTrue(all(res["bad_variants_caught"].values()))

    def test_step4_adversary_suite_catches_known_bad_tells(self):
        humans = {
            "H1": known_bad_bots.generate_synthetic_human("H1", seed=1),
            "H2": known_bad_bots.generate_synthetic_human("H2", seed=2),
        }
        looper = {"L1": known_bad_bots.generate_same_loop_walker("L1")}
        adv = adversary_suite.run_adversary_suite(looper, humans, clip_s=35.0)
        self.assertGreaterEqual(adv["mmo_detectors"]["waypoint_repeat_ratio"]["bot_mean"], 0.45)
        self.assertTrue(adv["tells"])

    def test_step5_population_metrics_flags_clones(self):
        clones = {
            f"Clone{i}": known_bad_bots.generate_plain_playerbots(f"Clone{i}", seed=101)
            for i in range(4)
        }
        pop = population_metrics.analyze_population(clones, clip_s=35.0)
        self.assertIsNotNone(pop["inter_bot_similarity"])
        self.assertGreaterEqual(pop["inter_bot_similarity"], 0.99)
        self.assertTrue(pop["tells"])

    def test_step6_layer_a_catches_instant_whisper_and_refusal_loop(self):
        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            bdir = td / "bot" / "BadInstant"
            bdir.mkdir(parents=True)
            rows = known_bad_bots.generate_instant_reply("BadInstant")
            (bdir / "2026-11-19.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
            stats = {
                "bots": 1, "decisions": 5, "llm_errors": 0, "invalid_plans": 0,
                "max_wakes_per_min": 5.0, "do_p95_ms": 200.0, "max_repeated_refusals": 9,
            }
            res = soak_scorer.score_soak(td, run_stats=stats)
            self.assertFalse(res["guardrails_ok"])
            failed = {g["name"] for g in res["guardrails"] if not g["ok"]}
            self.assertIn("human_whisper_timing", failed)
            self.assertIn("no_refusal_loop", failed)


if __name__ == "__main__":
    unittest.main()
