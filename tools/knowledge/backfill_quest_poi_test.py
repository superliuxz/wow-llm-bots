#!/usr/bin/env -S python3 -I
"""Unit tests for tools/knowledge/backfill_quest_poi.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import backfill_quest_poi as bqp  # noqa: E402


class BackfillQuestPoiTest(unittest.TestCase):
    def test_medoid_picks_real_spawn_not_empty_midpoint(self) -> None:
        # Two spawns at (0, 0) and (1000, 0) plus one at (10, 0):
        # median is (10, 0), so medoid must pick (10, 0), never (337, 0).
        self.assertEqual(bqp.medoid_point([(0.0, 0.0), (10.0, 0.0), (1000.0, 0.0)]), (10, 0))

    def test_reference_loot_resolves_multi_hop(self) -> None:
        ref = bqp.resolve_reference_loot([
            (10, 0, 20),
            (20, 500, 0),
            (20, 501, 0),
        ])
        self.assertEqual(ref[10], {500, 501})
        self.assertEqual(ref[20], {500, 501})

    def test_compute_backfill_covers_turnin_killcredit_go_and_item_loot(self) -> None:
        quests = [
            {
                "quest_id": 100,
                "npc_go_objectives": [(301, 5), (-401, 1), (0, 0), (0, 0)],
                "item_objectives": [(900, 3), (0, 0), (0, 0), (0, 0), (0, 0), (0, 0)],
            },
            {
                "quest_id": 101,
                "npc_go_objectives": [(301, 1), (0, 0), (0, 0), (0, 0)],
                "item_objectives": [(0, 0)] * 6,
            },
        ]
        # Quest 101 already has both turn-in (-1) and obj 0 upstream.
        existing = {(101, -1), (101, 0)}
        ref_items = bqp.resolve_reference_loot([(50, 900, 0)])
        item_c, item_g = bqp.build_item_sources(
            creature_loot_rows=[(700, 0, 50)],
            go_loot_rows=[],
            ref_items=ref_items,
            lootid_to_creatures={700: {305}},
            lootid_to_gos={},
        )
        rows, stats = bqp.compute_backfill_rows(
            quests=quests,
            existing_poi_keys=existing,
            quest_map_area={(100, 1): 14},
            map_area_samples={0: [(100, 200, 30)]},
            creature_spawns={
                302: {1: [(-500.0, -4000.0)]},  # KillCredit for 301
                305: {1: [(-510.0, -4010.0)]},  # Drops item 900 via ref 50
                999: {0: [(110.0, 210.0)]},     # Quest ender
            },
            go_spawns={
                401: {1: [(-520.0, -4020.0)]},
            },
            kill_credit_to_creatures={301: {302}},
            quest_creature_enders={100: {999}},
            quest_go_enders={},
            item_creatures=item_c,
            item_gos=item_g,
        )
        self.assertEqual(stats["turnins_added"], 1)
        self.assertEqual(stats["npc_go_objectives_added"], 2)
        self.assertEqual(stats["item_objectives_added"], 1)
        self.assertEqual(len(rows), 4)
        # Quest 101 must not be duplicated.
        self.assertTrue(all(r.quest_id == 100 for r in rows))
        by_obj = {r.objective_index: r for r in rows}
        self.assertEqual((by_obj[-1].map_id, by_obj[-1].world_map_area_id), (0, 30))
        self.assertEqual((by_obj[0].x, by_obj[0].y, by_obj[0].world_map_area_id), (-500, -4000, 14))
        self.assertEqual((by_obj[1].x, by_obj[1].y), (-520, -4020))
        self.assertEqual((by_obj[4].x, by_obj[4].y), (-510, -4010))

        sql = bqp.render_sql(rows)
        self.assertIn("DELETE FROM `quest_poi` WHERE `id` >= 1000;", sql)
        self.assertIn("INSERT INTO `quest_poi`", sql)
        self.assertIn("INSERT INTO `quest_poi_points`", sql)


if __name__ == "__main__":
    unittest.main()
