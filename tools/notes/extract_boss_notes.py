#!/usr/bin/env python3
"""Extract draft boss mechanics and .wlb standing orders from mod-playerbots C++ AI.

Parses instance strategies, triggers, actions, and shared helpers under
`third_party/mod-playerbots/src/Ai/{Dungeon,Raid}/*`, extracts ground hazards,
cast AoEs, add target switches, interrupts, debuffs/auras, kiting, and facing
mechanics, and emits concise draft `notes.md` files with standing orders in the
`harness/grammar/orders.wlb` grammar (including Phase 2 `in <mech>`, `out of <thing>`,
and `back`).
"""

from __future__ import annotations

import argparse
import dataclasses
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

# Known human-readable instance display names for common playerbots folder codes.
INSTANCE_NAMES: Dict[str, str] = {
    "AC": "Auchenai Crypts",
    "AK": "Ahn'kahet: The Old Kingdom",
    "AN": "Azjol-Nerub",
    "CoS": "The Culling of Stratholme",
    "DTK": "Drak'Tharon Keep",
    "FoS": "The Forge of Souls",
    "GD": "Gundrak",
    "HoL": "Halls of Lightning",
    "HoS": "Halls of Stone",
    "Mech": "The Mechanar",
    "MgT": "Magisters' Terrace",
    "Nex": "The Nexus",
    "OC": "The Oculus",
    "PoS": "Pit of Saron",
    "Ramp": "Hellfire Ramparts",
    "Seth": "Sethekk Halls",
    "TOC": "Trial of the Champion",
    "UB": "The Underbog",
    "UK": "Utgarde Keep",
    "UP": "Utgarde Pinnacle",
    "VH": "The Violet Hold",
    "Aq20": "Ruins of Ahn'Qiraj",
    "BT": "Black Temple",
    "BWL": "Blackwing Lair",
    "EoE": "The Eye of Eternity",
    "Gruul": "Gruul's Lair",
    "Hyjal": "Hyjal Summit",
    "ICC": "Icecrown Citadel",
    "Kara": "Karazhan",
    "Mag": "Magtheridon's Lair",
    "MC": "Molten Core",
    "Naxx": "Naxxramas",
    "Ony": "Onyxia's Lair",
    "OS": "The Obsidian Sanctum",
    "RS": "The Ruby Sanctum",
    "SSC": "Serpentshrine Cavern",
    "TK": "Tempest Keep",
    "Uld": "Ulduar",
    "VoA": "Vault of Archavon",
    "ZA": "Zul'Aman",
    "ZG": "Zul'Gurub",
}

# Whole-phrase aliases (only applied when the full string matches, never per-word).
WHOLE_PHRASE_ALIASES: Dict[str, str] = {
    "flamestrike": "Flame Strike",
    "kaelthas": "Kael'thas Sunstrider",
    "kael": "Kael'thas Sunstrider",
    "bjarngrim": "General Bjarngrim",
    "ikiss": "Talon King Ikiss",
    "syth": "Darkweaver Syth",
    "selin": "Selin Fireheart",
    "delrissa": "Priestess Delrissa",
    "bronjahm": "Bronjahm",
    "devourer": "Devourer of Souls",
    "loken": "Loken",
    "ionar": "Ionar",
    "volkhan": "Volkhan",
    "anzu": "Anzu",
    "dos": "Devourer of Souls",
    "lm": "Lord Marrowgar",
    "dbs": "Deathbringer Saurfang",
    "pp": "Professor Putricide",
    "bpc": "Blood Prince Council",
    "bql": "Blood-Queen Lana'thel",
    "vdw": "Valithria Dreamwalker",
    "lk": "The Lich King",
}

# Per-word acronyms / special tokens.
SPECIAL_WORDS: Dict[str, str] = {
    "flamestrike": "Flame Strike",
    "kaelthas": "Kael'thas",
    "aoe": "AoE",
    "los": "LoS",
}

# Words kept lowercase in Title Case (except at the start).
MINOR_WORDS: Set[str] = {"of", "the", "in", "on", "to", "from", "and", "or", "with", "at", "by", "for"}

# Suffixes on C++ enum names that are mode/variant qualifiers, not part of the in-game name.
STRIP_SUFFIXES: Tuple[str, ...] = (
    "_NORMAL",
    "_HEROIC",
    "_10N",
    "_25N",
    "_10H",
    "_25H",
    "_10",
    "_25",
    "_N",
    "_H",
    "_TRIGGER",
    "_VISUAL2",
    "_VISUAL",
    "_INITIAL",
    "_DOT",
    "_FLY",
    "_RANK_1",
    "_RANK_2",
    "_BUFF",
    "_DEBUFF",
    "_AURA",
    "_EFFECT",
    "_DAMAGE",
    "_DMG",
    "_CAST",
)

# Known boss-qualifier suffixes/prefixes inside SPELL_* enum identifiers.
BOSS_QUALIFIER_WORDS: Set[str] = {
    "BJARNGRIM",
    "KAEL",
    "DELRISSA",
    "ANZU",
    "SELIN",
    "LOKEN",
    "IONAR",
    "VOLKHAN",
    "BRONJAHM",
    "IKISS",
    "SYTH",
    "PUTRICIDE",
    "ROTFACE",
    "FESTERGUT",
    "SINDRAGOSA",
}

# Multi-word ground hazard spell/trigger names (never bare single words like
# "fire" or "acid" that false-match unrelated triggers such as "fire resistance").
GROUND_HAZARD_KW = (
    "flame strike",
    "flamestrike",
    "dampening field",
    "phoenix burn",
    "ooze puddle",
    "void zone",
    "defile",
    "malleable goo",
    "blizzard",
    "consecration",
    "poison cloud",
    "gas cloud",
    "vile gas",
    "frost patch",
    "ice patch",
    "arcane field",
    "shadow fissure",
    "desecration",
    "death and decay",
    "rain of fire",
    "shadow crash",
    "living bomb",
    "ring of frost",
)

