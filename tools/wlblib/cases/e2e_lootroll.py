"""A group loot roll wakes the brain while it is open (RH-H1).

  1. Level Loada and Telda to 20 and group them (Loada leads).
  2. `llmtest lootroll` kills a boar near Loada and starts a group roll on a
     green item (Buckskin Cape) under group loot.
  3. Telda gets a `loot_roll` event at once and `Need` / `Greed` / `Pass`
     options; choosing `Greed` is accepted (no error), and the roll options
     go away.
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

NAME = "lootroll"
DESCRIPTION = "A group loot roll wakes the member's brain; Greed is accepted"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2
ITEM = 1355  # Buckskin Cape (green).


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def run(stack):
    for bot in ("Loada", "Telda"):
        server.console(f"character level {bot} 20", inst=TEST)
    server.console("llmtest group Loada Telda", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()
    out = server.console(f"llmtest lootroll Loada {ITEM}", inst=TEST)
    started_at = time.time()
    expect("roll started" in out, f"setup: no roll started ({out.strip()[-200:]})")

    roll_event_at = None
    greed_cmd = None
    greed_sent_at = None
    options_gone = False
    errors = []
    deadline = time.time() + 30.0
    try:
        while time.time() < deadline and not options_gone:
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                continue
            for payload in reader.feed(chunk):
                msg = wire_py.decode_module_message(payload)
                if not msg:
                    continue
                if "event" in msg:
                    if msg["event"].get("bot") != "Telda":
                        continue
                    ev = msg["event"].get("event", {})
                    if "loot_roll" in ev and roll_event_at is None:
                        roll_event_at = time.time()
                    err = ev.get("error")
                    if err and err.get("command_id") == greed_cmd:
                        errors.append(err.get("text"))
                    continue
                snap = msg.get("snapshot")
                if not snap or snap.get("bot") != "Telda":
                    continue
                options = snap.get("options", [])
                greed = next((i for i, o in enumerate(options, 1)
                              if o.get("text", "").startswith("Greed Buckskin Cape")), None)
                if greed_cmd is None and roll_event_at is not None and greed:
                    greed_cmd = 99
                    _send_cmd(sock, {"bot": "Telda", "command_id": greed_cmd,
                                     "choose": {"option": greed, "seq": snap.get("seq", 0)}})
                    greed_sent_at = time.time()
                elif greed_cmd is not None and time.time() - greed_sent_at > 1.0:
                    options_gone = not any("Buckskin Cape" in o.get("text", "")
                                           for o in options)
    finally:
        sock.close()
        server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

    expect(roll_event_at is not None, "Telda got no loot_roll event after the roll started")
    delay = roll_event_at - started_at
    expect(delay <= 5.0, f"loot_roll event took {delay:.1f} s (> 5 s)")
    expect(greed_cmd is not None, "Telda was never offered `Greed Buckskin Cape`")
    expect(not errors, f"Greed produced errors: {errors}")
    expect(options_gone, "the roll options stayed after Telda chose Greed")
    return f"loot_roll woke Telda {delay:.2f} s after the roll started; Greed accepted"
