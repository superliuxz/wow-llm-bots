"""Bots hear the LookingForGroup channel (RH-M7).

`party` test config (Channel.RestrictedLfg = 0, so bots outside the queue can be in the channel).

  1. Loada and Loadb join LookingForGroup.
  2. Loada types "LFM RFC need heals" there (through the client's chat handler): Loadb hears it
     as LFG channel chat, without being woken (not named).
  3. Loada types "loadb want to come to RFC?": Loadb hears it, woken (named).
"""

import socket
import sys
import time

from wlblib import server
from wlblib.common import ROOT, TEST
from wlblib.testing import expect

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
import wire_py  # noqa: E402
from frame_reader import FrameReader  # noqa: E402

NAME = "lfg_channel"
DESCRIPTION = "LookingForGroup channel chat reaches bots; only a named bot is woken"
TEST_CONFIG = "party"
BOT = "Loada"
ONLINE = 4

PLAIN = "LFM RFC need heals"
NAMED = "loadb want to come to RFC?"


def run(stack):
    for bot in ("Loada", "Loadb"):
        out = server.console(f"llmtest joinchannel {bot} LookingForGroup", inst=TEST)
        expect("joined" in out, f"setup: {bot} could not join LookingForGroup ({out.strip()[-160:]})")

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()
    heard = {}  # text -> chat event as Loadb heard it
    try:
        time.sleep(1.0)  # Let the harness connection settle.
        server.console(f"llmtest chat Loada channel:LookingForGroup {PLAIN}", inst=TEST)
        server.console(f"llmtest chat Loada channel:LookingForGroup {NAMED}", inst=TEST)
        deadline = time.time() + 10.0
        while time.time() < deadline and len(heard) < 2:
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                continue
            for payload in reader.feed(chunk):
                msg = wire_py.decode_module_message(payload)
                if not msg or "event" not in msg or msg["event"].get("bot") != "Loadb":
                    continue
                chat = msg["event"].get("event", {}).get("chat")
                if chat and chat.get("from") == "Loada" and chat.get("text") in (PLAIN, NAMED):
                    heard[chat["text"]] = chat
    finally:
        sock.close()

    plain, named = heard.get(PLAIN), heard.get(NAMED)
    expect(plain, f"Loadb never heard {PLAIN!r} in LookingForGroup (heard {list(heard)})")
    expect(named, f"Loadb never heard {NAMED!r} in LookingForGroup (heard {list(heard)})")
    expect(plain.get("channel") == "LFG" and named.get("channel") == "LFG",
           f"not reported as LFG channel chat: {plain.get('channel')}, {named.get('channel')}")
    expect(not plain.get("at_me"), "an unnamed LFG line woke Loadb")
    expect(named.get("at_me"), "a line naming Loadb did not wake it")
    return "Loadb heard both LookingForGroup lines; only the one naming it woke it"
