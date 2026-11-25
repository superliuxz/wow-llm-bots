"""Live multi-bot soak test + human-likeness scorer on the isolated test server.

Supports both open-world questing and rotated 5-man Tank/Healer/3DPS dungeon
runs across varied classes, factions, levels, and zones, with real
`out/common/harness run` + real `llama-server` (Gemma 4) and unified behavior
tracing (`var/soak_traces/`).

Suites (`--suite`):
  - `varied` (default): 2 varied runs per evaluation —
      1. `soak_alliance_dm`: 5 Alliance questing classes across 5 zones +
         5-man Alliance Tank/Healer/3DPS in Deadmines (L20)
      2. `soak_horde_rfc`: 5 Horde questing classes across 5 zones +
         5-man Horde Tank/Healer/3DPS in Ragefire Chasm (L15)
  - `heldout`: generalization check (never used for hill-climb tuning) —
      1. `soak_northrend_hol`: 5 mid/high-level Horde questing bots +
         5-man Halls of Lightning (L80)
      2. `soak_heldout_uk`: 5 unseen Alliance questing classes +
         5-man Utgarde Keep (L70)
  - `endurance`: 6 Classic + TBC + WotLK scenarios (quest+professions + hard 5-man
    dungeons across leveling brackets and endgame/heroic caps) —
      1. `soak_classic_ulda`: L30-L45 Classic quest+prof + Uldaman (L42)
      2. `soak_classic_strat`: L50-L58 Classic quest+prof + Stratholme (L60)
      3. `soak_tbc_seth`: L60-L66 TBC quest+prof + Sethekk Halls (L67)
      4. `soak_tbc_mgt`: L65-L69 TBC quest+prof + Magisters' Terrace (L70 Heroic)
      5. `soak_wotlk_ak`: L70-L75 WotLK quest+prof + Ahn'kahet (L74)
      6. `soak_wotlk_pos`: L75-L78 WotLK quest+prof + Pit of Saron (L80 Heroic)
  - `quest`: legacy 10-bot level-1 Priest Valley of Trials soak (`soak`)
  - Or any single scenario name above.
"""

import contextlib
import json
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

from wlblib import build, server
from wlblib.common import OUT, ROOT, TEST, VAR, say
from wlblib.testing import LlamaServer, Stack, expect

sys.path.insert(0, str(ROOT / "tools" / "bench"))
import soak_scorer  # noqa: E402

NAME = "soak"
DESCRIPTION = "varied questing + 5-man Tank/Healer/3DPS dungeon soak + scorer"
TEST_CONFIG = "soak"
BOT = "Loada"
ONLINE = 10

DUNGEON_PARTY = ["Loadf", "Loadg", "Loadh", "Loadi", "Loadj"]

# Verified WotLK 3.3.5a profession definitions (Spell.dbc + item_template):
#   name -> (skill_id, [(cap, learn_spell_id), ...], extra_spells, tool_item_id)
PROFESSIONS = {
    "herbalism": (
        182,
        [
            (75, 2366),
            (150, 2368),
            (225, 3570),
            (300, 11993),
            (375, 28695),
            (450, 50300),
        ],
        (2383,),  # Find Herbs
        None,
    ),
    "mining": (
        186,
        [
            (75, 2575),
            (150, 2576),
            (225, 3564),
            (300, 10248),
            (375, 29354),
            (450, 50310),
        ],
        (2580,),  # Find Minerals
        2901,  # Mining Pick
    ),
    "skinning": (
        393,
        [
            (75, 8613),
            (150, 8617),
            (225, 8618),
            (300, 10768),
            (375, 32678),
            (450, 50305),
        ],
        (),
        7005,  # Skinning Knife
    ),
    "first_aid": (
        129,
        [
            (75, 3273),
            (150, 3274),
            (225, 7924),
            (300, 10846),
            (375, 27028),
            (450, 45542),
        ],
        (),
        None,
    ),
    "cooking": (
        185,
        [
            (75, 2550),
            (150, 3102),
            (225, 3413),
            (300, 18260),
            (375, 33359),
            (450, 51296),
        ],
        (),
        None,
    ),
    "fishing": (
        356,
        [
            (75, 7620),
            (150, 7731),
            (225, 7732),
            (300, 18248),
            (375, 33095),
            (450, 51294),
        ],
        (),
        6256,  # Fishing Pole
    ),
}

