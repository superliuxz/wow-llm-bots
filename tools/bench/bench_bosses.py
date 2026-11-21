#!/usr/bin/env python3
"""10-Boss Benchmark Suite: 3 WotLK + 3 TBC + 3 Classic + 1 Trivial Raid.

Supports 4 apples-to-apples modes and parallel multi-party execution (`--parallel 3`)
inside a single persistent test `worldserver` (~5x faster than restarting per trial):
  - `no_brain`:     no harness, playerbots boss scripts OFF
  - `llm_no_notes`: harness ON, no notes.md, playerbots boss scripts OFF
  - `llm`:          harness ON + notes.md, playerbots boss scripts OFF
  - `baseline`:     no harness, playerbots hardcoded C++ boss scripts ON

Per-trial hygiene enforced before every pull:
  - `AccountInstancesPerHour = 1000` in test `worldserver.conf`.
  - `llmtest resetbot` revives, clears all auras (incl. Sated/Exhaustion/Res Sickness),
    resets spell cooldowns, repairs gear, restores full HP/mana, and resets instances.
  - Verifies exact roles (`role=tank|heal|dps`), distinct non-zero `instance=<id>`
    across parallel parties, and post-arrival per-bot `llmtest checkstrat`.
  - Uses a fresh `--bots-dir` per wave and logs `harness` decision latency (p50/p95).
  - Invalidates and retries zero-pull trials (0 DPS, 0 deaths, not killed).
"""

import argparse
import json
import math
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import boss_meter  # noqa: E402
from wlblib import build, server  # noqa: E402
from wlblib.common import OUT, TEST, VAR, say  # noqa: E402
from wlblib.testing import LlamaServer  # noqa: E402

# (role_suffix, race:class, spec_tab, expected_role)
COMP_5 = [
    ("a", "human:warrior", 2, "tank"),     # Protection Warrior (Tank)
    ("b", "human:priest", 1, "heal"),      # Holy Priest (Healer)
    ("c", "human:mage", 2, "dps"),         # Frost Mage (DPS)
    ("d", "human:warlock", 2, "dps"),      # Destruction Warlock (DPS)
    ("e", "nightelf:hunter", 1, "dps"),    # Marksmanship Hunter (DPS)
]

COMP_10 = COMP_5 + [
    ("f", "dwarf:paladin", 1, "tank"),     # Protection Paladin (Off-tank)
    ("g", "nightelf:druid", 2, "heal"),    # Restoration Druid (Healer 2)
    ("h", "human:rogue", 1, "dps"),        # Combat Rogue (Melee DPS)
    ("i", "dwarf:warrior", 1, "dps"),      # Fury Warrior (Melee DPS)
    ("j", "human:mage", 0, "dps"),         # Arcane Mage (Ranged DPS)
]

PREFIXES = ["Load", "Ptwo", "Pthr", "Pfou"]

ICK_NOTES = """# Pit of Saron — Ick & Krick
- **Pursuit**: Ick fixates on one player for 12s and chases them (taunt-immune).
  - Standing order: `Pursuit on me -> kite Ick`
- **Poison Nova**: 5s cast dealing heavy nature damage within 15 yd.
  - Standing order: `Poison Nova cast -> away Ick 22`
- **Toxic Waste**: green poison pools on the ground.
  - Standing order: `Toxic Waste appears -> avoid Toxic Waste`
"""

DEVOURER_NOTES = """# The Forge of Souls — Devourer of Souls
- **Mirrored Soul**: links boss to a party member for 8s — all boss damage hits that ally!
  - Standing order: `Mirrored Soul cast -> stop attacking`
  - Standing order: `Mirrored Soul on any -> stop attacking`
- **Wailing Souls**: sweeping frontal beam; stay behind the boss.
  - Standing order: `Wailing Souls cast -> to behind Devourer of Souls`
- **Well of Souls**: purple void zone on the ground.
  - Standing order: `Well of Souls appears -> avoid Well of Souls`
- **Phantom Blast**: heavy shadow nuke on tank; interrupt when possible.
"""

EREGOS_NOTES = """# The Oculus — Ley-Guardian Eregos (3D Drake Vehicle Fight)
- Before pulling, use your Drake Essence in your bags (`Ruby Essence` Tank, `Emerald Essence` Healer, `Amber Essence` DPS) to mount your drake!
- **Planar Shift**: Eregos becomes immune and spawns **Planar Anomaly** orbs that explode for heavy arcane damage; fly away from orbs while he is shifted.
  - Standing order: `Planar Anomaly appears -> away Planar Anomaly 25`
- **Enraged Assault**: heavy damage phase; tank drake uses Martyr / Evasive Maneuvers and healer drake focuses tank.
"""

MURMUR_NOTES = """# Shadow Labyrinth — Murmur
- **Sonic Boom**: 5s cast dealing massive damage within 34 yards!
  - Standing order: `Sonic Boom cast -> away Murmur 38`
- **Murmur's Touch**: bomb debuff on one player; move away from allies so you don't knock them up.
  - Standing order: `Murmur's Touch on me -> away`
"""

