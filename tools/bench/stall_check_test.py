#!/usr/bin/env python3
"""Unit tests for tools/bench/stall_check.py."""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import stall_check as sc  # noqa: E402


class StallCheckTest(unittest.TestCase):
    def test_detects_full_stall_duration_without_truncating_at_threshold(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bdir = pathlib.Path(tmp) / "bot" / "Stuckbot"
            bdir.mkdir(parents=True)
            lines = [
                json.dumps({"kind": "sample", "t_ms": i * 10_000, "x": 100.0, "y": 50.0, "z": 10.0, "dead": False, "in_combat": False})
                for i in range(43)  # 0..420 s
            ]
            (bdir / "trace.jsonl").write_text("\n".join(lines) + "\n")
            res = sc.check_trace_dir(pathlib.Path(tmp), max_stall_s=300.0)
            self.assertIn("Stuckbot", res)
            self.assertEqual(len(res["Stuckbot"]), 1)
            self.assertEqual(res["Stuckbot"][0]["duration_s"], 420.0)

    def test_mob_kill_and_quest_turnin_reset_stall_timer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bdir = pathlib.Path(tmp) / "bot" / "Activebot"
            bdir.mkdir(parents=True)
            lines = []
            for i in range(45):
                lines.append(
                    json.dumps({"kind": "sample", "t_ms": i * 10_000, "x": 100.0, "y": 50.0, "z": 10.0, "dead": False, "in_combat": False})
                )
                if i == 15:
                    lines.append(json.dumps({"kind": "event", "t_ms": i * 10_000 + 500, "event": "mob_kill"}))
                if i == 30:
                    lines.append(json.dumps({"kind": "event", "t_ms": i * 10_000 + 500, "event": "quest_turnin"}))
            (bdir / "trace.jsonl").write_text("\n".join(lines) + "\n")
            res = sc.check_trace_dir(pathlib.Path(tmp), max_stall_s=300.0)
            self.assertEqual(res, {})

    def test_logout_gap_before_sample_or_progress_event_is_not_a_stall(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bdir = pathlib.Path(tmp) / "bot" / "Relogbot"
            bdir.mkdir(parents=True)
            lines = [
                json.dumps({"kind": "sample", "t_ms": 0, "x": 10.0, "y": 10.0, "z": 0.0, "dead": False, "in_combat": False}),
                json.dumps({"kind": "sample", "t_ms": 20_000, "x": 10.0, "y": 10.0, "z": 0.0, "dead": False, "in_combat": False}),
                # Bot logs out for 400 s and logs back in emitting xp_gain before next sample.
                json.dumps({"kind": "event", "t_ms": 420_000, "event": "xp_gain"}),
                json.dumps({"kind": "sample", "t_ms": 421_000, "x": 10.0, "y": 10.0, "z": 0.0, "dead": False, "in_combat": False}),
            ]
            (bdir / "trace.jsonl").write_text("\n".join(lines) + "\n")
            res = sc.check_trace_dir(pathlib.Path(tmp), max_stall_s=300.0)
            self.assertEqual(res, {})


    def _still_bot(self, tmp: str, name: str, event: dict | None) -> dict:
        bdir = pathlib.Path(tmp) / "bot" / name
        bdir.mkdir(parents=True)
        lines = []
        for i in range(45):
            lines.append(json.dumps({"kind": "sample", "t_ms": i * 10_000, "x": 1.0, "y": 1.0,
                                     "z": 0.0, "dead": False, "in_combat": False}))
            if event and i in (15, 30):
                lines.append(json.dumps({"kind": "event", "t_ms": i * 10_000 + 500, **event}))
        (bdir / "trace.jsonl").write_text("\n".join(lines) + "\n")
        return sc.check_trace_dir(pathlib.Path(tmp), max_stall_s=300.0)

    def test_hub_work_is_progress(self) -> None:
        # Quests, vendors and trainers write option_chosen, not combat events.
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self._still_bot(tmp, "Hubbot", {"event": "option_chosen",
                                                              "detail": "npc"}), {})

    def test_buffing_or_resting_in_a_corner_is_a_stall(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("Buffbot", self._still_bot(tmp, "Buffbot", {"event": "spell_cast"}))
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("Restbot", self._still_bot(tmp, "Restbot", {"event": "option_chosen",
                                                                       "detail": "rest"}))

    def test_progress_events_are_ones_the_module_writes(self) -> None:
        header = (ROOT / "server_module" / "src" / "snapshot" / "behavior_trace.h").read_text()
        for ev in sc.PROGRESS_EVENTS:
            self.assertIn(f"`{ev}`", header, f"{ev} is not a trace event the module writes")


if __name__ == "__main__":
    unittest.main()