CAST_AOE_KW = (
    "lightning nova",
    "arcane explosion",
    "arcane nova",
    "whirlwind",
    "poison nova",
    "frost nova",
    "shockwave",
    "pound",
    "thunderclap",
    "shatter",
    "explosion",
    "nova",
    "blistering cold",
    "breath",
    "cleave",
    "stomp",
    "quake",
    "detonate",
    "supernova",
    "hellfire",
    "bladestorm",
    "bonestorm",
    "soulstorm",
    "starfall",
)

ADD_SWITCH_KW = (
    "crystal",
    "egg",
    "fragment",
    "totem",
    "lieutenant",
    "watcher",
    "elemental",
    "spike",
    "add",
    "adds",
    "ooze",
    "bomb",
    "val'kyr",
    "valkyr",
    "sphere",
    "spark",
    "hound",
    "web wrap",
    "snake wrap",
    "ghoul",
    "skeleton",
    "whelp",
    "imp",
    "spirit",
    "minion",
    "guardian",
    "portal",
    "tentacle",
    "parasite",
    "zombie",
)

# Regex for validating emitted standing orders against harness/grammar/orders.wlb
# plus Phase 2 extensions (`in <mech>`, `out of <thing>`, `back`).
_WHO = r"(?:me|tank|offtank|healer|leader|anyone|boss|it)"
_ICON = r"(?:skull|cross|square|moon|triangle|diamond|circle|star)"
_STATE = r"(?:flying|grounded|untargetable|submerged)"
_MY_STATE = r"(?:in vehicle|mounted|swimming|nearest to it)"
_NUM = r"\d+(?:\.\d)?"
_NAME = r"[A-Za-z][A-Za-z0-9'\- ]*[A-Za-z0-9]"
_THING = (
    rf"(?:{_WHO}|my target|marker {_ICON}|nearest (?:ally|player|enemy|{_NAME})"
    rf"|ally with {_NAME}|group|allies|focus|{_NAME})"
)
_CHECK_PATTERNS = [
    rf"{_NAME} stacks [<>] \d+ on {_WHO}",
    rf"{_NAME} ends<{_NUM}s on {_WHO}",
    rf"{_NAME} cast on {_WHO}",
    rf"{_NAME} cast in last {_NUM}s",
    rf"{_NAME} cast",
    rf"{_NAME} appears in last {_NUM}s",
    rf"{_NAME} appears",
    rf"{_NAME} count [<>] \d+",
    rf"{_NAME} on {_WHO}",
    rf"in {_NAME}",
    rf"{_THING} within {_NUM}(?: of {_THING})?",
    rf"{_THING} beyond {_NUM}(?: of {_THING})?",
    rf"{_THING} targets {_THING}",
    rf"{_THING} hp[<>]{_NUM}",
    rf"mana_points[<>]\d+",
    rf"mana[<>]{_NUM}",
    rf"{_THING} is marked {_ICON}",
    rf"{_THING} is {_STATE}",
    rf"{_THING} in sight",
    rf"{_THING} out of sight",
    rf"me behind {_THING}",
    rf"my target is {_THING}",
    rf"me has {_NAME}",
    rf"me {_MY_STATE}",
    rf"phase \d",
    rf"after {_NUM}s",
    rf"every {_NUM}s",
]
_COND_RE = re.compile(r"^(?:not )?(?:" + "|".join(_CHECK_PATTERNS) + r")$")

_PLACE = (
    rf"(?:behind {_THING}|in front of {_THING}|max range of {_THING}"
    rf"|{_NUM} yd from {_THING}|{_THING})"
)
_ACT_PATTERNS = [
    rf"to {_PLACE}",
    rf"away(?: {_THING})?(?: {_NUM})?",
    rf"out of {_THING}",
    r"back",
    rf"avoid {_THING}",
    rf"target {_THING}",
    rf"use {_NAME}(?: on {_THING})?",
    r"stop(?: (?:casting|attacking|moving))?",
    rf"face away {_THING}",
    rf"face {_THING}",
    r"jump",
    rf"cancel {_NAME}",
    rf"mark {_THING} {_ICON}",
    rf"interact {_THING}",
    rf"board {_THING}",
    r"leave vehicle",
    rf"equip {_NAME}",
    rf"pet attack {_THING}",
    r"pet (?:stay|follow|passive|defensive)",
    rf"kite {_THING}",
    r"wake",
]
_ACT_RE = re.compile(r"^(?:" + "|".join(_ACT_PATTERNS) + r")$")


def is_valid_order(order: str) -> bool:
    """Validate a standing order string against orders.wlb + Phase 2 extensions."""
    if " -> " not in order:
        return False
    when_part, steps_part = order.split(" -> ", 1)
    when_part = when_part.strip()
    steps_part = steps_part.strip()
    if not when_part or not steps_part:
        return False

    if when_part != "always":
        conds = [c.strip() for c in when_part.split(" & ")]
        if not (1 <= len(conds) <= 4):
            return False
        for cond in conds:
            if not _COND_RE.match(cond):
                return False

    steps = [s.strip() for s in steps_part.split(", then ")]
    if not (1 <= len(steps) <= 3):
        return False
    for step in steps:
        if not _ACT_RE.match(step):
            return False
    return True


@dataclasses.dataclass
class TriggerSpec:
    name: str
    actions: List[str]
    boss_hint: str = ""
    trigger_body: str = ""
    action_bodies: List[str] = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class Mechanic:
    boss: str
    name: str
    kind: str  # ground_hazard, cast_aoe, add_switch, interrupt, debuff, facing, kite, positioning
    description: str
    order: Optional[str] = None
    radius_yd: Optional[float] = None
    source_trigger: str = ""


