#!/usr/bin/env python3
"""Unit tests for tools/knowledge/export_knowledge.py (self-contained SQLite fixture)."""

from __future__ import annotations

import json
import pathlib
import sqlite3
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "knowledge"))
import export_knowledge as ek  # noqa: E402


def _make_fixture_dbs(tmp: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    atlas = tmp / "atlas.sqlite"
    wiki = tmp / "wiki.sqlite"

    con_a = sqlite3.connect(atlas)
    con_a.executescript(
        """
        CREATE TABLE quests (
            quest_id INTEGER PRIMARY KEY, title TEXT, quest_level INTEGER,
            min_level INTEGER, log_description TEXT, wiki_page_id INTEGER
        );
        CREATE TABLE quest_relations (
            quest_id INTEGER, role TEXT, kind TEXT, entry INTEGER, name TEXT
        );
        CREATE TABLE quest_objectives (
            quest_id INTEGER, obj_index INTEGER, kind TEXT, target_entry INTEGER,
            target_name TEXT, req_count INTEGER, objective_text TEXT
        );
        CREATE TABLE item_drops (
            item_entry INTEGER, source_kind TEXT, source_entry INTEGER,
            source_name TEXT, chance REAL, quest_required INTEGER
        );
        CREATE TABLE creatures (
            entry INTEGER PRIMARY KEY, name TEXT, rank INTEGER
        );
        INSERT INTO creatures VALUES
            (28923, 'Loken', 1),
            (36597, 'The Lich King', 3),
            (10184, 'Onyxia', 3),
            (19622, 'Kael''thas Sunstrider', 3),
            (11502, 'Ragnaros', 3),
            (40, 'Kobold Miner', 0);
        INSERT INTO quests VALUES
            (7, 'Kobold Camp Cleanup', 2, 1, 'Kill 8 Kobold Vermin.$bReturn to Marshal McBride, $N $g fella:lady;.', 101);
        INSERT INTO quests VALUES
            (60, 'Kobold Candles', 7, 4, 'Collect 8 Large Candles.', 102);
        INSERT INTO quest_relations VALUES
            (7, 'starter', 'creature', 197, 'Marshal McBride'),
            (7, 'ender', 'creature', 197, 'Marshal McBride'),
            (60, 'ender', 'creature', 253, 'William Pestle');
        INSERT INTO quest_objectives VALUES
            (60, 0, 'item', 772, 'Large Candle', 8, '');
        INSERT INTO item_drops VALUES
            (772, 'creature', 40, 'Kobold Miner', 80.0, 1),
            (772, 'creature', 475, 'Kobold Tunneler', 60.0, 1);
        """
    )
    con_a.commit()
    con_a.close()

    con_w = sqlite3.connect(wiki)
    con_w.executescript(
        """
        CREATE TABLE pages (
            page_id INTEGER PRIMARY KEY, title TEXT UNIQUE, redirect TEXT,
            categories TEXT, wikitext TEXT
        );
        INSERT INTO pages VALUES (
            101, 'Kobold Camp Cleanup', '', '',
            'Intro text.\n==Notes==\n* [[Kobold Vermin]] are north of [[Northshire Abbey|the abbey]].{{fact}}\n==Rewards==\nXP.'
        );
        INSERT INTO pages VALUES (
            201, 'Elwynn Forest', '', '',
            '{{Infobox zone |name=Elwynn Forest |faction=Alliance |level=1-10}} Elwynn Forest is the [[Human]] starting zone.'
        );
        INSERT INTO pages VALUES (
            202, 'Westfall (Classic)', '', '',
            '{{Infobox zone |name=Westfall |faction=Alliance |level=10-20}} Classic Westfall.'
        );
        INSERT INTO pages VALUES (
            203, 'Westfall', '', '',
            '{{Infobox zone |name=Westfall |faction=Alliance |level=10-15}} Post-Cataclysm Westfall.'
        );
        INSERT INTO pages VALUES (
            204, 'Durotar', '', '',
            '{{Infobox zone |name=Durotar |faction=Horde |level=1-10}} Horde starting zone.'
        );
        INSERT INTO pages VALUES (
            207, 'The Barrens', '', '',
            '{{Infobox zone |name=The Barrens |faction=Horde |level=7-30}} Horde leveling zone.'
        );
        INSERT INTO pages VALUES (
            205, 'Northern Barrens', '', '',
            '{{Infobox zone |name=Northern Barrens |faction=Horde |level=10-20}} Cataclysm zone.'
        );
        INSERT INTO pages VALUES (
            206, 'Badlands (Classic)', '', '',
            '{{Infobox zone |name=The Badlands |faction=Contested |level=35-45}} Badlands.'
        );
        INSERT INTO pages VALUES (
            301, 'Deadmines', '', '',
            '{{Infobox instance |name=Deadmines |level=17-26}} ==Strategy== Pull patrols carefully and watch for [[Defias]] adds &aring;.'
        );
        INSERT INTO pages VALUES (
            302, 'The Deadmines', 'Deadmines', '', '#REDIRECT [[Deadmines]]'
        );
        INSERT INTO pages VALUES (
            303, 'Shadowfang Keep (Classic)', '', '',
            '{{Infobox instance |name=Shadowfang Keep |level=18-24}} Classic SFK.'
        );
        INSERT INTO pages VALUES (
            304, 'Shadowfang Keep', '', '',
            '{{Infobox instance |name=Shadowfang Keep |level=18-24 / 85}} Cataclysm SFK.'
        );
        INSERT INTO pages VALUES (
            305, 'Firelands', '', '',
            '{{Infobox instance |name=Firelands |level=85}} Cataclysm raid.'
        );
        INSERT INTO pages VALUES (
            306, 'Naxxramas (Classic)', '', '',
            '{{removedfromgame|patch=3.0.2}} {{Infobox instance |name=Naxxramas |level=60}} Vanilla 40-man.'
        );
        INSERT INTO pages VALUES (
            307, 'Naxxramas', '', '',
            '{{Infobox instance |name=Naxxramas |level=80}} === Wings === WotLK level-80 raid.'
        );
        INSERT INTO pages VALUES (
            308, 'Slave Pens', '', '',
            '{{Infobox instance |name=The Slave Pens |level=62-64}} Coilfang Reservoir wing.'
        );
        INSERT INTO pages VALUES (
            309, 'Stormwind Stockade', '', '',
            '{{Infobox instance |name=Stormwind Stockade |level=22-30}} Prison wing in Stormwind.'
        );
        INSERT INTO pages VALUES (
            401, 'Loken', '', '',
            'Biography page.\n==Strategy==\nOld stub.'
        );
        INSERT INTO pages VALUES (
            402, 'Loken (tactics)', '', '',
            '==Abilities==\n*{{abilities|Lightning Nova|20 yd AoE Nature damage.|id=52960|notes=5 sec cast}}\n==Tactics==\nRun 20 yd away when he casts {{spell|52960|Lightning Nova}}.'
        );
        INSERT INTO pages VALUES (
            403, 'The Lich King', 'Lich King', '', '#REDIRECT [[Lich King]]'
        );
        INSERT INTO pages VALUES (
            404, 'Lich King (tactics)', '', '',
            '==Abilities==\n*{{abilities|Defile|Growing shadow void zone.}}\n==Tactics==\nSpread out for Defile.'
        );
        INSERT INTO pages VALUES (
            405, 'Onyxia (Classic)', '', '',
            '{{removedfromgame|patch=3.2.2}}\n==Strategy==\nLevel 60 vanilla encounter.'
        );
        INSERT INTO pages VALUES (
            406, 'Onyxia (tactics)', '', '',
            '==Abilities==\n*Deep Breath\n==Strategy==\nWotLK level 80 encounter.'
        );
        INSERT INTO pages VALUES (
            407, 'Kael''thas Sunstrider (tactics)', '', '',
            '==Strategy==\nTempest Keep five-phase raid.'
        );
        INSERT INTO pages VALUES (
            408, 'Kael''thas Sunstrider (Magisters'' Terrace tactics)', '', '',
            '==Abilities==\n*{{abilities|Shock Barrier|Absorbs damage.}}\n==Strategy==\nBreak Shock Barrier and interrupt Pyroblast.'
        );
        INSERT INTO pages VALUES (
            409, 'Ragnaros (Firelands tactics)', '', '',
            '==Strategy==\nCataclysm Firelands raid.'
        );
        INSERT INTO pages VALUES (
            410, 'Deathwing (tactics)', '', '',
            '==Strategy==\nCataclysm-only boss.'
        );
        """
    )
    con_w.commit()
    con_w.close()
    return atlas, wiki


class ExportKnowledgeTest(unittest.TestCase):
    def test_strip_wikitext_cleans_markup(self) -> None:
        raw = "{{Template|a={{nested}}}} '''Hello''' [[Target|World]]<ref>cite</ref> [[File:Foo.png]]"
        self.assertEqual(ek.strip_wikitext(raw), "Hello World")
        ab = "*{{abilities|Lightning Nova|20 yd AoE.|id=52960|notes=5 sec cast}} and {{spell|52960|Lightning Nova}}"
        cleaned = ek.strip_wikitext(ab, max_chars=200)
        self.assertIn("Lightning Nova: 20 yd AoE. (5 sec cast)", cleaned)
        self.assertIn("and Lightning Nova", cleaned)

    def test_export_335a_era_pinning_and_quest_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp)
            atlas, wiki = _make_fixture_dbs(t)
            out = t / "world_knowledge.json"
            ek.export_knowledge(atlas, wiki, out)
            data = json.loads(out.read_text(encoding="utf-8"))

            self.assertEqual(data["version"], 1)
            q7 = data["quests"]["7"]
            self.assertEqual(q7["title"], "Kobold Camp Cleanup")
            self.assertEqual(q7["ender"], "Marshal McBride")
            self.assertNotIn("$b", q7["log"])
            self.assertNotIn("$N", q7["log"])
            self.assertNotIn("$g", q7["log"])
            self.assertIn("fella", q7["log"])
            self.assertIn("Kobold Vermin are north of the abbey.", q7["tip"])

            q60 = data["quests"]["60"]
            self.assertEqual(q60["drops"], ["Kobold Miner", "Kobold Tunneler"])

            # Era pinning: Cataclysm zone excluded; Classic Westfall (10-20) wins
            # over post-Cata Westfall (10-15); Badlands normalized; Durotar recommends The Barrens (7-30).
            self.assertNotIn("Northern Barrens", data["zones"])
            self.assertNotIn("Westfall (Classic)", data["zones"])
            self.assertEqual(data["zones"]["Westfall"]["max_level"], 20)
            self.assertIn("Badlands", data["zones"])
            self.assertEqual(data["zones"]["Badlands"]["min_level"], 35)
            self.assertIn("The Barrens (7-30)", data["zones"]["Durotar"]["next_zones"])

            elwynn = data["zones"]["Elwynn Forest"]
            self.assertEqual(elwynn["next_zones"], ["Westfall (10-20)"])

            # Dungeon era pinning: Cataclysm Firelands excluded, Classic SFK wins, HTML entity unescaped.
            self.assertNotIn("Firelands", data["dungeons"])
            self.assertNotIn("Shadowfang Keep (Classic)", data["dungeons"])
            self.assertEqual(data["dungeons"]["Shadowfang Keep"]["level"], "18-24")
            self.assertIn("Deadmines", data["dungeons"])
            self.assertIn("The Deadmines", data["dungeons"])
            self.assertIn("Pull patrols carefully", data["dungeons"]["The Deadmines"]["summary"])
            self.assertNotIn("&aring;", data["dungeons"]["Deadmines"]["summary"])
            # Pre-4.0 removed `(Classic)` raid (Naxxramas 3.0.2) must NOT overwrite level-80 WotLK Naxxramas.
            self.assertEqual(data["dungeons"]["Naxxramas"]["level"], "80")
            self.assertNotIn("===", data["dungeons"]["Naxxramas"]["summary"])
            self.assertIn("The Slave Pens", data["dungeons"])
            # Alias maps "Stormwind Stockade" -> "The Stockade", and the short/long
            # mirror fallback populates "Stockade" from "The Stockade".
            self.assertIn("The Stockade", data["dungeons"])
            self.assertIn("Stockade", data["dungeons"])
            self.assertIn("Prison wing in Stormwind.", data["dungeons"]["Stockade"]["summary"])

            # Boss strategy export: (tactics) beats biography, redirect resolves
            # The Lich King -> Lich King (tactics), pre-4.0 Onyxia (Classic) skipped,
            # 3.3.5a multi-instance tactics scoped, Cataclysm bosses/instances excluded.
            bosses = data["bosses"]
            self.assertIn("Loken", bosses)
            self.assertIn("Lightning Nova: 20 yd AoE Nature damage. (5 sec cast)", bosses["Loken"]["abilities"])
            self.assertIn("Run 20 yd away when he casts Lightning Nova.", bosses["Loken"]["strategy"])
            self.assertIn("The Lich King", bosses)
            self.assertIn("Spread out for Defile.", bosses["The Lich King"]["strategy"])
            self.assertIn("Onyxia", bosses)
            self.assertIn("WotLK level 80", bosses["Onyxia"]["strategy"])
            self.assertNotIn("Level 60 vanilla", bosses["Onyxia"]["strategy"])
            self.assertIn("Kael'thas Sunstrider", bosses)
            self.assertIn("Tempest Keep", bosses["Kael'thas Sunstrider"]["strategy"])
            self.assertIn("Magisters' Terrace::Kael'thas Sunstrider", bosses)
            self.assertIn("Shock Barrier", bosses["Magisters' Terrace::Kael'thas Sunstrider"]["abilities"])
            self.assertNotIn("Deathwing", bosses)
            self.assertFalse(any("Firelands" in k for k in bosses))


if __name__ == "__main__":
    unittest.main()