# Each scenario specifies its test config override plus optional per-bot gear/tele
# and 5-man dungeon party setup run via `llmtest` right after login.
SCENARIOS = {
    "soak": {
        "config": "soak",
        "desc": "10x L1 Priest questing (Valley of Trials)",
        "quest_setup": [],
        "dungeon": None,
    },
    "soak_horde_rfc": {
        "config": "soak_horde_rfc",
        "desc": "5 Horde questing (War/Dru/Mag/Pal/Hun) + 5-man RFC L15 (War/Pri/Rog/Mag/Sha)",
        "quest_setup": [
            # Loade: L15 Troll Hunter at The Sepulcher (Silverpine)
            ("Loade", 15, (0, 504.534, 1539.08, 129.502)),
        ],
        "dungeon": {
            "name": "Ragefire Chasm",
            "level": 15,
            "map": 389,
            "pos": (3.81, -14.82, -17.84),
        },
    },
    "soak_alliance_dm": {
        "config": "soak_alliance_dm",
        "desc": "5 Alliance questing (Pal/Hun/War/Loc/Sha) + 5-man Deadmines L20 (Pal/Dru/Rog/Mag/Hun)",
        "quest_setup": [
            # Loade: L15 Draenei Shaman at Sentinel Hill (Westfall)
            ("Loade", 15, (0, -10624.5, 1096.66, 33.7641)),
        ],
        "dungeon": {
            "name": "Deadmines",
            "level": 20,
            "map": 36,
            "pos": (-16.4, -383.07, 61.78),
            # ProtPal(1), RestoDru(2), CombatRog(1), FrostMag(2), BMHun(0)
            "spec_tabs": [1, 2, 1, 2, 0],
        },
    },
    "soak_northrend_hol": {
        "config": "soak_northrend_hol",
        "desc": "5 mid/high Horde questing (L15-L70) + 5-man Halls of Lightning L80",
        "quest_setup": [
            ("Loada", 15, (0, 504.534, 1539.08, 129.502)),
            ("Loadb", 35, (0, -12388.9, 172.578, 2.83358)),
            ("Loadc", 35, (0, -12388.9, 172.578, 2.83358)),
            ("Loadd", 70, (571, 2741.29, 6097.16, 76.9055)),
            ("Loade", 70, (571, 2741.29, 6097.16, 76.9055)),
        ],
        "dungeon": {
            "name": "Halls of Lightning",
            "level": 80,
            "map": 602,
            "pos": (1331.47, 259.619, 53.398),
            "notes": "HoL",
        },
    },
    "soak_heldout_uk": {
        "config": "soak_heldout_uk",
        "desc": "HELD-OUT: 5 unseen Alliance questing + 5-man Utgarde Keep L70",
        "quest_setup": [
            ("Loadd", 15, (0, -10624.5, 1096.66, 33.7641)),
        ],
        "dungeon": {
            "name": "Utgarde Keep",
            "level": 70,
            "map": 574,
            "pos": (153.789, -86.548, 12.551),
            "notes": "UK",
        },
    },
    # Endurance suite: 6 Classic + TBC + WotLK scenarios (quest+professions +
    # hard 5-man dungeons with explicit Tank/Healer/3DPS spec tabs).
    "soak_classic_ulda": {
        "config": "soak_classic_ulda",
        "desc": "Classic L30-L45 Horde quest+prof (STV/Tanaris) + 5-man Uldaman L42 (ProtWar/HolyPri/3DPS)",
        "quest_setup": [
            # Loada: L32 Orc Warrior at Nesingwary's Expedition (STV, neutral)
            ("Loada", 32, (0, -11616.7, -54.83, 11.14), [("herbalism", 160), ("skinning", 160), ("first_aid", 160)]),
            # Loadb: L36 Tauren Druid at Grom'gol Base Camp (STV, Horde)
            ("Loadb", 36, (0, -12347.4, 161.43, 3.24), [("mining", 180), ("skinning", 180), ("cooking", 180)]),
            # Loadc: L38 Undead Warlock at Booty Bay (STV, neutral)
            ("Loadc", 38, (0, -14453.3, 501.46, 26.36), [("herbalism", 190), ("mining", 190), ("fishing", 190)]),
            # Loadd: L42 Blood Elf Paladin at Grom'gol Base Camp (STV, Horde)
            ("Loadd", 42, (0, -12347.4, 161.43, 3.24), [("skinning", 210), ("first_aid", 210), ("cooking", 210)]),
            # Loade: L45 Troll Hunter at Gadgetzan (Tanaris, neutral)
            ("Loade", 45, (1, -7139.19, -3683.27, 21.03), [("herbalism", 220), ("fishing", 220), ("first_aid", 220)]),
        ],
        "dungeon": {
            "name": "Uldaman",
            "level": 42,
            "map": 70,
            "pos": (-226.8, 49.09, -46.03),
            # ProtWar(2), HolyPri(1), CombatRog(1), FrostMag(2), BMHun(0)
            "spec_tabs": [2, 1, 1, 2, 0],
            "notes": "Ulda",
        },
    },
    "soak_classic_strat": {
        "config": "soak_classic_strat",
        "desc": "Classic L50-L58 Alliance quest+prof (Tanaris/EPL) + 5-man Stratholme L60 (ProtPal/HolyPri/3DPS)",
        "quest_setup": [
            # Loada: L50 Human Paladin at Gadgetzan (Tanaris, neutral)
            ("Loada", 50, (1, -7139.19, -3683.27, 21.03), [("herbalism", 250), ("skinning", 250), ("first_aid", 250)]),
            # Loadb: L52 Night Elf Druid at Gadgetzan (Tanaris, neutral)
            ("Loadb", 52, (1, -7139.19, -3683.27, 21.03), [("mining", 260), ("skinning", 260), ("cooking", 260)]),
            # Loadc: L55 Dwarf Hunter at Light's Hope Chapel (EPL, neutral)
            ("Loadc", 55, (0, 2257.31, -5340.87, 86.8), [("herbalism", 275), ("mining", 275), ("fishing", 275)]),
            # Loadd: L56 Gnome Warlock at Light's Hope Chapel (EPL, neutral)
            ("Loadd", 56, (0, 2257.31, -5340.87, 86.8), [("skinning", 280), ("first_aid", 280), ("cooking", 280)]),
            # Loade: L58 Draenei Shaman at Light's Hope Chapel (EPL, neutral)
            ("Loade", 58, (0, 2257.31, -5340.87, 86.8), [("herbalism", 290), ("fishing", 290), ("first_aid", 290)]),
        ],
        "dungeon": {
            "name": "Stratholme",
            "level": 60,
            "map": 329,
            # Service / Undead Entrance (areatrigger 2214)
            "pos": (3593.15, -3646.56, 138.5),
            # ProtPal(1), HolyPri(1), CombatRog(1), FrostMag(2), BMHun(0)
            "spec_tabs": [1, 1, 1, 2, 0],
            "notes": "Strat",
        },
    },
    "soak_tbc_seth": {
        "config": "soak_tbc_seth",
        "desc": "TBC L60-L66 Horde quest+prof (Hellfire/Nagrand) + 5-man Sethekk Halls L67 (BearDru/RestoSha/3DPS)",
        "quest_setup": [
            # Loada: L60 Tauren Warrior at Thrallmar (Hellfire Peninsula, Horde)
            ("Loada", 60, (530, 243.0, 2698.2, 89.84), [("herbalism", 300), ("skinning", 300), ("first_aid", 300)]),
            # Loadb: L61 Orc Hunter at Thrallmar (Hellfire Peninsula, Horde)
            ("Loadb", 61, (530, 243.0, 2698.2, 89.84), [("mining", 310), ("skinning", 310), ("cooking", 310)]),
            # Loadc: L64 Blood Elf Mage at Garadar (Nagrand, Horde)
            ("Loadc", 64, (530, -1344.14, 7226.02, 33.59), [("herbalism", 330), ("mining", 330), ("fishing", 330)]),
            # Loadd: L65 Undead Rogue at Garadar (Nagrand, Horde)
            ("Loadd", 65, (530, -1344.14, 7226.02, 33.59), [("skinning", 340), ("first_aid", 340), ("cooking", 340)]),
            # Loade: L66 Troll Shaman at Nesingwary Safari (Nagrand, neutral)
            ("Loade", 66, (530, -1461.78, 6344.3, 37.27), [("herbalism", 350), ("fishing", 350), ("first_aid", 350)]),
        ],
        "dungeon": {
            "name": "Sethekk Halls",
            "level": 67,
            "map": 556,
            "pos": (-4.68, -0.09, 0.01),
            # BearDru(1), RestoSha(2), DestroLoc(2), FireMag(1), CombatRog(1)
            "spec_tabs": [1, 2, 2, 1, 1],
            "notes": "Seth",
        },
    },
    "soak_tbc_mgt": {
        "config": "soak_tbc_mgt",
        "desc": "TBC L65-L69 Alliance quest+prof (Hellfire/Nagrand) + 5-man Magisters' Terrace L70H (ProtWar/RestoDru/3DPS)",
        "quest_setup": [
            # Loada: L65 Dwarf Warrior at Honor Hold (Hellfire Peninsula, Alliance)
            ("Loada", 65, (530, -815.61, 2614.2, 124.47), [("herbalism", 335), ("skinning", 335), ("first_aid", 335)]),
            # Loadb: L66 Night Elf Hunter at Telaar (Nagrand, Alliance)
            ("Loadb", 66, (530, -2568.84, 7271.46, 15.49), [("mining", 345), ("skinning", 345), ("cooking", 345)]),
            # Loadc: L67 Human Mage at Telaar (Nagrand, Alliance)
            ("Loadc", 67, (530, -2568.84, 7271.46, 15.49), [("herbalism", 350), ("mining", 350), ("fishing", 350)]),
            # Loadd: L68 Draenei Paladin at Nesingwary Safari (Nagrand, neutral)
            ("Loadd", 68, (530, -1461.78, 6344.3, 37.27), [("skinning", 355), ("first_aid", 355), ("cooking", 355)]),
            # Loade: L69 Gnome Rogue at Nesingwary Safari (Nagrand, neutral)
            ("Loade", 69, (530, -1461.78, 6344.3, 37.27), [("herbalism", 360), ("fishing", 360), ("first_aid", 360)]),
        ],
        "dungeon": {
            "name": "Magisters' Terrace",
            "level": 70,
            "heroic": True,
            "map": 585,
            "pos": (7.09, -0.45, -2.8),
            # ProtWar(2), RestoDru(2), EleSha(0), DestroLoc(2), MMHun(1)
            "spec_tabs": [2, 2, 0, 2, 1],
            "notes": "MgT",
        },
    },
    "soak_wotlk_ak": {
        "config": "soak_wotlk_ak",
        "desc": "WotLK L70-L75 Horde quest+prof (Borean/Dragonblight/Zul'Drak) + 5-man Ahn'kahet L74 (ProtPal/RestoDru/3DPS)",
        "quest_setup": [
            # Loada: L70 Orc Shaman at Warsong Hold (Borean Tundra, Horde)
            ("Loada", 70, (571, 2835.02, 6182.44, 84.68), [("herbalism", 375), ("skinning", 375), ("first_aid", 375)]),
            # Loadb: L71 Undead Priest at Warsong Hold (Borean Tundra, Horde)
            ("Loadb", 71, (571, 2835.02, 6182.44, 84.68), [("mining", 385), ("skinning", 385), ("cooking", 385)]),
            # Loadc: L73 Tauren Warrior at Agmar's Hammer (Dragonblight, Horde)
            ("Loadc", 73, (571, 3833.26, 1573.53, 86.78), [("herbalism", 395), ("mining", 395), ("fishing", 395)]),
            # Loadd: L74 Blood Elf Hunter at Agmar's Hammer (Dragonblight, Horde)
            ("Loadd", 74, (571, 3833.26, 1573.53, 86.78), [("skinning", 405), ("first_aid", 405), ("cooking", 405)]),
            # Loade: L75 Troll Rogue at The Argent Stand (Zul'Drak, neutral)
            ("Loade", 75, (571, 5450.91, -2610.93, 306.99), [("herbalism", 415), ("fishing", 415), ("first_aid", 415)]),
        ],
        "dungeon": {
            "name": "Ahn'kahet: The Old Kingdom",
            "level": 74,
            "map": 619,
            "pos": (333.35, -1109.94, 69.77),
            # ProtPal(1), RestoDru(2), FuryWar(1), MMHun(1), ArcaneMag(0)
            "spec_tabs": [1, 2, 1, 1, 0],
            "notes": "AK",
        },
    },
    "soak_wotlk_pos": {
        "config": "soak_wotlk_pos",
        "desc": "WotLK L75-L78 Alliance quest+prof (Dragonblight/Zul'Drak/Icecrown) + 5-man Pit of Saron L80H (ProtWar/HolyPri/3DPS)",
        "quest_setup": [
            # Loada: L75 Human Warlock at Wintergarde Keep (Dragonblight, Alliance)
            ("Loada", 75, (571, 3767.5, -775.05, 196.09), [("herbalism", 415), ("skinning", 415), ("first_aid", 415)]),
            # Loadb: L76 Night Elf Druid at The Argent Stand (Zul'Drak, neutral)
            ("Loadb", 76, (571, 5450.91, -2610.93, 306.99), [("mining", 420), ("skinning", 420), ("cooking", 420)]),
            # Loadc: L77 Dwarf Hunter at The Argent Stand (Zul'Drak, neutral)
            ("Loadc", 77, (571, 5450.91, -2610.93, 306.99), [("herbalism", 425), ("mining", 425), ("fishing", 425)]),
            # Loadd: L78 Draenei Shaman at Argent Vanguard (Icecrown, neutral)
            ("Loadd", 78, (571, 6383.94, 246.87, 396.43), [("skinning", 430), ("first_aid", 430), ("cooking", 430)]),
            # Loade: L78 Gnome Mage at Argent Vanguard (Icecrown, neutral)
            ("Loade", 78, (571, 6383.94, 246.87, 396.43), [("herbalism", 430), ("fishing", 430), ("first_aid", 430)]),
        ],
        "dungeon": {
            "name": "Pit of Saron",
            "level": 80,
            "heroic": True,
            "map": 658,
            "pos": (435.74, 212.41, 528.71),
            # ProtWar(2), HolyPri(1), RetPal(2), CombatRog(1), ArcaneMag(0)
            "spec_tabs": [2, 1, 2, 1, 0],
            "notes": "PoS",
        },
    },
}

