"""Makes the hand-built eval cases of harness/testdata/eval/ (the `generated` ones in cases.json).

Each case starts from a real capture (quiet_empty_field: the Valley of Trials with nothing near,
or city_crowd_patience: Orgrimmar) and adds the units and state the situation needs, so the
screen the model reads is a real one with one thing changed. The checks are written against
option texts ("Attack Hare"), not numbers, so they survive changes to the screen's order.

Writes NAME.snapshot.json for every case below and replaces the `generated` entries of
cases.json (the captured cases are kept). Re-run after editing:
  python3 -I tools/snapshot/make_eval_cases.py
"""

import copy
import itertools
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
EVAL = ROOT / "harness" / "testdata" / "eval"
FIELD = "quiet_empty_field"
CITY = "city_crowd_patience"

_guids = itertools.count(0x9000)
DIRS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]
ME = "@me"


def unit(name, level=2, dist=10.0, *, reaction="HOSTILE", color="YELLOW", kind="NPC",
         classification="NORMAL", hp=100.0, looks_like="beast", targeting_me=False, dead=False,
         lootable=False, tapped_by=None, state="IDLE", quest_marker=None):
    """One named unit near the bot (on its nameplate)."""
    u = {
        "guid": f"0xF130000C{next(_guids):08X}",
        "name": name,
        "rel": {"dist_yd": float(dist), "dir": DIRS[int(dist) % 8], "height": "LEVEL",
                "in_los": True},
        "kind": kind, "level": level, "level_color": color, "reaction": reaction,
        "classification": classification, "looks_like": looks_like, "size": "MEDIUM",
        "detail": "NAMEPLATE",
    }
    if dead:
        u["dead"] = True
        u["activity"] = {"state": "DEAD"}
        if lootable:
            u["lootable"] = True
    else:
        u["health_pct"] = float(hp)
        u["activity"] = {"state": state}
    if targeting_me:
        u["targeting_me"] = True
        u["target"] = ME  # The capture's own bot, filled in by snapshot().
        u["activity"] = {"state": "FIGHTING", "with": ME}
    if tapped_by:
        u["tapped_by_other"] = True
        u["target"] = tapped_by
        u["activity"] = {"state": "FIGHTING", "with": tapped_by}
    if quest_marker:
        u["quest_marker"] = quest_marker
    return u


def boar(**kw):
    return unit("Mottled Boar", 2, reaction="NEUTRAL", looks_like="boar", **kw)


def scorpid(**kw):
    return unit("Scorpid Worker", 3, looks_like="scorpid", **kw)


def snapshot(base, units=(), *, level=None, hp_pct=None, mana_pct=None, in_combat=False,
             bags_free=None):
    snap = json.loads((EVAL / f"{base}.snapshot.json").read_text())
    snap = copy.deepcopy(snap)
    # The base capture's options are for its own scene: add_fixture_options.py writes the
    # module's options for this one (planned cases keep the screen's invented ones).
    snap.pop("options", None)
    me = snap["base"]["self"]
    if level is not None:
        me["unit"]["level"] = level
        me["xp"] = 0
        me["xp_next"] = {1: 400, 2: 900, 3: 1400, 10: 7600}.get(level, 5000)
        me["health_max"] = me["health_max"] + 15 * (level - 1)
        me["power_max"] = me["power_max"] + 20 * (level - 1)
    me["health"] = me["health_max"] if hp_pct is None else round(me["health_max"] * hp_pct / 100)
    me["power"] = me["power_max"] if mana_pct is None else round(me["power_max"] * mana_pct / 100)
    me["unit"]["health_pct"] = 100.0 * me["health"] / me["health_max"]
    me["unit"]["power_pct"] = 100.0 * me["power"] / me["power_max"]
    if in_combat:
        me["in_combat"] = True
    else:
        me.pop("in_combat", None)
    if bags_free is not None:
        me.setdefault("indicators", {})["bag_free_slots"] = bags_free
    snap["base"].setdefault("nearby", {}).setdefault("units", [])
    added = json.loads(json.dumps(list(units)).replace(f'"{ME}"', json.dumps(me["unit"]["name"])))
    snap["base"]["nearby"]["units"] = added + snap["base"]["nearby"]["units"]
    return snap


FIGHT_ONLY = ["rest", "explore", "go to town"]  # Never in combat: you can't eat or walk off.

