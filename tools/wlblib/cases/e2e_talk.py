"""A bot approaches a quest giver, then sees and chooses an explicit per-quest option.

Medium 2 ("Talk to" split into real brain choices):
  1. Starting 6 yd from Kaltunk (just outside 5 yd talk range), the brain chooses
     `Talk to Kaltunk` to walk up; C++ must NOT auto-accept quests on approach.
  2. Once in talk range, the brain is offered `Accept quest "Your Place In The
     World" from Kaltunk`, chooses it, and receives the `info` event +
     `done_command_id`.
"""

from wlblib.testing import expect

NAME = "talk"
DESCRIPTION = "Walk to Kaltunk, then explicitly accept his quest; info event and done signal"
TEST_CONFIG = "talk"  # A fresh level-1 priest 6 yd in front of Kaltunk.
BOT = "Loada"
DONE_WITHIN_S = 20


def run(stack):
    t = stack.play(
        ["Talk to Kaltunk", 'Accept quest "Your Place In The World" from Kaltunk'],
        seconds=60,
        tail_s=DONE_WITHIN_S,
    )
    expect(not t["closed"], "the module closed the connection")
    expect(
        not t["never_offered"],
        f"expected option never offered ({t['never_offered']}; options seen: "
        f"{sorted({o for s in t['snapshots'] for o in s['options']})})",
    )
    expect(len(t["commands"]) == 2, f"expected 2 commands, got {t['commands']}")
    talk, accept = t["commands"]
    failed = [
        e["event"]["error"]
        for e in t["events"]
        if e["event"].get("error", {}).get("command_id")
        in (talk["command_id"], accept["command_id"])
    ]
    expect(not failed, f"command failed: {failed}")

    infos = [
        e["event"]["info"]["text"]
        for e in t["events"]
        if e["t"] >= accept["t"] and "info" in e["event"]
    ]
    said = next(
        (i for i in infos if "accepted" in i and "Your Place In The World" in i),
        None,
    )
    expect(said, f"no 'accepted Your Place In The World' info event (info events: {infos})")
    done = next(
        (s for s in t["snapshots"] if s["done_command_id"] == accept["command_id"]),
        None,
    )
    expect(
        done and done["t"] - accept["t"] <= DONE_WITHIN_S,
        f"no done signal within {DONE_WITHIN_S} s of quest accept",
    )
    return f"{said} (done {done['t'] - accept['t']:.1f} s later)"
