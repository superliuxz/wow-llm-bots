"""A bot hurt by a fight, told to Rest, eats or drinks until full and the step is done.

docs/plans/options.md, "Rest": offered out of combat below full health or mana; the body uses
the food and water of its starting kit (playerbots' eat / drink actions, no free food) or sits.
The fight comes first: the same scene as `e2e choose`.
"""

from wlblib.testing import expect

NAME = "rest"
DESCRIPTION = "after a fight, Rest: health and mana back to full, step reported done"
TEST_CONFIG = "choose"  # One level-1 priest next to a boar.
BOT = "Loada"
REST_WITHIN_S = 60
# The boar may still be respawning when `e2e choose` ran first on the same stack (about 2 min).
RUN_S = 300


def run(stack):
    t = stack.play(["Attack", "Rest"], seconds=RUN_S, tail_s=REST_WITHIN_S)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"],
           f"never offered: {', '.join(t['never_offered'])} (the fight left the bot at full "
           f"health and mana?)")
    rest = t["commands"][1]
    failed = [e["event"]["error"] for e in t["events"]
              if e["event"].get("error", {}).get("command_id") == rest["command_id"]]
    expect(not failed, f"Rest failed: {failed}")

    start = next(s for s in t["snapshots"] if s["seq"] == rest["seq"])
    done = next((s for s in t["snapshots"] if s["done_command_id"] == rest["command_id"]), None)
    last = t["snapshots"][-1]
    expect(done and done["t"] - rest["t"] <= REST_WITHIN_S,
           f"no done signal within {REST_WITHIN_S} s of Rest (watched "
           f"{last['t'] - rest['t']:.0f} s; last {last['health']}/{last['health_max']} hp, "
           f"{last['power']}/{last['power_max']} mana)")
    expect(done["health"] == done["health_max"] and done["power"] == done["power_max"],
           f"done at {done['health']}/{done['health_max']} health, "
           f"{done['power']}/{done['power_max']} mana")
    return (f"{start['health']}/{start['health_max']} hp, {start['power']}/{start['power_max']} "
            f"mana -> full, done {done['t'] - rest['t']:.1f} s after Rest")
