"""One bot whispers another and gets a whisper back.

The chat path of phase 2's first "done when" item (docs/design.md), without the model: the
module carries out a Say command as a whisper, its whisper hook hears it on the other bot, and the
harness gets it as a chat event. The brain is the scripted one of tools/snapshot/fake_harness.py.
"""

from wlblib.testing import expect

NAME = "whisper"
DESCRIPTION = "Loada whispers Telda, Telda hears it and whispers back, Loada hears that"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2
# From the Say command to the chat event on the other bot: one world tick or two.
HEARD_WITHIN_S = 2.0

ASK = "hey, want to group up?"
ANSWER = "sure, send me an invite"


def run(stack):
    t = stack.whisper("Loada", "Telda", ASK, ANSWER)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["errors"], f"a whisper failed: {t['errors']}")
    expect(len(t["commands"]) == 2, f"expected 2 whispers sent, got {t['commands']}")
    ask, answer = t["commands"]

    def heard(bot, sender, text):
        return next((h for h in t["heard"] if h["bot"] == bot and h.get("from") == sender
                     and h.get("text") == text and h.get("channel") == "WHISPER"), None)

    telda = heard("Telda", "Loada", ASK)
    expect(telda, f"Telda never heard Loada's whisper (heard: {t['heard']})")
    expect(telda["t"] - ask["t"] <= HEARD_WITHIN_S,
           f"Telda heard it {telda['t'] - ask['t']:.2f} s after it was sent")
    loada = heard("Loada", "Telda", ANSWER)
    expect(loada, f"Loada never heard Telda's answer (heard: {t['heard']})")
    expect(loada["t"] - answer["t"] <= HEARD_WITHIN_S,
           f"Loada heard it {loada['t'] - answer['t']:.2f} s after it was sent")
    said = [line for line in stack.server_log().splitlines() if " says (WHISPER to " in line]
    expect(len(said) == 2 and all(line.endswith(": ok") for line in said),
           f"the module's record of the whispers: {said}")
    return (f"heard in {telda['t'] - ask['t']:.2f} s and {loada['t'] - answer['t']:.2f} s")