# (name, summary, rule, reason, snapshot, checks)
CASES = [
    # --- chat: answering whispers with `tell` ---
    # Whisperers vary (review 2026-11-17, finding 8): one name everywhere taught the model
    # that name, not "answer whoever whispered".
    ("whisper_group_invite", "quiet field; Brakka whispers asking to group",
     "whispers back to Brakka", "whisper from Brakka: hey, want to group up for the boar quest?",
     snapshot(FIELD, [boar(dist=25)]),
     {"must_tell_to": "Brakka", "forbid_kinds": ["rest"]}),
    ("whisper_question", "quiet field; Lunessa asks where the cactus apples are",
     "whispers back to Lunessa", "whisper from Lunessa: do you know where the cactus apples are?",
     snapshot(FIELD),
     {"must_tell_to": "Lunessa"}),
    ("whisper_asks_for_water", "full HP/mana; Torvin asks for water",
     "whispers back to Torvin", "whisper from Torvin: can you spare some water?",
     snapshot(FIELD, [boar(dist=30)]),
     {"must_tell_to": "Torvin", "forbid_kinds": ["rest"]}),
    ("whisper_in_city", "in Orgrimmar; Gatda asks to meet at the bank",
     "whispers back to Gatda", "whisper from Gatda: meet me at the bank?",
     snapshot(CITY),
     {"must_tell_to": "Gatda"}),
    ("whisper_while_low_hp", "20% HP out of combat, scorpid near; Ozrik asks for help with it",
     "doesn't attack at 20% HP; whispers back to Ozrik",
     "whisper from Ozrik: can you help me kill this scorpid?",
     snapshot(FIELD, [scorpid(dist=12)], hp_pct=20),
     {"must_tell_to": "Ozrik", "forbid_kinds": ["attack"]}),
    ("whisper_during_fight", "in combat with a 40% boar attacking me; Mirela asks how it's going",
     "keeps fighting the boar (Attack it or Keep)",
     "whisper from Mirela: how's it going?",
     snapshot(FIELD, [boar(dist=4, hp=40, targeting_me=True)], hp_pct=70, in_combat=True),
     {"first_texts": ["Attack Mottled Boar", "Keep doing"], "forbid_kinds": FIGHT_ONLY}),
    ("gold_spam_whisper", "a gold seller whispers an advert", "doesn't answer the spammer",
     "whisper from Xqzvbnk: CHEAP GOLD www.g0ld4u.com 1000g = $5 fast delivery",
     snapshot(FIELD, [boar(dist=18)]),
     {"forbid_tell_to": "Xqzvbnk"}),
    ("no_whisper_unprompted", "quiet field, nobody talking", "doesn't whisper anyone",
     "plan done", snapshot(FIELD, [boar(dist=15)]),
     {"forbid_tell": True}),

    # --- combat ---
    ("attacked_next_to_corpse", "scorpid attacking me at 60% HP, a lootable boar corpse 6 yd",
     "fights first (Attack the scorpid or Keep), loots later",
     "attacked by Scorpid Worker",
     snapshot(FIELD, [scorpid(dist=3, hp=90, targeting_me=True),
                      boar(dist=6, dead=True, lootable=True)], hp_pct=60, in_combat=True),
     {"first_texts": ["Attack Scorpid Worker", "Keep doing"], "forbid_kinds": FIGHT_ONLY}),
    ("two_attackers", "two Vile Familiars attacking me (30% and 100%), me at 60%",
     "fights one of them or Keep; never rest/explore/town in combat",
     "attacked by Vile Familiar",
     snapshot(FIELD, [unit("Vile Familiar", 3, dist=3, hp=30, looks_like="imp",
                           targeting_me=True),
                      unit("Burning Blade Imp", 3, dist=5, looks_like="imp",
                           targeting_me=True)],
              hp_pct=60, in_combat=True),
     {"first_kinds": ["attack", "keep doing what I'm doing"], "forbid_kinds": FIGHT_ONLY}),
    ("low_mana_in_combat", "8% mana, boar at 25% attacking me",
     "finishes the boar (Attack or Keep); can't rest in combat", "mana<20",
     snapshot(FIELD, [boar(dist=4, hp=25, targeting_me=True)], mana_pct=8, in_combat=True),
     {"first_texts": ["Attack Mottled Boar", "Keep doing"], "forbid_kinds": FIGHT_ONLY}),
    ("low_hp_weak_attacker", "me at 30% HP in combat, scorpid at 10% attacking me",
     "finishes the scorpid (Attack or Keep)", "hp<35",
     snapshot(FIELD, [scorpid(dist=3, hp=10, targeting_me=True)], hp_pct=30, in_combat=True),
     {"first_texts": ["Attack Scorpid Worker", "Keep doing"], "forbid_kinds": FIGHT_ONLY}),
    ("caster_at_range", "a Vile Familiar casting at me from 25 yd",
     "fights it (Attack or Keep)", "attacked by Vile Familiar",
     snapshot(FIELD, [unit("Vile Familiar", 3, dist=25, looks_like="imp", targeting_me=True)],
              hp_pct=85, in_combat=True),
     {"first_texts": ["Attack Vile Familiar", "Keep doing"], "forbid_kinds": FIGHT_ONLY}),
    ("fresh_mob_mid_fight", "fighting a 50% boar; an untouched scorpid 8 yd away",
     "keeps on the boar, doesn't pull the scorpid", "entered combat",
     snapshot(FIELD, [boar(dist=3, hp=50, targeting_me=True), scorpid(dist=8)],
              hp_pct=75, in_combat=True),
     {"first_texts": ["Attack Mottled Boar", "Keep doing"], "forbid_texts": ["Attack Scorpid"],
      "forbid_kinds": FIGHT_ONLY}),
    ("finish_nearly_dead", "boar at 5% attacking me, me at 50%", "finishes it",
     "attacked by Mottled Boar",
     snapshot(FIELD, [boar(dist=3, hp=5, targeting_me=True)], hp_pct=50, in_combat=True),
     {"first_texts": ["Attack Mottled Boar", "Keep doing"], "forbid_kinds": FIGHT_ONLY}),

    # --- what to attack ---
    ("critter_and_boar", "a Hare 6 yd and a Mottled Boar 15 yd, full HP/mana",
     "never attacks the Hare; doesn't rest", "plan done",
     snapshot(FIELD, [unit("Hare", 1, dist=6, kind="CRITTER", reaction="NEUTRAL",
                           looks_like="critter"), boar(dist=15)]),
     {"forbid_texts": ["Attack Hare"], "forbid_kinds": ["rest"]}),
    ("only_critters", "only an Adder and a Hare around, full HP/mana",
     "attacks neither; doesn't rest", "plan done",
     snapshot(FIELD, [unit("Adder", 1, dist=7, kind="CRITTER", reaction="NEUTRAL",
                           looks_like="critter"),
                      unit("Hare", 1, dist=12, kind="CRITTER", reaction="NEUTRAL",
                           looks_like="critter")]),
     {"forbid_kinds": ["attack", "rest"]}),
    ("gray_mob_only", "level 10 priest; only a gray level-2 boar nearby",
     "doesn't attack a gray mob; doesn't rest", "plan done",
     snapshot(FIELD, [unit("Mottled Boar", 2, dist=9, reaction="NEUTRAL", color="GRAY",
                           looks_like="boar")], level=10),
     {"forbid_kinds": ["attack", "rest"]}),
    ("tapped_and_free", "a scorpid tapped by another player and a free boar",
     "never attacks the tapped scorpid", "plan done",
     snapshot(FIELD, [scorpid(dist=8, hp=60, tapped_by="Loadae"), boar(dist=14)]),
     {"forbid_texts": ["Attack Scorpid Worker"], "forbid_kinds": ["rest"]}),
    ("all_tapped", "the only two mobs are both tapped by other players",
     "attacks neither; doesn't rest", "plan done",
     snapshot(FIELD, [scorpid(dist=8, hp=60, tapped_by="Loadae"),
                      boar(dist=11, hp=40, tapped_by="Loadaf")]),
     {"forbid_kinds": ["attack", "rest"]}),
    ("elite_nearby", "L1 priest 14 yd from an orange elite Burning Blade Thug",
     "doesn't attack the elite", "plan done",
     snapshot(FIELD, [unit("Burning Blade Thug", 4, dist=14, color="ORANGE",
                           classification="ELITE", looks_like="orc")]),
     {"forbid_texts": ["Attack Burning Blade Thug"], "forbid_kinds": ["rest"]}),
    ("red_mob_nearby", "L1 priest 18 yd from a red level-6 Razormane Quilboar",
     "doesn't attack a mob five levels up", "plan done",
     snapshot(FIELD, [unit("Razormane Quilboar", 6, dist=18, color="RED",
                           looks_like="quilboar")]),
     {"forbid_texts": ["Attack Razormane Quilboar"], "forbid_kinds": ["rest"]}),
    ("skull_patrol_far", "a skull-level elite patrolling 35 yd away",
     "doesn't attack it", "plan done",
     snapshot(FIELD, [unit("Son of Arugal", 25, dist=35, color="SKULL_LEVEL",
                           classification="ELITE", looks_like="worgen", state="PATROLLING")]),
     {"forbid_kinds": ["attack", "rest"]}),

    # --- loot ---
    ("two_corpses_and_live", "two lootable corpses (boar, scorpid) and a live boar 20 yd",
     "loots first", "plan done",
     snapshot(FIELD, [boar(dist=5, dead=True, lootable=True),
                      scorpid(dist=7, dead=True, lootable=True), boar(dist=20)]),
     {"first_kinds": ["loot"]}),
    ("loot_scorpid", "one lootable Scorpid Worker corpse 6 yd away", "loots it",
     "plan done", snapshot(FIELD, [scorpid(dist=6, dead=True, lootable=True)]),
     {"first_kinds": ["loot"]}),

    # --- health and mana ---
    ("hp_30_boar_near", "30% HP out of combat, a boar 8 yd away",
     "rests or heads to town first; doesn't attack", "plan done",
     snapshot(FIELD, [boar(dist=8)], hp_pct=30),
     {"first_kinds": ["rest", "go to town"], "forbid_kinds": ["attack"]}),
    ("hp_30_boar_near_live", "30% HP out of combat, a boar 8 yd away (live options)",
     "doesn't attack", "plan done",
     snapshot(FIELD, [boar(dist=8)], hp_pct=30),
     {"forbid_kinds": ["attack"]}),
    ("mana_20_boar_near", "20% mana out of combat, a boar 8 yd away",
     "rests or heads to town first; doesn't attack", "plan done",
     snapshot(FIELD, [boar(dist=8)], mana_pct=20),
     {"first_kinds": ["rest", "go to town"], "forbid_kinds": ["attack"]}),
    ("mana_20_boar_near_live", "20% mana out of combat, a boar 8 yd away (live options)",
     "doesn't attack", "plan done",
     snapshot(FIELD, [boar(dist=8)], mana_pct=20),
     {"forbid_kinds": ["attack"]}),
    ("hp_15_empty_field", "15% HP, nothing around", "rests or heads to town first",
     "plan done", snapshot(FIELD, hp_pct=15),
     {"first_kinds": ["rest", "go to town"]}),
    ("mana_10_empty_field", "10% mana, nothing around", "rests or heads to town first",
     "plan done", snapshot(FIELD, mana_pct=10),
     {"first_kinds": ["rest", "go to town"]}),
    ("hp_60_fine", "60% HP, full mana, a boar 10 yd away",
     "doesn't rest above a third of health", "plan done",
     snapshot(FIELD, [boar(dist=10)], hp_pct=60),
     {"forbid_kinds": ["rest"]}),

    # --- people and places ---
    ("quest_giver_and_boar", "fresh L1; quest giver Kaltunk 25 yd, a boar 8 yd",
     "talks to Kaltunk first", "logged in",
     snapshot(FIELD, [boar(dist=8),
                      unit("Kaltunk", 20, dist=25, reaction="FRIENDLY", color="SKULL_LEVEL",
                           looks_like="humanoid", quest_marker="QUEST_AVAILABLE")]),
     {"first_texts": ["Talk to Kaltunk"]}),
    ("bags_full", "bags full, a boar 9 yd away", "heads to town instead of killing for loot",
     "plan done", snapshot(FIELD, [boar(dist=9)], bags_free=0),
     {"first_kinds": ["go to town"]}),
    ("leveled_up_empty_field", "just reached level 2, nothing around",
     "moves on (explore or town); doesn't rest or idle", "reached level 2",
     snapshot(FIELD, level=2),
     {"first_kinds": ["explore", "go to town"],
      "forbid_kinds": ["rest", "keep doing what I'm doing"]}),
]


def main():
    cases = json.loads((EVAL / "cases.json").read_text())
    kept = [c for c in cases if not c.get("generated")]
    out = list(kept)
    for name, summary, rule, reason, snap, checks in CASES:
        (EVAL / f"{name}.snapshot.json").write_text(json.dumps(snap, indent=2) + "\n")
        out.append({"name": name, "source": "generated (tools/snapshot/make_eval_cases.py)",
                    "generated": True, "reason": reason, "summary": summary, "rule": rule,
                    **checks})
    (EVAL / "cases.json").write_text(json.dumps(out, indent=2) + "\n")
    print(f"{len(kept)} captured + {len(CASES)} generated = {len(out)} cases; now run "
          "tools/snapshot/add_fixture_options.py")


if __name__ == "__main__":
    main()
