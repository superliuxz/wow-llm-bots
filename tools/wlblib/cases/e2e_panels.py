"""Open bags, point at items, close bags: the panel and tooltips show and go.

docs/specs/options.md: `open` / `close` / `hover` commands; the module fills `ext.bags` only while
open and the tooltip for a few snapshots; item "Use:" text comes from Spell.dbc with its values
filled in as the client does: the hearthstone's home ($z), the bread's "61 health over 18 sec"
($o1 of a regen aura: per 5 s, floating point) and the potion's "70 to 90" (a range, never a
roll): review of f40b0b6, finding 16.
"""

from wlblib.testing import expect

NAME = "panels"
DESCRIPTION = "open bags, hover the hearthstone (tooltip from Spell.dbc), close bags"
TEST_CONFIG = "talk"  # One fresh level-1 priest standing still at the Valley of Trials.
BOT = "Loada"


def run(stack):
    t = stack.play(["open:bags", "hover:Hearthstone", "hover:Tough Hunk of Bread",
                    "hover:Minor Healing Potion", "close:bags"], seconds=50, tail_s=6)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"], f"never sent: {', '.join(t['never_offered'])} (no "
           "Hearthstone in the open bags?)")
    open_cmd, hover_cmd, bread_cmd, potion_cmd, close_cmd = t["commands"]
    errors = [e["event"]["error"]["text"] for e in t["events"] if "error" in e["event"]]
    expect(not errors, f"the module refused: {errors}")

    shown = next((s for s in t["snapshots"]
                  if s["t"] > open_cmd["t"] and "BAGS" in s["open_panels"]), None)
    expect(shown, "no snapshot showed the bags after `open`")
    expect("Hearthstone" in shown["bag_items"], f"bags without a Hearthstone: {shown['bag_items']}")

    def use_line(cmd, until, start):
        tip = next((s["tooltips"][0] for s in t["snapshots"]
                    if cmd["t"] < s["t"] < until and s["tooltips"]
                    and any(line.startswith(start) for line in s["tooltips"][0])), None)
        expect(tip, f"no tooltip with '{start}' after `{cmd['text']}`")
        return next(line for line in tip if line.startswith(start))

    use = use_line(hover_cmd, bread_cmd["t"] + 2, "Use: Returns you to")
    expect("$z" not in use, f"the hearthstone's home not filled in: {use}")
    bread = use_line(bread_cmd, potion_cmd["t"] + 2, "Use: Restores")
    expect(bread.startswith("Use: Restores 61 health over 18 sec"),
           f"the bread's tooltip: {bread!r}, expected 'Restores 61 health over 18 sec'")
    potion = use_line(potion_cmd, close_cmd["t"] + 2, "Use: Restores")
    expect(potion.startswith("Use: Restores 70 to 90 health"),
           f"the potion's tooltip: {potion!r}, expected 'Restores 70 to 90 health'")

    gone = next((s for s in t["snapshots"]
                 if s["t"] > close_cmd["t"] and "BAGS" not in s["open_panels"]
                 and not s["bag_items"]), None)
    expect(gone, "the bags still showed after `close`")
    return (f"{len(shown['bag_items'])} items in the bags; '{use}'; '{bread}'; '{potion}'; closed "
            f"{gone['t'] - close_cmd['t']:.1f} s after close")
