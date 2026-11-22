#!/usr/bin/env python3
"""Unit tests for C2 self-revising notes.md competence hill-climb loop."""

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))

from tools.bench import notes_hill_climb as nhc  # noqa: E402


class NotesHillClimbTest(unittest.TestCase):
    def test_competence_gate_prefers_kills_and_lower_damage(self) -> None:
        base = nhc.CompetenceScore(
            kills=1, runs=3, median_dps=2000.0, median_deaths=4.0, median_damage_taken=100000.0
        )
        # More kills -> KEEP.
        better_kills = nhc.CompetenceScore(
            kills=2, runs=3, median_dps=1850.0, median_deaths=2.0, median_damage_taken=95000.0
        )
        keep, _ = nhc.is_competence_improvement(base, better_kills)
        self.assertTrue(keep)

        # Fewer kills -> REJECT even with lower damage.
        worse_kills = nhc.CompetenceScore(
            kills=0, runs=3, median_dps=2500.0, median_deaths=5.0, median_damage_taken=50000.0
        )
        keep, _ = nhc.is_competence_improvement(base, worse_kills)
        self.assertFalse(keep)

        # Same kills, >=5% lower damage taken without DPS collapse -> KEEP.
        less_dmg = nhc.CompetenceScore(
            kills=1, runs=3, median_dps=1900.0, median_deaths=4.0, median_damage_taken=90000.0
        )
        keep, _ = nhc.is_competence_improvement(base, less_dmg)
        self.assertTrue(keep)

        # Same kills, lower deaths, but >10% DPS collapse -> REJECT.
        dps_drop = nhc.CompetenceScore(
            kills=1, runs=3, median_dps=1700.0, median_deaths=2.0, median_damage_taken=80000.0
        )
        keep, _ = nhc.is_competence_improvement(base, dps_drop)
        self.assertFalse(keep)

        # Faster wipe: same kills, MORE deaths, less total damage -> REJECT.
        faster_wipe = nhc.CompetenceScore(
            kills=1, runs=3, median_dps=2000.0, median_deaths=5.0, median_damage_taken=60000.0
        )
        keep, _ = nhc.is_competence_improvement(base, faster_wipe)
        self.assertFalse(keep)

    def test_validate_standing_orders(self) -> None:
        good = (
            "# Kael'thas\n"
            "- Standing order: `in Flame Strike -> out of it`\n"
            "- Standing order: `Phoenix Egg appears -> target nearest Phoenix Egg`\n"
            "- Standing order: `always -> avoid Fire, then jump, then wake`\n"
            "```\nArcane Explosion cast -> out of it, then back\n```\n"
        )
        ok, errs = nhc.validate_standing_orders(good)
        self.assertTrue(ok, errs)

        for bad in (
            "# Broken\n- Standing order: `Flame Strike cast -> spread`\n",
            "# Too many steps\n- `in Fire -> out of it, then back, then jump, then wake`\n",
            "# Missing arg\n- `in Fire -> out of`\n",
        ):
            ok, errs = nhc.validate_standing_orders(bad)
            self.assertFalse(ok, f"expected failure for {bad!r}")

    def test_summarize_fight_timeline_reads_traces(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tdir = pathlib.Path(tmp)
            bot_dir = tdir / "bot" / "Loada"
            bot_dir.mkdir(parents=True)
            events = [
                # Prior wave (different inst=1 and t_ms < pull_ms): must be excluded.
                {"kind": "event", "t_ms": 9500, "inst": 1, "event": "damage_taken", "detail": "OldWave", "value": 99999},
                {"kind": "event", "t_ms": 12000, "inst": 2, "event": "damage_taken", "detail": "Arcane Sphere", "value": 9500},
                {"kind": "event", "t_ms": 14000, "inst": 2, "event": "act_sent", "detail": "OUT_OF"},
                {"kind": "event", "t_ms": 15500, "inst": 2, "event": "died", "detail": "Arcane Sphere"},
            ]
            (bot_dir / "2026-11-21.jsonl").write_text("\n".join(json.dumps(e) for e in events) + "\n")
            boss_res = {
                "boss": "Kael'thas Sunstrider",
                "by_arm": {"llm": {"kills": 0, "runs": 1, "median_dps": 1500.0, "median_deaths": 5.0}},
                "trials": [
                    {
                        "mode": "llm",
                        "killed": False,
                        "duration_s": 40.0,
                        "total_dps": 1500.0,
                        "total_deaths": 5,
                        "party_index": 0,
                        "instance_id": 2,
                        "pull_ms": 10000,
                    }
                ],
            }
            summary = nhc.summarize_fight_timeline(boss_res, tdir, arm="llm")
            self.assertNotIn("OldWave", summary)
            self.assertIn("Arcane Sphere=9500", summary)
            self.assertIn("5.5s Loada killed by Arcane Sphere", summary)
            self.assertIn("OUT_OF x1", summary)

    def test_hill_climb_loop_keeps_improvement_and_preserves_incumbent_traces(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp) / "loop"
            calls = {"n": 0}
            seen_trace_dirs: list[pathlib.Path] = []
            seen_rejected: list[list[str]] = []

            def fake_bench(boss_key, notes_path, runs, parallel, out_json, trace_dir):
                calls["n"] += 1
                seen_trace_dirs.append(trace_dir)
                txt = notes_path.read_text()
                if "IMPROVED" in txt:
                    kills, deaths, dps = 2, 1.0, 2100.0
                elif "WORSE" in txt:
                    kills, deaths, dps = 0, 5.0, 1200.0
                else:
                    kills, deaths, dps = 1, 3.0, 2000.0
                return {
                    "boss": "Test Boss",
                    "by_arm": {"llm": {"kills": kills, "runs": runs, "median_dps": dps, "median_deaths": deaths}},
                    "trials": [{"mode": "llm", "killed": kills > 0, "duration_s": 50.0, "total_dps": dps, "total_deaths": int(deaths), "party_index": 0, "instance_id": calls["n"], "pull_ms": 1000, "characters": {"Loada": {"damage_taken": 50000}}}],
                }

            proposals = iter(
                [
                    "# Bad syntax\n- `Foo cast -> bogus_verb`\n",
                    "# WORSE\n- `in Fire -> out of it`\n",
                    "# IMPROVED\n- `in Fire -> out of it`\n",
                ]
            )

            def fake_propose(bn, cn, tl, rej):
                seen_rejected.append(list(rej))
                return next(proposals)

            res = nhc.run_notes_hill_climb(
                boss_key="kaelthas",
                initial_notes="# Base\n",
                out_dir=out_dir,
                rounds=3,
                runs_per_eval=3,
                confirm_runs=0,
                bench_fn=fake_bench,
                propose_fn=fake_propose,
            )
            self.assertEqual(calls["n"], 3)
            # Each bench call got an isolated trace_dir so rejected rounds never clobber incumbent traces.
            self.assertEqual(len(set(seen_trace_dirs)), 3)
            # Rejected history passed to round 3 includes the rejected standing order text.
            self.assertTrue(any("in Fire -> out of it" in r for r in seen_rejected[-1]))
            self.assertEqual(res["best_score"]["kills"], 2)
            self.assertIn("IMPROVED", (out_dir / "best_notes.md").read_text())
            ledger_lines = [json.loads(l) for l in (out_dir / "ledger.jsonl").read_text().splitlines()]
            statuses = [e["status"] for e in ledger_lines]
            self.assertEqual(statuses, ["baseline", "rejected_syntax", "rejected", "kept"])


if __name__ == "__main__":
    unittest.main()