IKISS_NOTES = """# Sethekk Halls — Talon King Ikiss
- **Arcane Explosion**: after Blinking, casts a 5s lethal AoE; run far away or break line of sight behind a pillar.
  - Standing order: `Arcane Explosion cast -> away Talon King Ikiss 35`
- **Polymorph**: sheeps a party member; dispellers dispel it promptly.
"""

KAELTHAS_NOTES = """# Magisters' Terrace — Kael'thas Sunstrider
- **Phoenix / Phoenix Egg**: switch immediately to kill the Phoenix and destroy the **Phoenix Egg** before it hatches!
  - Standing order: `Phoenix Egg appears -> target nearest Phoenix Egg`
- **Flame Strike**: fiery ground AoE; move out immediately.
  - Standing order: `Flame Strike appears -> avoid Flame Strike`
- **Fireball**: interruptible nuke on the tank.
- **Gravity Lapse**: floats everyone in the air and spawns Arcane Spheres; stay clear of spheres.
"""

THERADRAS_NOTES = """# Maraudon — Princess Theradras
- **Dust Field**: heavy physical AoE + knockback around the boss; ranged/healers stay far back and melee run out during Dust Field.
  - Standing order: `Dust Field cast -> away Princess Theradras 20`
- **Repulsive Gaze**: fears the current target and drops threat; tank re-taunts promptly and healers keep the off-target alive.
- **Boulder**: throws a boulder at a ranged player, stunning them; stay spread.
"""

RATTLEGORE_NOTES = """# Scholomance — Rattlegore
- **Knock Away / War Stomp**: heavy melee knockback and stun; ranged/healers stay at max range and tank repositions promptly.
- **Strike**: heavy physical burst on the tank; keep tank topped.
"""

ANASTARI_NOTES = """# Stratholme — Baroness Anastari
- **Possess**: mind-controls a party member! Do not kill them — damage/CC them until Possess breaks (~50% HP), then resume the boss.
- **Banshee Curse**: reduces hit chance; dispel curses when available.
- **Silence**: interrupts casting nearby; ranged/healers stay spread.
"""

PATCHWERK_NOTES = """# Naxxramas (10-man) — Patchwerk
- Pure tank-and-spank throughput check.
- **Hateful Strike**: hits the highest-HP melee target after the main tank; off-tank stays in melee range and healers keep both tanks topped.
- Maximize DPS and HPS; no movement mechanics.
"""

BOSSES = {
    "ick": dict(
        era="WotLK",
        tag="ick_kite",
        boss_name="Ick",
        level=80,
        map_id=658,
        stage_xyz=(435.74, 212.41, 529.0),
        spawn_xyz=(846.0, 132.0, 510.5),
        strip_strat="wotlk-pos",
        attune_quests=[24499, 24511],
        notes_md=ICK_NOTES,
        max_s=210.0,
    ),
    "devourer": dict(
        era="WotLK",
        tag="devourer_coord",
        boss_name="Devourer of Souls",
        level=80,
        map_id=632,
        stage_xyz=(5615.0, 2507.4, 708.9),
        spawn_xyz=(5645.0, 2507.4, 708.9),
        strip_strat="wotlk-fos",
        notes_md=DEVOURER_NOTES,
        max_s=210.0,
    ),
    "eregos": dict(
        era="WotLK",
        tag="eregos_drake",
        boss_name="Ley-Guardian Eregos",
        level=80,
        map_id=578,
        stage_xyz=(1077.0, 1020.0, 655.5),
        spawn_xyz=(1077.0, 1055.0, 655.5),
        strip_strat="wotlk-occ",
        role_items={0: 37860, 1: 37815, 2: 37859, 3: 37859, 4: 37859},
        notes_md=EREGOS_NOTES,
        max_s=210.0,
    ),
    "murmur": dict(
        era="TBC",
        tag="murmur_boom",
        boss_name="Murmur",
        level=70,
        map_id=555,
        stage_xyz=(0.49, -0.22, -1.0),
        spawn_xyz=(-157.9, -490.0, 16.2),
        strip_strat="",
        notes_md=MURMUR_NOTES,
        max_s=210.0,
    ),
    "ikiss": dict(
        era="TBC",
        tag="ikiss_blink",
        boss_name="Talon King Ikiss",
        level=68,
        map_id=556,
        stage_xyz=(-4.68, -0.09, 0.2),
        spawn_xyz=(44.7, 268.0, 25.5),
        strip_strat="tbc-seth",
        notes_md=IKISS_NOTES,
        max_s=210.0,
    ),
    "kaelthas": dict(
        era="TBC",
        tag="kaelthas_mgt",
        boss_name="Kael'thas Sunstrider",
        level=70,
        map_id=585,
        stage_xyz=(7.09, -0.45, -2.5),
        spawn_xyz=(148.5, 180.0, -16.2),
        strip_strat="tbc-mgt",
        attune_quests=[11492],
        notes_md=KAELTHAS_NOTES,
        max_s=210.0,
    ),
    "theradras": dict(
        era="Classic",
        tag="theradras_mara",
        boss_name="Princess Theradras",
        level=50,
        map_id=349,
        stage_xyz=(27.9, 42.0, -124.5),
        spawn_xyz=(27.9, 68.0, -124.5),
        notes_md=THERADRAS_NOTES,
        max_s=210.0,
    ),
    "rattlegore": dict(
        era="Classic",
        tag="rattlegore_scholo",
        boss_name="Rattlegore",
        level=60,
        map_id=289,
        stage_xyz=(137.1, 138.0, 96.0),
        spawn_xyz=(137.1, 158.0, 96.0),
        notes_md=RATTLEGORE_NOTES,
        max_s=210.0,
    ),
    "anastari": dict(
        era="Classic",
        tag="anastari_strat",
        boss_name="Baroness Anastari",
        level=59,
        map_id=329,
        stage_xyz=(3855.3, -3675.0, 148.2),
        spawn_xyz=(3855.3, -3700.0, 148.2),
        notes_md=ANASTARI_NOTES,
        max_s=210.0,
    ),
    "patchwerk": dict(
        era="Trivial Raid",
        tag="patchwerk_naxx10",
        boss_name="Patchwerk",
        level=80,
        map_id=533,
        stage_xyz=(3205.0, -3230.3, 294.1),
        spawn_xyz=(3234.0, -3230.3, 294.1),
        raid=True,
        notes_md=PATCHWERK_NOTES,
        max_s=240.0,
    ),
}

