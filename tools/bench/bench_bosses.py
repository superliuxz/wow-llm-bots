#!/usr/bin/env python3
"""10-Boss Benchmark Suite: 3 WotLK + 3 TBC + 3 Classic + 1 Trivial Raid.

Runs 10 properly geared boss encounters on the isolated test server with real
`harness run` + `llama-server` (Gemma 4). By default, strips playerbots'
hardcoded dungeon/raid boss scripts so all mechanics come from the LLM brain +
`notes.md`; pass `--baseline` to leave playerbots' boss scripts enabled for an
apples-to-apples baseline comparison, and `--runs N` (e.g. 3) for multi-run
aggregation.
"""

import argparse
import json
import pathlib
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

# (name, race:class, spec_tab, expected_role)
PARTY_5 = [
    ("Loada", "human:warrior", 2, "tank"),     # Protection Warrior (Tank)
    ("Loadb", "human:priest", 1, "heal"),      # Holy Priest (Healer)
    ("Loadc", "human:mage", 2, "dps"),         # Frost Mage (DPS)
    ("Loadd", "human:warlock", 2, "dps"),      # Destruction Warlock (DPS)
    ("Loade", "nightelf:hunter", 1, "dps"),    # Marksmanship Hunter (DPS)
]

RAID_10 = PARTY_5 + [
    ("Loadf", "dwarf:paladin", 1, "tank"),     # Protection Paladin (Off-tank)
    ("Loadg", "nightelf:druid", 2, "heal"),    # Restoration Druid (Healer 2)
    ("Loadh", "human:rogue", 1, "dps"),        # Combat Rogue (Melee DPS)
    ("Loadi", "dwarf:warrior", 1, "dps"),      # Fury Warrior (Melee DPS)
    ("Loadj", "human:mage", 0, "dps"),         # Arcane Mage (Ranged DPS)
]

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
        spawn_xyz=(832.0, 123.5, 510.0),
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
        spawn_xyz=(5640.0, 2507.4, 708.9),
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
        spawn_xyz=(1077.0, 1050.0, 655.5),
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
        spawn_xyz=(-157.9, -475.0, 15.9),
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
        spawn_xyz=(44.7, 265.0, 25.2),
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
        spawn_xyz=(148.5, 165.0, -16.6),
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
        spawn_xyz=(27.9, 65.0, -124.5),
        notes_md=THERADRAS_NOTES,
        max_s=210.0,
    ),
    "rattlegore": dict(
        era="Classic",
        tag="rattlegore_scholo",
        boss_name="Rattlegore",
        level=60,
        map_id=289,
        spawn_xyz=(137.1, 156.0, 96.0),
        notes_md=RATTLEGORE_NOTES,
        max_s=210.0,
    ),
    "anastari": dict(
        era="Classic",
        tag="anastari_strat",
        boss_name="Baroness Anastari",
        level=59,
        map_id=329,
        spawn_xyz=(3855.3, -3698.0, 148.2),
        notes_md=ANASTARI_NOTES,
        max_s=210.0,
    ),
    "patchwerk": dict(
        era="Trivial Raid",
        tag="patchwerk_naxx10",
        boss_name="Patchwerk",
        level=80,
        map_id=533,
        spawn_xyz=(3232.0, -3230.3, 294.1),
        raid=True,
        notes_md=PATCHWERK_NOTES,
        max_s=240.0,
    ),
}


