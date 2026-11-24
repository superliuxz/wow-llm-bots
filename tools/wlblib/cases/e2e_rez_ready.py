"""Ready check and resurrection popups wake the brain; waiting for a rez (RH-H1, RH-M3).

  1. Level Loada and Telda to 20 and group them (Loada leads).
  2. Loada starts a ready check: Telda gets a READY_CHECK prompt event and a
     `Ready (confirm ready check)` option; choosing it works.
  3. Telda dies; it chooses `Wait for a resurrection`, and is still lying
     dead (not released) 8 s later.
  4. Loada offers a resurrection: Telda gets a RESURRECT_OFFER prompt event and
     an `Accept resurrection` option; choosing it brings Telda back alive.
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

NAME = "rez_ready"
DESCRIPTION = "Ready check and rez offer wake the brain; Wait for a resurrection holds the release"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2
STILL_DEAD_S = 8.0


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def _find(options, prefix):
    return next((i for i, o in enumerate(options, 1)
                 if o.get("text", "").startswith(prefix)), None)


def run(stack):
    for bot in ("Loada", "Telda"):
        server.console(f"character level {bot} 20", inst=TEST)
    server.console("llmtest group Loada Telda", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    sent = {}
    prompts = set()
    errors = []
    phase = "ready"  # ready -> die -> wait -> rez -> done
    waited_at = None
    still_dead_ok = False
    alive_again = False

    def choose(idx, seq, step):
        nonlocal cmd_id
        cmd_id += 1
        sent[step] = cmd_id
        _send_cmd(sock, {"bot": "Telda", "command_id": cmd_id,
                         "choose": {"option": idx, "seq": seq}})

    server.console("llmtest readycheck Loada", inst=TEST)
    deadline = time.time() + 60.0
    try:
        while time.time() < deadline and phase != "done":
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                chunk = None
            for payload in reader.feed(chunk or b""):
                msg = wire_py.decode_module_message(payload)
                if not msg:
                    continue
                if "event" in msg:
                    if msg["event"].get("bot") != "Telda":
                        continue
                    ev = msg["event"].get("event", {})
                    err = ev.get("error")
                    if err and err.get("command_id") in sent.values():
                        errors.append(err.get("text"))
                    if ev.get("prompt"):
                        prompts.add(str(ev["prompt"].get("kind")))
                    continue
                snap = msg.get("snapshot")
                if not snap or snap.get("bot") != "Telda":
                    continue
                seq, options = snap.get("seq", 0), snap.get("options", [])
                state = snap.get("base", {}).get("self", {}).get("state", "")
                if phase == "ready" and "READY_CHECK" in prompts:
                    idx = _find(options, "Ready (confirm ready check)")
                    if idx:
                        choose(idx, seq, "ready")
                        server.console("llmtest kill Telda", inst=TEST)
                        phase = "die"
                elif phase == "die" and state == "DEAD_BODY":
                    idx = _find(options, "Wait for a resurrection")
                    if idx:
                        choose(idx, seq, "wait")
                        waited_at = time.time()
                        phase = "wait"
                elif phase == "wait":
                    if state != "DEAD_BODY":
                        break  # Released (or rezzed) despite waiting.
                    if time.time() - waited_at >= STILL_DEAD_S:
                        still_dead_ok = True
                        server.console("llmtest rezoffer Telda Loada", inst=TEST)
                        phase = "rez"
                elif phase == "rez" and "RESURRECT_OFFER" in prompts and "accept" not in sent:
                    idx = _find(options, "Accept resurrection")
                    if idx:
                        choose(idx, seq, "accept")
                elif phase == "rez" and "accept" in sent and state == "ALIVE":
                    alive_again = True
                    phase = "done"
    finally:
        sock.close()
        server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

    expect("READY_CHECK" in prompts, f"no READY_CHECK prompt for Telda (got {prompts})")
    expect("ready" in sent, "Telda was never offered `Ready (confirm ready check)`")
    expect("wait" in sent, "dead Telda was never offered `Wait for a resurrection`")
    expect(still_dead_ok, f"Telda did not stay lying dead {STILL_DEAD_S:.0f} s after choosing to wait")
    expect("RESURRECT_OFFER" in prompts, f"no RESURRECT_OFFER prompt for Telda (got {prompts})")
    expect(alive_again, "Telda was not alive after `Accept resurrection`")
    expect(not errors, f"choices produced errors: {errors}")
    return (f"ready check answered; waited {STILL_DEAD_S:.0f} s dead; "
            "rez offer woke Telda and accepting revived her")