INST_RE = re.compile(r"\binstance=(\d+)")
PASSIVES_RE = re.compile(r"\bpassives=(\d+)")
SPECNO_RE = re.compile(r"\bspecNo=(\d+)")
EXISTS_RE = re.compile(r"\bexists=(\d+)")
COMBAT_RE = re.compile(r"\bcombat=(\d+)")
NONCOMBAT_RE = re.compile(r"\bnoncombat=(\d+)")


def _parse_checkstrat(cline: str) -> tuple[int, int, int] | None:
    """Parses `(exists, combat, noncombat)` from a `llmtest checkstrat` reply line.

    Uses word-bounded regexes so `combat=0` never matches the substring inside `noncombat=0`.
    """
    em = EXISTS_RE.search(cline)
    cm = COMBAT_RE.search(cline)
    nm = NONCOMBAT_RE.search(cline)
    if not (em and cm and nm):
        return None
    return int(em.group(1)), int(cm.group(1)), int(nm.group(1))


def _check_strat_line(cline: str, expected_on: bool) -> tuple[bool, str]:
    parsed = _parse_checkstrat(cline)
    if parsed is None:
        return False, f"unparseable checkstrat line: {cline!r}"
    exists, combat, noncombat = parsed
    if exists != 1:
        return False, f"strategy does not exist (exists={exists}): {cline!r}"
    if expected_on:
        if combat != 1 and noncombat != 1:
            return False, f"baseline strategy is NOT active (combat={combat}, noncombat={noncombat}): {cline!r}"
        return True, ""
    if combat != 0 or noncombat != 0:
        return False, f"strategy was NOT stripped (combat={combat}, noncombat={noncombat}): {cline!r}"
    return True, ""


def _console_bot(cmd: str, prefix: str, wait_s: float = 0.3, timeout_s: float = 5.0) -> tuple[str, str]:
    """Sends a console command ONCE and waits (up to `timeout_s`) for its bot-scoped reply.

    Never re-sends: state-changing commands (`gear` re-rolls `Randomize`, `resetbot`, `tele`,
    `setstrat`) must run exactly once. A slow reply is picked up by re-reading the worldserver
    log from the offset taken before the command was sent.
    """
    log = TEST.logs / "worldserver.out"
    start = log.stat().st_size if log.exists() else 0
    out = server.console(cmd, wait_s=wait_s, inst=TEST) or ""
    line = _find_bot_line(out, prefix)
    deadline = time.monotonic() + max(0.0, timeout_s - wait_s)
    while not line and time.monotonic() < deadline:
        time.sleep(0.25)
        with open(log, "rb") as f:
            f.seek(start)
            out = f.read().decode(errors="replace")
        line = _find_bot_line(out, prefix)
    return line, out


