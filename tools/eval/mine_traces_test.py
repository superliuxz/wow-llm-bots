#!/usr/bin/env python3
"""Unit tests for tools/eval/mine_traces.py."""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, List

from tools.eval import mine_traces as mt

REPO_ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT_CHECK = REPO_ROOT / "out" / "common" / "snapshot_check"


def _sample(t_ms: int, **overrides: Any) -> Dict[str, Any]:
    base: Dict[str, Any] = {
        "kind": "sample",
        "t_ms": t_ms,
        "mono_us": t_ms * 1000,
        "role": "bot",
        "name": "Aelindra",
        "map": 602,
        "inst": 1,
        "zone": "Halls of Lightning",
        "in_town": False,
        "in_group": True,
        "in_dungeon": True,
        "x": 100.0,
        "y": 200.0,
        "z": 50.0,
        "o": 0.0,
        "moving": False,
        "jumping": False,
        "mounted": False,
        "swimming": False,
        "sitting": False,
        "on_taxi": False,
        "in_combat": True,
        "dead": False,
        "cc_mask": 0,
        "bag_free_slots": 12,
        "pet_hp_pct": -1.0,
        "level": 80,
        "xp": 0,
        "hp_pct": 95.0,
        "mana_pct": 85.0,
        "casting": "",
        "target": "Loken",
        "target_dist": 6.0,
        "target_x": 106.0,
        "target_y": 200.0,
        "target_hp_pct": 62.0,
        "target_level": 82,
        "target_casting": "",
        "target_cast_radius": -1.0,
        "target_moving": False,
        "target_hostile": True,
        "target_targeting_me": False,
        "target_in_combat": True,
        "attackers": 1,
    }
    base.update(overrides)
    return base


def _event(t_ms: int, event: str, detail: str = "", value: int = 0) -> Dict[str, Any]:
    return {
        "kind": "event",
        "t_ms": t_ms,
        "mono_us": t_ms * 1000,
        "role": "bot",
        "name": "Aelindra",
        "event": event,
        "detail": detail,
        "value": value,
    }