@dataclasses.dataclass
class InstanceNotes:
    code: str
    display_name: str
    category: str  # Dungeon or Raid
    triggers_parsed: int
    triggers_mapped: int
    mechanics: List[Mechanic]


def title_case_name(raw: str) -> str:
    """Convert an identifier or lowercase phrase into clean WoW-style Title Case."""
    s = raw.strip().replace("_", " ")
    s = re.sub(r"\s+", " ", s)
    if not s:
        return ""
    low = s.lower()
    if low in WHOLE_PHRASE_ALIASES:
        return WHOLE_PHRASE_ALIASES[low]
    if low in SPECIAL_WORDS:
        return SPECIAL_WORDS[low]
    words = s.split(" ")
    out: List[str] = []
    for idx, w in enumerate(words):
        wl = w.lower()
        if wl in SPECIAL_WORDS:
            out.append(SPECIAL_WORDS[wl])
        elif idx > 0 and wl in MINOR_WORDS:
            out.append(wl)
        elif "'" in w:
            # Preserve apostrophe capitalization like Kael'thas or Kel'Thuzad.
            parts = w.split("'")
            out.append(parts[0].capitalize() + "'" + "'".join(p.lower() for p in parts[1:]))
        else:
            out.append(w.capitalize())
    return " ".join(out)


def clean_enum_symbol(sym: str) -> Tuple[str, str]:
    """Return (kind, display_name) for an enum symbol like NPC_PHOENIX_EGG or SPELL_LIGHTNING_NOVA_N."""
    kind = "other"
    rest = sym
    if rest.startswith("NPC_"):
        kind = "npc"
        rest = rest[4:]
    elif rest.startswith("SPELL_"):
        kind = "spell"
        rest = rest[6:]
    elif rest.startswith("GO_") or rest.startswith("OBJECT_"):
        kind = "object"
        rest = rest.split("_", 1)[1]
    elif rest.startswith("ITEM_"):
        kind = "item"
        rest = rest[5:]

    is_trigger_npc = kind == "npc" and rest.endswith("_TRIGGER")

    changed = True
    while changed:
        changed = False
        for suf in STRIP_SUFFIXES:
            if rest.endswith(suf) and len(rest) > len(suf):
                rest = rest[: -len(suf)]
                changed = True

    tokens = [re.sub(r"\d+(?:[-_]\d+)?$", "", t) for t in rest.split("_")]
    tokens = [t for t in tokens if t and not t.isdigit()]
    # Drop trailing/leading boss qualifier on spells if other tokens remain.
    if kind == "spell" and len(tokens) > 1:
        if tokens[-1] in BOSS_QUALIFIER_WORDS:
            tokens = tokens[:-1]
        elif tokens[0] in BOSS_QUALIFIER_WORDS:
            tokens = tokens[1:]

    name = title_case_name(" ".join(tokens))
    if is_trigger_npc:
        kind = "hazard_npc"
    return kind, name


def strip_cpp_comments(text: str) -> str:
    """Remove /* ... */ block comments while preserving line count."""
    return re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)


def extract_balanced_body(text: str, open_brace_idx: int) -> str:
    """Return substring inside `{ ... }` starting at `open_brace_idx`."""
    depth = 0
    for i in range(open_brace_idx, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_idx : i + 1]
    return text[open_brace_idx:]


def parse_methods(files_text: Dict[Path, str]) -> Dict[str, str]:
    """Map C++ ClassName::MethodName to its body text."""
    methods: Dict[str, str] = {}
    pat = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*(?:const\s*)?\{")
    for text in files_text.values():
        cleaned = strip_cpp_comments(text)
        for m in pat.finditer(cleaned):
            cls_name, meth_name = m.group(1), m.group(2)
            body = extract_balanced_body(cleaned, m.end() - 1)
            methods[f"{cls_name}::{meth_name}"] = body
    return methods


def parse_name_to_class(files_text: Dict[Path, str]) -> Tuple[Dict[str, str], Dict[str, str]]:
    """Map trigger string -> TriggerClass and action string -> ActionClass."""
    trig_map: Dict[str, str] = {}
    act_map: Dict[str, str] = {}

    # Pattern 1: Constructor `ClassName(PlayerbotAI* botAI) : Trigger(botAI, "name")`
    ctor_pat = re.compile(
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^)]*\)\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*\([^,)]*,\s*\"([^\"]+)\""
    )
    # Pattern 2: Context creators `creators["name"] = &Ctx::fn;` + `return new ClassName(...)`
    creator_pat = re.compile(r'creators\["([^"]+)"\]\s*=\s*&[A-Za-z0-9_]+::([A-Za-z0-9_]+)\s*;')
    factory_pat = re.compile(
        r"static\s+(Trigger|Action)\*\s+([A-Za-z0-9_]+)\s*\([^)]*\)\s*\{\s*return\s+new\s+([A-Za-z0-9_]+)"
    )

    factories: Dict[str, Tuple[str, str]] = {}
    for text in files_text.values():
        cleaned = strip_cpp_comments(text)
        for m in factory_pat.finditer(cleaned):
            factories[m.group(2)] = (m.group(1), m.group(3))
        for m in ctor_pat.finditer(cleaned):
            cls_name, base_cls, reg_name = m.group(1), m.group(2), m.group(3)
            if "Trigger" in cls_name or "Trigger" in base_cls:
                trig_map[reg_name] = cls_name
            elif "Action" in cls_name or "Action" in base_cls:
                act_map[reg_name] = cls_name

    for text in files_text.values():
        cleaned = strip_cpp_comments(text)
        for m in creator_pat.finditer(cleaned):
            reg_name, fn_name = m.group(1), m.group(2)
            if fn_name in factories:
                kind, cls_name = factories[fn_name]
                if kind == "Trigger":
                    trig_map.setdefault(reg_name, cls_name)
                else:
                    act_map.setdefault(reg_name, cls_name)

    return trig_map, act_map