SUITES = {
    "varied": ["soak_alliance_dm", "soak_horde_rfc"],
    "heldout": ["soak_northrend_hol", "soak_heldout_uk"],
    "endurance": [
        "soak_classic_ulda",
        "soak_classic_strat",
        "soak_tbc_seth",
        "soak_tbc_mgt",
        "soak_wotlk_ak",
        "soak_wotlk_pos",
    ],
    "quest": ["soak"],
}


def _profession_setup_cmds(bot_name: str, prof_name: str, rank: int) -> list[str]:
    """Builds `llmtest learn`/`setskill`/`item` commands for a profession."""
    skill_id, tiers, extra_spells, tool_item = PROFESSIONS[prof_name]
    chosen_cap, chosen_spell = tiers[-1]
    for cap, spell_id in tiers:
        if rank <= cap:
            chosen_cap, chosen_spell = cap, spell_id
            break
    value = max(1, min(int(rank), chosen_cap))
    out = [f"llmtest learn {bot_name} {chosen_spell}"]
    for extra in extra_spells:
        out.append(f"llmtest learn {bot_name} {extra}")
    out.append(f"llmtest setskill {bot_name} {skill_id} {value} {chosen_cap}")
    if tool_item is not None:
        out.append(f"llmtest item {bot_name} {tool_item}")
    return out