class MineTracesTest(unittest.TestCase):
    def test_mine_missed_dodge(self) -> None:
        rows: List[Dict[str, Any]] = [
            _sample(1000, target_casting="Lightning Nova", target_cast_radius=20.0, target_dist=6.0),
            _sample(2000, target_casting="Lightning Nova", target_cast_radius=20.0, target_dist=6.0),
            _event(3000, "damage_taken", detail="Loken", value=14500),
            _sample(3200, hp_pct=30.0, target_casting="", target_cast_radius=-1.0),
        ]
        found = mt.mine_trace_rows(rows, source_file="bot/Aelindra/2026-11-21.jsonl")
        self.assertEqual(len(found), 1)
        f = found[0]
        self.assertEqual(f.kind, "missed_dodge")
        self.assertEqual(f.boss, "Loken")
        self.assertEqual(f.spell, "Lightning Nova")
        self.assertEqual(f.damage, 14500)
        self.assertAlmostEqual(f.radius_yd, 20.0)

    def test_dodge_act_suppresses_missed_dodge(self) -> None:
        rows: List[Dict[str, Any]] = [
            _sample(1000, target_casting="Lightning Nova", target_cast_radius=20.0, target_dist=6.0),
            _event(1400, "act_sent", detail="AWAY Loken", value=22),
            _event(3000, "damage_taken", detail="Loken", value=14500),
        ]
        found = mt.mine_trace_rows(rows, source_file="bot/Aelindra/2026-11-21.jsonl")
        self.assertEqual(found, [])

    def test_mine_death(self) -> None:
        rows: List[Dict[str, Any]] = [
            _sample(5000, hp_pct=18.0, target="Kael'thas Sunstrider", target_casting="Flame Strike", target_cast_radius=8.0),
            _event(5400, "died", detail="Kael'thas Sunstrider", value=72),
            _sample(5600, dead=True, hp_pct=0.0, in_combat=False),
        ]
        found = mt.mine_trace_rows(rows, source_file="bot/Aelindra/2026-11-21.jsonl")
        self.assertEqual(len(found), 1)
        f = found[0]
        self.assertEqual(f.kind, "death")
        self.assertEqual(f.killer, "Kael'thas Sunstrider")
        self.assertEqual(f.spell, "Flame Strike")
        self.assertAlmostEqual(f.sample["hp_pct"], 18.0)

    def test_mine_stall(self) -> None:
        rows: List[Dict[str, Any]] = [
            _sample(0, in_combat=False, moving=False, casting="", target="", target_dist=-1.0),
            _sample(60_000, in_combat=False, moving=False, casting="", target="", target_dist=-1.0),
            _sample(125_000, in_combat=False, moving=False, casting="", target="", target_dist=-1.0),
            _sample(130_000, in_combat=False, moving=True, casting="", target="", target_dist=-1.0),
        ]
        found = mt.mine_trace_rows(rows, source_file="bot/Aelindra/2026-11-21.jsonl", stall_threshold_s=120.0)
        self.assertEqual(len(found), 1)
        f = found[0]
        self.assertEqual(f.kind, "stall")
        self.assertGreaterEqual(f.stall_s, 120.0)

    def test_end_to_end_cli_and_snapshot_check(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            trace_file = tmp_path / "traces" / "bot" / "Aelindra" / "2026-11-21.jsonl"
            trace_file.parent.mkdir(parents=True, exist_ok=True)

            rows = [
                # 1) missed_dodge
                _sample(1000, target_casting="Lightning Nova", target_cast_radius=20.0, target_dist=5.0),
                _event(2500, "damage_taken", detail="Loken", value=12000),
                # 2) death
                _sample(5000, hp_pct=12.0, target="Loken", target_casting="Lightning Nova", target_cast_radius=20.0),
                _event(5200, "died", detail="Loken", value=82),
                # 3) stall
                _sample(10_000, in_combat=False, dead=False, moving=False, casting="", target="", target_dist=-1.0),
                _sample(140_000, in_combat=False, dead=False, moving=False, casting="", target="", target_dist=-1.0),
            ]
            trace_file.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")

            notes_file = tmp_path / "loken_notes.md"
            notes_file.write_text("Loken, Lightning Nova: run out 20 yd when cast.\n", encoding="utf-8")

            out_dir = tmp_path / "eval_out"
            rc = mt.main([
                "--trace-dir", str(tmp_path / "traces"),
                "--notes-file", str(notes_file),
                "--out-dir", str(out_dir),
                "--max-cases", "10",
            ])
            self.assertEqual(rc, 0)

            cases_path = out_dir / "cases.json"
            self.assertTrue(cases_path.is_file())
            cases = json.loads(cases_path.read_text(encoding="utf-8"))
            self.assertEqual(len(cases), 3)

            snap_paths: List[str] = []
            for entry in cases:
                for key in ("name", "source", "summary", "rule", "reason", "notes", "order_names"):
                    self.assertIn(key, entry)
                snap_p = out_dir / f"{entry['name']}.snapshot.json"
                facts_p = out_dir / f"{entry['name']}.facts.json"
                self.assertTrue(snap_p.is_file())
                self.assertTrue(facts_p.is_file())
                self.assertEqual(json.loads(facts_p.read_text(encoding="utf-8")), {})
                snap = json.loads(snap_p.read_text(encoding="utf-8"))
                self.assertIn("base", snap)
                self.assertIn("options", snap)
                snap_paths.append(str(snap_p))

            # Verify missed_dodge case predicates.
            md_case = next(c for c in cases if "missed_dodge" in c["name"])
            self.assertEqual(md_case["min_orders"], 1)
            self.assertIn("Lightning Nova", md_case["must_order_contain"])
            self.assertIn("Loken, Lightning Nova", md_case["notes"])

            # Verify stall case predicates.
            stall_case = next(c for c in cases if "stall" in c["name"])
            self.assertIn("explore", stall_case["first_kinds"])

            # If out/common/snapshot_check is built, validate every generated snapshot!
            if SNAPSHOT_CHECK.is_file():
                res = subprocess.run(
                    [str(SNAPSHOT_CHECK), *snap_paths],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    res.returncode,
                    0,
                    f"snapshot_check failed:\n{res.stdout}\n{res.stderr}",
                )


if __name__ == "__main__":
    unittest.main()
