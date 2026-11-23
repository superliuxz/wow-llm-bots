#!/usr/bin/env python3
"""Exports compact, prompt-ready 3.3.5a world knowledge from Atlas + Wiki SQLite DBs.

Reads `var/atlas-335a/atlas.sqlite` and `var/wiki-335a/wiki_335a.sqlite` and
writes `var/knowledge/world_knowledge.json` so the C++ harness can inject
compact, situation-relevant Markdown snippets into the brain's prompt at
sub-millisecond latency without linking libsqlite3 into sanitizer builds.
"""

from __future__ import annotations

import argparse
import html
import json
import pathlib
import re
import sqlite3
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
DEFAULT_ATLAS = ROOT / "var" / "atlas-335a" / "atlas.sqlite"
DEFAULT_WIKI = ROOT / "var" / "wiki-335a" / "wiki_335a.sqlite"
DEFAULT_OUT = ROOT / "var" / "knowledge" / "world_knowledge.json"

# Canonical WotLK 3.3.5a open-world leveling zones (matching in-game AreaTable
# zone names verbatim). Pins era strictly to 3.3.5a: excludes Cataclysm (4.0+)
# zones, continents, battlegrounds, and `(Classic)` duplicates.
WOTLK_335A_ZONES: frozenset[str] = frozenset({
    # Eastern Kingdoms
    "Alterac Mountains",
    "Arathi Highlands",
    "Badlands",
    "Blasted Lands",
    "Burning Steppes",
    "Deadwind Pass",
    "Dun Morogh",
    "Duskwood",
    "Eastern Plaguelands",
    "Elwynn Forest",
    "Eversong Woods",
    "Ghostlands",
    "Hillsbrad Foothills",
    "Isle of Quel'Danas",
    "Loch Modan",
    "Redridge Mountains",
    "Searing Gorge",
    "Silverpine Forest",
    "Stranglethorn Vale",
    "Swamp of Sorrows",
    "The Hinterlands",
    "Tirisfal Glades",
    "Western Plaguelands",
    "Westfall",
    "Wetlands",
    # Kalimdor
    "Ashenvale",
    "Azshara",
    "Azuremyst Isle",
    "Bloodmyst Isle",
    "Darkshore",
    "Desolace",
    "Durotar",
    "Dustwallow Marsh",
    "Felwood",
    "Feralas",
    "Mulgore",
    "Silithus",
    "Stonetalon Mountains",
    "Tanaris",
    "Teldrassil",
    "The Barrens",
    "Thousand Needles",
    "Un'Goro Crater",
    "Winterspring",
    # Outland
    "Blade's Edge Mountains",
    "Hellfire Peninsula",
    "Nagrand",
    "Netherstorm",
    "Shadowmoon Valley",
    "Terokkar Forest",
    "Zangarmarsh",
    # Northrend
    "Borean Tundra",
    "Crystalsong Forest",
    "Dragonblight",
    "Grizzly Hills",
    "Howling Fjord",
    "Icecrown",
    "Sholazar Basin",
    "The Storm Peaks",
    "Wintergrasp",
    "Zul'Drak",
})

# Questless / PvP zones that exist in 3.3.5a (for current-zone info) but should
# never be recommended as `next_zones` leveling targets.
_NON_LEVELING_NEXT_ZONES: frozenset[str] = frozenset({
    "Deadwind Pass",
    "Wintergrasp",
})

