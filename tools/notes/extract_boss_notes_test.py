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
HAS_PLAYERBOTS = (PLAYERBOTS_AI / "Dungeon" / "MgT").is_dir()


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

    def test_synthetic_fixture_extraction_and_cli(self) -> None:
        """Self-contained extraction test that runs even without mod-playerbots checked out."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mgt = root / "Dungeon" / "MgT"
            mgt.mkdir(parents=True)
            (mgt / "MgTShared.h").write_text(
                'enum MgTNpcs {\n'
                '    NPC_FLAME_STRIKE = 24666,\n'
                '    NPC_PHOENIX_EGG = 24675,\n'
                '};\n',
                encoding="utf-8",
            )
            (mgt / "MgTTriggers.h").write_text(
                'class MgTKaelFlameStrikeTrigger : public Trigger {\n'
                'public:\n'
                '    MgTKaelFlameStrikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt kael flame strike") {}\n'
                '    bool IsActive() override;\n'
                '};\n'
                'class MgTKaelPhoenixEggTrigger : public Trigger {\n'
                'public:\n'
                '    MgTKaelPhoenixEggTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt kael phoenix egg") {}\n'
                '    bool IsActive() override;\n'
                '};\n',
                encoding="utf-8",
            )
            (mgt / "MgTTriggers.cpp").write_text(
                'bool MgTKaelFlameStrikeTrigger::IsActive() {\n'
                '    return FindNearestCreature(NPC_FLAME_STRIKE, 7.5f) != nullptr;\n'
                '}\n'
                'bool MgTKaelPhoenixEggTrigger::IsActive() {\n'
                '    return FindNearestCreature(NPC_PHOENIX_EGG, 40.0f) != nullptr;\n'
                '}\n',
                encoding="utf-8",
            )
            (mgt / "MgTStrategy.cpp").write_text(
                'void MgTStrategy::InitTriggers(std::vector<TriggerNode*>& triggers) {\n'
                '    triggers.push_back(new TriggerNode("mgt kael flame strike",\n'
                '        NextAction::array(0, new NextAction("mgt avoid flame strike", 90.0f), nullptr)));\n'
                '    triggers.push_back(new TriggerNode("mgt kael phoenix egg",\n'
                '        NextAction::array(0, new NextAction("mgt attack phoenix egg", 90.0f), nullptr)));\n'
                '}\n',
                encoding="utf-8",
            )
            notes = ebn.extract_instance(mgt, "Dungeon")
            md = ebn.render_notes_markdown(notes)
            self.assertIn("Magisters' Terrace", md)
            self.assertIn("Flame Strike", md)
            self.assertIn("Phoenix Egg", md)
            self.assertIn("Phoenix Egg appears -> target nearest Phoenix Egg", md)
            self.assertGreater(notes.triggers_parsed, 0)
            for m in notes.mechanics:
                if m.order:
                    self.assertTrue(ebn.is_valid_order(m.order), f"Invalid order: {m.order}")

            out_dir = root / "out"
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = ebn.main([
                    "--source", str(root),
                    "--only", "MgT",
                    "--out", str(out_dir),
                    "--summary",
                ])
            self.assertEqual(rc, 0)
            self.assertIn("Instances: 1", buf.getvalue())
            self.assertTrue((out_dir / "MgT" / "notes.md").is_file())

    @unittest.skipUnless(HAS_PLAYERBOTS, "third_party/mod-playerbots submodule not checked out")
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

    @unittest.skipUnless(HAS_PLAYERBOTS, "third_party/mod-playerbots submodule not checked out")
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

    @unittest.skipUnless(HAS_PLAYERBOTS, "third_party/mod-playerbots submodule not checked out")
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

    @unittest.skipUnless(HAS_PLAYERBOTS, "third_party/mod-playerbots submodule not checked out")
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

    @unittest.skipUnless(HAS_PLAYERBOTS, "third_party/mod-playerbots submodule not checked out")
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
