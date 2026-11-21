#!/usr/bin/env python3
"""Unit tests for tools/notes/extract_boss_notes.py."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
from tools.notes import extract_boss_notes as ebn  # noqa: E402

PLAYERBOTS_AI = REPO_ROOT / "third_party" / "mod-playerbots" / "src" / "Ai"


class ExtractBossNotesTest(unittest.TestCase):
    def test_valid_order_syntax_checker(self) -> None:
        valid_orders = [
            "in Flame Strike -> out of it",
            "Lightning Nova cast -> out of it, then back",
            "Arcane Explosion cast -> out of it, then back",
            "Phoenix Egg appears -> target nearest Phoenix Egg",
            "Mirrored Soul on boss -> face away boss, then stop attacking",
            "Static Overload on me -> away group 10",
            "always -> use Tremor Totem",
        ]
        for o in valid_orders:
            self.assertTrue(ebn.is_valid_order(o), f"Expected valid order: {o}")

        invalid_orders = [
            "",
            "no arrow here",
            "in Flame Strike -> ",
            " -> out of it",
            "bogus syntax -> dance wildly",
        ]
        for o in invalid_orders:
            self.assertFalse(ebn.is_valid_order(o), f"Expected invalid order: {o}")

    def test_mgt_extraction(self) -> None:
        notes = ebn.extract_instance(PLAYERBOTS_AI / "Dungeon" / "MgT", "Dungeon")
        md = ebn.render_notes_markdown(notes)

        self.assertIn("Flame Strike", md)
        self.assertIn("Phoenix Egg", md)
        self.assertIn("in Flame Strike -> out of it", md)
        self.assertIn("Phoenix Egg appears -> target nearest Phoenix Egg", md)
        self.assertGreater(notes.triggers_parsed, 0)
        self.assertGreater(notes.triggers_mapped, 0)
        for m in notes.mechanics:
            if m.order:
                self.assertTrue(ebn.is_valid_order(m.order), f"Invalid MgT order: {m.order}")

    def test_hol_extraction(self) -> None:
        notes = ebn.extract_instance(PLAYERBOTS_AI / "Dungeon" / "HoL", "Dungeon")
        md = ebn.render_notes_markdown(notes)

        self.assertIn("Lightning Nova", md)
        self.assertIn("Lightning Nova cast -> out of it, then back", md)
        self.assertIn("General Bjarngrim", md)
        self.assertNotIn("General General Bjarngrim", md)
        for m in notes.mechanics:
            if m.order:
                self.assertTrue(ebn.is_valid_order(m.order), f"Invalid HoL order: {m.order}")

    def test_fos_extraction(self) -> None:
        notes = ebn.extract_instance(PLAYERBOTS_AI / "Dungeon" / "FoS", "Dungeon")
        md = ebn.render_notes_markdown(notes)

        self.assertIn("Mirrored Soul", md)
        self.assertIn("Mirrored Soul on boss -> face away boss, then stop attacking", md)
        self.assertIn("Corrupted Soul Fragment", md)
        self.assertNotIn("Devourer of Souls of Souls", md)
        for m in notes.mechanics:
            if m.order:
                self.assertTrue(ebn.is_valid_order(m.order), f"Invalid FoS order: {m.order}")

    def test_seth_extraction(self) -> None:
        notes = ebn.extract_instance(PLAYERBOTS_AI / "Dungeon" / "Seth", "Dungeon")
        md = ebn.render_notes_markdown(notes)

        self.assertIn("Arcane Explosion", md)
        self.assertIn("Arcane Explosion cast -> out of it, then back", md)
        self.assertIn("Talon King Ikiss", md)
        self.assertNotIn("Talon King Talon King Ikiss", md)
        for m in notes.mechanics:
            if m.order:
                self.assertTrue(ebn.is_valid_order(m.order), f"Invalid Seth order: {m.order}")

    def test_cli_out_and_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = ebn.main([
                    "--source", str(PLAYERBOTS_AI),
                    "--only", "MgT,HoL,FoS,Seth",
                    "--out", str(out_dir),
                    "--summary",
                ])
            self.assertEqual(rc, 0)
            summary = buf.getvalue()
            self.assertIn("Instances: 4", summary)
            self.assertIn("Triggers parsed:", summary)
            for code in ("MgT", "HoL", "FoS", "Seth"):
                p = out_dir / code / "notes.md"
                self.assertTrue(p.is_file(), f"Missing output file {p}")
                self.assertTrue(p.read_text(encoding="utf-8").strip())


if __name__ == "__main__":
    unittest.main()