# Canonical WotLK 3.3.5a dungeons & raids (Vanilla + TBC + WotLK). Excludes
# Cataclysm (4.0+) instances and `(Classic)` duplicates.
WOTLK_335A_DUNGEONS: frozenset[str] = frozenset({
    # Classic dungeons & raids
    "Blackfathom Deeps",
    "Blackrock Depths",
    "Blackrock Spire",
    "Blackwing Lair",
    "Deadmines",
    "Dire Maul",
    "Gnomeregan",
    "Maraudon",
    "Molten Core",
    "Onyxia's Lair",
    "Ragefire Chasm",
    "Razorfen Downs",
    "Razorfen Kraul",
    "Ruins of Ahn'Qiraj",
    "Scarlet Monastery",
    "Scholomance",
    "Shadowfang Keep",
    "Stockade",
    "Stratholme",
    "Sunken Temple",
    "Temple of Ahn'Qiraj",
    "The Deadmines",
    "The Stockade",
    "Uldaman",
    "Wailing Caverns",
    "Zul'Farrak",
    "Zul'Gurub",
    # TBC dungeons & raids
    "Auchenai Crypts",
    "Black Temple",
    "Gruul's Lair",
    "Hellfire Ramparts",
    "Hyjal Summit",
    "Karazhan",
    "Magisters' Terrace",
    "Magtheridon's Lair",
    "Mana-Tombs",
    "Old Hillsbrad Foothills",
    "Serpentshrine Cavern",
    "Sethekk Halls",
    "Shadow Labyrinth",
    "Sunwell Plateau",
    "Tempest Keep",
    "The Arcatraz",
    "The Black Morass",
    "The Blood Furnace",
    "The Botanica",
    "The Eye",
    "The Mechanar",
    "The Shattered Halls",
    "The Slave Pens",
    "The Steamvault",
    "The Underbog",
    "Zul'Aman",
    # WotLK dungeons & raids
    "Ahn'kahet: The Old Kingdom",
    "Azjol-Nerub",
    "Drak'Tharon Keep",
    "Gundrak",
    "Halls of Lightning",
    "Halls of Reflection",
    "Halls of Stone",
    "Icecrown Citadel",
    "Naxxramas",
    "Pit of Saron",
    "The Culling of Stratholme",
    "The Eye of Eternity",
    "The Forge of Souls",
    "The Nexus",
    "The Obsidian Sanctum",
    "The Oculus",
    "The Ruby Sanctum",
    "The Violet Hold",
    "Trial of the Champion",
    "Trial of the Crusader",
    "Ulduar",
    "Utgarde Keep",
    "Utgarde Pinnacle",
    "Vault of Archavon",
})

_DUNGEON_TITLE_ALIASES: dict[str, str] = {
    "Ahn'Qiraj": "Temple of Ahn'Qiraj",
    "Battle for Mount Hyjal": "Hyjal Summit",
    "Blood Furnace": "The Blood Furnace",
    "Culling of Stratholme": "The Culling of Stratholme",
    "Eye (Tempest Keep)": "The Eye",
    "Eye of Eternity": "The Eye of Eternity",
    "Forge of Souls": "The Forge of Souls",
    "Nexus": "The Nexus",
    "Obsidian Sanctum": "The Obsidian Sanctum",
    "Oculus": "The Oculus",
    "Opening the Dark Portal": "The Black Morass",
    "Ruby Sanctum": "The Ruby Sanctum",
    "Shattered Halls": "The Shattered Halls",
    "Slave Pens": "The Slave Pens",
    "Steamvault": "The Steamvault",
    "Stormwind Stockade": "The Stockade",
    "Temple of Atal'Hakkar": "Sunken Temple",
    "The Shadowfang Keep": "Shadowfang Keep",
    "Underbog": "The Underbog",
    "Violet Hold": "The Violet Hold",
}

_ZONE_TITLE_ALIASES: dict[str, str] = {
    "Barrens": "The Barrens",
    "Hinterlands": "The Hinterlands",
    "Storm Peaks": "The Storm Peaks",
    "The Badlands": "Badlands",
    "The Blasted Lands": "Blasted Lands",
    "The Dragonblight": "Dragonblight",
    "The Western Plaguelands": "Western Plaguelands",
}