def parse_enums_and_constants(
    files_text: Dict[Path, str],
) -> Tuple[Dict[str, Tuple[str, str, int]], Dict[str, float]]:
    """Extract enum symbols -> (kind, display_name, numeric_id) and float constants."""
    symbols: Dict[str, Tuple[str, str, int]] = {}
    floats: Dict[str, float] = {}

    enum_item_pat = re.compile(
        r"\b((?:NPC|SPELL|GO|OBJECT|ITEM)_[A-Z0-9_]+)\s*=\s*(\d+)"
    )
    const_float_pat = re.compile(
        r"(?:constexpr\s+)?float\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\d+(?:\.\d+)?)f?"
    )

    for text in files_text.values():
        cleaned = strip_cpp_comments(text)
        for m in enum_item_pat.finditer(cleaned):
            sym, num = m.group(1), int(m.group(2))
            kind, disp = clean_enum_symbol(sym)
            if disp:
                symbols[sym] = (kind, disp, num)
        for m in const_float_pat.finditer(cleaned):
            floats[m.group(1)] = float(m.group(2))

    return symbols, floats


def parse_strategy_triggers(files_text: Dict[Path, str]) -> List[TriggerSpec]:
    """Parse TriggerNode registrations from *Strategy.cpp files."""
    specs: List[TriggerSpec] = []
    # Match TriggerNode("name", { NextAction("act", ...), ... })
    trig_node_pat = re.compile(r'new\s+TriggerNode\s*\(\s*"([^"]+)"\s*,')
    next_act_pat = re.compile(r'NextAction\s*\(\s*"([^"]+)"')

    for path, text in sorted(files_text.items()):
        if "Strategy" not in path.name or path.suffix != ".cpp":
            continue
        cleaned = strip_cpp_comments(text)
        lines = cleaned.splitlines()
        # Track // Boss Name comments by line number.
        boss_by_line: Dict[int, str] = {}
        current_boss = ""
        for idx, line in enumerate(lines, 1):
            stripped = line.strip()
            if stripped.startswith("//"):
                comment = stripped.lstrip("/").strip()
                # Ignore TODO/license/notes comments; short 1-4 word comments are boss headers.
                if (
                    comment
                    and not comment.lower().startswith(("todo", "fixme", "note", "triggers.", "boss taunt"))
                    and len(comment.split()) <= 4
                    and re.match(r"^[A-Za-z][A-Za-z0-9'\- ]+$", comment)
                ):
                    current_boss = title_case_name(comment)
            boss_by_line[idx] = current_boss

        # Walk through non-comment lines to find TriggerNode calls.
        non_comment = "\n".join(
            "" if ln.lstrip().startswith("//") else ln for ln in lines
        )
        for m in trig_node_pat.finditer(non_comment):
            trig_name = m.group(1)
            line_no = non_comment[: m.start()].count("\n") + 1
            # Grab up to the closing `));` of push_back.
            end_idx = non_comment.find("));", m.end())
            chunk = non_comment[m.end() : end_idx if end_idx != -1 else m.end() + 300]
            actions = next_act_pat.findall(chunk)
            specs.append(
                TriggerSpec(
                    name=trig_name,
                    actions=actions,
                    boss_hint=boss_by_line.get(line_no, ""),
                )
            )
    return specs


def infer_boss_name(spec: TriggerSpec, instance_code: str, instance_display: str) -> str:
    """Infer the boss name from trigger/action code, comments, or trigger name."""
    combined = spec.trigger_body + "\n" + "\n".join(spec.action_bodies)
    # 1. Explicit `AI_VALUE2(Unit*, "find target", "<boss>")`
    m = re.search(r'"find target"\s*,\s*"([^"]+)"', combined)
    if m:
        return title_case_name(m.group(1))
    # 2. Helper calls like `GetSelin(bot)`, `GetKaelthas(bot)`
    m = re.search(r"\bGet(Selin|Kaelthas|Delrissa|Loken|Ionar|Volkhan|Bronjahm|Anzu|Ikiss|Syth)\s*\(", combined)
    if m:
        return title_case_name(m.group(1))
    # 3. Section comment above TriggerNode
    if spec.boss_hint:
        return spec.boss_hint
    # 4. Known boss keywords in trigger name
    low = spec.name.lower()
    for kw, display in (
        ("kael", "Kael'thas Sunstrider"),
        ("flame strike", "Kael'thas Sunstrider"),
        ("phoenix", "Kael'thas Sunstrider"),
        ("gravity lapse", "Kael'thas Sunstrider"),
        ("delrissa", "Priestess Delrissa"),
        ("crystal", "Selin Fireheart"),
        ("out of room", "Selin Fireheart"),
        ("bronjahm", "Bronjahm"),
        ("soul fragment", "Bronjahm"),
        ("devourer of souls", "Devourer of Souls"),
        ("loken", "Loken"),
        ("lightning nova", "Loken"),
        ("ionar", "Ionar"),
        ("static overload", "Ionar"),
        ("ball lightning", "Ionar"),
        ("bjarngrim", "General Bjarngrim"),
        ("stormforged lieutenant", "General Bjarngrim"),
        ("whirlwind", "General Bjarngrim"),
        ("volkhan", "Volkhan"),
        ("talon king ikiss", "Talon King Ikiss"),
        ("anzu", "Anzu"),
        ("darkweaver syth", "Darkweaver Syth"),
        ("sethekk prophet", "Trash / Sethekk Prophet"),
        ("time-lost controller", "Trash / Time-Lost Controller"),
    ):
        if kw in low:
            return display
    # 5. Strip instance prefix from trigger name if present (e.g. "icc sindragosa ...")
    prefix = instance_code.lower() + " "
    if low.startswith(prefix):
        rest = low[len(prefix) :].split()
        if rest and rest[0] in WHOLE_PHRASE_ALIASES:
            return WHOLE_PHRASE_ALIASES[rest[0]]
    return instance_display


