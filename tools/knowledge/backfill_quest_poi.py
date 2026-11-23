#!/usr/bin/env python3
"""Backfills missing AzerothCore `quest_poi` and `quest_poi_points` rows from
`acore_world`'s own creature/gameobject spawns, quest enders, and loot tables.

Generates an idempotent module SQL file at:
  server_module/data/sql/db-world/quest_poi_backfill.sql

All backfilled rows use `id >= 1000` (`Idx1 >= 1000`) so they never collide with
upstream Blizzard/AzerothCore `quest_poi` rows and can be regenerated cleanly.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import pathlib
import re
import statistics
import subprocess
import sys
from typing import Iterable

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_MYSQL_CNF = ROOT / "out" / "server" / "etc" / "mysql.cnf"
DEFAULT_OUT_SQL = ROOT / "server_module" / "data" / "sql" / "db-world" / "quest_poi_backfill.sql"
BACKFILL_ID_START = 1000
MAX_MAPS_PER_OBJECTIVE = 3


@dataclass(frozen=True)
class PoiRow:
    quest_id: int
    poi_id: int
    objective_index: int
    map_id: int
    world_map_area_id: int
    x: int
    y: int


def medoid_point(points: list[tuple[float, float]]) -> tuple[int, int]:
    """Returns the actual spawn point closest to `(median_x, median_y)`.

    Using a real spawn medoid instead of an arithmetic mean avoids placing a POI
    in an unwalkable mountain/ocean between split spawn clusters.
    """
    if len(points) == 1:
        return round(points[0][0]), round(points[0][1])
    mx = statistics.median(p[0] for p in points)
    my = statistics.median(p[1] for p in points)
    best = min(points, key=lambda p: (p[0] - mx) ** 2 + (p[1] - my) ** 2)
    return round(best[0]), round(best[1])


def resolve_reference_loot(
    ref_rows: Iterable[tuple[int, int, int]],
    max_depth: int = 3,
) -> dict[int, set[int]]:
    """Maps `reference_loot_template.Entry` -> set of dropped `Item` IDs."""
    direct_items: dict[int, set[int]] = defaultdict(set)
    sub_refs: dict[int, set[int]] = defaultdict(set)
    for entry, item, ref in ref_rows:
        if ref > 0:
            sub_refs[entry].add(ref)
        elif item > 0:
            direct_items[entry].add(item)

    resolved: dict[int, set[int]] = {k: set(v) for k, v in direct_items.items()}
    for entry in set(direct_items.keys()) | set(sub_refs.keys()):
        resolved.setdefault(entry, set())
    for _ in range(max_depth):
        changed = False
        for entry, refs in sub_refs.items():
            before = len(resolved[entry])
            for r in refs:
                if r in resolved:
                    resolved[entry].update(resolved[r])
            if len(resolved[entry]) > before:
                changed = True
        if not changed:
            break
    return resolved


def build_item_sources(
    creature_loot_rows: Iterable[tuple[int, int, int]],
    go_loot_rows: Iterable[tuple[int, int, int]],
    ref_items: dict[int, set[int]],
    lootid_to_creatures: dict[int, set[int]],
    lootid_to_gos: dict[int, set[int]],
) -> tuple[dict[int, set[int]], dict[int, set[int]]]:
    """Maps `item_id` -> `(creature_entries, go_entries)` that can drop it."""
    item_creatures: dict[int, set[int]] = defaultdict(set)
    item_gos: dict[int, set[int]] = defaultdict(set)

    for loot_entry, item, ref in creature_loot_rows:
        creatures = lootid_to_creatures.get(loot_entry)
        if not creatures:
            continue
        if ref > 0:
            for it in ref_items.get(ref, ()):
                item_creatures[it].update(creatures)
        elif item > 0:
            item_creatures[item].update(creatures)

    for loot_entry, item, ref in go_loot_rows:
        gos = lootid_to_gos.get(loot_entry)
        if not gos:
            continue
        if ref > 0:
            for it in ref_items.get(ref, ()):
                item_gos[it].update(gos)
        elif item > 0:
            item_gos[item].update(gos)

    return item_creatures, item_gos


def compute_backfill_rows(
    *,
    quests: list[dict],
    existing_poi_keys: set[tuple[int, int]],
    quest_map_area: dict[tuple[int, int], int],
    map_area_samples: dict[int, list[tuple[int, int, int]]],
    creature_spawns: dict[int, dict[int, list[tuple[float, float]]]],
    go_spawns: dict[int, dict[int, list[tuple[float, float]]]],
    kill_credit_to_creatures: dict[int, set[int]],
    quest_creature_enders: dict[int, set[int]],
    quest_go_enders: dict[int, set[int]],
    item_creatures: dict[int, set[int]],
    item_gos: dict[int, set[int]],
) -> tuple[list[PoiRow], dict[str, int]]:
    """Computes missing `(quest_poi, quest_poi_points)` rows deterministically."""
    stats = {
        "quests_total": len(quests),
        "turnins_added": 0,
        "npc_go_objectives_added": 0,
        "item_objectives_added": 0,
        "rows_added": 0,
    }
    rows: list[PoiRow] = []

    def lookup_world_map_area(quest_id: int, map_id: int, x: int, y: int) -> int:
        if (quest_id, map_id) in quest_map_area:
            return quest_map_area[(quest_id, map_id)]
        samples = map_area_samples.get(map_id)
        if not samples:
            return 0
        best = min(samples, key=lambda s: (s[0] - x) ** 2 + (s[1] - y) ** 2)
        return best[2]

    def gather_spawns(
        c_entries: Iterable[int], g_entries: Iterable[int]
    ) -> dict[int, list[tuple[float, float]]]:
        by_map: dict[int, list[tuple[float, float]]] = defaultdict(list)
        for cid in c_entries:
            for m, pts in creature_spawns.get(cid, {}).items():
                by_map[m].extend(pts)
        for gid in g_entries:
            for m, pts in go_spawns.get(gid, {}).items():
                by_map[m].extend(pts)
        return by_map

    def emit_for_objective(
        quest_id: int,
        next_id: int,
        obj_idx: int,
        by_map: dict[int, list[tuple[float, float]]],
        stat_key: str,
    ) -> int:
        if not by_map:
            return next_id
        # Prefer maps with the most spawns (tie-break by map_id for determinism).
        ranked_maps = sorted(
            by_map.items(), key=lambda kv: (-len(kv[1]), kv[0])
        )[:MAX_MAPS_PER_OBJECTIVE]
        added_any = False
        for map_id, pts in ranked_maps:
            x, y = medoid_point(pts)
            wma = lookup_world_map_area(quest_id, map_id, x, y)
            rows.append(PoiRow(quest_id, next_id, obj_idx, map_id, wma, x, y))
            next_id += 1
            stats["rows_added"] += 1
            added_any = True
        if added_any:
            stats[stat_key] += 1
        return next_id

    for q in sorted(quests, key=lambda r: r["quest_id"]):
        qid = q["quest_id"]
        next_id = BACKFILL_ID_START

        # 1) Turn-in (-1)
        if (qid, -1) not in existing_poi_keys:
            by_map = gather_spawns(
                quest_creature_enders.get(qid, ()),
                quest_go_enders.get(qid, ()),
            )
            next_id = emit_for_objective(qid, next_id, -1, by_map, "turnins_added")

        # 2) Creature / GameObject objectives (0..3)
        for idx, (npc_or_go, count) in enumerate(q["npc_go_objectives"]):
            if npc_or_go == 0 or count == 0 or (qid, idx) in existing_poi_keys:
                continue
            if npc_or_go > 0:
                c_entries = {npc_or_go} | kill_credit_to_creatures.get(npc_or_go, set())
                by_map = gather_spawns(c_entries, ())
            else:
                by_map = gather_spawns((), (-npc_or_go,))
            next_id = emit_for_objective(
                qid, next_id, idx, by_map, "npc_go_objectives_added"
            )

        # 3) Item objectives (4..9)
        for idx, (item_id, count) in enumerate(q["item_objectives"]):
            obj_idx = 4 + idx
            if item_id == 0 or count == 0 or (qid, obj_idx) in existing_poi_keys:
                continue
            by_map = gather_spawns(
                item_creatures.get(item_id, ()),
                item_gos.get(item_id, ()),
            )
            next_id = emit_for_objective(
                qid, next_id, obj_idx, by_map, "item_objectives_added"
            )

    return rows, stats


def render_sql(rows: list[PoiRow]) -> str:
    """Renders an idempotent AzerothCore world SQL file for the backfilled POIs."""
    lines = [
        "-- Auto-generated by tools/knowledge/backfill_quest_poi.py from acore_world",
        "-- creature/gameobject spawns, quest enders, and loot tables.",
        f"-- Idempotent: all backfilled rows use id/Idx1 >= {BACKFILL_ID_START}.",
        "",
        f"DELETE FROM `quest_poi_points` WHERE `Idx1` >= {BACKFILL_ID_START};",
        f"DELETE FROM `quest_poi` WHERE `id` >= {BACKFILL_ID_START};",
        "",
    ]
    if not rows:
        return "\n".join(lines)

    batch_size = 500
    for i in range(0, len(rows), batch_size):
        chunk = rows[i : i + batch_size]
        lines.append(
            "INSERT INTO `quest_poi` (`QuestID`,`id`,`ObjectiveIndex`,`MapID`,"
            "`WorldMapAreaId`,`Floor`,`Priority`,`Flags`,`VerifiedBuild`) VALUES"
        )
        vals = [
            f"({r.quest_id},{r.poi_id},{r.objective_index},{r.map_id},"
            f"{r.world_map_area_id},0,0,1,0)"
            for r in chunk
        ]
        lines.append(",\n".join(vals) + ";")
        lines.append("")

    for i in range(0, len(rows), batch_size):
        chunk = rows[i : i + batch_size]
        lines.append(
            "INSERT INTO `quest_poi_points` (`QuestID`,`Idx1`,`Idx2`,`X`,`Y`,`VerifiedBuild`) VALUES"
        )
        vals = [
            f"({r.quest_id},{r.poi_id},0,{r.x},{r.y},0)"
            for r in chunk
        ]
        lines.append(",\n".join(vals) + ";")
        lines.append("")

    return "\n".join(lines)


def _mysql_query(mysql_cnf: pathlib.Path, db: str, sql: str) -> list[list[str]]:
    res = subprocess.run(
        ["mysql", f"--defaults-file={mysql_cnf}", "-uroot", "-N", "-B", db, "-e", sql],
        check=True,
        capture_output=True,
        text=True,
    )
    out: list[list[str]] = []
    for line in res.stdout.splitlines():
        if line:
            out.append(line.split("\t"))
    return out


def generate_from_mysql(
    mysql_cnf: pathlib.Path,
    db: str = "acore_world",
) -> tuple[list[PoiRow], dict[str, int]]:
    quests: list[dict] = []
    for r in _mysql_query(
        mysql_cnf,
        db,
        "SELECT ID,"
        " RequiredNpcOrGo1, RequiredNpcOrGoCount1,"
        " RequiredNpcOrGo2, RequiredNpcOrGoCount2,"
        " RequiredNpcOrGo3, RequiredNpcOrGoCount3,"
        " RequiredNpcOrGo4, RequiredNpcOrGoCount4,"
        " RequiredItemId1, RequiredItemCount1,"
        " RequiredItemId2, RequiredItemCount2,"
        " RequiredItemId3, RequiredItemCount3,"
        " RequiredItemId4, RequiredItemCount4,"
        " RequiredItemId5, RequiredItemCount5,"
        " RequiredItemId6, RequiredItemCount6"
        " FROM quest_template",
    ):
        vals = [int(x) for x in r]
        quests.append({
            "quest_id": vals[0],
            "npc_go_objectives": [(vals[1 + 2 * i], vals[2 + 2 * i]) for i in range(4)],
            "item_objectives": [(vals[9 + 2 * i], vals[10 + 2 * i]) for i in range(6)],
        })

    existing_poi_keys: set[tuple[int, int]] = set()
    quest_map_area: dict[tuple[int, int], int] = {}
    map_area_samples: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    for r in _mysql_query(
        mysql_cnf,
        db,
        f"SELECT p.QuestID, p.ObjectiveIndex, p.MapID, p.WorldMapAreaId,"
        f" COALESCE(ROUND(AVG(pt.X)), 0), COALESCE(ROUND(AVG(pt.Y)), 0)"
        f" FROM quest_poi p"
        f" LEFT JOIN quest_poi_points pt ON pt.QuestID = p.QuestID AND pt.Idx1 = p.id"
        f" WHERE p.id < {BACKFILL_ID_START}"
        f" GROUP BY p.QuestID, p.id, p.ObjectiveIndex, p.MapID, p.WorldMapAreaId",
    ):
        qid, obj_idx, map_id, wma, ax, ay = (int(float(x)) for x in r)
        existing_poi_keys.add((qid, obj_idx))
        if wma > 0:
            quest_map_area.setdefault((qid, map_id), wma)
            map_area_samples[map_id].append((ax, ay, wma))

    creature_spawns: dict[int, dict[int, list[tuple[float, float]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for r in _mysql_query(mysql_cnf, db, "SELECT id, map, position_x, position_y FROM creature"):
        creature_spawns[int(r[0])][int(r[1])].append((float(r[2]), float(r[3])))

    go_spawns: dict[int, dict[int, list[tuple[float, float]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for r in _mysql_query(mysql_cnf, db, "SELECT id, map, position_x, position_y FROM gameobject"):
        go_spawns[int(r[0])][int(r[1])].append((float(r[2]), float(r[3])))

    kill_credit_to_creatures: dict[int, set[int]] = defaultdict(set)
    lootid_to_creatures: dict[int, set[int]] = defaultdict(set)
    for r in _mysql_query(
        mysql_cnf, db, "SELECT entry, KillCredit1, KillCredit2, lootid FROM creature_template"
    ):
        entry, kc1, kc2, lootid = int(r[0]), int(r[1]), int(r[2]), int(r[3])
        if kc1 > 0:
            kill_credit_to_creatures[kc1].add(entry)
        if kc2 > 0:
            kill_credit_to_creatures[kc2].add(entry)
        # In AzerothCore (Unit.cpp:13741), lootid == 0 means no loot; never
        # fall back to creature_template.entry.
        if lootid > 0:
            lootid_to_creatures[lootid].add(entry)

    lootid_to_gos: dict[int, set[int]] = defaultdict(set)
    for r in _mysql_query(
        mysql_cnf, db, "SELECT entry, Data1 FROM gameobject_template WHERE type = 3 AND Data1 > 0"
    ):
        lootid_to_gos[int(r[1])].add(int(r[0]))

    quest_creature_enders: dict[int, set[int]] = defaultdict(set)
    for r in _mysql_query(mysql_cnf, db, "SELECT id, quest FROM creature_questender"):
        quest_creature_enders[int(r[1])].add(int(r[0]))

    quest_go_enders: dict[int, set[int]] = defaultdict(set)
    for r in _mysql_query(mysql_cnf, db, "SELECT id, quest FROM gameobject_questender"):
        quest_go_enders[int(r[1])].add(int(r[0]))

    ref_rows = [
        (int(r[0]), int(r[1]), int(r[2]))
        for r in _mysql_query(mysql_cnf, db, "SELECT Entry, Item, Reference FROM reference_loot_template")
    ]
    ref_items = resolve_reference_loot(ref_rows)

    c_loot_rows = [
        (int(r[0]), int(r[1]), int(r[2]))
        for r in _mysql_query(mysql_cnf, db, "SELECT Entry, Item, Reference FROM creature_loot_template")
    ]
    g_loot_rows = [
        (int(r[0]), int(r[1]), int(r[2]))
        for r in _mysql_query(mysql_cnf, db, "SELECT Entry, Item, Reference FROM gameobject_loot_template")
    ]
    item_creatures, item_gos = build_item_sources(
        c_loot_rows, g_loot_rows, ref_items, lootid_to_creatures, lootid_to_gos
    )

    return compute_backfill_rows(
        quests=quests,
        existing_poi_keys=existing_poi_keys,
        quest_map_area=quest_map_area,
        map_area_samples=map_area_samples,
        creature_spawns=creature_spawns,
        go_spawns=go_spawns,
        kill_credit_to_creatures=kill_credit_to_creatures,
        quest_creature_enders=quest_creature_enders,
        quest_go_enders=quest_go_enders,
        item_creatures=item_creatures,
        item_gos=item_gos,
    )


def apply_sql(mysql_cnf: pathlib.Path, sql_path: pathlib.Path, dbs: list[str]) -> None:
    sql_text = sql_path.read_text(encoding="utf-8")
    for db in list(dict.fromkeys(dbs)):
        if not re.fullmatch(r"[A-Za-z0-9_]+", db):
            raise ValueError(f"invalid MySQL database name: {db!r}")
        exists = _mysql_query(mysql_cnf, "information_schema",
                              f"SELECT SCHEMA_NAME FROM SCHEMATA WHERE SCHEMA_NAME='{db}'")
        if not exists:
            continue
        subprocess.run(
            ["mysql", f"--defaults-file={mysql_cnf}", "-uroot", db],
            input=sql_text,
            text=True,
            check=True,
        )
        print(f"applied {sql_path.name} to {db}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Backfill missing quest_poi rows from acore_world")
    ap.add_argument("--mysql-cnf", type=pathlib.Path, default=DEFAULT_MYSQL_CNF)
    ap.add_argument("--db", default="acore_world")
    ap.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT_SQL)
    ap.add_argument(
        "--apply",
        action="store_true",
        help="Also apply the generated SQL directly to acore_world (and acore_world_test if present)",
    )
    args = ap.parse_args()

    rows, stats = generate_from_mysql(args.mysql_cnf, args.db)
    sql = render_sql(rows)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(sql, encoding="utf-8")
    print(
        f"wrote {args.out}: {stats['rows_added']} POI rows "
        f"(turn-ins={stats['turnins_added']}, "
        f"npc/go_obj={stats['npc_go_objectives_added']}, "
        f"item_obj={stats['item_objectives_added']})"
    )
    if args.apply:
        apply_sql(args.mysql_cnf, args.out, [args.db, "acore_world_test"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