_RE_REF = re.compile(r"<ref[^>]*>.*?</ref>|<ref[^>]*/>", re.DOTALL | re.IGNORECASE)
_RE_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
_RE_HTML = re.compile(r"<[^>]+>")
_RE_WIKILINK = re.compile(r"\[\[([^|\]]+\|)?([^\]]+)\]\]")
_RE_EXTLINK = re.compile(r"\[https?://[^\s\]]+(?:\s+([^\]]+))?\]")
_RE_BOLD_ITALIC = re.compile(r"'{2,5}")
_RE_HEADING = re.compile(r"={2,6}\s*([^=\n]+?)\s*={2,6}")
_RE_REMOVED_PRE_CATA = re.compile(
    r"\{\{\s*removedfromgame[^}]*patch\s*=\s*([123])\.", re.IGNORECASE
)
_RE_CATA_BANNER = re.compile(r"\{\{\s*Cataclysm\b", re.IGNORECASE)
_RE_MULTI_SPACE = re.compile(r"\s+")
_RE_QUEST_BREAK = re.compile(r"\$[bB]")
_RE_QUEST_GENDER = re.compile(r"\$[gG]\s*([^:;]+):([^;]+);")
_RE_QUEST_PLACEHOLDER = re.compile(r"\$[nNcCrR]")
_RE_LEVEL_PAIR = re.compile(r"(\d{1,2})\s*[-–—]\s*(\d{1,2})")
_RE_LEVEL_SINGLE = re.compile(r"\b(\d{1,2})\b")
_RE_SECTION = re.compile(
    r"^==\s*(Notes|Tips|Walkthrough|Strategy|Tactics|Overview)\s*==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_BOSS_ABILITIES_EXACT = re.compile(
    r"^==\s*(?:Abilities|Abilites|Attacks and abilities|Skills|Boss abilities)\s*==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_BOSS_ABILITIES_BROAD = re.compile(
    r"^==\s*[^=\n]*?\b(?:Abilities|Abilites|Attacks and abilities|Skills)\b[^=\n]*?==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_BOSS_STRATEGY_EXACT = re.compile(
    r"^==\s*(?:Strategy|Strategies|Tactics|Encounter|Walkthrough)\s*==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_BOSS_STRATEGY_BROAD = re.compile(
    r"^==\s*[^=\n]*?\b(?:Strategy|Strategies|Tactics|Encounter|Walkthrough)\b[^=\n]*?==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_BOSS_PHASE_FALLBACK = re.compile(
    r"^==\s*Phase\s+(?:1|One)\b[^=\n]*==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_INST_TACTICS = re.compile(r"^(.+?)\s+\((.+?)\s+tactics\)$")
_RE_NEXT_HEADING = re.compile(r"^==[^=]", re.MULTILINE)


def _split_top_level_pipes(body: str) -> list[str]:
    """Splits a MediaWiki template body on top-level `|` characters."""
    parts: list[str] = []
    cur: list[str] = []
    d_brace = 0
    d_brack = 0
    j = 0
    bn = len(body)
    while j < bn:
        ch = body[j]
        nxt = body[j + 1] if j + 1 < bn else ""
        if ch == "{" and nxt == "{":
            d_brace += 1
            cur.append("{{")
            j += 2
            continue
        if ch == "}" and nxt == "}" and d_brace > 0:
            d_brace -= 1
            cur.append("}}")
            j += 2
            continue
        if ch == "[" and nxt == "[":
            d_brack += 1
            cur.append("[[")
            j += 2
            continue
        if ch == "]" and nxt == "]" and d_brack > 0:
            d_brack -= 1
            cur.append("]]")
            j += 2
            continue
        if ch == "|" and d_brace == 0 and d_brack == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
        j += 1
    if cur:
        parts.append("".join(cur))
    return parts


def _expand_or_strip_templates(text: str) -> str:
    """Expands `{{abilities|...}}`/`{{spell|...}}` and strips all other `{{...}}`."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if i + 1 < n and text[i] == "{" and text[i + 1] == "{":
            depth = 1
            start = i + 2
            j = start
            while j < n and depth > 0:
                if j + 1 < n and text[j] == "{" and text[j + 1] == "{":
                    depth += 1
                    j += 2
                elif j + 1 < n and text[j] == "}" and text[j + 1] == "}":
                    depth -= 1
                    j += 2
                else:
                    j += 1
            end = j - 2 if depth == 0 else j
            body = text[start : max(start, end)]
            parts = _split_top_level_pipes(body)
            tname = parts[0].strip().lower() if parts else ""
            if tname in ("abilities", "ability") and len(parts) >= 2:
                pos: list[str] = []
                named: dict[str, str] = {}
                for p in parts[1:]:
                    if "=" in p:
                        k, v = p.split("=", 1)
                        named[k.strip().lower()] = _expand_or_strip_templates(v).strip()
                    else:
                        pos.append(_expand_or_strip_templates(p).strip())
                name = pos[0] if pos else ""
                desc = pos[1] if len(pos) > 1 else ""
                notes = named.get("notes", "")
                if name:
                    frag = name
                    if desc:
                        frag = f"{frag}: {desc}"
                    if notes:
                        frag = f"{frag} ({notes})"
                    out.append(frag)
            elif tname == "spell" and len(parts) >= 2:
                pos = [
                    _expand_or_strip_templates(p).strip()
                    for p in parts[1:]
                    if "=" not in p
                ]
                if len(pos) >= 2 and pos[0].isdigit():
                    out.append(pos[1])
                elif pos:
                    out.append(pos[0])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _strip_file_links(text: str) -> str:
    """Removes balanced `[[File:...]]`, `[[Image:...]]`, and `[[Category:...]]` links."""
    out: list[str] = []
    i = 0
    n = len(text)
    prefixes = ("file:", "image:", "category:")
    while i < n:
        if i + 1 < n and text[i] == "[" and text[i + 1] == "[":
            k = i + 2
            while k < n and text[k].isspace():
                k += 1
            head = text[k : k + 9].lower()
            if any(head.startswith(p) for p in prefixes):
                depth = 1
                j = i + 2
                while j < n and depth > 0:
                    if j + 1 < n and text[j] == "[" and text[j + 1] == "[":
                        depth += 1
                        j += 2
                    elif j + 1 < n and text[j] == "]" and text[j + 1] == "]":
                        depth -= 1
                        j += 2
                    else:
                        j += 1
                out.append(" ")
                i = j
                continue
        out.append(text[i])
        i += 1
    return "".join(out)


def strip_wikitext(text: str, max_chars: int = 200) -> str:
    """Converts MediaWiki markup into clean, single-line plain text."""
    if not text:
        return ""
    s = _RE_COMMENT.sub(" ", text)
    s = _RE_REF.sub(" ", s)
    s = _expand_or_strip_templates(s)
    s = _strip_file_links(s)
    s = _RE_WIKILINK.sub(r"\2", s)
    s = _RE_EXTLINK.sub(lambda m: m.group(1) or "", s)
    s = _RE_HTML.sub(" ", s)
    s = html.unescape(s)
    s = _RE_HEADING.sub(r"\1:", s)
    s = _RE_BOLD_ITALIC.sub("", s)
    lines = []
    for raw in s.splitlines():
        line = raw.strip()
        if not line or line.startswith(("{|", "|}", "|-", "|", "!")):
            continue
        line = line.lstrip("*#:; ").strip()
        if line:
            lines.append(line)
    cleaned = _RE_MULTI_SPACE.sub(" ", " ".join(lines)).strip()
    if len(cleaned) <= max_chars:
        return cleaned
    cut = cleaned[: max_chars - 3].rsplit(" ", 1)[0]
    return (cut if cut else cleaned[: max_chars - 3]).rstrip(" ,;:") + "..."


def _extract_infobox(wikitext: str, prefix: str) -> dict[str, str]:
    """Parses key=value fields from a `{{Infobox <prefix> ...}}` template."""
    m = re.search(
        r"\{\{\s*Infobox\s+" + re.escape(prefix) + r"\b",
        wikitext,
        re.IGNORECASE,
    )
    if not m:
        return {}
    start = m.end()
    depth = 1
    i = start
    n = len(wikitext)
    while i < n and depth > 0:
        if i + 1 < n and wikitext[i] == "{" and wikitext[i + 1] == "{":
            depth += 1
            i += 2
        elif i + 1 < n and wikitext[i] == "}" and wikitext[i + 1] == "}":
            depth -= 1
            i += 2
        else:
            i += 1
    body = wikitext[start : max(start, i - 2)]
    fields: dict[str, str] = {}
    parts = _split_top_level_pipes(body)

    for part in parts:
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        key = k.strip().lower()
        val = strip_wikitext(v, max_chars=120)
        if key and val:
            fields[key] = val
    return fields


def _is_stub_section(text: str) -> bool:
    """Returns True if a cleaned wiki section is empty or a placeholder/stub."""
    s = text.strip()
    if len(s) < 20:
        return True
    low = s.lower().rstrip(".! ")
    if low in ("to be announced", "tba", "tbd", "coming soon", "none", "unknown"):
        return True
    if ("to be announced" in low or low.startswith("see ")) and len(s) < 120:
        return True
    return False


def _extract_section(
    wikitext: str,
    heading_res: tuple[re.Pattern[str], ...] | re.Pattern[str],
    max_chars: int = 200,
    *,
    skip_stubs: bool = False,
) -> str:
    """Extracts the first non-empty section body matching `heading_res` in priority order."""
    patterns = (heading_res,) if isinstance(heading_res, re.Pattern) else heading_res
    for heading_re in patterns:
        for m in heading_re.finditer(wikitext):
            rest = wikitext[m.end() :]
            nxt = _RE_NEXT_HEADING.search(rest)
            sec = rest[: nxt.start()] if nxt else rest
            cleaned = strip_wikitext(sec, max_chars=max_chars)
            if not cleaned:
                continue
            if skip_stubs and _is_stub_section(cleaned):
                continue
            return cleaned
    return ""


def _extract_wiki_tip(wikitext: str, max_chars: int = 200) -> str:
    """Extracts a concise tip from Notes/Tips/Walkthrough/Strategy sections."""
    return _extract_section(wikitext, _RE_SECTION, max_chars=max_chars)


def _extract_lead_summary(wikitext: str, max_chars: int = 180) -> str:
    """Extracts the first clean lead paragraph before the first `==` heading."""
    nxt = _RE_NEXT_HEADING.search(wikitext)
    lead = wikitext[: nxt.start()] if nxt else wikitext
    return strip_wikitext(lead, max_chars=max_chars)


def _parse_level_range(raw: str) -> tuple[int, int]:
    if not raw:
        return (0, 0)
    m = _RE_LEVEL_PAIR.search(raw)
    if m:
        lo, hi = int(m.group(1)), int(m.group(2))
        if 1 <= lo <= hi <= 80:
            return (lo, hi)
    m1 = _RE_LEVEL_SINGLE.search(raw)
    if m1:
        lv = int(m1.group(1))
        if 1 <= lv <= 80:
            return (lv, lv)
    return (0, 0)


def _norm_faction(raw: str) -> str:
    low = raw.lower()
    if "alliance" in low and "horde" not in low:
        return "Alliance"
    if "horde" in low and "alliance" not in low:
        return "Horde"
    if "contested" in low:
        return "Contested"
    if "sanctuary" in low or "neutral" in low:
        return "Neutral"
    return ""


def _canonical_335a_zone(page_title: str, infobox_name: str) -> tuple[str, bool]:
    """Maps a wiki zone title to its canonical 3.3.5a AreaTable name.

    Returns `(canonical_name, is_classic_page)` or `("", False)` if the zone is
    not a WotLK 3.3.5a open-world leveling zone.
    """
    is_classic = page_title.endswith(" (Classic)")
    stripped = page_title[: -len(" (Classic)")] if is_classic else page_title
    for cand in (stripped, _ZONE_TITLE_ALIASES.get(stripped, ""), infobox_name, _ZONE_TITLE_ALIASES.get(infobox_name, "")):
        if cand in WOTLK_335A_ZONES:
            return (cand, is_classic)
    return ("", False)


def _canonical_335a_dungeon(page_title: str, infobox_name: str) -> tuple[str, bool]:
    """Maps a wiki instance title to its canonical 3.3.5a dungeon/raid name."""
    is_classic = page_title.endswith(" (Classic)")
    stripped = page_title[: -len(" (Classic)")] if is_classic else page_title
    for cand in (
        stripped,
        _DUNGEON_TITLE_ALIASES.get(stripped, ""),
        infobox_name,
        _DUNGEON_TITLE_ALIASES.get(infobox_name, ""),
    ):
        if cand in WOTLK_335A_DUNGEONS:
            return (cand, is_classic)
    return ("", False)


def _clean_quest_log(raw: str) -> str:
    if not raw:
        return ""
    s = _RE_QUEST_BREAK.sub(" ", raw)
    s = _RE_QUEST_GENDER.sub(lambda m: m.group(1).strip(), s)
    s = _RE_QUEST_PLACEHOLDER.sub("you", s)
    s = _RE_MULTI_SPACE.sub(" ", s).strip()
    if len(s) > 200:
        s = s[:197].rsplit(" ", 1)[0] + "..."
    return s


def export_knowledge(
    atlas_path: pathlib.Path,
    wiki_path: pathlib.Path | None,
    out_path: pathlib.Path,
) -> dict[str, Any]:
    atlas_con = sqlite3.connect(f"file:{atlas_path}?mode=ro", uri=True)
    wiki_con = (
        sqlite3.connect(f"file:{wiki_path}?mode=ro", uri=True)
        if wiki_path and wiki_path.exists()
        else None
    )

    try:
        # 1) Quests: ender NPC and item drop sources.
        enders: dict[int, str] = {}
        for qid, name in atlas_con.execute(
            "SELECT quest_id, name FROM quest_relations "
            "WHERE role = 'ender' ORDER BY quest_id, entry"
        ):
            if qid not in enders and name:
                enders[qid] = name

        item_sources: dict[int, list[str]] = {}
        for item_entry, src_name in atlas_con.execute(
            "SELECT item_entry, source_name FROM item_drops "
            "WHERE source_name != '' ORDER BY item_entry, chance DESC, source_name"
        ):
            lst = item_sources.setdefault(item_entry, [])
            if len(lst) < 3 and src_name not in lst:
                lst.append(src_name)

        quest_drops: dict[int, list[str]] = {}
        for qid, target_entry in atlas_con.execute(
            "SELECT quest_id, target_entry FROM quest_objectives "
            "WHERE kind = 'item' ORDER BY quest_id, obj_index"
        ):
            for src in item_sources.get(target_entry, []):
                lst = quest_drops.setdefault(qid, [])
                if len(lst) < 3 and src not in lst:
                    lst.append(src)

        quest_rows = list(
            atlas_con.execute(
                "SELECT quest_id, title, quest_level, log_description, wiki_page_id "
                "FROM quests ORDER BY quest_id"
            )
        )
        needed_pids = sorted({int(r[4]) for r in quest_rows if r[4]})
        wiki_by_id: dict[int, str] = {}
        if wiki_con is not None and needed_pids:
            # Query only the quest wiki_page_ids referenced by atlas.sqlite.
            chunk = 900
            for start in range(0, len(needed_pids), chunk):
                batch = needed_pids[start : start + chunk]
                placeholders = ",".join("?" * len(batch))
                for pid, wt in wiki_con.execute(
                    f"SELECT page_id, wikitext FROM pages WHERE page_id IN ({placeholders})",
                    batch,
                ):
                    if wt:
                        wiki_by_id[int(pid)] = wt

        quests: dict[str, dict[str, Any]] = {}
        for qid, title, qlevel, log_desc, wiki_pid in quest_rows:
            entry: dict[str, Any] = {
                "title": title or "",
                "level": int(qlevel or 0),
            }
            cleaned_log = _clean_quest_log(log_desc or "")
            if cleaned_log:
                entry["log"] = cleaned_log
            if qid in enders:
                entry["ender"] = enders[qid]
            if qid in quest_drops:
                entry["drops"] = quest_drops[qid]
            if wiki_pid and int(wiki_pid) in wiki_by_id:
                tip = _extract_wiki_tip(wiki_by_id[int(wiki_pid)], max_chars=200)
                if tip:
                    entry["tip"] = tip
            quests[str(qid)] = entry

        # 2) Zones & Dungeons from wiki infoboxes (strictly pinned to 3.3.5a).
        zones: dict[str, dict[str, Any]] = {}
        zone_is_classic: dict[str, bool] = {}
        dungeons: dict[str, dict[str, Any]] = {}
        if wiki_con is not None:
            for title, wikitext in wiki_con.execute(
                "SELECT title, wikitext FROM pages "
                "WHERE redirect = '' AND wikitext LIKE '%Infobox zone%'"
            ):
                info = _extract_infobox(wikitext, "zone")
                if not info:
                    continue
                zname, is_classic = _canonical_335a_zone(title, info.get("name", ""))
                if not zname:
                    continue
                lo, hi = _parse_level_range(info.get("level", ""))
                if lo == 0 or hi == 0:
                    continue
                # Prefer `(Classic)` pre-Cataclysm page over post-Cataclysm revamp.
                if zname in zones and zone_is_classic.get(zname, False) and not is_classic:
                    continue
                faction = _norm_faction(info.get("faction", ""))
                z_entry: dict[str, Any] = {
                    "min_level": lo,
                    "max_level": hi,
                }
                if faction:
                    z_entry["faction"] = faction
                zones[zname] = z_entry
                zone_is_classic[zname] = is_classic

            # Compute next_zones strictly among canonical 3.3.5a leveling zones.
            unique_zones = sorted(zones.items(), key=lambda kv: (kv[1]["min_level"], kv[0]))
            for zname, zdata in zones.items():
                z_hi = zdata["max_level"]
                z_fac = zdata.get("faction", "")
                if z_hi >= 78:
                    continue
                cands: list[tuple[int, int, str]] = []
                for other_name, odata in unique_zones:
                    if other_name == zname or other_name in _NON_LEVELING_NEXT_ZONES:
                        continue
                    o_lo, o_hi = odata["min_level"], odata["max_level"]
                    if o_lo < max(1, z_hi - 3) or o_lo > z_hi + 6 or o_hi <= z_hi:
                        continue
                    o_fac = odata.get("faction", "")
                    if z_fac == "Alliance" and o_fac == "Horde":
                        continue
                    if z_fac == "Horde" and o_fac == "Alliance":
                        continue
                    cands.append((abs(o_lo - z_hi), o_hi - o_lo, f"{other_name} ({o_lo}-{o_hi})"))
                cands.sort()
                dedup: list[str] = []
                for _, _, label in cands:
                    if label not in dedup:
                        dedup.append(label)
                    if len(dedup) >= 3:
                        break
                if dedup:
                    zdata["next_zones"] = dedup

            dungeon_is_classic: dict[str, bool] = {}
            for title, wikitext in wiki_con.execute(
                "SELECT title, wikitext FROM pages "
                "WHERE redirect = '' AND wikitext LIKE '%Infobox instance%'"
            ):
                info = _extract_infobox(wikitext, "instance")
                if not info:
                    continue
                dname, is_classic = _canonical_335a_dungeon(title, info.get("name", ""))
                if not dname:
                    continue
                # Skip Vanilla `(Classic)` pages removed before 4.0 (e.g. level-60
                # Naxxramas 3.0.2 / Onyxia 3.2.2) so WotLK 3.3.5a versions stay intact.
                if is_classic and _RE_REMOVED_PRE_CATA.search(wikitext):
                    continue
                # Prefer `(Classic)` pre-Cataclysm page over post-Cataclysm revamp.
                if dname in dungeons and dungeon_is_classic.get(dname, False) and not is_classic:
                    continue
                lvl = info.get("level", "")
                # Strip any post-80 Cataclysm heroic level suffix (e.g. "18-24 / 85").
                if "/" in lvl and any(int(x) > 80 for x in re.findall(r"\b\d+\b", lvl)):
                    lvl = lvl.split("/", 1)[0].strip()
                summary = _extract_wiki_tip(wikitext, max_chars=220) or _extract_lead_summary(
                    wikitext, max_chars=220
                )
                if not summary:
                    continue
                d_entry: dict[str, Any] = {"summary": summary}
                if lvl:
                    d_entry["level"] = lvl
                dungeons[dname] = d_entry
                dungeon_is_classic[dname] = is_classic

            redirects: dict[str, str] = {}
            for rtitle, rtarget in wiki_con.execute(
                "SELECT title, redirect FROM pages WHERE redirect != ''"
            ):
                redirects[rtitle] = rtarget
                if rtitle in WOTLK_335A_DUNGEONS and rtarget in dungeons and rtitle not in dungeons:
                    dungeons[rtitle] = dict(dungeons[rtarget])
            # Mirror canonical short/long pairs (e.g. Stockade <-> The Stockade).
            for a, b in (("Stockade", "The Stockade"), ("Deadmines", "The Deadmines")):
                if a in dungeons and b not in dungeons:
                    dungeons[b] = dict(dungeons[a])
                elif b in dungeons and a not in dungeons:
                    dungeons[a] = dict(dungeons[b])

        # 3) 3.3.5a Boss strategies (pinned to creatures existing in atlas.sqlite).
        bosses: dict[str, dict[str, Any]] = {}
        if wiki_con is not None:
            boss_names = {
                r[0]
                for r in atlas_con.execute(
                    "SELECT DISTINCT name FROM creatures "
                    "WHERE rank IN (1, 3) AND name != ''"
                )
            }
            alias_to_boss: dict[str, set[str]] = {}
            for bname in boss_names:
                alias_to_boss.setdefault(bname, set()).add(bname)
                cur_t = bname
                for _ in range(2):
                    nxt_t = redirects.get(cur_t, "")
                    if not nxt_t or nxt_t == cur_t:
                        break
                    alias_to_boss.setdefault(nxt_t, set()).add(bname)
                    cur_t = nxt_t

            boss_rank: dict[str, tuple[int, int]] = {}
            for title, wikitext in wiki_con.execute(
                "SELECT title, wikitext FROM pages WHERE redirect = ''"
            ):
                if not wikitext:
                    continue
                inst = ""
                is_classic = False
                if title.endswith(" (tactics)"):
                    base = title[: -len(" (tactics)")]
                    prio = 3
                elif m_inst := _RE_INST_TACTICS.match(title):
                    base = m_inst.group(1)
                    inst, _ = _canonical_335a_dungeon(m_inst.group(2), "")
                    if not inst:
                        continue
                    prio = 3
                elif title.endswith(" (Classic)"):
                    base = title[: -len(" (Classic)")]
                    prio = 2
                    is_classic = True
                else:
                    base = title
                    prio = 1

                matched_names = alias_to_boss.get(base)
                if not matched_names:
                    continue
                if is_classic and _RE_REMOVED_PRE_CATA.search(wikitext):
                    continue
                if _RE_CATA_BANNER.search(wikitext):
                    continue

                abilities = _extract_section(
                    wikitext,
                    (_RE_BOSS_ABILITIES_EXACT, _RE_BOSS_ABILITIES_BROAD),
                    max_chars=360,
                    skip_stubs=True,
                )
                strategy = _extract_section(
                    wikitext,
                    (
                        _RE_BOSS_STRATEGY_EXACT,
                        _RE_BOSS_STRATEGY_BROAD,
                        _RE_BOSS_PHASE_FALLBACK,
                    ),
                    max_chars=420,
                    skip_stubs=True,
                )
                if not strategy and not (prio == 3 and abilities):
                    continue

                b_entry: dict[str, Any] = {}
                if abilities:
                    b_entry["abilities"] = abilities
                if strategy:
                    b_entry["strategy"] = strategy

                for bname in matched_names:
                    key = f"{inst}::{bname}" if inst else bname
                    score = (prio, 1 if base == bname else 0)
                    if score <= boss_rank.get(key, (0, 0)):
                        continue
                    bosses[key] = dict(b_entry)
                    boss_rank[key] = score

        payload = {
            "version": 1,
            "quests": quests,
            "zones": zones,
            "dungeons": dungeons,
            "bosses": bosses,
        }
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=out_path.parent,
            prefix=".world_knowledge_",
            suffix=".tmp",
            delete=False,
        ) as tf:
            json.dump(payload, tf, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
            tmp_path = pathlib.Path(tf.name)
        tmp_path.replace(out_path)
        return payload
    finally:
        atlas_con.close()
        if wiki_con is not None:
            wiki_con.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--atlas", type=pathlib.Path, default=DEFAULT_ATLAS)
    ap.add_argument("--wiki", type=pathlib.Path, default=DEFAULT_WIKI)
    ap.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    data = export_knowledge(args.atlas, args.wiki, args.out)
    size_mb = args.out.stat().st_size / (1024 * 1024)
    print(
        f"exported {len(data['quests'])} quests, {len(data['zones'])} zones, "
        f"{len(data['dungeons'])} dungeons, {len(data['bosses'])} bosses "
        f"-> {args.out} ({size_mb:.2f} MiB)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