def extract_radius(code: str, floats: Dict[str, float], mech_name: str) -> Optional[float]:
    """Extract a radius/distance in yards from action/trigger code or float constants."""
    m_rad = re.search(r"\bradius\s*=\s*(\d+(?:\.\d+)?)f?", code)
    m_ext = re.search(r"\bdistanceExtra\s*=\s*(\d+(?:\.\d+)?)f?", code)
    if m_rad:
        val = float(m_rad.group(1))
        if m_ext:
            val += float(m_ext.group(1))
        return round(val, 1)
    m_flee = re.search(r"FleePosition\s*\([^,]+,\s*(\d+(?:\.\d+)?)f?", code)
    if m_flee:
        return round(float(m_flee.group(1)), 1)
    m_away = re.search(r"MoveAway\s*\([^,]+,\s*(\d+(?:\.\d+)?)f?", code)
    if m_away:
        return round(float(m_away.group(1)), 1)

    # Check instance float constants matching mechanic tokens (e.g. FLAMESTRIKE_CLEAR, NOVA_CLEAR).
    tokens = [t.upper() for t in re.findall(r"[A-Za-z]+", mech_name) if len(t) >= 4]
    joined = "".join(tokens)
    for cname, val in floats.items():
        cu = cname.upper()
        if not any(s in cu for s in ("CLEAR", "RADIUS", "STAND", "DAMAGE", "RANGE")):
            continue
        if (joined and joined in cu) or any(t in cu for t in tokens):
            if 2.0 <= val <= 45.0:
                return round(val, 1)
    return None


def find_referenced_symbols(
    code: str, symbols: Dict[str, Tuple[str, str, int]]
) -> List[Tuple[str, str, str]]:
    """Return list of (sym, kind, display_name) referenced in `code`."""
    found: List[Tuple[str, str, str]] = []
    seen: Set[str] = set()
    for sym, (kind, disp, _) in symbols.items():
        if sym in code and sym not in seen:
            seen.add(sym)
            found.append((sym, kind, disp))
    # Also check `#define SPELL_...` macros without _N/_H suffix
    for m in re.finditer(r"\b((?:SPELL|NPC)_[A-Z0-9_]+)\b", code):
        sym = m.group(1)
        if sym in seen:
            continue
        for candidate in (sym, sym + "_N", sym + "_H", sym + "_10", sym + "_25"):
            if candidate in symbols:
                kind, disp, _ = symbols[candidate]
                seen.add(sym)
                found.append((sym, kind, disp))
                break
        else:
            kind, disp = clean_enum_symbol(sym)
            if disp:
                seen.add(sym)
                found.append((sym, kind, disp))
    return found


def find_string_auras(code: str) -> List[str]:
    """Extract string aura names from GetAura("...", ...) or HasAura("...")."""
    auras: List[str] = []
    for m in re.finditer(r'(?:GetAura|HasAura)\s*\(\s*"([^"]+)"', code):
        name = title_case_name(m.group(1))
        if name and name not in auras:
            auras.append(name)
    return auras


def _fmt_yd(radius: Optional[float]) -> str:
    if radius is None:
        return ""
    return f"{int(radius)}" if radius == int(radius) else f"{radius:.1f}"


