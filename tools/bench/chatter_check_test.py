#!/usr/bin/env python3
"""Unit and adversarial tests for tools/bench/chatter_check.py."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "bench"))

import chatter_check as cc  # noqa: E402


def _line(
    text: str,
    *,
    bot: str = "Aldric",
    t_s: float = 0.0,
    channel: str = "party",
) -> cc.ChatLine:
    return cc.ChatLine(bot=bot, t_ms=int(t_s * 1000), channel=channel, text=text)


def _write_decisions(
    base_dir: pathlib.Path,
    bot: str,
    records: list[dict],
) -> None:
    ev_dir = base_dir / "bots" / bot / "events"
    ev_dir.mkdir(parents=True, exist_ok=True)
    lines = [json.dumps(r) for r in records]
    (ev_dir / "2026-11-26.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")


class ChatterCheckTest(unittest.TestCase):
    def test_extract_plan_chats_parses_all_channels_and_ignores_non_chat(self) -> None:
        plan = "\n".join([
            "do: 2",
            "expect: none",
            "watch: hp<35",
            "look: 90",
            "goal: help tank",
            "say: omw",
            "party: oom, drinking",
            "general: anyone seen hogger?",
            "lfg: LFM RFC need heals",
            "guild: grats!",
            "tell Kelra: sure, send me an invite",
            "remember: Kelra is a good healer",
            "order: Flame Strike -> out of it",
            "act: ping",
        ])
        got = cc.extract_plan_chats(plan)
        self.assertEqual(
            got,
            [
                ("say", "omw"),
                ("party", "oom, drinking"),
                ("general", "anyone seen hogger?"),
                ("lfg", "LFM RFC need heals"),
                ("guild", "grats!"),
                ("tell:Kelra", "sure, send me an invite"),
            ],
        )

    def test_normal_concise_wow_callouts_pass(self) -> None:
        callouts = [
            "oom, drinking",
            "r",
            "ty",
            "np",
            "gg",
            "tyfg",
            "gj",
            "nice pull",
            "inc pat",
            "sheep moon",
            "kill skull",
            "rez pls",
            "omw",
            "sure, send me an invite",
            "LFM RFC need heals",
            "my bad",
            "got it",
            "sounds good",
            "pulling",
            "wait for mana",
        ]
        # Spread over 1 hour (180 s apart).
        lines = [_line(txt, t_s=i * 180.0) for i, txt in enumerate(callouts)]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertTrue(res["ok"], f"unexpectedly flagged normal callouts: {res['flagged']}")
        self.assertEqual(res["flagged_lines"], 0)
        self.assertEqual(res["total_lines"], len(callouts))

    def test_short_acks_allowed_over_time_but_flagged_when_spammed(self) -> None:
        # "ty" 5 minutes apart is normal.
        ok_lines = [_line("ty", t_s=0.0), _line("ty", t_s=300.0)]
        res_ok = cc.check_chat_lines(ok_lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertEqual(res_ok["flagged_lines"], 0)

        # "ty" repeated 10 seconds later is spam.
        spam_lines = [_line("ty", t_s=0.0), _line("ty", t_s=10.0)]
        res_spam = cc.check_chat_lines(spam_lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertEqual(res_spam["flagged_lines"], 1)
        self.assertEqual(res_spam["counts"]["repeated"], 1)

    def test_adversarial_repeated_and_near_duplicate_lines_flagged(self) -> None:
        lines = [
            _line("oom, drinking", t_s=0.0),
            _line("oom, drinking", t_s=120.0),  # exact repeat within 10 min
            _line("watch out for the patrol on the left side", t_s=200.0),
            _line("watch out for the patrol on the left side!", t_s=260.0),  # punctuation dup
            _line("watch out for the patrol on the left side now", t_s=310.0),  # near-dup
        ]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=2.0)
        self.assertFalse(res["ok"])
        self.assertEqual(res["counts"]["repeated"], 3)
        self.assertEqual(res["flagged_per_hour"], 3.0)

    def test_slow_session_catchphrase_repetition_flagged_on_third_use(self) -> None:
        lines = [
            _line("for the alliance, onwards!", t_s=0.0),
            _line("for the alliance, onwards!", t_s=1200.0),  # > 10 min: 2nd use allowed
            _line("for the alliance, onwards!", t_s=2400.0),  # 3rd use: catchphrase loop
        ]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertFalse(res["ok"])
        self.assertEqual(res["counts"]["repeated"], 1)

    def test_different_bots_saying_same_callout_not_flagged(self) -> None:
        lines = [
            _line("oom, drinking", bot="Mirabel", t_s=0.0),
            _line("oom, drinking", bot="Vesper", t_s=5.0),
        ]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertTrue(res["ok"])
        self.assertEqual(res["flagged_lines"], 0)

    def test_adversarial_narration_lines_flagged(self) -> None:
        narration = [
            "I'm attacking the kobold now",
            "Going to loot the corpse",
            "Looting the boar now",
            "Time to rest and drink",
            "My health is full now",
            "That mob is dead",
            "We are in combat",
            "I will now turn in the quest",
            "Heading over to the quest giver",
        ]
        lines = [_line(txt, t_s=i * 60.0) for i, txt in enumerate(narration)]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertFalse(res["ok"])
        self.assertEqual(res["counts"]["narrating"], len(narration))
        self.assertEqual(res["flagged_lines"], len(narration))

    def test_adversarial_sycophancy_lines_flagged(self) -> None:
        sycophancy = [
            "Great idea! Right away!",
            "You're the best tank ever!",
            "Absolutely, happy to help!",
            "Certainly, consider it done!",
            "Brilliant plan, you're amazing!",
            "Thank you so much for leading us, you're totally right!",
            "My pleasure, boss!",
        ]
        lines = [_line(txt, t_s=i * 60.0) for i, txt in enumerate(sycophancy)]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=0.0)
        self.assertFalse(res["ok"])
        self.assertEqual(res["counts"]["sycophantic"], len(sycophancy))
        self.assertEqual(res["flagged_lines"], len(sycophancy))

    def test_configurable_ceiling_controls_pass_fail(self) -> None:
        lines = [
            _line("I'm looting the corpse", t_s=0.0),
            _line("My mana is full now", t_s=600.0),
            _line("Great idea!", t_s=1200.0),
        ]
        # 3 flagged lines over 0.5 bot-hours = 6.0/h.
        res_pass = cc.check_chat_lines(lines, duration_hours=0.5, max_per_hour=6.0)
        self.assertTrue(res_pass["ok"])
        self.assertEqual(res_pass["flagged_per_hour"], 6.0)

        res_fail = cc.check_chat_lines(lines, duration_hours=0.5, max_per_hour=5.9)
        self.assertFalse(res_fail["ok"])

    def test_pluggable_judge_flags_nuanced_lines_and_skips_already_flagged(self) -> None:
        seen_by_judge: list[str] = []

        def fake_judge(line: cc.ChatLine, history: list[cc.ChatLine]) -> str | None:
            seen_by_judge.append(line.text)
            if "finest warrior" in line.text.lower():
                return "sycophantic: excessive flattery"
            return "OK"

        lines = [
            _line("oom, drinking", t_s=0.0),  # clean -> goes to judge -> OK
            _line("Great idea!", t_s=60.0),  # deterministic sycophancy -> skips judge
            _line("You truly are the finest warrior in Azeroth", t_s=120.0),  # judge flags
        ]
        res = cc.check_chat_lines(lines, duration_hours=1.0, max_per_hour=1.0, judge=fake_judge)
        self.assertFalse(res["ok"])
        self.assertEqual(
            seen_by_judge,
            ["oom, drinking", "You truly are the finest warrior in Azeroth"],
        )
        self.assertEqual(res["judge_calls"], 2)
        self.assertEqual(res["judge_errors"], 0)
        self.assertEqual(res["counts"]["sycophantic"], 2)
        self.assertEqual(res["flagged"][1]["source"], "judge")

    def test_pluggable_judge_error_is_recorded_without_crashing(self) -> None:
        def broken_judge(line: cc.ChatLine, history: list[cc.ChatLine]) -> str | None:
            raise RuntimeError("llm offline")

        res = cc.check_chat_lines(
            [_line("oom, drinking", t_s=0.0)],
            duration_hours=1.0,
            max_per_hour=6.0,
            judge=broken_judge,
        )
        self.assertTrue(res["ok"])
        self.assertEqual(res["judge_calls"], 1)
        self.assertEqual(res["judge_errors"], 1)

    def test_make_llm_judge_parses_verdicts(self) -> None:
        judge = cc.make_llm_judge(base_url="http://127.0.0.1:8090")
        with mock.patch.object(cc.llm_lib, "chat_completion", return_value="OK"):
            self.assertIsNone(judge(_line("oom, drinking"), []))
        with mock.patch.object(
            cc.llm_lib,
            "chat_completion",
            return_value="NARRATING: states obvious looting",
        ):
            self.assertEqual(
                judge(_line("just grabbed the shiny drop"), []),
                "narrating: states obvious looting",
            )

    def test_load_decision_dirs_multi_run_isolation_and_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            run0 = root / "run0"
            run1 = root / "run1"
            # Both runs have Loada saying "oom, drinking" once -> must NOT collide across runs.
            _write_decisions(
                run0,
                "Loada",
                [
                    {"time_ms": 0, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60\nparty: oom, drinking"},
                    {"time_ms": 1_800_000, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60"},
                ],
            )
            _write_decisions(
                run1,
                "Loada",
                [
                    {"time_ms": 10_000, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60\nparty: oom, drinking"},
                    {"time_ms": 1_810_000, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60"},
                ],
            )
            res = cc.check_decision_dirs([run0, run1], max_per_hour=0.0)
            self.assertTrue(res["ok"])
            self.assertEqual(res["total_lines"], 2)
            self.assertEqual(res["flagged_lines"], 0)
            self.assertAlmostEqual(res["bot_hours"], 1.0, places=3)

            # CLI exit code 0 on clean run.
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = cc.main([str(run0), "--max-per-hour", "0", "--json"])
            self.assertEqual(rc, 0)
            parsed = json.loads(buf.getvalue())
            self.assertTrue(parsed["ok"])

            # Bad run returns CLI exit code 1.
            bad_run = root / "bad"
            _write_decisions(
                bad_run,
                "Loadb",
                [
                    {"time_ms": 0, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60\nparty: Great idea!"},
                    {"time_ms": 600_000, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60\nparty: I'm looting the corpse"},
                    {"time_ms": 1_800_000, "plan": "do: 1\nexpect: none\nwatch: none\nlook: 60"},
                ],
            )
            buf2 = io.StringIO()
            with contextlib.redirect_stdout(buf2):
                rc_bad = cc.main([str(bad_run), "--max-per-hour", "1.0"])
            self.assertEqual(rc_bad, 1)
            self.assertIn("CHATTER CHECK: FAIL", buf2.getvalue())


if __name__ == "__main__":
    unittest.main()