def _wave_arms(arms: tuple[str, ...], wave: int, n_parties: int) -> list[str]:
    """Arms for every party slot in `wave` (1-based): fills all slots, balances arm counts
    every `len(arms) // gcd(n_parties, len(arms))` waves, and shifts starting party slot each
    cycle so no party prefix is permanently pinned to one arm when `gcd > 1`."""
    cycle = max(1, len(arms) // math.gcd(n_parties, len(arms)))
    b = (wave - 1) % cycle
    k = (wave - 1) // cycle
    return [arms[(b * n_parties + p_idx + k) % len(arms)] for p_idx in range(n_parties)]


def _had_pre_pull_combat(
    all_traces: dict[str, list[dict]],
    pnames: list[str],
    stage_ms: int,
    pull_start_ms: int,
) -> bool:
    """Returns True if any bot in `pnames` had `in_combat=True` between staging and pull."""
    for n in pnames:
        for r in all_traces.get(n, []):
            if r.get("kind") != "sample":
                continue
            t = int(r.get("t_ms", 0))
            if stage_ms <= t < pull_start_ms and r.get("in_combat"):
                return True
    return False


def _summarize_trials(trials: list[dict]) -> dict:
    kills = sum(1 for r in trials if r.get("killed"))
    lat_p50s = [
        r["harness_latency"]["p50_ms"]
        for r in trials
        if r.get("harness_latency") and r["harness_latency"].get("p50_ms") is not None
    ]
    return {
        "runs": len(trials),
        "kills": kills,
        "median_dps": round(statistics.median(r["total_dps"] for r in trials), 1) if trials else 0.0,
        "median_hps": round(statistics.median(r["total_hps"] for r in trials), 1) if trials else 0.0,
        "median_deaths": round(statistics.median(r["total_deaths"] for r in trials), 1) if trials else 0.0,
        "median_duration_s": round(statistics.median(r["duration_s"] for r in trials), 1) if trials else 0.0,
        "median_first_line_ms": round(statistics.median(lat_p50s), 1) if lat_p50s else None,
    }


def _find_bot_line(output: str | None, prefix: str) -> str:
    """Returns the line in `output` matching `prefix` (bot-scoped), or ''."""
    for line in (output or "").splitlines():
        if prefix in line:
            return line
    return ""


def _parse_harness_latency(bots_dir: pathlib.Path, bot_names: list[str]) -> dict[str, float | int | None]:
    """Reads `first_line_ms` from `bots_dir/<name>/events/*.jsonl` (same as soak.py)."""
    vals: list[float] = []
    for name in bot_names:
        ev_dir = bots_dir / name / "events"
        if not ev_dir.is_dir():
            continue
        for f in sorted(ev_dir.glob("*.jsonl")):
            for line in f.read_text(errors="replace").splitlines():
                if not line.strip():
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                if d.get("first_line_ms", 0) > 0:
                    vals.append(float(d["first_line_ms"]))
    if not vals:
        return {"p50_ms": None, "p95_ms": None, "calls": 0}
    vals.sort()
    p50 = vals[len(vals) // 2]
    p95 = vals[min(len(vals) - 1, int(len(vals) * 0.95))]
    return {"p50_ms": round(p50, 1), "p95_ms": round(p95, 1), "calls": len(vals)}


def _make_parties(n_parties: int, raid: bool) -> list[list[tuple[str, str, int, str]]]:
    comp = COMP_10 if raid else COMP_5
    if raid:
        n_parties = 1
    out = []
    for p_idx in range(n_parties):
        pref = PREFIXES[p_idx]
        out.append([(f"{pref}{suf}", rc, spec, role) for suf, rc, spec, role in comp])
    return out


def _set_instances_per_hour(cap: int = 1000) -> None:
    """Raises AccountInstancesPerHour on the test server (note: TELE_TO_GM_MODE
    bypasses this cap; kept high in case non-GM dungeon entry paths are used)."""
    ws_conf = TEST.etc / "worldserver.conf"
    if not ws_conf.exists():
        return
    txt = ws_conf.read_text()
    new_txt = re.sub(
        r"(?m)^AccountInstancesPerHour\s*=.*$",
        f"AccountInstancesPerHour = {cap}",
        txt,
    )
    if new_txt == txt and "AccountInstancesPerHour" not in txt:
        new_txt = txt + f"\nAccountInstancesPerHour = {cap}\n"
    ws_conf.write_text(new_txt)


class _TraceTailer:
    """Incrementally tails newly appended JSONL lines per bot (avoids O(N^2) re-reads).

    Offsets are keyed by file `(st_dev, st_ino)` rather than path so that when
    `behavior_trace.cc` renames `YYYY-MM-DD.jsonl` -> `YYYY-MM-DD.1.jsonl`, the
    rotated file keeps its read offset and is never read twice.
    """

    def __init__(self, trace_dir: pathlib.Path, bot_names: list[str]) -> None:
        self.trace_dir = trace_dir / "bot"
        self.bot_names = list(bot_names)
        self.offsets: dict[tuple[int, int], int] = {}
        self.rows: dict[str, list[dict]] = {n: [] for n in bot_names}

    def poll(self) -> dict[str, list[dict]]:
        for name in self.bot_names:
            bdir = self.trace_dir / name
            if not bdir.is_dir():
                continue
            for f in sorted(bdir.glob("*.jsonl")):
                try:
                    st = f.stat()
                except OSError:
                    continue
                fkey = (st.st_dev, st.st_ino)
                off = self.offsets.get(fkey, 0)
                sz = st.st_size
                if sz < off:
                    off = 0  # Same inode truncated in place.
                if sz == off:
                    continue
                with f.open("r", encoding="utf-8", errors="replace") as fp:
                    fp.seek(off)
                    while True:
                        pos = fp.tell()
                        raw = fp.readline()
                        if not raw:
                            break
                        if not raw.endswith("\n"):
                            # Incomplete write in progress; retry from `pos` next poll.
                            fp.seek(pos)
                            break
                        line = raw.strip()
                        if not line:
                            continue
                        try:
                            self.rows[name].append(json.loads(line))
                        except json.JSONDecodeError:
                            continue
                    self.offsets[fkey] = fp.tell()
        return self.rows


def _slice_traces_after(
    all_traces: dict[str, list[dict]],
    names: list[str],
    t_min_ms: int,
) -> dict[str, list[dict]]:
    out = {}
    for n in names:
        rows = all_traces.get(n, [])
        out[n] = [r for r in rows if int(r.get("t_ms", 0)) >= t_min_ms]
    return out


def run_boss_waves(
    era: str,
    tag: str,
    boss_name: str,
    level: int,
    map_id: int,
    stage_xyz: tuple[float, float, float],
    spawn_xyz: tuple[float, float, float],
    strip_strat: str = "",
    attune_quests: list[int] | None = None,
    role_items: dict[int, int] | None = None,
    raid: bool = False,
    notes_md: str = "",
    max_s: float = 210.0,
    mode: str = "llm",
    arms_override: tuple[str, ...] | None = None,
    target_runs: int = 3,
    parallel: int = 3,
) -> dict:
    """Runs `target_runs` valid trials PER ARM for one boss inside a single persistent worldserver."""
    if arms_override:
        arms = arms_override
    elif mode == "mixed":
        arms = ("llm", "llm_no_notes")
    elif mode == "mixed_all":
        arms = ("llm", "llm_no_notes", "no_brain", "baseline")
    else:
        arms = (mode,)
    n_parties = 1 if raid else max(1, min(parallel, target_runs * len(arms), len(PREFIXES)))
    parties = _make_parties(n_parties, raid=raid)
    all_bots = [b for p in parties for b in p]

    trace_dir = VAR / f"bench_boss_{tag}_{mode}"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)

    server.server_stop(inst=TEST)
    server.test_databases_reset()
    server.config("soak", inst=TEST)
    _set_instances_per_hour(1000)

    conf_path = TEST.etc / "modules" / "mod-llm-brain.conf"
    bot_names = ",".join(b[0] for b in all_bots)
    fixtures = ",".join(f"{b[0]}=new:{b[1]}" for b in all_bots)
    lines = [
        l for l in conf_path.read_text().splitlines()
        if not l.startswith((
            "LlmBrain.Bots ",
            "LlmBrain.TestFixtures ",
            "LlmBrain.TraceDir ",
            "LlmBrain.CombatStrategies ",
            "LlmBrain.NonCombatStrategies ",
        ))
    ]
    lines.append(f'LlmBrain.Bots = "{bot_names}"')
    lines.append(f'LlmBrain.TestFixtures = "{fixtures}"')
    lines.append(f'LlmBrain.TraceDir = "{trace_dir}"')
    c_strats = "+boost,+dps assist,-threat"
    nc_strats = "-follow,-grind,-new rpg"
    # When mixing baseline + non-baseline arms in the same wave, per-bot
    # overrides (`llmtest setstrat`) control `strip_strat` per party.
    per_bot_strat_control = bool(strip_strat and len(arms) > 1 and "baseline" in arms)
    if strip_strat and not per_bot_strat_control and arms != ("baseline",):
        c_strats += f",-{strip_strat}"
        nc_strats += f",-{strip_strat}"
    lines.append(f'LlmBrain.CombatStrategies = "{c_strats}"')
    lines.append(f'LlmBrain.NonCombatStrategies = "{nc_strats}"')
    conf_path.write_text("\n".join(lines) + "\n")

    valid_runs: list[dict] = []
    extra_trials: list[dict] = []
    invalid_runs = 0
    wave = 0
    seen_instances: set[int] = set()
    per_arm_target = target_runs
    arm_counts: dict[str, int] = {a: 0 for a in arms}
    # Enough waves for every arm to reach its target, plus slack for invalid trials.
    max_waves = -(-per_arm_target * len(arms) // n_parties) + 2 * -(-len(arms) // n_parties) + 2
    wave_errors: list[str] = []

    try:
        server.server_start(TEST)
        server.wait_online(all_bots[0][0], inst=TEST)
        server.wait_online_count(len(all_bots), inst=TEST)

        # Group each party separately and gear every member with exact deterministic spec.
        for party in parties:
            leader = party[0][0]
            for name, _, _, _ in party[1:]:
                server.console(f"llmtest group {leader} {name}", wait_s=0.3, inst=TEST)
            for idx, (name, _, spec_tab, expected_role) in enumerate(party):
                line, out = _console_bot(f"llmtest gear {name} {level} {spec_tab}", f"geared {name} at level {level}", wait_s=0.5)
                if f"role={expected_role}" not in line:
                    raise RuntimeError(
                        f"role mismatch for {name}: expected role={expected_role}, got: {out!r}"
                    )
                sm = SPECNO_RE.search(line)
                if not sm or int(sm.group(1)) != spec_tab + 1:
                    raise RuntimeError(
                        f"stored specNo mismatch for {name}: expected specNo={spec_tab + 1}, got: {line!r}"
                    )
                for qid in (attune_quests or []):
                    qline, qout = _console_bot(f"llmtest questcomplete {name} {qid}", f" quest {qid} ", wait_s=0.2)
                    if not qline or not qline.rstrip().endswith(f" on {name}"):
                        raise RuntimeError(f"questcomplete {qid} failed for {name}: {qout!r}")
                if role_items and idx in role_items:
                    server.console(f"llmtest item {name} {role_items[idx]}", wait_s=0.2, inst=TEST)

        while any(arm_counts[a] < per_arm_target for a in arms) and wave < max_waves:
            wave += 1
            # Every slot is filled every wave (constant party count / GPU load); a trial for an arm
            # that already has `per_arm_target` valid trials is kept as an `extra` (not scored).
            party_modes = _wave_arms(arms, wave, n_parties)
            active_parties = parties[: len(party_modes)]
            llm_parties_in_wave = sum(1 for pm in party_modes if pm in ("llm", "llm_no_notes"))
            say(f"boss benchmark [{era} | {tag} | {mode} | wave {wave}]: modes={party_modes} arm_counts={arm_counts}")

            try:
                # 1. Move ALL bots off the dungeon map (to map 0) first, confirm map=0, then resetbot
                #    (so Group::ResetInstances never silently fails due to someone still in the dungeon).
                for p_idx, party in enumerate(active_parties):
                    hx, hy, hz = -8949.95 + p_idx * 10.0, -132.49, 83.53
                    for name, _, _, _ in party:
                        server.console(f"llmtest tele {name} 0 {hx:.2f} {hy:.2f} {hz:.2f} 0", wait_s=0.25, inst=TEST)
                time.sleep(2.0)
                for party in active_parties:
                    for name, _, _, _ in party:
                        wline, wout = _console_bot(f"llmtest where {name}", f"where {name} ", wait_s=0.25)
                        if "map=0 " not in wline:
                            raise RuntimeError(f"{name} did not leave dungeon before reset: {wout!r}")
                    for name, _, _, _ in party:
                        rline, out = _console_bot(f"llmtest resetbot {name}", f"resetbot {name} ", wait_s=0.25)
                        pm = PASSIVES_RE.search(rline)
                        if "alive=1" not in rline or not pm or int(pm.group(1)) <= 0:
                            raise RuntimeError(f"dirty start state / missing passives for {name}: {out!r}")

                # 2. Teleport each party to its own fresh dungeon instance at stage_xyz, wait for
                #    cross-map worldport, then assert distinct non-zero AND never-reused instance IDs.
                stage_ms = int(time.time() * 1000)
                sx, sy, sz = stage_xyz
                for party in active_parties:
                    for idx, (name, _, _, _) in enumerate(party):
                        tline, out = _console_bot(
                            f"llmtest tele {name} {map_id} {sx + (idx % 5) * 0.8:.2f} {sy + (idx // 5) * 0.8:.2f} {sz:.2f} 0",
                            f"teleported {name} to map {map_id}",
                            wait_s=0.35,
                        )
                        if not tline:
                            raise RuntimeError(f"stage teleport failed for {name}: {out!r}")

                time.sleep(3.5)
                party_instances: list[int] = []
                for party in active_parties:
                    inst_id = None
                    for name, _, _, _ in party:
                        wline, wout = _console_bot(f"llmtest where {name}", f"where {name} ", wait_s=0.25)
                        m = INST_RE.search(wline)
                        if f"map={map_id} " not in wline or not m:
                            raise RuntimeError(f"{name} did not arrive on map {map_id}: {wout!r}")
                        if not re.search(r"\balive=1\b", wline):
                            raise RuntimeError(f"{name} arrived on map {map_id} dead: {wline!r}")
                        cur_inst = int(m.group(1))
                        if inst_id is None:
                            inst_id = cur_inst
                        elif cur_inst != inst_id:
                            raise RuntimeError(
                                f"party member {name} landed in instance {cur_inst}, expected {inst_id}"
                            )
                    if inst_id == 0:
                        raise RuntimeError(f"expected non-zero dungeon instance id on map {map_id}")
                    if inst_id in seen_instances:
                        raise RuntimeError(f"dungeon instance {inst_id} was reused from an earlier wave!")
                    party_instances.append(inst_id)

                if len(set(party_instances)) != len(party_instances):
                    raise RuntimeError(
                        f"parallel parties collided in same dungeon instance: {party_instances}"
                    )
                seen_instances.update(party_instances)
                say(f"  verified fresh distinct dungeon instances: {party_instances}")

                # 3. Apply per-party strategy override (when mixing baseline with stripped arms)
                #    and verify dungeon strategy state on every bot after arrival.
                if strip_strat:
                    for p_idx, party in enumerate(active_parties):
                        expected_on = party_modes[p_idx] == "baseline"
                        if per_bot_strat_control:
                            delta = f"+{strip_strat}" if expected_on else f"-{strip_strat}"
                            for name, _, _, _ in party:
                                _console_bot(f"llmtest setstrat {name} {delta}", f"setstrat {name} ", wait_s=0.2)
                        for name, _, _, _ in party:
                            cline, chk = _console_bot(
                                f"llmtest checkstrat {name} {strip_strat}",
                                f"strat {strip_strat} on {name} ",
                                wait_s=0.25,
                            )
                            ok, err = _check_strat_line(cline, expected_on=expected_on)
                            if not ok:
                                raise RuntimeError(f"checkstrat failed for {name}: {err} (raw: {chk!r})")

                # 4. Start fresh harness (driving ONLY llm / llm_no_notes parties via --only-bots)
                #    with a clean --bots-dir for this wave.
                llm_bots = [
                    name
                    for p_idx, party in enumerate(active_parties)
                    if party_modes[p_idx] in ("llm", "llm_no_notes")
                    for name, _, _, _ in party
                ]
                with tempfile.TemporaryDirectory(prefix=f"wlb-boss-{tag}-{mode}-w{wave}-") as tmp:
                    tmp_dir = pathlib.Path(tmp)
                    bots_dir = tmp_dir / "bots"
                    bots_dir.mkdir(parents=True)
                    for p_idx, party in enumerate(active_parties):
                        if notes_md and party_modes[p_idx] == "llm":
                            for name, _, _, _ in party:
                                bdir = bots_dir / name
                                bdir.mkdir(parents=True, exist_ok=True)
                                (bdir / "notes.md").write_text(notes_md)
                    harness_log = tmp_dir / "harness.log"
                    proc = None
                    if llm_bots:
                        proc = subprocess.Popen(
                            [
                                str(OUT / "common" / "harness"),
                                "run",
                                f"--socket={TEST.run / 'brain.sock'}",
                                f"--rules={ROOT / 'content' / 'RULES.md'}",
                                f"--bots-dir={bots_dir}",
                                f"--only-bots={','.join(llm_bots)}",
                                "--host=127.0.0.1",
                                "--port=8090",
                                "--slots=16",
                                "--temperature=0",
                            ],
                            cwd=ROOT,
                            stdout=harness_log.open("w"),
                            stderr=subprocess.STDOUT,
                        )
                    # Brief connect window (3 s) right before pull so bots don't wander/early-pull.
                    time.sleep(3.0)

                    # 5. Move each party to spawn_xyz to initiate its pull, recording each party's
                    #    own pull time so a party that engaged while earlier parties were being
                    #    moved is still caught by the pre-pull combat check.
                    party_pull_ms: list[int] = []
                    x, y, z = spawn_xyz
                    for party in active_parties:
                        party_pull_ms.append(int(time.time() * 1000))
                        for idx, (name, _, _, _) in enumerate(party):
                            server.console(
                                f"llmtest move {name} {x + (idx % 5) * 0.8:.2f} {y + (idx // 5) * 0.8:.2f} {z:.2f} 0",
                                wait_s=0.2,
                                inst=TEST,
                            )
                    t0 = time.monotonic()
                    done_flags = [False] * len(active_parties)
                    tailer = _TraceTailer(trace_dir, [b[0] for p in active_parties for b in p])
                    try:
                        while time.monotonic() - t0 < max_s:
                            if proc is not None and proc.poll() is not None:
                                break
                            time.sleep(5.0)
                            all_tr = tailer.poll()
                            for p_idx, party in enumerate(active_parties):
                                if done_flags[p_idx]:
                                    continue
                                pnames = [b[0] for b in party]
                                sub = _slice_traces_after(all_tr, pnames, party_pull_ms[p_idx] - 5000)
                                res = boss_meter.analyze_boss_fight(sub, boss_name=boss_name)
                                if res["killed"]:
                                    say(f"  party {p_idx + 1} ({party_modes[p_idx]}, inst {party_instances[p_idx]}): {boss_name} KILLED in {res['duration_s']:.1f}s!")
                                    done_flags[p_idx] = True
                                elif res["total_deaths"] >= len(party):
                                    say(f"  party {p_idx + 1} ({party_modes[p_idx]}, inst {party_instances[p_idx]}): wiped ({res['total_deaths']} deaths)")
                                    done_flags[p_idx] = True
                            if all(done_flags):
                                time.sleep(2.0)
                                break
                    finally:
                        if proc is not None:
                            proc.terminate()
                            try:
                                proc.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                proc.kill()

                    # Read per-party `first_line_ms` from `bots_dir/<name>/events/*.jsonl`.
                    party_lats = []
                    for p_idx, party in enumerate(active_parties):
                        pnames = [b[0] for b in party]
                        lat = _parse_harness_latency(bots_dir, pnames)
                        if party_modes[p_idx] in ("llm", "llm_no_notes") and lat["calls"] == 0:
                            raise RuntimeError(
                                f"harness recorded 0 LLM decisions (first_line_ms) for party {p_idx + 1} ({pnames})"
                            )
                        party_lats.append(lat)

                # 6. Score each party in this wave from its post-pull slice, verifying no pre-pull combat.
                all_tr = tailer.poll()
                for p_idx, party in enumerate(active_parties):
                    pnames = [b[0] for b in party]
                    arm = party_modes[p_idx]
                    pre_pull = _had_pre_pull_combat(all_tr, pnames, stage_ms, party_pull_ms[p_idx])
                    sub = _slice_traces_after(all_tr, pnames, party_pull_ms[p_idx] - 1000)
                    res = boss_meter.analyze_boss_fight(sub, boss_name=boss_name)
                    res["era"] = era
                    res["party_size"] = len(party)
                    res["level"] = level
                    res["mode"] = arm
                    res["wave"] = wave
                    res["party_index"] = p_idx
                    res["instance_id"] = party_instances[p_idx]
                    res["harness_latency"] = party_lats[p_idx]
                    res["llm_parties_in_wave"] = llm_parties_in_wave
                    res["pull_ms"] = party_pull_ms[p_idx]
                    res["pre_pull_combat"] = pre_pull
                    no_pull = res["total_dps"] == 0.0 and res["total_deaths"] == 0 and not res["killed"]
                    res["invalid"] = pre_pull or no_pull
                    if pre_pull:
                        res["invalid_reason"] = "pre_pull_combat"
                    elif no_pull:
                        res["invalid_reason"] = "no_pull"
                    print("\n" + boss_meter.format_boss_meter(res))
                    if res["invalid"]:
                        invalid_runs += 1
                        say(f"  party {p_idx + 1} ({arm}): INVALID trial ({res['invalid_reason']})")
                    elif arm_counts[arm] < per_arm_target:
                        valid_runs.append(res)
                        arm_counts[arm] += 1
                    else:
                        res["extra"] = True
                        extra_trials.append(res)
                        say(f"  party {p_idx + 1} ({arm}): extra trial (arm already has {per_arm_target}), not scored")
            except RuntimeError as exc:
                invalid_runs += len(active_parties)
                wave_errors.append(f"wave {wave}: {exc}")
                say(f"  WAVE {wave} FAILED (continuing): {exc}")
    finally:
        if per_bot_strat_control:
            try:
                server.console("llmtest clearstrat", wait_s=0.3, inst=TEST)
            except Exception:
                pass  # The server may already be down; overrides die with it.
        server.server_stop(inst=TEST)

    by_arm = {
        a: _summarize_trials([r for r in valid_runs if r["mode"] == a])
        for a in arms
    }
    overall = _summarize_trials(valid_runs)
    for a, st in by_arm.items():
        say(
            f"  summary [{boss_name} | {a}]: kills={st['kills']}/{st['runs']} "
            f"med_dps={st['median_dps']} med_deaths={st['median_deaths']} "
            f"med_lat={st['median_first_line_ms']}ms"
        )
    short = [a for a in arms if arm_counts[a] < per_arm_target]
    if short:
        say(f"  WARNING: arms below {per_arm_target} valid trials after {wave} waves: "
            f"{ {a: arm_counts[a] for a in short} }")
    return {
        "boss": boss_name,
        "era": era,
        "level": level,
        "party_size": len(COMP_10 if raid else COMP_5),
        "mode": mode,
        "arms": list(arms),
        "runs_per_arm_target": per_arm_target,
        "parallel": n_parties,
        "runs": overall["runs"],
        "invalid_runs": invalid_runs,
        "kills": overall["kills"],
        "median_dps": overall["median_dps"],
        "median_hps": overall["median_hps"],
        "median_deaths": overall["median_deaths"],
        "by_arm": by_arm,
        "wave_errors": wave_errors,
        "trials": valid_runs,
        "extra_trials": extra_trials,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="Comma-separated subset of boss keys: " + ",".join(BOSSES))
    ap.add_argument(
        "--runs",
        type=int,
        default=3,
        help="Valid runs PER ARM per boss (default: 3; use 8-10 for a comparison report)",
    )
    ap.add_argument("--parallel", type=int, default=3, help="Parallel 5-man parties in one worldserver (default: 3)")
    ap.add_argument("--append", action="store_true", help="Merge into existing output JSON instead of overwriting")
    ap.add_argument(
        "--mode",
        choices=("llm", "llm_no_notes", "mixed", "mixed_all", "no_brain", "baseline"),
        default="llm",
        help="llm=harness+notes, scripts OFF; llm_no_notes=harness, no notes, scripts OFF; "
             "mixed=rotate llm & llm_no_notes across parallel parties; "
             "mixed_all=rotate all 4 modes (llm, llm_no_notes, no_brain, baseline) in waves; "
             "no_brain=no harness, scripts OFF; baseline=no harness, playerbots C++ scripts ON",
    )
    ap.add_argument(
        "--arms",
        default="",
        help="Optional comma-separated arms to mix in waves (e.g. llm,llm_no_notes,no_brain,baseline)",
    )
    args = ap.parse_args()
    keys = [k.strip() for k in args.only.split(",") if k.strip()] if args.only else list(BOSSES.keys())
    valid_arms = {"llm", "llm_no_notes", "no_brain", "baseline"}
    arms_override: tuple[str, ...] | None = None
    if args.arms:
        parsed_arms = tuple(a.strip() for a in args.arms.split(",") if a.strip())
        bad = [a for a in parsed_arms if a not in valid_arms]
        if bad:
            ap.error(f"invalid --arms: {bad}")
        arms_override = parsed_arms

    build.common("default", tests=False)
    build.server(test=True)
    out_json = VAR / f"bench_boss_10_{args.mode}.json"
    results = json.loads(out_json.read_text()) if (args.append and out_json.exists()) else {}
    needs_llm = (
        args.mode in ("llm", "llm_no_notes", "mixed", "mixed_all")
        or (arms_override is not None and any(a in ("llm", "llm_no_notes") for a in arms_override))
    )
    ctx = LlamaServer(slots=16) if needs_llm else None
    if ctx is not None:
        ctx.__enter__()
    try:
        for k in keys:
            try:
                results[k] = run_boss_waves(
                    **BOSSES[k],
                    mode=args.mode,
                    arms_override=arms_override,
                    target_runs=max(1, args.runs),
                    parallel=max(1, args.parallel),
                )
            except Exception as exc:
                say(f"BOSS {k} FAILED (continuing to remaining bosses): {exc}")
                results[k] = {"boss": BOSSES[k]["boss_name"], "mode": args.mode, "error": str(exc)}
            out_json.write_text(json.dumps(results, indent=2) + "\n")
    finally:
        if ctx is not None:
            ctx.__exit__(None, None, None)
    say(f"saved 10-boss benchmark results -> {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
