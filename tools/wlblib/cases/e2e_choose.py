"""A bot told to Attack a boar fights it; told to Loot, it loots the corpse.

The whole chain of step M1 (docs/plans/harness.md): the module offers the options, a Choose
names one of them in a snapshot, the bot's own body (playerbots) carries it out, and the world
shows it. The brain here is the scripted one of tools/snapshot/fake_harness.py.
"""

import re

from wlblib.testing import expect

NAME = "choose"
DESCRIPTION = "Attack a boar, it fights; Loot the corpse, it is looted (one Choose each)"
TEST_CONFIG = "choose"  # config/tests/choose.*.overrides: one level-1 priest next to a boar.
BOT = "Loada"
# age_at_apply here is the plumbing only: the scripted brain answers at once.
MAX_AGE_AT_APPLY_MS = 100
# After a Choose, how long the world may take to show it (walking to the boar, a fight).
FIGHT_WITHIN_S = 20
LOOTED_WITHIN_S = 30

CHOSE = re.compile(r"chose (\d+) of snapshot (\d+) \((.*?)\): (.*?), age_at_apply=(-?\d+) ms")


def run(stack):
    t = stack.play(["Attack", "Loot"])
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"],
           f"never offered: {', '.join(t['never_offered'])} "
           f"(options seen: {sorted({o for s in t['snapshots'] for o in s['options']})})")
    attack, loot = t["commands"]
    snapshots = t["snapshots"]

    # No Choose failed: a failed one comes back as an error event naming its command.
    ids = {attack["command_id"], loot["command_id"]}
    failed = [e["event"]["error"] for e in t["events"]
              if e["event"].get("error", {}).get("command_id") in ids]
    expect(not failed, f"a choice failed: {failed}")

    # Attack: the bot is in combat soon after.
    fight = next((s for s in snapshots if s["t"] > attack["t"] and s["in_combat"]), None)
    expect(fight and fight["t"] - attack["t"] <= FIGHT_WITHIN_S,
           f"the bot wasn't in combat within {FIGHT_WITHIN_S} s of Attack")

    # Loot: the corpse is looted, so the Loot option goes away.
    cleared = next((s for s in snapshots if s["t"] > loot["t"]
                    and not any(o.startswith("Loot") for o in s["options"])), None)
    expect(cleared and cleared["t"] - loot["t"] <= LOOTED_WITHIN_S,
           f"the corpse still offered Loot {LOOTED_WITHIN_S} s after Loot")

    # The module's own record of each choice: accepted, and how old the picture was.
    chose = CHOSE.findall(stack.server_log())
    expect(len(chose) == 2, f"expected 2 'chose' lines in the server log, found {len(chose)}")
    for option, seq, what, result, age_ms in chose:
        expect(result == "ok", f"the module refused '{what}': {result}")
        expect(int(age_ms) <= MAX_AGE_AT_APPLY_MS,
               f"age_at_apply {age_ms} ms for '{what}' (limit {MAX_AGE_AT_APPLY_MS} ms)")
    ages = "/".join(c[4] for c in chose)
    return (f"in combat {fight['t'] - attack['t']:.1f} s after Attack, looted "
            f"{cleared['t'] - loot['t']:.1f} s after Loot, age_at_apply {ages} ms")