def _apply_scenario_setup(scenario: dict) -> None:
    """Runs `llmtest gear` / professions / `group` / `difficulty` / `tele`."""
    cmds = []
    for entry in scenario.get("quest_setup", []):
        if len(entry) == 4:
            bot_name, level, tele, professions = entry
        else:
            bot_name, level, tele = entry
            professions = ()
        cmds.append(f"llmtest gear {bot_name} {level}")
        for prof_name, rank in professions or ():
            cmds.extend(_profession_setup_cmds(bot_name, prof_name, rank))
        if tele:
            m, x, y, z = tele
            cmds.append(f"llmtest tele {bot_name} {m} {x} {y} {z} 0")

    dungeon = scenario.get("dungeon")
    if dungeon:
        leader = DUNGEON_PARTY[0]
        for member in DUNGEON_PARTY[1:]:
            cmds.append(f"llmtest group {leader} {member}")
        if dungeon.get("heroic"):
            cmds.append(f"llmtest difficulty {leader} heroic")
        lvl = dungeon["level"]
        spec_tabs = dungeon.get("spec_tabs")
        if spec_tabs is not None:
            expect(
                len(spec_tabs) == len(DUNGEON_PARTY),
                f"dungeon spec_tabs length {len(spec_tabs)} != {len(DUNGEON_PARTY)}",
            )
            for member, spec_tab in zip(DUNGEON_PARTY, spec_tabs):
                cmds.append(f"llmtest gear {member} {lvl} {spec_tab}")
        else:
            for member in DUNGEON_PARTY:
                cmds.append(f"llmtest gear {member} {lvl}")

    if not cmds:
        return

    for cmd in cmds:
        server.console(cmd, inst=TEST)

    if dungeon:
        # Let gear/spell setup settle briefly before teleporting the 5-man party
        # into the shared dungeon instance.
        time.sleep(2.0)
        m = dungeon["map"]
        x, y, z = dungeon["pos"]
        for member in DUNGEON_PARTY:
            server.console(f"llmtest tele {member} {m} {x} {y} {z} 0", inst=TEST)
        time.sleep(3.0)


