"""Open bags, point at an item, close bags: the panel and tooltip show and go.

docs/plans/panels.md: `open` / `close` / `hover` commands; the module fills `ext.bags` only while
open and the tooltip for a few snapshots; item "Use:" text comes from Spell.dbc (the hearthstone:
"Returns you to $z" with the home filled in).
"""

from wlblib.testing import expect

NAME = "panels"
DESCRIPTION = "open bags, hover the hearthstone (tooltip from Spell.dbc), close bags"
TEST_CONFIG = "talk"  # One fresh level-1 priest standing still at the Valley of Trials.
BOT = "Loada"


def run(stack):
    t = stack.play(["open:bags", "hover:Hearthstone", "close:bags"], seconds=40, tail_s=6)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"], f"never sent: {', '.join(t['never_offered'])} (no "
           "Hearthstone in the open bags?)")
    open_cmd, hover_cmd, close_cmd = t["commands"]
    errors = [e["event"]["error"]["text"] for e in t["events"] if "error" in e["event"]]
    expect(not errors, f"the module refused: {errors}")

    shown = next((s for s in t["snapshots"]
                  if s["t"] > open_cmd["t"] and "BAGS" in s["open_panels"]), None)
    expect(shown, "no snapshot showed the bags after `open`")
    expect("Hearthstone" in shown["bag_items"], f"bags without a Hearthstone: {shown['bag_items']}")

    tip = next((s["tooltips"][0] for s in t["snapshots"]
                if s["t"] > hover_cmd["t"] and s["tooltips"]), None)
    expect(tip, "no tooltip after `hover`")
    use = next((line for line in tip if line.startswith("Use: Returns you to")), None)
    expect(use and "$z" not in use, f"no filled-in 'Use:' line in the tooltip: {tip}")

    gone = next((s for s in t["snapshots"]
                 if s["t"] > close_cmd["t"] and "BAGS" not in s["open_panels"]
                 and not s["bag_items"]), None)
    expect(gone, "the bags still showed after `close`")
    return (f"{len(shown['bag_items'])} items in the bags; '{use}'; closed "
            f"{gone['t'] - close_cmd['t']:.1f} s after close")
