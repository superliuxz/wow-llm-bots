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

        # Also test server-side 5 Hz polling of stair-stepped ~500 ms human
        # client movement packets vs smooth 5 Hz bot ticks: change-point
        # reconstruction must eliminate the human stair-step speed spikes.
        stepped_human = []
        last_pkt_dist = 0.0
        for dt_ms in range(0, 10_001, 200):
            if dt_ms % 600 == 0:
                last_pkt_dist = speed * (dt_ms / 1000.0)
            stepped_human.append({
                "kind": "sample", "t_ms": t0 + dt_ms, "mono_us": dt_ms * 1000,
                "role": "human", "name": "Hele", "map": 1, "zone": "Elwynn",
                "x": round(last_pkt_dist * math.cos(heading), 3),
                "y": round(last_pkt_dist * math.sin(heading), 3),
                "z": 5.0, "o": heading, "moving": True, "jumping": False,
                "in_combat": False, "dead": False, "in_town": False,
            })
        sh_seg = trace_parity.reconstruct_trajectory(stepped_human, step_ms=200)[0]
        # Interior frames must glide smoothly at ~7 yd/s, never 0 / 21 yd/s steps.
        interior_speeds = [f["speed_yd_s"] for f in sh_seg["frames"][3:-3]]
        self.assertTrue(all(6.0 <= s <= 8.0 for s in interior_speeds), f"stair-step leaked: {interior_speeds}")

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
        # Without allow_synthetic, refuses missing real traces / plain_playerbots / judge.
        refused = scorer_calibration.run_calibration()
        self.assertFalse(refused["passed"])
        self.assertIn("plain_playerbots", refused.get("error", ""))

        # Real-route same_loop perturbation repeats a real route with high waypoint repeat.
        real_bot = {"Curr": known_bad_bots.generate_synthetic_current_bot("Curr", duration_s=120)}
        looped = known_bad_bots.perturb_same_loop(real_bot, loop_s=30.0, laps=4)
        self.assertIn("Curr_SameLoop", looped)
        wp = adversary_suite.waypoint_repetition_score(looped["Curr_SameLoop"])
        self.assertGreaterEqual(wp["repeat_ratio"], 0.5)

        # With allow_synthetic=True (unit-test mode), full meta-checks pass.
        res = scorer_calibration.run_calibration(
            pairs_per_matchup=10, seed=11, allow_synthetic=True
        )
        self.assertTrue(res["passed"], f"calibration failed: {res}")
        self.assertTrue(res["ranking_ok"])
        self.assertTrue(res["repeatable"])
        self.assertTrue(all(res["bad_variants_caught"].values()))

    def test_review_fixes_candidate_id_judge_gate_parity_and_self_blind(self):
        import blind_rate

        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            cand_dir = td / "cand"
            (cand_dir / "bot" / "Loada").mkdir(parents=True)
            rows = known_bad_bots.generate_synthetic_current_bot("Loada", duration_s=90)
            (cand_dir / "bot" / "Loada" / "2026-11-20.jsonl").write_text(
                "\n".join(json.dumps(r) for r in rows) + "\n"
            )
            cand_id = trace_parity.trace_set_hash(cand_dir, role="bot")
            self.assertTrue(cand_id)

            # Finding 1: stale ratings for another candidate_id must NOT count.
            ratings_file = td / "blind.jsonl"
            ratings_file.write_text(
                json.dumps({
                    "a": "candidate", "b": "human", "winner": "a",
                    "a_id": "stale_old_hash", "b_id": "human_hash",
                }) + "\n"
            )
            stats = {"bots": 1, "decisions": 5, "llm_errors": 0, "invalid_plans": 0,
                     "max_wakes_per_min": 5.0, "do_p95_ms": 200.0}
            res = soak_scorer.score_soak(cand_dir, run_stats=stats, blind_ratings_path=ratings_file)
            self.assertEqual(res["ranking"]["comparisons"], 0)
            self.assertIsNone(res["rating"])

            # Finding 2: --judge is blocked when judge_gate.json is missing.
            missing_gate = td / "no_gate.json"
            res_j = soak_scorer.score_soak(
                cand_dir, run_stats=stats, use_judge=True, judge_gate_path=missing_gate
            )
            self.assertEqual(res_j["ranking"]["mode"], "llm_judge_blocked_uncalibrated")
            self.assertTrue(res_j["ranking"]["judge_gate_error"])

            # Finding 4: 1 Hz human traces vs 5 Hz bot traces fail recording_parity.
            h1hz = {
                "Hele": [
                    {"kind": "sample", "t_ms": 1000 * i, "x": float(i), "y": 0.0, "z": 0.0}
                    for i in range(100)
                ]
            }
            b5hz = {"Loada": rows}
            parity = trace_parity.check_source_rate_parity(h1hz, b5hz)
            self.assertFalse(parity["ok"])
            self.assertIn("too low", parity["reason"])

            # Finding 5: excluding self-human yields bot-vs-bot pairs only.
            dummy_clip = {"situation": "travel", "frames": [], "events": []}
            pairs = blind_rate.sample_matched_pairs(
                {
                    "human": {"travel": [dummy_clip]},
                    "current": {"travel": [dummy_clip]},
                    "candidate": {"travel": [dummy_clip]},
                },
                n_pairs=10,
                seed=1,
                exclude_labels={"human"},
            )
            self.assertEqual(len(pairs), 10)
            for p in pairs:
                self.assertNotEqual(p["a_label"], "human")
                self.assertNotEqual(p["b_label"], "human")

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

    def test_parity_strips_human_only_events(self):
        rows = known_bad_bots.generate_synthetic_human("H1", seed=7)
        t0 = next(int(r["t_ms"]) for r in rows if r.get("kind") == "sample") + 2000
        # Inject human-only events plus one symmetric event.
        for ev in ("vendor_repair", "item_open", "emote", "mail_take_item", "client_input", "vendor_buy", "item_unequip", "quest_abandon"):
            rows.append({"kind": "event", "t_ms": t0, "event": ev, "detail": "", "value": 1})
        rows.append({"kind": "event", "t_ms": t0 + 200, "event": "cast_start", "detail": "Shadow Bolt", "value": 686})
        frames = trace_parity.reconstruct_trajectory(rows)
        seen = {e["event"] for f in frames for e in f.get("events", [])}
        for forbidden in ("vendor_repair", "item_open", "emote", "mail_take_item", "client_input", "vendor_buy", "item_unequip", "quest_abandon"):
            self.assertNotIn(forbidden, seen)
        self.assertIn("cast_start", seen)


if __name__ == "__main__":
    unittest.main()
