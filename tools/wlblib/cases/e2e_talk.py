"""A bot told to Talk to a quest giver accepts the quest it offers.

docs/plans/options.md, "Talk to <name>": the bot walks up (here it is already in reach), and the
module does what the NPC is there for with the server's own quest code, then reports it in an
`info` event and sets `done_command_id`.
"""

from wlblib.testing import expect

NAME = "talk"
DESCRIPTION = "Talk to Kaltunk: the bot accepts his quest; info event and done signal"
TEST_CONFIG = "talk"  # A fresh level-1 priest 6 yd in front of Kaltunk.
BOT = "Loada"
DONE_WITHIN_S = 20


def run(stack):
    t = stack.play(["Talk to Kaltunk"], seconds=60, tail_s=DONE_WITHIN_S)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"],
           f"Talk to Kaltunk never offered (options seen: "
           f"{sorted({o for s in t['snapshots'] for o in s['options']})})")
    (talk,) = t["commands"]
    failed = [e["event"]["error"] for e in t["events"]
              if e["event"].get("error", {}).get("command_id") == talk["command_id"]]
    expect(not failed, f"the talk failed: {failed}")

    infos = [e["event"]["info"]["text"] for e in t["events"]
             if e["t"] >= talk["t"] and "info" in e["event"]]
    said = next((i for i in infos if i.startswith("talked to Kaltunk")), None)
    expect(said, f"no 'talked to Kaltunk' info event (info events: {infos})")
    expect("accepted" in said, f"Kaltunk's quest wasn't accepted: {said}")
    done = next((s for s in t["snapshots"] if s["done_command_id"] == talk["command_id"]), None)
    expect(done and done["t"] - talk["t"] <= DONE_WITHIN_S,
           f"no done signal within {DONE_WITHIN_S} s of Talk")
    return f"{said} (done {done['t'] - talk['t']:.1f} s later)"
