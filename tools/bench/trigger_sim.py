"""Trigger evaluator prototype: when would each bot wake its brain?

Replays captured snapshots (tools/bench/capture_snapshots.py) through the
default triggers of docs/specs/triggers.md that today's snapshot can show:
  died           self.state leaves ALIVE
  combat         entered combat with an attacker the plan doesn't expect
  new_attacker   an add: a new unit attacks the bot while it already fights another
  control        self.control appears (stunned, feared, charmed)
  hostile_player a hostile player comes within 40 yd (an `appears` watch, off by default)
plus the plan's own end, which isn't in the snapshot yet:
  plan_done      every plan lasts a random time with mean PLAN_S (the brain's choice)

Stand-in plan between wakes (the brain isn't called here): "questing: fight
what I pull". A fight the bot starts itself counts as expected: the attackers
present when combat starts are the plan's targets. Units that join later are
not. After a wake the bot's current attackers become expected (the brain saw
them and made a new plan).

Output: JSON lines of wakes {t, bot, why, seq} on stdout; a summary on stderr.
Usage: python3 -I tools/bench/trigger_sim.py <snapshot_dir> [PLAN_S=60] [--watch-players]
"""

import json
import pathlib
import random
import sys

# "combat" (entering combat when the plan expected no fight) never fires here yet: without the
# bot's own target in the snapshot (ext.target) a fight it starts can't be told from one that
# starts on it, so every fight is counted as expected. It is listed for the harness's urgency.
COMBAT_REASONS = {"died", "combat", "new_attacker", "control", "hostile_player"}


def attackers(snap):
    out = set()
    # Anything unfriendly targeting the bot (neutral mobs like boars count once
    # they fight back), as the combat feedback on its portrait would show.
    for u in snap.get("base", {}).get("nearby", {}).get("units", []):
        if u.get("targeting_me") and u.get("reaction") != "FRIENDLY" and not u.get("dead"):
            out.add(u.get("guid") or u.get("name"))
    return out


def hostile_players(snap):
    return {u.get("guid") or u.get("name")
            for u in snap.get("base", {}).get("nearby", {}).get("units", [])
            if u.get("kind") == "PLAYER" and u.get("reaction") == "HOSTILE"
            and (u.get("rel") or {}).get("dist_yd", 999) <= 40}


def simulate(bot, frames, plan_s, watch_players, rng):
    wakes = []
    expected, seen_players = set(), set()
    prev = None
    plan_ends = rng.expovariate(1 / plan_s) if plan_s else float("inf")
    t0 = int(frames[0]["time_ms"])
    for snap in frames:
        t = (int(snap["time_ms"]) - t0) / 1000
        self_ = snap.get("base", {}).get("self", {})
        why = None
        now_attackers = attackers(snap)
        in_combat = bool(self_.get("in_combat"))
        if prev is not None:
            was = prev.get("base", {}).get("self", {})
            if was.get("state", "ALIVE") == "ALIVE" and self_.get("state", "ALIVE") != "ALIVE":
                why = "died"
            elif self_.get("control") and not was.get("control"):
                why = "control"
            elif in_combat and not was.get("in_combat"):
                expected |= now_attackers  # The bot pulled these itself.
            elif in_combat and now_attackers - expected and len(now_attackers) > 1:
                # An add: someone new while the bot already fights another. A
                # lone attacker is the mob the bot pulled turning on it (the
                # bot's own target isn't in the snapshot yet: ext.target).
                why = "new_attacker"
            elif in_combat and len(now_attackers) == 1:
                expected |= now_attackers
        if not in_combat:
            expected = set()
        if why is None and watch_players:
            near = hostile_players(snap)
            new_players = near - seen_players
            # One who left is forgotten, so coming back is noticed again.
            seen_players = near
            if new_players:
                why = "hostile_player"
        if why is None and t >= plan_ends:
            why = "plan_done"
        if why:
            wakes.append({"t": t, "bot": bot, "why": why, "seq": snap["seq"]})
            expected |= now_attackers
            plan_ends = t + (rng.expovariate(1 / plan_s) if plan_s else float("inf"))
        prev = snap
    return wakes, (int(frames[-1]["time_ms"]) - t0) / 1000


def main():
    watch_players = "--watch-players" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--watch-players"]
    root = pathlib.Path(args[0])
    plan_s = float(args[1]) if len(args) > 1 else 60
    rng = random.Random(1)
    all_wakes, duration = [], 0
    for bot_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        files = sorted(bot_dir.glob("*.json"), key=lambda f: int(f.stem))
        frames = [json.loads(f.read_text()) for f in files]
        if len(frames) < 2:
            continue
        wakes, d = simulate(bot_dir.name, frames, plan_s, watch_players, rng)
        all_wakes += wakes
        duration = max(duration, d)
    bots = len({p.name for p in root.iterdir() if p.is_dir()})
    for w in sorted(all_wakes, key=lambda w: w["t"]):
        print(json.dumps(w))
    by = {}
    for w in all_wakes:
        by[w["why"]] = by.get(w["why"], 0) + 1
    per_min = 60 / duration / bots if duration else 0
    print(f"{bots} bots, {duration:.0f} s, plan {plan_s:g} s: "
          f"{len(all_wakes) * per_min:.2f} wakes per bot per minute ("
          + ", ".join(f"{k} {v * per_min:.2f}" for k, v in sorted(by.items())) + ")",
          file=sys.stderr)


if __name__ == "__main__":
    main()