def classify_trigger(
    spec: TriggerSpec,
    instance_code: str,
    instance_display: str,
    symbols: Dict[str, Tuple[str, str, int]],
    floats: Dict[str, float],
) -> List[Mechanic]:
    """Map one TriggerSpec (+ its trigger/action bodies) to zero or more Mechanics."""
    boss = infer_boss_name(spec, instance_code, instance_display)
    trig_low = spec.name.lower()
    acts_low = " ".join(spec.actions).lower()
    all_names_low = trig_low + " " + acts_low
    code = spec.trigger_body + "\n" + "\n".join(spec.action_bodies)
    refs = find_referenced_symbols(code, symbols)
    str_auras = find_string_auras(code)

    mechanics: List[Mechanic] = []

    # 1. Facing / stop attacking (e.g., Devourer of Souls Mirrored Soul)
    if (
        "SetFacingTo(" in code
        and ("M_PI" in code or "oppositeAngle" in code or "face away" in code.lower())
        and (str_auras or any(k == "spell" for _, k, _ in refs))
    ):
        aura_name = str_auras[0] if str_auras else next(d for _, k, d in refs if k == "spell")
        order = f"{aura_name} on boss -> face away boss, then stop attacking"
        mechanics.append(
            Mechanic(
                boss=boss,
                name=aura_name,
                kind="facing",
                description=f"When `{aura_name}` is active on {boss}, face away and stop attacking.",
                order=order if is_valid_order(order) else None,
                source_trigger=spec.name,
            )
        )
        return mechanics

    # Skip non-combat / gear / buff / safe-area checks that are not hazards.
    if any(w in all_names_low for w in ("resistance", "safe area", "safe spot")):
        return mechanics

    # 2. Ground hazards (require a concrete spell/trigger-NPC symbol or a
    # specific multi-word hazard name — never bare generic keywords).
    matched_hazard = next((kw for kw in GROUND_HAZARD_KW if kw in all_names_low), None)
    matched_aoe = next((kw for kw in CAST_AOE_KW if kw in all_names_low), None)
    has_hazard_ref = any(k == "hazard_npc" for _, k, _ in refs)
    has_spell_ref = any(k == "spell" for _, k, _ in refs)
    if (
        matched_hazard
        or (
            not matched_aoe
            and (has_hazard_ref or has_spell_ref)
            and (
                "escape" in code.lower()
                or "leave " in acts_low
                or has_hazard_ref
            )
        )
    ):
        # Prefer actual game symbols (hazard_npc / spell) over keyword strings.
        hazard_name = ""
        if has_hazard_ref:
            hazard_name = next(d for _, k, d in refs if k == "hazard_npc")
        elif has_spell_ref:
            hazard_name = next(d for _, k, d in refs if k == "spell")
        elif matched_hazard:
            hazard_name = title_case_name(matched_hazard)
        if hazard_name and not any(m.name.lower() == hazard_name.lower() for m in mechanics):
            rad = extract_radius(code, floats, hazard_name)
            order = f"in {hazard_name} -> out of it"
            rad_txt = f" (~{_fmt_yd(rad)} yd)" if rad else ""
            mechanics.append(
                Mechanic(
                    boss=boss,
                    name=hazard_name,
                    kind="ground_hazard",
                    description=f"Ground hazard{rad_txt}; move out immediately.",
                    order=order if is_valid_order(order) else None,
                    radius_yd=rad,
                    source_trigger=spec.name,
                )
            )
            return mechanics

    # 3. Cast AoE / LoS / Dodge (boss casting spell + avoid/move away/los/spread)
    is_cast_check = (
        "UNIT_STATE_CASTING" in code
        or "FindCurrentSpellBySpellId" in code
        or "casting " in trig_low
        or matched_aoe is not None
    )
    is_dodge_act = any(
        w in (acts_low + " " + code.lower())
        for w in ("avoid", "moveaway", "fleeposition", "escape", "los", "clear", "spread", "move from")
    )
    if is_cast_check and is_dodge_act:
        spell_name = ""
        if matched_aoe:
            spell_name = title_case_name(matched_aoe)
        elif any(k == "spell" for _, k, _ in refs):
            spell_name = next(d for _, k, d in refs if k == "spell")
        else:
            cleaned = re.sub(r".*\bcasting\s+", "", trig_low)
            spell_name = title_case_name(cleaned)
        if spell_name:
            rad = extract_radius(code, floats, spell_name)
            # If the fight also stacks/returns to boss (e.g. Loken, Ikiss LoS), emit `out of it, then back`.
            if "los" in acts_low or "lightning nova" in spell_name.lower():
                order = f"{spell_name} cast -> out of it, then back"
            else:
                order = f"{spell_name} cast -> out of it"
            rad_txt = f" ({_fmt_yd(rad)} yd radius)" if rad else ""
            mechanics.append(
                Mechanic(
                    boss=boss,
                    name=spell_name,
                    kind="cast_aoe",
                    description=f"Boss cast AoE{rad_txt}; move out during cast.",
                    order=order if is_valid_order(order) else None,
                    radius_yd=rad,
                    source_trigger=spec.name,
                )
            )
            return mechanics

    # 4. Totem / utility spell reactions (e.g. Tremor Totem on fear)
    if "tremor totem" in all_names_low:
        order = "always -> use Tremor Totem"
        mechanics.append(
            Mechanic(
                boss=boss,
                name="Tremor Totem",
                kind="debuff",
                description="Keep Tremor Totem down for Fear.",
                order=order if is_valid_order(order) else None,
                source_trigger=spec.name,
            )
        )
        return mechanics

    # 5. Interrupts
    if "interrupt" in all_names_low or "CastInterrupt" in code:
        spell_refs = [d for _, k, d in refs if k == "spell"]
        label = ", ".join(spell_refs[:3]) if spell_refs else title_case_name(spec.name)
        order = None
        if spell_refs:
            cand = f"{spell_refs[0]} cast -> target it"
            if is_valid_order(cand):
                order = cand
        mechanics.append(
            Mechanic(
                boss=boss,
                name=f"Interrupt ({label})" if spell_refs else label,
                kind="interrupt",
                description=f"Priority interrupt target: {label}.",
                order=order,
                source_trigger=spec.name,
            )
        )
        return mechanics

    # 6. Add target switches
    add_npcs = [d for _, k, d in refs if k == "npc" and d.lower() != boss.lower()]
    matched_add_kw = next((kw for kw in ADD_SWITCH_KW if kw in all_names_low), None)
    is_target_act = any(
        w in (acts_low + " " + code.lower())
        for w in ("attack(", "kill ", "target", "focus", "mark", "skull", "switch to")
    )
    if (add_npcs or matched_add_kw) and is_target_act:
        add_name = add_npcs[0] if add_npcs else title_case_name(matched_add_kw or spec.name)
        order = f"{add_name} appears -> target nearest {add_name}"
        mechanics.append(
            Mechanic(
                boss=boss,
                name=add_name,
                kind="add_switch",
                description=f"Priority add; switch targets to `{add_name}` immediately.",
                order=order if is_valid_order(order) else None,
                source_trigger=spec.name,
            )
        )
        return mechanics

    # 7. Kiting
    if "kite" in all_names_low or "KiteState" in code:
        target_name = add_npcs[0] if add_npcs else boss
        order = f"{target_name} targets me -> kite it"
        mechanics.append(
            Mechanic(
                boss=boss,
                name=f"Kite {target_name}",
                kind="kite",
                description=f"Kite `{target_name}` away from the group.",
                order=order if is_valid_order(order) else None,
                source_trigger=spec.name,
            )
        )
        return mechanics

    # 8. Debuff / Aura reactions (HasAura / GetAura)
    if "HasAura" in code or "GetAura" in code or "aura" in trig_low:
        aura_name = ""
        if any(k == "spell" for _, k, _ in refs):
            aura_name = next(d for _, k, d in refs if k == "spell")
        elif str_auras:
            aura_name = str_auras[0]
        if aura_name and not any(m.name.lower() == aura_name.lower() for m in mechanics):
            rad = extract_radius(code, floats, aura_name)
            yd_str = _fmt_yd(rad) or "10"
            # Determine whether the aura is checked on the boss/target vs on the bot.
            on_boss = bool(
                re.search(r"\b(?:boss|target|unit)\s*->\s*HasAura\b", code)
                or re.search(r"HasAura\s*\([^,]+,\s*(?:boss|target|Get[A-Z][A-Za-z]+\()", code)
                or "boss has" in trig_low
            )
            who = "boss" if on_boss else "me"
            is_gather = any(
                w in (acts_low + " " + code.lower())
                for w in ("gather", "stack", "move to", "moveto(")
            )
            order: Optional[str] = None
            if is_gather:
                desc = f"Aura `{aura_name}` ({who}); gather/position per encounter script (hand-check)."
            elif not on_boss and ("spread" in acts_low or "MoveAway" in code):
                order = f"{aura_name} on me -> away group {yd_str}"
                desc = f"Debuff; spread {yd_str} yd away from allies."
            elif not on_boss and ("FleePosition" in code or "move from" in acts_low):
                order = f"{aura_name} on me -> away boss {yd_str}"
                desc = f"Debuff; run {yd_str} yd away from {boss}."
            elif "totem" in all_names_low:
                order = f"{aura_name} cast -> use Tremor Totem"
                desc = "Fear/debuff mechanic; drop Tremor Totem."
            else:
                # Never invent a fallback `away boss 10` order when the script's
                # response is not clearly an away/spread/totem action.
                desc = f"Aura `{aura_name}` on {who}; hand-check encounter reaction."
            mechanics.append(
                Mechanic(
                    boss=boss,
                    name=aura_name,
                    kind="debuff",
                    description=desc,
                    order=order if (order and is_valid_order(order)) else None,
                    radius_yd=rad,
                    source_trigger=spec.name,
                )
            )
            return mechanics

    # 9. Generic positioning (stack / disperse / room leash / ranged stack)
    if any(w in all_names_low for w in ("stack", "position", "disperse", "out of room", "gravity lapse")):
        if "stack" in all_names_low or "near tank" in acts_low:
            order = "always -> to boss"
            desc = f"Stack near {boss} / tank."
        elif "disperse" in all_names_low:
            order = "Disperse cast -> away boss 15"
            desc = "Boss disperses into sparks; move to safe disperse position."
        elif "out of room" in trig_low:
            order = "boss beyond 30 -> to boss"
            desc = "Stay inside the boss room to avoid leashing/resetting."
        else:
            order = None
            desc = f"Encounter positioning ({spec.name})."
        mechanics.append(
            Mechanic(
                boss=boss,
                name=title_case_name(spec.name),
                kind="positioning",
                description=desc,
                order=order if (order and is_valid_order(order)) else None,
                source_trigger=spec.name,
            )
        )
        return mechanics

    return mechanics