def _classify_option_text(text: str) -> str:
    low = text.strip().lower()
    if low.startswith("keep doing"):
        return "Keep"
    if low.startswith("attack ") or low.startswith("assist "):
        return "Attack"
    if low.startswith("loot "):
        return "Loot"
    if low.startswith("talk to ") or low.startswith("accept quest ") or low.startswith("turn in "):
        return "Talk"
    if low.startswith("explore"):
        return "Explore"
    if low.startswith("go to ") or low.startswith("follow "):
        return "GoTo"
    if low.startswith("rest"):
        return "Rest"
    if low.startswith("gather "):
        return "Gather"
    if low.startswith("equip "):
        return "Equip"
    if low.startswith("spend talent"):
        return "Talent"
    if low.startswith("use ") or low.startswith("buy ") or low.startswith("train ") or low.startswith("sell ") or low.startswith("repair ") or low.startswith("fly ") or low.startswith("mount "):
        return "Use"
    return "Other"


def _extract_first_option_kind(decision: dict) -> str | None:
    """Extracts the option category of step 1 from a decision log record."""
    plan_text = decision.get("plan")
    if not isinstance(plan_text, str):
        return None
    do_line = next((ln for ln in plan_text.splitlines() if ln.startswith("do:")), "")
    if not do_line:
        return None
    first_tok = do_line[3:].strip().split(",")[0].strip().split("x")[0].strip()
    if not first_tok.isdigit():
        return None
    opt_num = int(first_tok)
    prompt = decision.get("prompt_user", "")
    if not isinstance(prompt, str):
        return None
    prefix = f"  {opt_num}. "
    for ln in prompt.splitlines():
        if ln.startswith(prefix):
            return _classify_option_text(ln[len(prefix):])
    return None


