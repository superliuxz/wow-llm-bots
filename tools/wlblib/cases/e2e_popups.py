"""A timed group popup wakes the brain while it is still open (RH-H1).

The leader queues the party for Dungeon Finder; the member must get a
`ROLE_CHECK` prompt event at once (not only on its next unrelated wake) and a
snapshot that offers `Confirm Dungeon Finder role`. Answering it must work.

  1. Level Loada and Telda to 20 and group them (Loada leads).
  2. Choose Loada's `Queue Random Dungeon` option.
  3. Telda gets an ImmediateEvent `prompt: ROLE_CHECK` within 5 s, then a
     snapshot with a `Confirm Dungeon Finder role` option.
  4. Choosing it succeeds without an error event.
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

NAME = "popups"
DESCRIPTION = "LFG role check wakes the party member's brain while the popup is open"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def _is_role_check(prompt):
    return prompt.get("kind") in ("ROLE_CHECK", 6)


def run(stack):
    for bot in ("Loada", "Telda"):
        server.console(f"character level {bot} 20", inst=TEST)
    server.console("llmtest group Loada Telda", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    queue_cmd = None
    queued_at = None
    role_prompt_at = None
    role_cmd = None
    role_sent_at = None
    role_done = False
    errors = []

    deadline = time.time() + 40.0
    try:
        while time.time() < deadline and not role_done:
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                continue
            if not chunk:
                break
            for payload in reader.feed(chunk):
                msg = wire_py.decode_module_message(payload)
                if not msg:
                    continue
                if "event" in msg:
                    bot = msg["event"].get("bot")
                    ev = msg["event"].get("event", {})
                    if "error" in ev:
                        err = ev["error"]
                        if err.get("command_id") in (queue_cmd, role_cmd):
                            errors.append(f"{bot}: {err.get('text')}")
                    prompt = ev.get("prompt")
                    if (bot == "Telda" and prompt and _is_role_check(prompt)
                            and queued_at is not None and role_prompt_at is None):
                        role_prompt_at = time.time()
                    continue
                snap = msg.get("snapshot")
                if not snap:
                    continue
                bot = snap.get("bot", "")
                seq = snap.get("seq", 0)
                options = snap.get("options", [])
                members = [m.get("name", "") for m in
                           snap.get("ext", {}).get("party", {}).get("members", [])]

                if bot == "Loada" and queue_cmd is None and "Telda" in members:
                    idx = next((i for i, o in enumerate(options, 1)
                                if o.get("text", "").startswith("Queue Random Dungeon")),
                               None)
                    if idx is not None:
                        cmd_id += 1
                        queue_cmd = cmd_id
                        _send_cmd(sock, {"bot": "Loada", "command_id": cmd_id,
                                         "choose": {"option": idx, "seq": seq}})
                        queued_at = time.time()
                    continue

                if bot == "Telda" and role_prompt_at is not None and role_cmd is None:
                    idx = next((i for i, o in enumerate(options, 1)
                                if o.get("text", "").startswith("Confirm Dungeon Finder role")),
                               None)
                    if idx is not None:
                        cmd_id += 1
                        role_cmd = cmd_id
                        _send_cmd(sock, {"bot": "Telda", "command_id": cmd_id,
                                         "choose": {"option": idx, "seq": seq}})
                        role_sent_at = time.time()
                    continue

                if role_cmd is not None and time.time() - role_sent_at >= 2.0:
                    role_done = True
                    break
    finally:
        sock.close()
        server.console("lfg clean", inst=TEST)
        server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

    expect(queue_cmd is not None, "Loada never offered `Queue Random Dungeon` while grouped")
    expect(role_prompt_at is not None,
           "Telda got no ROLE_CHECK prompt event after Loada queued (popup did not wake the brain)")
    delay = role_prompt_at - queued_at
    expect(delay <= 5.0, f"ROLE_CHECK prompt took {delay:.1f} s (> 5 s)")
    expect(role_cmd is not None, "Telda's snapshot never offered `Confirm Dungeon Finder role`")
    expect(not errors, f"queue/role choice produced errors: {errors}")
    return f"ROLE_CHECK woke Telda {delay:.2f} s after Loada queued; role confirmed without error"