def run_boss(
    era: str,
    tag: str,
    boss_name: str,
    level: int,
    map_id: int,
    spawn_xyz: tuple[float, float, float],
    strip_strat: str = "",
    attune_quests: list[int] | None = None,
    role_items: dict[int, int] | None = None,
    raid: bool = False,
    notes_md: str = "",
    max_s: float = 210.0,
    baseline: bool = False,
    run_idx: int = 1,
) -> dict:
    party = RAID_10 if raid else PARTY_5
    mode_tag = f"{tag}_{'base' if baseline else 'llm'}_r{run_idx}"
    trace_dir = VAR / f"bench_boss_{mode_tag}"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)

    server.server_stop(inst=TEST)
    server.test_databases_reset()
    server.config("soak", inst=TEST)

    conf_path = TEST.etc / "modules" / "mod-llm-brain.conf"
    bot_names = ",".join(b[0] for b in party)
    fixtures = ",".join(f"{b[0]}=new:{b[1]}" for b in party)
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
    if strip_strat and not baseline:
        c_strats += f",-{strip_strat}"
        nc_strats += f",-{strip_strat}"
    lines.append(f'LlmBrain.CombatStrategies = "{c_strats}"')
    lines.append(f'LlmBrain.NonCombatStrategies = "{nc_strats}"')
    conf_path.write_text("\n".join(lines) + "\n")

    try:
        server.server_start(TEST)
        server.wait_online(party[0][0], inst=TEST)
        server.wait_online_count(len(party), inst=TEST)

        leader = party[0][0]
        for name, _, _, _ in party[1:]:
            server.console(f"llmtest group {leader} {name}", inst=TEST)
        for idx, (name, _, spec_tab, expected_role) in enumerate(party):
            out = server.console(f"llmtest gear {name} {level} {spec_tab}", inst=TEST)
            if out and f"role={expected_role}" not in out:
                raise RuntimeError(
                    f"role mismatch for {name}: expected role={expected_role}, got: {out!r}"
                )
            for qid in (attune_quests or []):
                server.console(f"llmtest questcomplete {name} {qid}", inst=TEST)
            if role_items and idx in role_items:
                server.console(f"llmtest item {name} {role_items[idx]}", inst=TEST)

        # Wait for BotManager::EnsureStrategies to re-apply strategies after gear/spec reset,
        # then verify any stripped dungeon boss script actually exists and is OFF.
        time.sleep(2.5)
        if strip_strat:
            chk = server.console(f"llmtest checkstrat {leader} {strip_strat}", inst=TEST)
            if chk and "exists=1" not in chk:
                raise RuntimeError(f"stripped strategy {strip_strat!r} does not exist: {chk!r}")
            if not baseline and chk and ("combat=0" not in chk or "noncombat=0" not in chk):
                raise RuntimeError(f"strategy {strip_strat!r} was not stripped: {chk!r}")

        with tempfile.TemporaryDirectory(prefix=f"wlb-boss-{mode_tag}-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            bots_dir = tmp_dir / "bots"
            bots_dir.mkdir(parents=True)
            if notes_md and not baseline:
                for name, _, _, _ in party:
                    bdir = bots_dir / name
                    bdir.mkdir(parents=True, exist_ok=True)
                    (bdir / "notes.md").write_text(notes_md)
            harness_log = tmp_dir / "harness.log"
            # Start harness BEFORE teleporting to the boss so the LLM brain is live
            # before pull and handles the opening instead of playerbots acting alone.
            proc = subprocess.Popen(
                [
                    str(OUT / "common" / "harness"),
                    "run",
                    f"--socket={TEST.run / 'brain.sock'}",
                    f"--rules={ROOT / 'content' / 'RULES.md'}",
                    f"--bots-dir={bots_dir}",
                    "--host=127.0.0.1",
                    "--port=8090",
                    "--slots=16",
                    "--temperature=0",
                ],
                cwd=ROOT,
                stdout=harness_log.open("w"),
                stderr=subprocess.STDOUT,
            )
            # Pre-pull prep window: let harness connect and bots buff/conjure safely.
            time.sleep(10.0)
            x, y, z = spawn_xyz
            for idx, (name, _, _, _) in enumerate(party):
                server.console(
                    f"llmtest tele {name} {map_id} {x + (idx % 5) * 0.8:.2f} {y + (idx // 5) * 0.8:.2f} {z:.2f} 0",
                    inst=TEST,
                )
            time.sleep(2.0)
            t0 = time.monotonic()
            say(f"boss benchmark [{era} | {mode_tag}]: {boss_name} (L{level} {len(party)}-man, cap {max_s:.0f}s)")
            try:
                while time.monotonic() - t0 < max_s:
                    if proc.poll() is not None:
                        break
                    time.sleep(5.0)
                    traces = boss_meter.trace_parity.load_traces(trace_dir)["bot"]
                    res = boss_meter.analyze_boss_fight(traces, boss_name=boss_name)
                    if res["killed"]:
                        say(f"boss benchmark [{mode_tag}]: {boss_name} KILLED in {res['duration_s']:.1f}s!")
                        time.sleep(2.0)
                        break
                    if res["total_deaths"] >= len(party):
                        say(f"boss benchmark [{mode_tag}]: party wiped ({res['total_deaths']} deaths)")
                        break
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        server.server_stop(inst=TEST)

    traces = boss_meter.trace_parity.load_traces(trace_dir)["bot"]
    res = boss_meter.analyze_boss_fight(traces, boss_name=boss_name)
    res["era"] = era
    res["party_size"] = len(party)
    res["level"] = level
    res["baseline"] = baseline
    print("\n" + boss_meter.format_boss_meter(res))
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="Comma-separated subset of boss keys: " + ",".join(BOSSES))
    ap.add_argument("--runs", type=int, default=3, help="Runs per boss (default: 3)")
    ap.add_argument("--baseline", action="store_true", help="Keep playerbots' hardcoded boss scripts enabled")
    args = ap.parse_args()
    keys = [k.strip() for k in args.only.split(",") if k.strip()] if args.only else list(BOSSES.keys())

    build.common("default", tests=False)
    build.server(test=True)
    out_json = VAR / ("bench_boss_10_baseline.json" if args.baseline else "bench_boss_10_results.json")
    results = json.loads(out_json.read_text()) if out_json.exists() else {}
    with LlamaServer(slots=16):
        for k in keys:
            runs = [
                run_boss(**BOSSES[k], baseline=args.baseline, run_idx=i + 1)
                for i in range(max(1, args.runs))
            ]
            kills = sum(1 for r in runs if r["killed"])
            results[k] = {
                "boss": BOSSES[k]["boss_name"],
                "era": BOSSES[k]["era"],
                "level": BOSSES[k]["level"],
                "party_size": len(RAID_10 if BOSSES[k].get("raid") else PARTY_5),
                "baseline": args.baseline,
                "runs": len(runs),
                "kills": kills,
                "median_dps": round(statistics.median(r["total_dps"] for r in runs), 1),
                "median_hps": round(statistics.median(r["total_hps"] for r in runs), 1),
                "median_deaths": round(statistics.median(r["total_deaths"] for r in runs), 1),
                "trials": runs,
            }
            out_json.write_text(json.dumps(results, indent=2) + "\n")
    say(f"saved 10-boss benchmark results -> {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
