#!/usr/bin/env python3
"""Unit tests for C2 self-revising notes.md competence hill-climb loop."""

import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

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

        # Kill gain with >15% DPS collapse -> REJECT.
        kill_with_dps_collapse = nhc.CompetenceScore(
            kills=2, runs=3, median_dps=1600.0, median_deaths=1.0, median_damage_taken=80000.0
        )
        keep, _ = nhc.is_competence_improvement(base, kill_with_dps_collapse)
        self.assertFalse(keep)

    def test_validate_standing_orders(self) -> None:
        good = (
            "# Kael'thas\n"
            "- Standing order: `in Flame Strike -> out of it`\n"
            "- Standing order: `Phoenix Egg appears -> target nearest Phoenix Egg`\n"
            "- Standing order: `Valkyr appears in last 2.5s -> target nearest Valkyr`\n"
            "- Standing order: `not me in vehicle -> use Ruby Essence`\n"
            "- Standing order: `me nearest to it -> away boss 12`\n"
            "- Standing order: `always -> avoid Fire, then jump, then wake`\n"
            "- Standing order: `Mirrored Soul on anyone & not phase 2 -> stop attacking, then pet passive`\n"
            "```\nArcane Explosion cast -> out of it, then back\n```\n"
        )
        ok, errs = nhc.validate_standing_orders(good)
        self.assertTrue(ok, errs)

        # Cross-check against every `fits` row in docs/specs/trigger-conditions.tsv.
        tsv = ROOT / "docs" / "specs" / "trigger-conditions.tsv"
        fits_checked = 0
        for line in tsv.read_text().splitlines()[1:]:
            cols = line.split("\t")
            if len(cols) >= 7 and cols[5] == "fits":
                cond = cols[6].strip()
                if cond and cond != "-":
                    ok_row, err_row = nhc.validate_standing_orders(f"- `{cond} -> wake`\n")
                    self.assertTrue(ok_row, f"rejected audited trigger {cond!r}: {err_row}")
                    fits_checked += 1
        self.assertGreater(fits_checked, 300)

        for bad in (
            "# Non-existent verb\n- Standing order: `Flame Strike cast -> spread`\n",
            "# Uppercase keyword\n- `BOSS HP<20 -> WAKE`\n",
            "# Uppercase act\n- `Defile on me -> OUT OF it`\n",
            "# Invalid Who\n- `Defile on somebody -> away`\n",
            "# Invalid State\n- `boss is dancing -> wake`\n",
            "# Invalid Icon in cond\n- `boss is marked purple -> wake`\n",
            "# Invalid Icon in act\n- `always -> mark boss purple`\n",
            "# Invalid MyState\n- `me dancing -> wake`\n",
            "# Invalid Digit\n- `phase 12 -> wake`\n",
            "# Spaces around Cmp\n- `boss hp < 20 -> wake`\n",
            "# Extra numeric arg on away\n- `always -> away boss 10 20`\n",
            "# Out-of-range Num on away\n- `always -> away 1000`\n",
            "# 2dp Num on away\n- `always -> away 10.55`\n",
            "# Out-of-range second numeric arg on away\n- `always -> away boss 10 1000`\n",
            "# Missing spaces around ->\n- `Flame Strike cast->wake`\n",
            "# Double space before cast\n- `Flame Strike  cast -> wake`\n",
            "# Double space after not\n- `not  boss hp<20 -> wake`\n",
            "# Non-name punctuation\n- `!!! cast -> wake`\n",
            "# Digits-only name\n- `123 cast -> wake`\n",
            "# Double space in stop\n- `always -> stop  casting`\n",
            "# Double space in mark\n- `always -> mark boss  skull`\n",
            "# Prefix collision\n- `in Fire -> backflip`\n",
            "# Prefix collision 2\n- `in Fire -> stomp`\n",
            "# Zero-arg verb with arg\n- `in Fire -> jump high`\n",
            "# Zero-arg back with arg\n- `in Fire -> back 10`\n",
            "# Invalid stop enum\n- `in Fire -> stop dancing`\n",
            "# Invalid pet enum\n- `in Fire -> pet dance`\n",
            "# Unvalidated prose condition\n- `boss gets angry -> wake`\n",
            "# Too many steps\n- `in Fire -> out of it, then back, then jump, then wake`\n",
            "# Missing arg\n- `in Fire -> out of`\n",
        ):
            ok, errs = nhc.validate_standing_orders(bad)
            self.assertFalse(ok, f"expected failure for {bad!r}")

    def test_summarize_fight_timeline_reads_traces_and_bounds_by_sample_inst(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tdir = pathlib.Path(tmp)
            bot_dir = tdir / "bot" / "Loada"
            bot_dir.mkdir(parents=True)
            # Realistic AzerothCore trace: only `sample` carries `inst`, `event` does NOT.
            records = [
                {"kind": "sample", "t_ms": 10000, "inst": 2},
                {"kind": "event", "t_ms": 12000, "event": "damage_taken", "detail": "Arcane Sphere", "value": 9500},
                {"kind": "event", "t_ms": 14000, "event": "act_sent", "detail": "OUT_OF"},
                {"kind": "event", "t_ms": 15500, "event": "died", "detail": "Arcane Sphere"},
                # Fast next wave (inst=3) starts 200 ms after trial end (dur=10s -> 20000 ms),
                # inside the +500 ms tail; sample inst transition at 20200 must exclude 20300 event.
                {"kind": "sample", "t_ms": 20200, "inst": 3},
                {"kind": "event", "t_ms": 20300, "event": "damage_taken", "detail": "NextWaveLeak", "value": 88888},
            ]
            (bot_dir / "2026-11-21.jsonl").write_text("\n".join(json.dumps(e) for e in records) + "\n")
            boss_res = {
                "boss": "Kael'thas Sunstrider",
                "by_arm": {"llm": {"kills": 0, "runs": 1, "median_dps": 1500.0, "median_deaths": 5.0}},
                "trials": [
                    {
                        "mode": "llm",
                        "killed": False,
                        "duration_s": 10.0,
                        "total_dps": 1500.0,
                        "total_deaths": 5,
                        "party_index": 0,
                        "instance_id": 2,
                        "pull_ms": 10000,
                    }
                ],
            }
            summary = nhc.summarize_fight_timeline(boss_res, tdir, arm="llm")
            self.assertNotIn("NextWaveLeak", summary)
            self.assertIn("Arcane Sphere=9500", summary)
            self.assertIn("5.5s Loada killed by Arcane Sphere", summary)
            self.assertIn("OUT_OF x1", summary)

    def test_summarize_fight_timeline_sorts_unsorted_multifile_shards(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tdir = pathlib.Path(tmp)
            bot_dir = tdir / "bot" / "Loada"
            bot_dir.mkdir(parents=True)
            # Newer shard sorts lexicographically AFTER `.jsonl`, yet holds an earlier
            # instance-transition sample at 15200 (and unsorted samples within `.jsonl`).
            (bot_dir / "2026-11-21.jsonl").write_text(
                "\n".join(
                    json.dumps(r)
                    for r in [
                        {"kind": "sample", "t_ms": 19000, "inst": 3},
                        {"kind": "sample", "t_ms": 10000, "inst": 2},
                        {"kind": "event", "t_ms": 12000, "event": "damage_taken", "detail": "InTrialHit", "value": 4200},
                        {"kind": "event", "t_ms": 15300, "event": "damage_taken", "detail": "LeakedAfterEarlyShard", "value": 99999},
                    ]
                )
                + "\n"
            )
            (bot_dir / "2026-11-21.1.jsonl").write_text(
                json.dumps({"kind": "sample", "t_ms": 15200, "inst": 3}) + "\n"
            )
            boss_res = {
                "boss": "Test Boss",
                "by_arm": {"llm": {"kills": 0, "runs": 1, "median_dps": 1000.0, "median_deaths": 5.0}},
                "trials": [
                    {
                        "mode": "llm",
                        "killed": False,
                        "duration_s": 10.0,
                        "total_dps": 1000.0,
                        "total_deaths": 5,
                        "party_index": 0,
                        "instance_id": 2,
                        "pull_ms": 10000,
                    }
                ],
            }
            summary = nhc.summarize_fight_timeline(boss_res, tdir, arm="llm")
            self.assertIn("InTrialHit=4200", summary)
            self.assertNotIn("LeakedAfterEarlyShard", summary)

    def test_propose_notes_edit_disables_thinking_in_request_payload(self) -> None:
        captured: list[dict] = []

        class FakeResp(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

        def fake_urlopen(req, timeout=0):
            captured.append(json.loads(req.data.decode("utf-8")))
            body = {
                "choices": [
                    {"message": {"content": "```markdown\n# Boss\n- `in Fire -> out of it`\n```"}}
                ]
            }
            return FakeResp(json.dumps(body).encode("utf-8"))

        with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            out = nhc.propose_notes_edit(
                boss_name="Test Boss",
                current_notes="# Boss\n",
                timeline_summary="Boss: Test Boss",
                rejected_history=[],
            )
        self.assertEqual(len(captured), 1)
        self.assertEqual(captured[0].get("chat_template_kwargs"), {"enable_thinking": False})
        self.assertIn("in Fire -> out of it", out)

    def test_confirmation_rerun_keeps_confirmed_and_logs_conf_score_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp) / "confirm_loop"
            # Round 1: initial looks good (2/3), confirmation fails (0/3).
            # Round 2: initial looks good (2/3), confirmation succeeds (3/3).
            bench_seq = iter(
                [
                    (1, 3.0, 2000.0),  # baseline
                    (2, 1.0, 2100.0),  # r1 initial
                    (0, 5.0, 1900.0),  # r1 confirm -> fail
                    (2, 1.0, 2100.0),  # r2 initial
                    (3, 0.0, 2200.0),  # r2 confirm -> pass
                ]
            )

            def fake_bench(boss_key, notes_path, runs, parallel, out_json, trace_dir):
                kills, deaths, dps = next(bench_seq)
                return {
                    "boss": "Test Boss",
                    "by_arm": {"llm": {"kills": kills, "runs": runs, "median_dps": dps, "median_deaths": deaths}},
                    "trials": [],
                }

            proposals = iter(
                [
                    "# Flaky\n- `in Fire -> out of it`\n",
                    "# Solid\n- `in Frost -> out of it`\n",
                ]
            )
            res = nhc.run_notes_hill_climb(
                boss_key="kaelthas",
                initial_notes="# Base\n",
                out_dir=out_dir,
                rounds=2,
                runs_per_eval=3,
                confirm_runs=3,
                bench_fn=fake_bench,
                propose_fn=lambda bn, cn, tl, rej: next(proposals),
            )
            self.assertEqual(res["best_score"]["kills"], 3)
            ledger = [json.loads(l) for l in (out_dir / "ledger.jsonl").read_text().splitlines()]
            self.assertEqual([e["status"] for e in ledger], ["baseline", "rejected", "kept"])
            # Failed confirmation must log conf_score (0 kills), not initial cand_score (2 kills).
            self.assertEqual(ledger[1]["score"]["kills"], 0)
            self.assertEqual(ledger[1]["initial_score"]["kills"], 2)
            self.assertEqual(ledger[2]["score"]["kills"], 3)

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
            # Each bench call got an isolated absolute trace_dir so rejected rounds never clobber incumbent traces.
            self.assertEqual(len(set(seen_trace_dirs)), 3)
            self.assertTrue(all(p.is_absolute() for p in seen_trace_dirs))
            # Rejected history passed to round 3 includes the rejected standing order text.
            self.assertTrue(any("in Fire -> out of it" in r for r in seen_rejected[-1]))
            self.assertEqual(res["best_score"]["kills"], 2)
            self.assertIn("IMPROVED", (out_dir / "best_notes.md").read_text())
            ledger_lines = [json.loads(l) for l in (out_dir / "ledger.jsonl").read_text().splitlines()]
            statuses = [e["status"] for e in ledger_lines]
            self.assertEqual(statuses, ["baseline", "rejected_syntax", "rejected", "kept"])


if __name__ == "__main__":
    unittest.main()
