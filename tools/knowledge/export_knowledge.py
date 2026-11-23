#!/usr/bin/env python3
"""Exports compact, prompt-ready world knowledge from Atlas + Wiki SQLite DBs.

Reads `var/atlas-335a/atlas.sqlite` and `var/wiki-335a/wiki_335a.sqlite` and
writes `var/knowledge/world_knowledge.json` so the C++ harness can inject
compact, situation-relevant Markdown snippets into the brain's prompt at
sub-millisecond latency without linking libsqlite3 into sanitizer builds.
"""

from __future__ import annotations

import argparse
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

_RE_REF = re.compile(r"<ref[^>]*>.*?</ref>|<ref[^>]*/>", re.DOTALL | re.IGNORECASE)
_RE_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
_RE_HTML = re.compile(r"<[^>]+>")
_RE_FILE_LINK = re.compile(
    r"\[\[(?:File|Image|Category):[^\]]*\]\]", re.IGNORECASE
)
_RE_WIKILINK = re.compile(r"\[\[([^|\]]+\|)?([^\]]+)\]\]")
_RE_EXTLINK = re.compile(r"\[https?://[^\s\]]+(?:\s+([^\]]+))?\]")
_RE_BOLD_ITALIC = re.compile(r"'{2,5}")
_RE_MULTI_SPACE = re.compile(r"\s+")
_RE_REDIRECT = re.compile(r"^\s*#REDIRECT\s*\[\[([^\]|#]+)", re.IGNORECASE)
_RE_LEVEL_PAIR = re.compile(r"(\d{1,2})\s*[-–—]\s*(\d{1,2})")
_RE_LEVEL_SINGLE = re.compile(r"\b(\d{1,2})\b")
_RE_SECTION = re.compile(
    r"^==\s*(Notes|Tips|Walkthrough|Strategy|Tactics|Overview)\s*==\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_RE_NEXT_HEADING = re.compile(r"^==[^=]", re.MULTILINE)


def _strip_templates(text: str) -> str:
    """Removes balanced `{{...}}` MediaWiki templates."""
    out: list[str] = []
    depth = 0
    i = 0
    n = len(text)
    while i < n:
        if i + 1 < n and text[i] == "{" and text[i + 1] == "{":
            depth += 1
            i += 2
        elif i + 1 < n and text[i] == "}" and text[i + 1] == "}" and depth > 0:
            depth -= 1
            i += 2
        else:
            if depth == 0:
                out.append(text[i])
            i += 1
    return "".join(out)


def strip_wikitext(text: str, max_chars: int = 200) -> str:
    """Converts MediaWiki markup into clean, single-line plain text."""
    if not text:
        return ""
    s = _RE_COMMENT.sub(" ", text)
    s = _RE_REF.sub(" ", s)
    s = _strip_templates(s)
    s = _RE_FILE_LINK.sub(" ", s)
    s = _RE_WIKILINK.sub(r"\2", s)
    s = _RE_EXTLINK.sub(lambda m: m.group(1) or "", s)
    s = _RE_HTML.sub(" ", s)
    s = _RE_BOLD_ITALIC.sub("", s)
    # Drop leading bullet/table noise.
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
    # Split on top-level `|` (depth 0 inside the infobox).
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

    for part in parts:
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        key = k.strip().lower()
        val = strip_wikitext(v, max_chars=120)
        if key and val:
            fields[key] = val
    return fields


def _extract_wiki_tip(wikitext: str, max_chars: int = 200) -> str:
    """Extracts a concise tip from Notes/Tips/Walkthrough/Strategy sections."""
    m = _RE_SECTION.search(wikitext)
    if not m:
        return ""
    rest = wikitext[m.end() :]
    nxt = _RE_NEXT_HEADING.search(rest)
    sec = rest[: nxt.start()] if nxt else rest
    return strip_wikitext(sec, max_chars=max_chars)


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
        if 1 <= lo <= hi <= 85:
            return (lo, hi)
    m1 = _RE_LEVEL_SINGLE.search(raw)
    if m1:
        lv = int(m1.group(1))
        if 1 <= lv <= 85:
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


def _resolve_wiki_page(
    wiki_con: sqlite3.Connection,
    title: str,
    max_hops: int = 2,
) -> tuple[str, str] | None:
    cur_title = title
    for _ in range(max_hops + 1):
        row = wiki_con.execute(
            "SELECT title, redirect, wikitext FROM pages WHERE title = ?",
            (cur_title,),
        ).fetchone()
        if not row:
            return None
        resolved_title, redirect, wikitext = row[0], row[1] or "", row[2] or ""
        if redirect:
            cur_title = redirect
            continue
        m = _RE_REDIRECT.match(wikitext)
        if m:
            cur_title = m.group(1).strip()
            continue
        return (resolved_title, wikitext)
    return None


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
        # 1) Quests: relations (starter/ender) and item drop sources.
        starters: dict[int, str] = {}
        enders: dict[int, str] = {}
        for qid, role, name in atlas_con.execute(
            "SELECT quest_id, role, name FROM quest_relations ORDER BY quest_id, entry"
        ):
            if role == "starter" and qid not in starters and name:
                starters[qid] = name
            elif role == "ender" and qid not in enders and name:
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
        for qid, kind, target_entry in atlas_con.execute(
            "SELECT quest_id, kind, target_entry FROM quest_objectives "
            "WHERE kind = 'item' ORDER BY quest_id, obj_index"
        ):
            for src in item_sources.get(target_entry, []):
                lst = quest_drops.setdefault(qid, [])
                if len(lst) < 3 and src not in lst:
                    lst.append(src)

        wiki_by_id: dict[int, str] = {}
        if wiki_con is not None:
            for pid, wt in wiki_con.execute(
                "SELECT page_id, wikitext FROM pages WHERE page_id IN "
                "(SELECT page_id FROM pages) AND wikitext LIKE '%==%'"
            ):
                if wt:
                    wiki_by_id[pid] = wt

        quests: dict[str, dict[str, Any]] = {}
        for qid, title, qlevel, min_level, log_desc, wiki_pid in atlas_con.execute(
            "SELECT quest_id, title, quest_level, min_level, log_description, wiki_page_id "
            "FROM quests ORDER BY quest_id"
        ):
            entry: dict[str, Any] = {
                "title": title or "",
                "level": int(qlevel or 0),
                "min_level": int(min_level or 0),
            }
            cleaned_log = _RE_MULTI_SPACE.sub(" ", (log_desc or "").replace("$B", " ")).strip()
            if len(cleaned_log) > 200:
                cleaned_log = cleaned_log[:197].rsplit(" ", 1)[0] + "..."
            if cleaned_log:
                entry["log"] = cleaned_log
            if qid in starters:
                entry["starter"] = starters[qid]
            if qid in enders:
                entry["ender"] = enders[qid]
            if qid in quest_drops:
                entry["drops"] = quest_drops[qid]
            if wiki_pid and wiki_pid in wiki_by_id:
                tip = _extract_wiki_tip(wiki_by_id[wiki_pid], max_chars=200)
                if tip:
                    entry["tip"] = tip
            quests[str(qid)] = entry

        # 2) Zones & Dungeons from wiki infoboxes.
        zones: dict[str, dict[str, Any]] = {}
        dungeons: dict[str, dict[str, Any]] = {}
        if wiki_con is not None:
            for title, wikitext in wiki_con.execute(
                "SELECT title, wikitext FROM pages "
                "WHERE redirect = '' AND wikitext LIKE '%Infobox zone%'"
            ):
                info = _extract_infobox(wikitext, "zone")
                if not info:
                    continue
                lo, hi = _parse_level_range(info.get("level", ""))
                if lo == 0 or hi == 0:
                    continue
                zname = info.get("name") or title
                faction = _norm_faction(info.get("faction", ""))
                summary = _extract_lead_summary(wikitext, max_chars=160)
                z_entry: dict[str, Any] = {
                    "min_level": lo,
                    "max_level": hi,
                }
                if faction:
                    z_entry["faction"] = faction
                if summary:
                    z_entry["summary"] = summary
                zones[zname] = z_entry
                if title != zname and title not in zones:
                    zones[title] = dict(z_entry)

            # Compute next_zones for each zone (up to 3 zones at the next bracket).
            unique_zones = sorted(zones.items(), key=lambda kv: (kv[1]["min_level"], kv[0]))
            for zname, zdata in zones.items():
                z_hi = zdata["max_level"]
                z_fac = zdata.get("faction", "")
                if z_hi >= 78:
                    continue
                cands: list[tuple[int, int, str]] = []
                for other_name, odata in unique_zones:
                    if other_name == zname:
                        continue
                    o_lo, o_hi = odata["min_level"], odata["max_level"]
                    # Skip battlegrounds / wide-span non-leveling hubs (e.g. 10-80).
                    if o_hi - o_lo > 20:
                        continue
                    if o_lo < max(1, z_hi - 2) or o_lo > z_hi + 6 or o_hi <= z_hi:
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

            for title, wikitext in wiki_con.execute(
                "SELECT title, wikitext FROM pages "
                "WHERE redirect = '' AND wikitext LIKE '%Infobox instance%'"
            ):
                info = _extract_infobox(wikitext, "instance")
                if not info:
                    continue
                dname = info.get("name") or title
                lvl = info.get("level", "")
                summary = _extract_wiki_tip(wikitext, max_chars=220) or _extract_lead_summary(
                    wikitext, max_chars=220
                )
                if not summary:
                    continue
                d_entry: dict[str, Any] = {"summary": summary}
                if lvl:
                    d_entry["level"] = lvl
                dungeons[dname] = d_entry
                if title != dname and title not in dungeons:
                    dungeons[title] = dict(d_entry)

            # Also alias redirects pointing to known dungeons (e.g. "The Deadmines" -> "Deadmines").
            for rtitle, rtarget in wiki_con.execute(
                "SELECT title, redirect FROM pages WHERE redirect != ''"
            ):
                if rtarget in dungeons and rtitle not in dungeons:
                    dungeons[rtitle] = dict(dungeons[rtarget])

        payload = {
            "version": 1,
            "quests": quests,
            "zones": zones,
            "dungeons": dungeons,
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
        f"{len(data['dungeons'])} dungeons -> {args.out} ({size_mb:.2f} MiB)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
