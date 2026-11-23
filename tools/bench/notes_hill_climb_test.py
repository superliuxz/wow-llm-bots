#!/usr/bin/env python3
"""Unit tests for C2 self-revising notes.md competence hill-climb loop."""

import io
import json
import os
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

        # Same kills, lower damage taken alone does NOT buy a KEEP (would reward wiping faster).
        less_dmg = nhc.CompetenceScore(
            kills=1, runs=3, median_dps=1900.0, median_deaths=4.0, median_damage_taken=90000.0
        )
        keep, _ = nhc.is_competence_improvement(base, less_dmg, expected_runs=3)
        self.assertFalse(keep)

        # Incomplete runs cannot buy a KEEP (1/1 must NOT beat 1/3).
        incomplete = nhc.CompetenceScore(
            kills=1, runs=1, median_dps=2200.0, median_deaths=0.0, median_damage_taken=10000.0
        )
        keep, reason = nhc.is_competence_improvement(base, incomplete, expected_runs=3)
        self.assertFalse(keep)
        self.assertIn("incomplete", reason)

        # Same kills, lower deaths without DPS collapse on fixed n -> KEEP.
        fewer_deaths = nhc.CompetenceScore(
            kills=1, runs=3, median_dps=1950.0, median_deaths=2.0, median_damage_taken=100000.0
        )
        keep, _ = nhc.is_competence_improvement(base, fewer_deaths, expected_runs=3)
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
            "- Standing order: `always -> use Lay on Hands`\n"
            "- Standing order: `always -> use Lay on Hands on tank`\n"
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

        # Cross-check against every `fits` row in docs/specs/trigger-actions.tsv.
        acts_tsv = ROOT / "docs" / "specs" / "trigger-actions.tsv"
        acts_checked = 0
        for line in acts_tsv.read_text().splitlines()[1:]:
            cols = line.split("\t")
            if len(cols) >= 7 and cols[5] == "fits":
                act = cols[6].strip()
                if act and act != "-":
                    ok_row, err_row = nhc.validate_standing_orders(f"- `always -> {act}`\n")
                    self.assertTrue(ok_row, f"rejected audited action {act!r}: {err_row}")
                    acts_checked += 1
        self.assertGreater(acts_checked, 200)

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
            "# Negative numeric arg on away\n- `always -> away boss -5`\n",
            "# Malformed use ... on ...\n- `always -> use Potion on  boss`\n",
            "# Double space before on in use\n- `always -> use Potion  on boss`\n",
            "# Digits-only target in use\n- `always -> use Potion on 123`\n",
            "# Invalid marker in use\n- `always -> use Potion on marker purple`\n",
            "# Out-of-range Num in to yd from\n- `always -> to 1000 yd from boss`\n",
            "# Double space in to in front of\n- `always -> to in front  of boss`\n",
            "# Double space before cast on me\n- `Flame Strike  cast on me -> wake`\n",
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
            "# Dangling ', then'\n- `Blistering Zombie targets me -> kite it, then`\n",
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
        prompt_text = captured[0]["messages"][0]["content"]
        for verb in ("out of", "away", "target", "use", "stop", "kite", "wake"):
            self.assertIn(verb, prompt_text)
        self.assertIn("in Fire -> out of it", out)

    def test_confirmation_rerun_keeps_confirmed_and_logs_conf_score_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp) / "confirm_loop"
            # confirm_runs=0 must be refused (mandatory confirmation).
            with self.assertRaises(ValueError):
                nhc.run_notes_hill_climb(
                    boss_key="kaelthas",
                    initial_notes="# Base\n",
                    out_dir=out_dir,
                    rounds=1,
                    runs_per_eval=4,
                    confirm_runs=0,
                    bench_fn=lambda *a: {},
                    propose_fn=lambda *a: "",
                )

            # Round 1: initial looks good (6/8), confirmation fails (1/8).
            # Round 2: initial looks good (7/8), confirmation succeeds (8/8) -> Fisher p < 0.025 -> KEEP.
            bench_seq = iter(
                [
                    (4, 3.0, 2000.0),  # baseline (4/16)
                    (6, 1.0, 2100.0),  # r1 initial
                    (1, 5.0, 1900.0),  # r1 confirm -> fail
                    (7, 1.0, 2100.0),  # r2 initial
                    (8, 0.0, 2200.0),  # r2 confirm -> pass (pooled 15/16 vs 4/16)
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
                runs_per_eval=8,
                confirm_runs=8,
                bench_fn=fake_bench,
                propose_fn=lambda bn, cn, tl, rej: next(proposals),
            )
            self.assertEqual(res["best_score"]["kills"], 15)
            self.assertEqual(res["best_score"]["runs"], 16)
            ledger = [json.loads(l) for l in (out_dir / "ledger.jsonl").read_text().splitlines()]
            self.assertEqual([e["status"] for e in ledger], ["baseline", "rejected", "kept"])
            # Baseline logs flags and code provenance hashes.
            self.assertEqual(ledger[0]["flags"]["runs_per_eval"], 8)
            self.assertEqual(ledger[0]["flags"]["confirm_runs"], 8)
            self.assertEqual(len(ledger[0]["code_provenance"]["notes_hill_climb_sha256"]), 64)
            self.assertEqual(len(ledger[0]["code_provenance"]["bench_bosses_sha256"]), 64)
            # Failed confirmation must log conf_score (1 kill), not initial cand_score (6 kills).
            self.assertEqual(ledger[1]["score"]["kills"], 1)
            self.assertEqual(ledger[1]["initial_score"]["kills"], 6)
            # Kept round logs pooled score (15/16) matching best_score, plus confirm_score (8/8).
            self.assertEqual(ledger[2]["score"]["kills"], 15)
            self.assertEqual(ledger[2]["score"]["runs"], 16)
            self.assertEqual(ledger[2]["confirm_score"]["kills"], 8)
            self.assertLessEqual(ledger[2]["fisher_p"], 0.025)

    def test_hill_climb_loop_keeps_improvement_and_preserves_incumbent_traces(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            orig_cwd = os.getcwd()
            os.chdir(tmp)
            try:
                out_dir = pathlib.Path("loop")
                self.assertFalse(out_dir.is_absolute())
                calls = {"n": 0}
                seen_trace_dirs: list[pathlib.Path] = []
                seen_rejected: list[list[str]] = []

                def fake_bench(boss_key, notes_path, runs, parallel, out_json, trace_dir):
                    calls["n"] += 1
                    seen_trace_dirs.append(trace_dir)
                    txt = notes_path.read_text()
                    if "IMPROVED" in txt:
                        kills, deaths, dps = runs, 1.0, 2100.0
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
                    runs_per_eval=8,
                    confirm_runs=8,
                    bench_fn=fake_bench,
                    propose_fn=fake_propose,
                )
                # 1 baseline + 1 WORSE + 1 IMPROVED stage1 + 1 IMPROVED confirm = 4 calls.
                self.assertEqual(calls["n"], 4)
                # Each bench call got an isolated absolute trace_dir even when out_dir is relative.
                self.assertEqual(len(set(seen_trace_dirs)), 4)
                self.assertTrue(all(p.is_absolute() for p in seen_trace_dirs))
                # Rejected history passed to round 3 includes the rejected standing order text.
                self.assertTrue(any("in Fire -> out of it" in r for r in seen_rejected[-1]))
                self.assertEqual(res["best_score"]["kills"], 16)
                self.assertIn("IMPROVED", (out_dir / "best_notes.md").read_text())
                ledger_lines = [json.loads(l) for l in (out_dir / "ledger.jsonl").read_text().splitlines()]
                statuses = [e["status"] for e in ledger_lines]
                self.assertEqual(statuses, ["baseline", "rejected_syntax", "rejected", "kept"])
            finally:
                os.chdir(orig_cwd)

    def test_held_out_boss_regression_rejects_overfit_candidate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp) / "held_out"

            def fake_bench(boss_key, notes_path, runs, parallel, out_json, trace_dir):
                txt = notes_path.read_text()
                if boss_key == "loken":
                    # Held-out boss regresses sharply on OVERFIT notes (0/8 vs 8/8).
                    kills = 0 if "OVERFIT" in txt else runs
                else:
                    kills = runs if "OVERFIT" in txt else 2
                return {
                    "boss": boss_key,
                    "by_arm": {"llm": {"kills": kills, "runs": runs, "median_dps": 2000.0, "median_deaths": 1.0}},
                    "trials": [],
                }

            res = nhc.run_notes_hill_climb(
                boss_key="kaelthas",
                initial_notes="# Base\n",
                out_dir=out_dir,
                rounds=1,
                runs_per_eval=8,
                confirm_runs=8,
                held_out_boss="loken",
                held_out_runs=8,
                bench_fn=fake_bench,
                propose_fn=lambda *a: "# OVERFIT\n- `in Fire -> out of it`\n",
            )
            self.assertEqual(res["best_score"]["kills"], 2)
            ledger = [json.loads(l) for l in (out_dir / "ledger.jsonl").read_text().splitlines()]
            self.assertEqual(ledger[1]["status"], "rejected")
            self.assertIn("held-out regression", ledger[1]["reason"])

    def test_baseline_incomplete_and_two_sample_deaths_and_pooling(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            # 1) Incomplete baseline aborts with RuntimeError.
            with self.assertRaisesRegex(RuntimeError, r"baseline incomplete"):
                nhc.run_notes_hill_climb(
                    boss_key="kaelthas",
                    initial_notes="# Base\n",
                    out_dir=pathlib.Path(tmp) / "inc_base",
                    rounds=1,
                    runs_per_eval=4,
                    confirm_runs=4,
                    bench_fn=lambda *a: {"boss": "B", "by_arm": {"llm": {"kills": 1, "runs": 2, "median_dps": 1000, "median_deaths": 2}}, "trials": []},
                    propose_fn=lambda *a: "# X\n",
                )

            # 2) _pool_boss_results computes exact medians from pooled non-empty trials.
            res_a = {
                "trials": [
                    {"mode": "llm", "total_dps": 1000.0, "total_deaths": 2, "characters": {"a": {"damage_taken": 100}}},
                    {"mode": "llm", "total_dps": 1200.0, "total_deaths": 4, "characters": {"a": {"damage_taken": 200}}},
                ]
            }
            res_b = {
                "trials": [
                    {"mode": "llm", "total_dps": 2000.0, "total_deaths": 0, "characters": {"a": {"damage_taken": 300}}},
                    {"mode": "llm", "total_dps": 2200.0, "total_deaths": 0, "characters": {"a": {"damage_taken": 400}}},
                ]
            }
            pooled = nhc._pool_boss_results(
                res_a, nhc.CompetenceScore(1, 2, 1100.0, 3.0, 150.0),
                res_b, nhc.CompetenceScore(2, 2, 2100.0, 0.0, 350.0),
            )
            self.assertEqual((pooled.kills, pooled.runs), (3, 4))
            self.assertEqual(pooled.median_dps, 1600.0)
            self.assertEqual(pooled.median_deaths, 1.0)

            # 3) 100%-kill exact Mann-Whitney test handles 0-death ties cleanly (keeps clear
            # drop even when >=50% of pooled trials have 0 deaths, and rejects noise).
            def make_trials_list(deaths_list):
                return [{"mode": "llm", "killed": True, "total_dps": 2000.0, "total_deaths": d} for d in deaths_list]

            # 3a) Zero-tie case: incumbent [0] + [1]*15 vs candidate [0]*16 -> MUST KEEP.
            seq_zero = iter([
                {"boss": "B", "by_arm": {"llm": {"kills": 16, "runs": 16, "median_dps": 2000.0, "median_deaths": 1.0}}, "trials": make_trials_list([0] + [1] * 15)},
                {"boss": "B", "by_arm": {"llm": {"kills": 8, "runs": 8, "median_dps": 2000.0, "median_deaths": 0.0}}, "trials": make_trials_list([0] * 8)},
                {"boss": "B", "by_arm": {"llm": {"kills": 8, "runs": 8, "median_dps": 2000.0, "median_deaths": 0.0}}, "trials": make_trials_list([0] * 8)},
            ])
            res_d = nhc.run_notes_hill_climb(
                boss_key="loken",
                initial_notes="# Base\n",
                out_dir=pathlib.Path(tmp) / "deaths_ok",
                rounds=1,
                runs_per_eval=8,
                confirm_runs=8,
                bench_fn=lambda *a: next(seq_zero),
                propose_fn=lambda *a: "# Better\n- `in Fire -> out of it`\n",
            )
            self.assertEqual(res_d["rounds"][1]["status"], "kept")
            self.assertEqual(res_d["best_score"]["median_deaths"], 0.0)

            # 3b) Noise case: incumbent [0]*7 + [1]*9 vs candidate [0]*9 + [1]*7 -> MUST REJECT.
            seq_noise = iter([
                {"boss": "B", "by_arm": {"llm": {"kills": 16, "runs": 16, "median_dps": 2000.0, "median_deaths": 1.0}}, "trials": make_trials_list([0] * 7 + [1] * 9)},
                {"boss": "B", "by_arm": {"llm": {"kills": 8, "runs": 8, "median_dps": 2000.0, "median_deaths": 0.0}}, "trials": make_trials_list([0] * 5 + [1] * 3)},
                {"boss": "B", "by_arm": {"llm": {"kills": 8, "runs": 8, "median_dps": 2000.0, "median_deaths": 0.5}}, "trials": make_trials_list([0] * 4 + [1] * 4)},
            ])
            res_noise = nhc.run_notes_hill_climb(
                boss_key="loken",
                initial_notes="# Base\n",
                out_dir=pathlib.Path(tmp) / "deaths_noise",
                rounds=1,
                runs_per_eval=8,
                confirm_runs=8,
                bench_fn=lambda *a: next(seq_noise),
                propose_fn=lambda *a: "# Noisy\n- `in Fire -> out of it`\n",
            )
            self.assertEqual(res_noise["rounds"][1]["status"], "rejected")
            self.assertIn("not statistically significant", res_noise["rounds"][1]["reason"])

            # 3c) summarize_fight_timeline reads traces across a list of trace dirs (cand + confirm).
            td1 = pathlib.Path(tmp) / "td1"
            td2 = pathlib.Path(tmp) / "td2"
            (td1 / "bot" / "Loada").mkdir(parents=True)
            (td2 / "bot" / "Loada").mkdir(parents=True)
            (td1 / "bot" / "Loada" / "t.jsonl").write_text(
                json.dumps({"kind": "event", "t_ms": 11000, "event": "damage_taken", "detail": "CandSpell", "value": 5000}) + "\n"
            )
            (td2 / "bot" / "Loada" / "t.jsonl").write_text(
                json.dumps({"kind": "event", "t_ms": 21000, "event": "damage_taken", "detail": "ConfSpell", "value": 6000}) + "\n"
            )
            tl = nhc.summarize_fight_timeline(
                {
                    "boss": "B",
                    "by_arm": {"llm": {"kills": 0, "runs": 2}},
                    "trials": [
                        {"mode": "llm", "killed": False, "duration_s": 5.0, "total_dps": 1000, "total_deaths": 5, "party_index": 0, "pull_ms": 10000},
                        {"mode": "llm", "killed": False, "duration_s": 5.0, "total_dps": 1000, "total_deaths": 5, "party_index": 0, "pull_ms": 20000},
                    ],
                },
                [td1, td2],
                arm="llm",
                max_wipes=2,
            )
            self.assertIn("CandSpell=5000", tl)
            self.assertIn("ConfSpell=6000", tl)

            # 4) Noisy 1-kill held-out dip (7/8 vs 8/8, p=0.5 > 0.10) does NOT falsely reject.
            def fake_ho(boss_key, notes_path, runs, parallel, out_json, trace_dir):
                txt = notes_path.read_text()
                if boss_key == "loken":
                    kills = 7 if "CAND" in txt else 8
                else:
                    kills = runs if "CAND" in txt else 2
                return {
                    "boss": boss_key,
                    "by_arm": {"llm": {"kills": kills, "runs": runs, "median_dps": 2000.0, "median_deaths": 1.0}},
                    "trials": [],
                }

            res_ho = nhc.run_notes_hill_climb(
                boss_key="kaelthas",
                initial_notes="# Base\n",
                out_dir=pathlib.Path(tmp) / "ho_noisy",
                rounds=1,
                runs_per_eval=8,
                confirm_runs=8,
                held_out_boss="loken",
                held_out_runs=8,
                bench_fn=fake_ho,
                propose_fn=lambda *a: "# CAND\n- `in Fire -> out of it`\n",
            )
            self.assertEqual(res_ho["rounds"][1]["status"], "kept")

    def test_all_content_notes_standing_orders_valid(self) -> None:
        def has_unexported_boss_aura(lhs: str) -> bool:
            for clause in lhs.split(" & "):
                c = clause.strip()
                if c.endswith(" on boss") and not c.endswith(" cast on boss"):
                    return True
            return False

        self.assertTrue(has_unexported_boss_aura("Shield on boss & not phase 2"))
        self.assertTrue(has_unexported_boss_aura("Flame Breath cast on boss & Shield on boss"))
        self.assertFalse(has_unexported_boss_aura("Flame Breath cast on boss"))
        self.assertFalse(has_unexported_boss_aura("Flame Breath cast on boss & not phase 2"))

        notes_files = sorted((ROOT / "content" / "notes").glob("*/notes.md"))
        self.assertGreaterEqual(len(notes_files), 40)
        for path in notes_files:
            txt = path.read_text(encoding="utf-8")
            ok, errs = nhc.validate_standing_orders(txt)
            self.assertTrue(ok, f"{path.relative_to(ROOT)}: {errs}")
            self.assertNotIn("DRAFT", txt, f"{path.relative_to(ROOT)} still marked DRAFT")
            self.assertNotIn("hand-check", txt.lower(), f"{path.relative_to(ROOT)} has hand-check prose")
            for order in nhc.extract_standing_orders(txt):
                lhs = order.split("->", 1)[0].strip()
                self.assertFalse(
                    has_unexported_boss_aura(lhs),
                    f"{path.relative_to(ROOT)}: unexported boss aura {order!r}",
                )
            seen_in_block: set[str] = set()
            in_code = False
            for line in txt.splitlines():
                s = line.strip()
                if s.startswith("```"):
                    in_code = not in_code
                    seen_in_block.clear()
                    continue
                if in_code and "->" in s:
                    self.assertNotIn(s, seen_in_block, f"{path.relative_to(ROOT)}: duplicate order {s!r}")
                    seen_in_block.add(s)


if __name__ == "__main__":
    unittest.main()