def _run_once(
    scenario_name: str,
    minutes: float,
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
    temperature: float = 0.7,
    no_brain: bool = False,
) -> dict:
    scenario = SCENARIOS[scenario_name]
    trace_dir = VAR / "soak_traces"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)

    run_stamp = time.strftime("%Y%m%d-%H%M%S")
    mode_tag = "no_brain" if no_brain else f"temp{temperature:g}"
    run_dir = VAR / "soak_decisions" / f"{scenario_name}_{mode_tag}_{run_stamp}"
    run_dir.mkdir(parents=True, exist_ok=True)
    bots_dir = run_dir / "bots"
    bots_dir.mkdir(parents=True, exist_ok=True)

    say(
        f"soak scenario [{scenario_name}] ({mode_tag}): {scenario['desc']} "
        f"(logs: {run_dir.relative_to(ROOT)})"
    )
    with Stack(scenario["config"], BOT, online=ONLINE) as stack:
        _apply_scenario_setup(scenario)
        dungeon = scenario.get("dungeon")
        notes_key = dungeon.get("notes") if dungeon else None
        if notes_key:
            notes_src = ROOT / "content" / "notes" / notes_key / "notes.md"
            if notes_src.is_file():
                notes_text = notes_src.read_text(encoding="utf-8")
                for member in DUNGEON_PARTY:
                    mdir = bots_dir / member
                    mdir.mkdir(parents=True, exist_ok=True)
                    (mdir / "notes.md").write_text(notes_text, encoding="utf-8")
        harness_log = run_dir / "harness.log"
        proc = None
        log_fh = None
        if not no_brain:
            log_fh = harness_log.open("w")
            proc = subprocess.Popen(
                [
                    str(OUT / "common" / "harness"),
                    "run",
                    f"--socket={stack.socket}",
                    f"--rules={ROOT / 'content' / 'RULES.md'}",
                    f"--bots-dir={bots_dir}",
                    "--host=127.0.0.1",
                    "--port=8090",
                    "--slots=16",
                    f"--temperature={temperature}",
                ],
                cwd=ROOT,
                stdout=log_fh,
                stderr=subprocess.STDOUT,
            )
        duration_s = max(30.0, minutes * 60.0)
        t0 = time.monotonic()
        try:
            say(
                f"soak [{scenario_name}]: running {ONLINE} bots for {duration_s:.0f} s "
                + ("(playerbots-only, no brain)..." if no_brain else f"with real llama-server (temp={temperature})...")
            )
            while time.monotonic() - t0 < duration_s:
                if proc is not None and proc.poll() is not None:
                    tail = "\n".join(harness_log.read_text().splitlines()[-20:])
                    raise AssertionError(
                        f"harness exited early (rc={proc.returncode}):\n{tail}"
                    )
                time.sleep(2.0)
        finally:
            if proc is not None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
            if log_fh is not None:
                log_fh.close()

        elapsed_min = max((time.monotonic() - t0) / 60.0, 1e-6)
        decisions_by_bot = {}
        option_counts: dict[str, int] = {}
        do_ms = []
        invalid_plans = 0
        llm_errors = 0
        max_repeated_refusals = 0
        for bot_dir in sorted(bots_dir.iterdir()):
            ev_dir = bot_dir / "events"
            if not ev_dir.is_dir():
                continue
            decs = []
            refusal_counts: dict[str, int] = {}
            for f in sorted(ev_dir.glob("*.jsonl")):
                for line in f.read_text().splitlines():
                    if not line.strip():
                        continue
                    d = json.loads(line)
                    decs.append(d)
                    if d.get("error"):
                        llm_errors += 1
                    elif d.get("plan") is None:
                        invalid_plans += 1
                    else:
                        kind = _extract_first_option_kind(d)
                        if kind:
                            option_counts[kind] = option_counts.get(kind, 0) + 1
                    if d.get("first_line_ms", 0) > 0:
                        do_ms.append(float(d["first_line_ms"]))
                    for w in d.get("wake_reasons", []):
                        if isinstance(w, str) and w.startswith("error:"):
                            refusal_counts[w] = refusal_counts.get(w, 0) + 1
            if refusal_counts:
                max_repeated_refusals = max(
                    max_repeated_refusals, max(refusal_counts.values())
                )
            decisions_by_bot[bot_dir.name] = decs

        total_decs = sum(len(v) for v in decisions_by_bot.values())
        max_wakes_per_min = max(
            (len(v) / elapsed_min for v in decisions_by_bot.values()),
            default=0.0,
        )
        # The busiest minute of any bot: an average over the run hides a wake
        # loop (soak_scorer's no_wake_loop reads this).
        peak_wakes_per_min = max(
            (soak_scorer.peak_per_minute([d.get("time_ms") for d in v])
             for v in decisions_by_bot.values()),
            default=0,
        )
        p50_do = statistics.median(do_ms) if do_ms else 0.0
        p95_do = (
            sorted(do_ms)[min(len(do_ms) - 1, int(0.95 * (len(do_ms) - 1)))]
            if do_ms
            else 0.0
        )
        run_stats = {
            "scenario": scenario_name,
            "mode": "no_brain" if no_brain else "llm",
            "temperature": None if no_brain else temperature,
            "decisions_dir": str(run_dir.relative_to(ROOT)),
            "bots": ONLINE,
            "elapsed_min": elapsed_min,
            "decisions": total_decs,
            "option_counts": option_counts,
            "max_wakes_per_min": max_wakes_per_min,
            "peak_wakes_per_min": float(peak_wakes_per_min),
            "do_p50_ms": p50_do,
            "do_p95_ms": p95_do,
            "llm_errors": llm_errors,
            "invalid_plans": invalid_plans,
            "max_repeated_refusals": max_repeated_refusals,
        }
        res = soak_scorer.score_soak(
            trace_dir,
            run_stats=run_stats,
            min_eval_passed=min_eval_passed,
            eval_passed=eval_passed,
        )
        res["scenario"] = scenario_name
        return res


