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
        stage_xyz=(805.0, 123.5, 510.0),
        spawn_xyz=(835.0, 123.5, 510.0),
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
        stage_xyz=(-157.9, -445.0, 15.9),
        spawn_xyz=(-157.9, -478.0, 15.9),
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
        stage_xyz=(44.7, 238.0, 25.2),
        spawn_xyz=(44.7, 268.0, 25.2),
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
        stage_xyz=(148.5, 138.0, -16.6),
        spawn_xyz=(148.5, 168.0, -16.6),
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

INST_RE = re.compile(r"instance=(\d+)")
DO_MS_RE = re.compile(r"do_ms=(\d+)")


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


def _parse_harness_latency(log_path: pathlib.Path) -> dict[str, float | None]:
    if not log_path.exists():
        return {"p50_ms": None, "p95_ms": None, "calls": 0}
    vals = [int(m.group(1)) for m in DO_MS_RE.finditer(log_path.read_text(errors="replace"))]
    if not vals:
        return {"p50_ms": None, "p95_ms": None, "calls": 0}
    vals.sort()
    p50 = vals[len(vals) // 2]
    p95 = vals[min(len(vals) - 1, int(len(vals) * 0.95))]
    return {"p50_ms": float(p50), "p95_ms": float(p95), "calls": len(vals)}


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
    target_runs: int = 3,
    parallel: int = 3,
) -> dict:
    """Runs `target_runs` valid trials for one boss inside a single persistent worldserver."""
    n_parties = 1 if raid else max(1, min(parallel, target_runs, len(PREFIXES)))
    parties = _make_parties(n_parties, raid=raid)
    all_bots = [b for p in parties for b in p]
    use_harness = mode in ("llm", "llm_no_notes")

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
    if strip_strat and mode != "baseline":
        c_strats += f",-{strip_strat}"
        nc_strats += f",-{strip_strat}"
    lines.append(f'LlmBrain.CombatStrategies = "{c_strats}"')
    lines.append(f'LlmBrain.NonCombatStrategies = "{nc_strats}"')
    conf_path.write_text("\n".join(lines) + "\n")

    valid_runs: list[dict] = []
    invalid_runs = 0
    wave = 0
    max_waves = (target_runs // n_parties) + 3

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
                out = server.console(f"llmtest gear {name} {level} {spec_tab}", wait_s=0.5, inst=TEST)
                if f"role={expected_role}" not in (out or ""):
                    raise RuntimeError(
                        f"role mismatch for {name}: expected role={expected_role}, got: {out!r}"
                    )
                for qid in (attune_quests or []):
                    server.console(f"llmtest questcomplete {name} {qid}", wait_s=0.2, inst=TEST)
                if role_items and idx in role_items:
                    server.console(f"llmtest item {name} {role_items[idx]}", wait_s=0.2, inst=TEST)

        while len(valid_runs) < target_runs and wave < max_waves:
            wave += 1
            active_parties = parties[: min(n_parties, target_runs - len(valid_runs))]
            say(f"boss benchmark [{era} | {tag} | {mode} | wave {wave}]: {len(active_parties)} parallel parties")

            # 1. Move to holding spot outside the dungeon (map 0) and cleanly reset every bot + instance.
            for p_idx, party in enumerate(active_parties):
                hx, hy, hz = -8949.95 + p_idx * 10.0, -132.49, 83.53
                for name, _, _, _ in party:
                    server.console(f"llmtest tele {name} 0 {hx:.2f} {hy:.2f} {hz:.2f} 0", wait_s=0.25, inst=TEST)
                for name, _, _, _ in party:
                    out = server.console(f"llmtest resetbot {name}", wait_s=0.25, inst=TEST)
                    if "alive=1" not in (out or ""):
                        raise RuntimeError(f"dirty start state for {name}: {out!r}")

            # 2. Teleport each party to its own fresh dungeon instance at stage_xyz, wait for
            #    cross-map worldport to complete, then assert distinct non-zero instance IDs.
            sx, sy, sz = stage_xyz
            for party in active_parties:
                for idx, (name, _, _, _) in enumerate(party):
                    out = server.console(
                        f"llmtest tele {name} {map_id} {sx + (idx % 5) * 0.8:.2f} {sy + (idx // 5) * 0.8:.2f} {sz:.2f} 0",
                        wait_s=0.35,
                        inst=TEST,
                    )
                    if "teleported" not in (out or ""):
                        raise RuntimeError(f"stage teleport failed for {name}: {out!r}")

            # Wait for cross-map worldport + Playerbots dungeon strategy attachment.
            time.sleep(3.5)
            party_instances: list[int] = []
            for party in active_parties:
                inst_id = None
                for name, _, _, _ in party:
                    wout = server.console(f"llmtest where {name}", wait_s=0.25, inst=TEST)
                    m = INST_RE.search(wout or "")
                    if f"map={map_id}" not in (wout or "") or not m:
                        raise RuntimeError(f"{name} did not arrive on map {map_id}: {wout!r}")
                    cur_inst = int(m.group(1))
                    if inst_id is None:
                        inst_id = cur_inst
                    elif cur_inst != inst_id:
                        raise RuntimeError(
                            f"party member {name} landed in instance {cur_inst}, expected {inst_id}"
                        )
                if inst_id == 0:
                    raise RuntimeError(f"expected non-zero dungeon instance id on map {map_id}")
                party_instances.append(inst_id)

            if len(set(party_instances)) != len(party_instances):
                raise RuntimeError(
                    f"parallel parties collided in same dungeon instance: {party_instances}"
                )
            say(f"  verified distinct dungeon instances: {party_instances}")

            # 3. Verify dungeon strategy state on every bot after arrival.
            if strip_strat:
                for party in active_parties:
                    for name, _, _, _ in party:
                        chk = server.console(f"llmtest checkstrat {name} {strip_strat}", wait_s=0.25, inst=TEST)
                        if "exists=1" not in (chk or ""):
                            raise RuntimeError(f"strategy {strip_strat!r} does not exist on {name}: {chk!r}")
                        if mode != "baseline":
                            if "combat=0" not in chk or "noncombat=0" not in chk:
                                raise RuntimeError(
                                    f"strategy {strip_strat!r} was not stripped on {name}: {chk!r}"
                                )

            # 4. Start fresh harness (if llm / llm_no_notes) with a clean --bots-dir for this wave.
            wave_start_ms = int(time.time() * 1000)
            with tempfile.TemporaryDirectory(prefix=f"wlb-boss-{tag}-{mode}-w{wave}-") as tmp:
                tmp_dir = pathlib.Path(tmp)
                bots_dir = tmp_dir / "bots"
                bots_dir.mkdir(parents=True)
                if notes_md and mode == "llm":
                    for party in active_parties:
                        for name, _, _, _ in party:
                            bdir = bots_dir / name
                            bdir.mkdir(parents=True, exist_ok=True)
                            (bdir / "notes.md").write_text(notes_md)
                harness_log = tmp_dir / "harness.log"
                proc = None
                if use_harness:
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
                time.sleep(8.0)

                # 5. Move all active parties to spawn_xyz simultaneously to initiate the pull.
                pull_start_ms = int(time.time() * 1000)
                x, y, z = spawn_xyz
                for party in active_parties:
                    for idx, (name, _, _, _) in enumerate(party):
                        server.console(
                            f"llmtest move {name} {x + (idx % 5) * 0.8:.2f} {y + (idx // 5) * 0.8:.2f} {z:.2f} 0",
                            wait_s=0.2,
                            inst=TEST,
                        )
                t0 = time.monotonic()
                done_flags = [False] * len(active_parties)
                try:
                    while time.monotonic() - t0 < max_s:
                        if proc is not None and proc.poll() is not None:
                            break
                        time.sleep(5.0)
                        all_tr = boss_meter.trace_parity.load_traces(trace_dir)["bot"]
                        for p_idx, party in enumerate(active_parties):
                            if done_flags[p_idx]:
                                continue
                            pnames = [b[0] for b in party]
                            sub = _slice_traces_after(all_tr, pnames, pull_start_ms - 1000)
                            res = boss_meter.analyze_boss_fight(sub, boss_name=boss_name)
                            if res["killed"]:
                                say(f"  party {p_idx + 1} (inst {party_instances[p_idx]}): {boss_name} KILLED in {res['duration_s']:.1f}s!")
                                done_flags[p_idx] = True
                            elif res["total_deaths"] >= len(party):
                                say(f"  party {p_idx + 1} (inst {party_instances[p_idx]}): wiped ({res['total_deaths']} deaths)")
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
                lat = _parse_harness_latency(harness_log)

            # 6. Score each party in this wave from its post-pull slice.
            all_tr = boss_meter.trace_parity.load_traces(trace_dir)["bot"]
            for p_idx, party in enumerate(active_parties):
                pnames = [b[0] for b in party]
                sub = _slice_traces_after(all_tr, pnames, pull_start_ms - 1000)
                res = boss_meter.analyze_boss_fight(sub, boss_name=boss_name)
                res["era"] = era
                res["party_size"] = len(party)
                res["level"] = level
                res["mode"] = mode
                res["wave"] = wave
                res["party_index"] = p_idx
                res["instance_id"] = party_instances[p_idx]
                res["harness_latency"] = lat
                res["invalid"] = (res["total_dps"] == 0.0 and res["total_deaths"] == 0 and not res["killed"])
                print("\n" + boss_meter.format_boss_meter(res))
                if res["invalid"]:
                    invalid_runs += 1
                    say(f"  party {p_idx + 1}: INVALID trial (no pull / 0 DPS / 0 deaths)")
                elif len(valid_runs) < target_runs:
                    valid_runs.append(res)
    finally:
        server.server_stop(inst=TEST)

    kills = sum(1 for r in valid_runs if r["killed"])
    return {
        "boss": boss_name,
        "era": era,
        "level": level,
        "party_size": len(COMP_10 if raid else COMP_5),
        "mode": mode,
        "parallel": n_parties,
        "runs": len(valid_runs),
        "invalid_runs": invalid_runs,
        "kills": kills,
        "median_dps": round(statistics.median(r["total_dps"] for r in valid_runs), 1) if valid_runs else 0.0,
        "median_hps": round(statistics.median(r["total_hps"] for r in valid_runs), 1) if valid_runs else 0.0,
        "median_deaths": round(statistics.median(r["total_deaths"] for r in valid_runs), 1) if valid_runs else 0.0,
        "trials": valid_runs,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="Comma-separated subset of boss keys: " + ",".join(BOSSES))
    ap.add_argument("--runs", type=int, default=3, help="Valid runs per boss (default: 3)")
    ap.add_argument("--parallel", type=int, default=3, help="Parallel 5-man parties in one worldserver (default: 3)")
    ap.add_argument(
        "--mode",
        choices=("llm", "llm_no_notes", "no_brain", "baseline"),
        default="llm",
        help="llm=harness+notes, scripts OFF; llm_no_notes=harness, no notes, scripts OFF; "
             "no_brain=no harness, scripts OFF; baseline=no harness, playerbots C++ scripts ON",
    )
    args = ap.parse_args()
    keys = [k.strip() for k in args.only.split(",") if k.strip()] if args.only else list(BOSSES.keys())

    build.common("default", tests=False)
    build.server(test=True)
    out_json = VAR / f"bench_boss_10_{args.mode}.json"
    results = json.loads(out_json.read_text()) if out_json.exists() else {}
    ctx = LlamaServer(slots=16) if args.mode in ("llm", "llm_no_notes") else None
    if ctx is not None:
        ctx.__enter__()
    try:
        for k in keys:
            results[k] = run_boss_waves(
                **BOSSES[k],
                mode=args.mode,
                target_runs=max(1, args.runs),
                parallel=max(1, args.parallel),
            )
            out_json.write_text(json.dumps(results, indent=2) + "\n")
    finally:
        if ctx is not None:
            ctx.__exit__(None, None, None)
    say(f"saved 10-boss benchmark results -> {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