def extract_shared_supplementary_mechanics(
    files_text: Dict[Path, str],
    symbols: Dict[str, Tuple[str, str, int]],
    floats: Dict[str, float],
    existing: List[Mechanic],
    instance_display: str,
) -> List[Mechanic]:
    """Supplement with key adds/hazards declared in *Shared.cpp/*Triggers.h not directly in trigger bodies.

    For example, in MgT, `NPC_PHOENIX_EGG` and `NPC_FLAMESTRIKE_TRIGGER` are referenced
    inside `MgTShared.cpp` helper functions called indirectly via AI values.
    """
    existing_names = {m.name.lower() for m in existing}
    extra: List[Mechanic] = []
    all_code = "\n".join(strip_cpp_comments(t) for t in files_text.values())

    for sym, (kind, disp, _) in symbols.items():
        if disp.lower() in existing_names:
            continue
        # Ground hazard trigger NPCs (e.g. NPC_FLAMESTRIKE_TRIGGER)
        if kind == "hazard_npc":
            rad = extract_radius(all_code, floats, disp)
            order = f"in {disp} -> out of it"
            if is_valid_order(order):
                extra.append(
                    Mechanic(
                        boss=infer_boss_from_symbol(sym, disp, instance_display),
                        name=disp,
                        kind="ground_hazard",
                        description=f"Ground hazard trigger (`{sym}`); move out immediately.",
                        order=order,
                        radius_yd=rad,
                        source_trigger=sym,
                    )
                )
                existing_names.add(disp.lower())
        # Priority adds like Phoenix Egg, Fel Crystal, Charming Totem
        elif kind == "npc" and any(kw in disp.lower() for kw in ("egg", "crystal", "totem", "fragment", "spike", "bomb")):
            order = f"{disp} appears -> target nearest {disp}"
            if is_valid_order(order):
                extra.append(
                    Mechanic(
                        boss=infer_boss_from_symbol(sym, disp, instance_display),
                        name=disp,
                        kind="add_switch",
                        description=f"Priority add (`{sym}`); switch and burn immediately.",
                        order=order,
                        source_trigger=sym,
                    )
                )
                existing_names.add(disp.lower())
        # Known cast-AoE spells declared in enums (e.g. SPELL_POISON_NOVA on Ick)
        elif kind == "spell" and any(kw in disp.lower() for kw in CAST_AOE_KW):
            rad = extract_radius(all_code, floats, disp)
            order = f"{disp} cast -> out of it"
            if is_valid_order(order):
                extra.append(
                    Mechanic(
                        boss=infer_boss_from_symbol(sym, disp, instance_display),
                        name=disp,
                        kind="cast_aoe",
                        description=f"Boss cast AoE (`{sym}`); move out during cast.",
                        order=order,
                        radius_yd=rad,
                        source_trigger=sym,
                    )
                )
                existing_names.add(disp.lower())

    return extra


def infer_boss_from_symbol(sym: str, disp: str, instance_display: str) -> str:
    low = (sym + " " + disp).lower()
    if any(k in low for k in ("phoenix", "flamestrike", "flame strike", "arcane sphere")):
        return "Kael'thas Sunstrider"
    if "fel crystal" in low:
        return "Selin Fireheart"
    if "soul fragment" in low:
        return "Bronjahm"
    if "charming totem" in low:
        return "Trash / Time-Lost Controller"
    return instance_display