def _resolve_scenarios(suite: str, runs: int) -> list[str]:
    if suite in SUITES:
        base = SUITES[suite]
        if runs <= 0:
            return list(base)
        # Repeat/cycle through the suite so `runs` matches requested run count,
        # or if `runs == 1` and suite has multiple scenarios, run the full suite
        # only when runs >= len(base).
        if suite in ("varied", "heldout", "endurance") and runs == 1:
            return list(base)
        return [base[i % len(base)] for i in range(runs)]
    if suite in SCENARIOS:
        return [suite] * max(1, runs)
    raise ValueError(f"unknown soak suite/scenario: {suite}")


def _merge_trace_dirs(src_dirs: list[pathlib.Path], dest_dir: pathlib.Path) -> None:
    """Merges per-run `<src>/<role>/<name>/*.jsonl` into `dest_dir`.

    Bot character directories are namespaced per run (`<name>_r00`, `<name>_r01`)
    so distinct scenarios reusing `Loada..Loadj` across different maps/levels
    never concatenate into one trajectory (preserving per-run duration, stuck
    distance, XP/death deltas, and population stats).
    """
    shutil.rmtree(dest_dir, ignore_errors=True)
    dest_dir.mkdir(parents=True, exist_ok=True)
    for run_idx, src in enumerate(src_dirs):
        if not src.exists():
            continue
        for role in ("human", "bot"):
            rdir = src / role
            if not rdir.is_dir():
                continue
            for cdir in sorted(rdir.iterdir()):
                if not cdir.is_dir():
                    continue
                char_name = f"{cdir.name}_r{run_idx:02d}" if role == "bot" else cdir.name
                out_cdir = dest_dir / role / char_name
                out_cdir.mkdir(parents=True, exist_ok=True)
                for f in sorted(cdir.glob("*.jsonl")):
                    target = out_cdir / f"{f.stem}_run{run_idx:02d}{f.suffix}"
                    shutil.copy2(f, target)


