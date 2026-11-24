"""Two tanks in a party: the brain lets the other tank, and can take it back (RH-M1).

  1. Level Loada and Telda to 20, group them, and give both playerbots' tank
     strategy (`+tank assist`, the strategy that makes a bot a tank).
  2. Telda is offered `Let Loada tank (stop tanking)`; choosing it removes
     Telda's `tank assist` (checked with `llmtest istank`), and offers
     `Tank for the group again`.
  3. Choosing that gives Telda `tank assist` back.
"""

import re
import socket
import sys
import time

from wlblib import server
from wlblib.common import ROOT, TEST
from wlblib.testing import expect

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
import wire_py  # noqa: E402
from frame_reader import FrameReader  # noqa: E402

NAME = "tank"
DESCRIPTION = "With two tanks grouped, the brain stands down as tank and back"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def _find(options, prefix):
    return next((i for i, o in enumerate(options, 1)
                 if o.get("text", "").startswith(prefix)), None)


def _telda_tanks():
    out = server.console("llmtest istank Telda", inst=TEST)
    m = re.search(r"istank Telda=(\d)", out)
    return None if not m else m.group(1) == "1"


def run(stack):
    for bot in ("Loada", "Telda"):
        server.console(f"character level {bot} 20", inst=TEST)
        server.console(f"llmtest maketank {bot}", inst=TEST)
    server.console("llmtest group Loada Telda", inst=TEST)
    tanked_before = _telda_tanks()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()
    cmd_id = 0
    sent = {}
    errors = []
    stood_down = None
    tank_again = None

    def choose(idx, seq, step):
        nonlocal cmd_id
        cmd_id += 1
        sent[step] = cmd_id
        _send_cmd(sock, {"bot": "Telda", "command_id": cmd_id,
                         "choose": {"option": idx, "seq": seq}})

    deadline = time.time() + 45.0
    try:
        while time.time() < deadline and tank_again is None:
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                continue
            for payload in reader.feed(chunk):
                msg = wire_py.decode_module_message(payload)
                if not msg:
                    continue
                if "event" in msg:
                    err = msg["event"].get("event", {}).get("error")
                    if err and err.get("command_id") in sent.values():
                        errors.append(err.get("text"))
                    continue
                snap = msg.get("snapshot")
                if not snap or snap.get("bot") != "Telda":
                    continue
                seq, options = snap.get("seq", 0), snap.get("options", [])
                if "notank" not in sent:
                    idx = _find(options, "Let Loada tank")
                    if idx:
                        choose(idx, seq, "notank")
                        time.sleep(2.5)  # BotManager applies it on its 1 s tick.
                        stood_down = _telda_tanks() is False
                elif "tank" not in sent:
                    idx = _find(options, "Tank for the group again")
                    if idx:
                        choose(idx, seq, "tank")
                        time.sleep(2.5)
                        tank_again = _telda_tanks() is True
    finally:
        sock.close()
        server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.
        server.console("llmtest clearstrat", inst=TEST)

    expect(tanked_before, "setup: Telda did not have `tank assist`")
    expect("notank" in sent, "Telda was never offered `Let Loada tank (stop tanking)`")
    expect(stood_down, "Telda still had `tank assist` after letting Loada tank")
    expect("tank" in sent, "Telda was never offered `Tank for the group again`")
    expect(tank_again, "Telda did not get `tank assist` back")
    expect(not errors, f"choices produced errors: {errors}")
    return "Telda stood down as tank for Loada and took tanking back"