def extract_instance(instance_dir: Path, category: str) -> InstanceNotes:
    """Parse all C++ files in one instance directory and return InstanceNotes."""
    code = instance_dir.name
    display_name = INSTANCE_NAMES.get(code, title_case_name(code))

    files_text: Dict[Path, str] = {}
    for p in sorted(instance_dir.rglob("*")):
        if p.is_file() and p.suffix in (".h", ".hpp", ".cpp", ".cc"):
            try:
                files_text[p] = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue

    symbols, floats = parse_enums_and_constants(files_text)
    methods = parse_methods(files_text)
    trig_map, act_map = parse_name_to_class(files_text)
    specs = parse_strategy_triggers(files_text)

    mechanics: List[Mechanic] = []
    mapped_triggers = 0

    for spec in specs:
        t_cls = trig_map.get(spec.name)
        if t_cls:
            spec.trigger_body = methods.get(f"{t_cls}::IsActive", "")
        for act in spec.actions:
            a_cls = act_map.get(act)
            if a_cls:
                body = methods.get(f"{a_cls}::Execute", "")
                if body:
                    spec.action_bodies.append(body)

        found = classify_trigger(spec, code, display_name, symbols, floats)
        if found:
            if any(m.order for m in found):
                mapped_triggers += 1
            mechanics.extend(found)

    # Supplement from shared helpers / enums (e.g. Phoenix Egg in MgTShared.cpp)
    mechanics.extend(
        extract_shared_supplementary_mechanics(files_text, symbols, floats, mechanics, display_name)
    )

    # Deduplicate by (boss.lower(), name.lower(), order)
    deduped: List[Mechanic] = []
    seen_keys: Set[Tuple[str, str]] = set()
    for m in mechanics:
        key = (m.boss.lower(), m.name.lower())
        if key in seen_keys:
            continue
        seen_keys.add(key)
        deduped.append(m)

    return InstanceNotes(
        code=code,
        display_name=display_name,
        category=category,
        triggers_parsed=len(specs),
        triggers_mapped=mapped_triggers,
        mechanics=deduped,
    )


def render_notes_markdown(notes: InstanceNotes) -> str:
    """Render concise draft notes.md markdown grouped by boss."""
    lines: List[str] = [
        f"# {notes.display_name} ({notes.code})",
        "",
        f"<!-- DRAFT extracted from mod-playerbots ({notes.category}/{notes.code}); hand-check before use. -->",
        "",
    ]
    if not notes.mechanics:
        lines.append("_No boss mechanics extracted._")
        lines.append("")
        return "\n".join(lines)

    by_boss: Dict[str, List[Mechanic]] = {}
    for m in notes.mechanics:
        by_boss.setdefault(m.boss, []).append(m)

    for boss, mechs in by_boss.items():
        lines.append(f"## {boss}")
        lines.append("")
        for m in mechs:
            lines.append(f"- **{m.name}** ({m.kind}): {m.description}")
        orders = [m.order for m in mechs if m.order]
        if orders:
            lines.append("")
            lines.append("```")
            for o in dict.fromkeys(orders):
                lines.append(o)
            lines.append("```")
        lines.append("")

    return "\n".join(lines)


def discover_instance_dirs(source_root: Path, only: Optional[str] = None) -> List[Tuple[str, Path]]:
    """Return sorted list of (category, instance_dir) under Ai/Dungeon and Ai/Raid."""
    only_set: Optional[Set[str]] = None
    if only:
        only_set = {x.strip().lower() for x in only.split(",") if x.strip()}

    found: List[Tuple[str, Path]] = []
    for category in ("Dungeon", "Raid"):
        cat_dir = source_root / category
        if not cat_dir.is_dir():
            continue
        for child in sorted(cat_dir.iterdir()):
            if not child.is_dir():
                continue
            if only_set is not None and child.name.lower() not in only_set:
                continue
            found.append((category, child))
    return found


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Extract draft boss notes and .wlb standing orders from mod-playerbots."
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("third_party/mod-playerbots/src/Ai"),
        help="Path to mod-playerbots src/Ai directory.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output directory for generated <instance>/notes.md files.",
    )
    parser.add_argument(
        "--only",
        type=str,
        default=None,
        help="Comma-separated instance folder codes to process (e.g. MgT,HoL,FoS,Seth).",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="Print coverage statistics (instances, triggers parsed, orders mapped).",
    )
    args = parser.parse_args(argv)

    instance_dirs = discover_instance_dirs(args.source, args.only)
    if not instance_dirs:
        print(f"No instance directories found under {args.source}", file=sys.stderr)
        return 1

    all_notes: List[InstanceNotes] = []
    for category, inst_dir in instance_dirs:
        notes = extract_instance(inst_dir, category)
        all_notes.append(notes)
        md = render_notes_markdown(notes)
        if args.out is not None:
            out_path = args.out / notes.code / "notes.md"
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(md, encoding="utf-8")
        elif not args.summary:
            print(md)

    if args.summary:
        total_inst = len(all_notes)
        total_trig = sum(n.triggers_parsed for n in all_notes)
        total_mapped = sum(n.triggers_mapped for n in all_notes)
        total_orders = sum(sum(1 for m in n.mechanics if m.order) for n in all_notes)
        pct = (100.0 * total_mapped / total_trig) if total_trig else 0.0
        print(
            f"Instances: {total_inst} | Triggers parsed: {total_trig} | "
            f"Triggers mapped to orders: {total_mapped} ({pct:.1f}%) | "
            f"Total standing orders emitted: {total_orders}"
        )
        for n in all_notes:
            ord_cnt = sum(1 for m in n.mechanics if m.order)
            print(
                f"  {n.category:7s} {n.code:6s} ({n.display_name}): "
                f"{n.triggers_parsed} triggers, {n.triggers_mapped} mapped, {ord_cnt} orders"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