def _aggregate_run_stats(results: list[dict]) -> dict:
    stats_list = [r.get("run_stats", {}) for r in results if r.get("run_stats")]
    if not stats_list:
        return {}
    p50s = [float(s["do_p50_ms"]) for s in stats_list if s.get("do_p50_ms", 0) > 0]
    p95s = [float(s["do_p95_ms"]) for s in stats_list if s.get("do_p95_ms", 0) > 0]
    merged_options: dict[str, int] = {}
    for s in stats_list:
        for k, v in (s.get("option_counts") or {}).items():
            merged_options[k] = merged_options.get(k, 0) + int(v)
    return {
        "scenario": "+".join(str(s.get("scenario", "soak")) for s in stats_list),
        "mode": stats_list[0].get("mode", "llm"),
        "temperature": stats_list[0].get("temperature"),
        "decisions_dirs": [
            s["decisions_dir"] for s in stats_list if s.get("decisions_dir")
        ],
        # Multi-run merged traces namespace bots per run (`Loada_r00`, `Loada_r01`),
        # so total traced bot-runs equals the sum across runs.
        "bots": sum(int(s.get("bots", 0)) for s in stats_list),
        "elapsed_min": sum(float(s.get("elapsed_min", 0.0)) for s in stats_list),
        "decisions": sum(int(s.get("decisions", 0)) for s in stats_list),
        "option_counts": merged_options,
        "max_wakes_per_min": max(float(s.get("max_wakes_per_min", 0.0)) for s in stats_list),
        "peak_wakes_per_min": max(float(s.get("peak_wakes_per_min", 0.0)) for s in stats_list),
        "do_p50_ms": statistics.median(p50s) if p50s else 0.0,
        "do_p95_ms": max(p95s) if p95s else 0.0,
        "llm_errors": sum(int(s.get("llm_errors", 0)) for s in stats_list),
        "invalid_plans": sum(int(s.get("invalid_plans", 0)) for s in stats_list),
        "max_repeated_refusals": max(
            int(s.get("max_repeated_refusals", 0)) for s in stats_list
        ),
    }


def run_soak(
    minutes: float = 5.0,
    runs: int = 0,
    suite: str = "varied",
    check_guardrails: bool = True,
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
    temperature: float = 0.7,
    no_brain: bool = False,
) -> dict:
    scenarios = _resolve_scenarios(suite, runs)
    if not no_brain:
        build.common("default", tests=False)
    results = []
    with tempfile.TemporaryDirectory(prefix="wlb-soak-runs-") as runs_tmp:
        per_run_dirs: list[pathlib.Path] = []
        llm_ctx = contextlib.nullcontext() if no_brain else LlamaServer(slots=16)
        with llm_ctx:
            for idx, sc_name in enumerate(scenarios):
                if len(scenarios) > 1:
                    say(f"== soak run {idx + 1}/{len(scenarios)} ({sc_name}) ==")
                res = _run_once(
                    scenario_name=sc_name,
                    minutes=minutes,
                    min_eval_passed=(min_eval_passed if len(scenarios) == 1 else None),
                    eval_passed=(eval_passed if len(scenarios) == 1 else None),
                    temperature=temperature,
                    no_brain=no_brain,
                )
                results.append(res)
                if len(scenarios) > 1:
                    run_trace_dst = pathlib.Path(runs_tmp) / f"run_{idx:02d}_{sc_name}"
                    if (VAR / "soak_traces").exists():
                        shutil.copytree(VAR / "soak_traces", run_trace_dst)
                    per_run_dirs.append(run_trace_dst)
                st = res["run_stats"]
                p = res["progress"]
                opt_counts = st.get("option_counts") or {}
                opt_total = sum(opt_counts.values())
                opt_str = ""
                if opt_total > 0:
                    top = sorted(opt_counts.items(), key=lambda kv: -kv[1])[:6]
                    opt_str = ", options={" + ", ".join(
                        f"{k}:{100.0 * v / opt_total:.0f}%" for k, v in top
                    ) + "}"
                say(
                    f"soak summary [{sc_name}] ({st['elapsed_min']:.1f} min, {ONLINE} bots, "
                    f"mode={st.get('mode', 'llm')}): "
                    f"decisions={st['decisions']}, "
                    f"max_wakes/min/bot={st['max_wakes_per_min']:.1f}, "
                    f"do_p50={st['do_p50_ms']:.0f}ms, do_p95={st['do_p95_ms']:.0f}ms, "
                    f"errors={st['llm_errors']}, invalid={st['invalid_plans']}, "
                    f"xp/h={p['xp_per_hour']:.0f}, deaths={p['deaths']}, "
                    f"moved={p['moved_bots']}/{p['traced_bots']}, "
                    f"stalled={p['stalled_bots']}{opt_str}"
                )

            if len(scenarios) > 1:
                _merge_trace_dirs(per_run_dirs, VAR / "soak_traces")
                merged_stats = _aggregate_run_stats(results)
                agg = soak_scorer.score_soak(
                    VAR / "soak_traces",
                    run_stats=merged_stats,
                    min_eval_passed=min_eval_passed,
                    eval_passed=eval_passed,
                )
                agg["scenarios"] = [r.get("scenario", "soak") for r in results]
                agg["guardrails_ok"] = bool(agg["guardrails_ok"]) and all(
                    r["guardrails_ok"] for r in results
                )
            else:
                agg = results[0]
    say(soak_scorer.format_score_report(agg))

    if check_guardrails and not no_brain:
        expect(agg["guardrails_ok"], "one or more soak guardrails failed (see above)")
    return agg
