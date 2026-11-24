"""Live verification of playability fixes: party chat non-loop & talent spending.

Verifies:
  1. High 4: When Loada and Telda are grouped and Loada sends a PARTY chat line,
     Telda hears it with `at_me=False` (bot-to-bot party chat must not trigger
     wake/reply loops).
  2. High 1: When Loada levels to 20 (gaining unspent talent points), Loada's
     snapshot offers `Spend talent points in <Tree>`, choosing it spends all
     free points, and the option disappears on subsequent snapshots.
  3. Cleanup: Loada leaves the group so shared `whisper` stack stays clean.
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

NAME = "playability"
DESCRIPTION = "Bot-to-bot party chat has at_me=False; bot spends unspent talent points"
TEST_CONFIG = "playability"
BOT = "Loada"
ONLINE = 2


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def run(stack):
    server.console("llmtest group Loada Telda", inst=TEST)
    server.console("character level Loada 20", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    party_sent = False
    heard_party_event = None
    talent_chosen = False
    talent_spent_verified = False
    left_group = False
    errors = []

    deadline = time.time() + 30.0
    try:
        while time.time() < deadline and not (
            heard_party_event is not None and talent_spent_verified and left_group
        ):
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
                    ev_bot = msg["event"].get("bot", "")
                    ev = msg["event"].get("event", {})
                    if "error" in ev:
                        errors.append(f"{ev_bot}: {ev['error'].get('text')}")
                    if "chat" in ev and ev_bot == "Telda":
                        ch = ev["chat"]
                        if (
                            ch.get("channel") == "PARTY"
                            and ch.get("from") == "Loada"
                            and ch.get("text") == "ready to pull"
                        ):
                            heard_party_event = ch
                    continue

                snap = msg.get("snapshot")
                if not snap:
                    continue
                bot = snap.get("bot", "")
                seq = snap.get("seq", 0)
                options = snap.get("options", [])
                party_members = [
                    m.get("name", "")
                    for m in snap.get("ext", {}).get("party", {}).get("members", [])
                ]

                if bot == "Loada" and not party_sent and "Telda" in party_members:
                    cmd_id += 1
                    _send_cmd(
                        sock,
                        {
                            "bot": "Loada",
                            "command_id": cmd_id,
                            "say": {"channel": "PARTY", "to": "", "text": "ready to pull"},
                        },
                    )
                    party_sent = True

                if bot == "Loada" and not talent_chosen:
                    talent_idx = next(
                        (
                            i
                            for i, o in enumerate(options, 1)
                            if o.get("guid", "").startswith("talent:")
                        ),
                        None,
                    )
                    if talent_idx is not None:
                        cmd_id += 1
                        _send_cmd(
                            sock,
                            {
                                "bot": "Loada",
                                "command_id": cmd_id,
                                "choose": {"option": talent_idx, "seq": seq},
                            },
                        )
                        talent_chosen = True
                elif bot == "Loada" and talent_chosen and not talent_spent_verified:
                    has_talent_opt = any(
                        o.get("guid", "").startswith("talent:") for o in options
                    )
                    unspent = snap.get("base", {}).get("self", {}).get("unspent_talents", 0)
                    if not has_talent_opt and unspent == 0:
                        talent_spent_verified = True

                if (
                    bot == "Loada"
                    and heard_party_event is not None
                    and talent_spent_verified
                    and not left_group
                ):
                    if "Telda" not in party_members:
                        left_group = True
                    else:
                        leave_idx = next(
                            (
                                i
                                for i, o in enumerate(options, 1)
                                if o.get("kind") == "LEAVE_GROUP"
                            ),
                            None,
                        )
                        if leave_idx is not None:
                            cmd_id += 1
                            _send_cmd(
                                sock,
                                {
                                    "bot": "Loada",
                                    "command_id": cmd_id,
                                    "choose": {"option": leave_idx, "seq": seq},
                                },
                            )
    finally:
        sock.close()

    expect(not errors, f"unexpected module errors: {errors}")
    expect(
        heard_party_event is not None,
        "Telda never received Loada's PARTY chat event",
    )
    expect(
        not heard_party_event.get("at_me", False),
        f"bot-to-bot PARTY chat must have at_me=False, got {heard_party_event}",
    )
    expect(talent_chosen, "Loada never saw a Spend talent points option at level 20")
    expect(
        talent_spent_verified,
        "Loada's unspent talent points did not drop to 0 after choosing talent option",
    )
    return "bot-to-bot PARTY chat at_me=False; level-20 talent points spent cleanly"
