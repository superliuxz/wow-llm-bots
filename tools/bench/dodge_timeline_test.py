#!/usr/bin/env python3
"""Unit tests for dodge_timeline, boss_meter impact-dodge, and bench_bosses._TraceTailer."""

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))

from tools.bench import bench_bosses, boss_meter, dodge_timeline  # noqa: E402


class DodgeTimelineAndMeterTest(unittest.TestCase):
    def test_dbc_radius_origin_distance_and_dodge_act_filter(self) -> None:
        traces = {
            "BotA": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 5.0,
                },
                {"kind": "event", "t_ms": 1100, "event": "act_sent", "detail": "TARGET Talon King Ikiss"},
                {"kind": "event", "t_ms": 1250, "event": "act_sent", "detail": "AWAY 40"},
                {
                    "kind": "sample", "t_ms": 2000, "in_combat": True,
                    "x": 120.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 20.0,
                },
                {
                    "kind": "sample", "t_ms": 3000, "in_combat": True,
                    "x": 140.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 40.0,
                },
            ],
            # BotB is healing/not targeting the boss, so target_casting="" and target_dist=-1.
            # Distance must still be measured from the cast origin (100, 100) -> 5.0 yd.
            "BotB": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True,
                    "x": 103.0, "y": 104.0, "moving": False,
                    "target_casting": "", "target_dist": -1.0,
                },
                {
                    "kind": "sample", "t_ms": 3000, "in_combat": True,
                    "x": 103.0, "y": 104.0, "moving": False,
                    "target_casting": "", "target_dist": -1.0,
                },
            ],
        }

        res = dodge_timeline.extract_dodge_timelines(traces, spell_filter="Arcane Explosion")
        self.assertEqual(len(res["windows"]), 1)
        w = res["windows"][0]
        self.assertEqual(w["radius_yd"], 34.0)
        self.assertEqual(w["origin"], [100.0, 100.0])

        a = w["bots"]["BotA"]
        self.assertTrue(a["outside_at_impact"])
        self.assertEqual(a["end_yd"], 40.0)
        self.assertEqual(a["act_delay_ms"], 250)  # Skipped TARGET at 1100, matched AWAY at 1250.
        self.assertEqual(a["act_detail"], "AWAY 40")

        b = w["bots"]["BotB"]
        self.assertEqual(b["end_yd"], 5.0)
        self.assertFalse(b["outside_at_impact"])

    def test_boss_meter_uses_impact_distance_not_max(self) -> None:
        # Bot runs to 40 yd mid-cast, then returns to 20 yd before impact -> NOT outside at impact.
        traces = {
            "BotA": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_dist": 40.0,
                },
                {
                    "kind": "sample", "t_ms": 2000, "in_combat": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_dist": 20.0,
                },
                {
                    "kind": "sample", "t_ms": 2500, "in_combat": True,
                    "target_casting": "", "target_dist": 20.0,
                },
            ],
        }
        res = boss_meter.analyze_boss_fight(traces, boss_name="Talon King Ikiss")
        c = res["characters"]["BotA"]
        self.assertEqual(c["impact_dist_yd"], 20.0)
        self.assertEqual(c["outside_at_impact"], "0/1")

    def test_trace_tailer_incremental_and_partial_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            bdir = root / "bot" / "BotA"
            bdir.mkdir(parents=True)
            f = bdir / "trace.jsonl"
            f.write_text(json.dumps({"t_ms": 100, "kind": "sample"}) + "\n", encoding="utf-8")

            tailer = bench_bosses._TraceTailer(root, ["BotA"])
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 1)

            # Append an incomplete line without newline: should not be dropped or consumed.
            with f.open("a", encoding="utf-8") as fp:
                fp.write('{"t_ms": 200, "kind": ')
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 1)

            # Finish the line: should now be parsed on the next poll.
            with f.open("a", encoding="utf-8") as fp:
                fp.write('"event"}\n')
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 2)
            self.assertEqual(rows["BotA"][1]["t_ms"], 200)


if __name__ == "__main__":
    unittest.main()
