"""Live verification of playability fixes: party chat, incremental talents, dead release.

Verifies on the isolated `playability` test stack:
  1. High 4: When Loada and Telda are grouped and Loada sends a PARTY chat line,
     Telda hears it with `at_me=False` (bot-to-bot party chat must not trigger
     wake/reply loops).
  2. High 1 + R8614690-H4: When Loada levels to 15 (6 points) and then levels
     1 point at a time (16, then 17), `Spend talent points in <Tree>` spends all
     free points each time and clears `self.indicators.talent_points_unspent`.
  3. High 3 + Medium 1 + R8614690-H2: When Loada is killed (`llmtest kill Loada`),
     Loada's snapshot enters `DEAD_BODY`, offers `Release spirit`, choosing it
     succeeds without `"you died"` error, and Loada transitions to `GHOST`.
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
DESCRIPTION = "Party chat non-loop, incremental 1-pt talent spending, and dead Release spirit"
TEST_CONFIG = "playability"
BOT = "Loada"
ONLINE = 2


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def run(stack):
    server.console("llmtest group Loada Telda", inst=TEST)
    server.console("character level Loada 15", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    party_sent = False
    heard_party_event = None
    # Talent stages: 0 = spend L15 (6 pts), 1 = spend L16 (1 pt), 2 = spend L17 (1 pt), 3 = done.
    talent_stage = 0
    talent_cmd_in_flight = False
    kill_issued = False
    release_cmd_sent = False
    ghost_verified = False
    leave_sent = False
    left_group = False
    errors = []

    deadline = time.time() + 45.0
    try:
        while time.time() < deadline and not (
            heard_party_event is not None
            and talent_stage == 3
            and ghost_verified
            and left_group
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
                if bot != "Loada":
                    continue
                seq = snap.get("seq", 0)
                options = snap.get("options", [])
                self_info = snap.get("base", {}).get("self", {})
                indicators = self_info.get("indicators", {})
                party_members = [
                    m.get("name", "")
                    for m in snap.get("ext", {}).get("party", {}).get("members", [])
                ]

                if not party_sent and "Telda" in party_members:
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

                # Incremental talent spending across L15 -> L16 (+1 pt) -> L17 (+1 pt).
                if talent_stage < 3:
                    talent_idx = next(
                        (
                            i
                            for i, o in enumerate(options, 1)
                            if o.get("guid") == "talent:0"
                        ),
                        None,
                    )
                    unspent_flag = bool(indicators.get("talent_points_unspent", False))
                    if not talent_cmd_in_flight:
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
                            talent_cmd_in_flight = True
                    elif talent_idx is None and not unspent_flag:
                        talent_cmd_in_flight = False
                        talent_stage += 1
                        if talent_stage == 1:
                            server.console("character level Loada 16", inst=TEST)
                        elif talent_stage == 2:
                            server.console("character level Loada 17", inst=TEST)

                # Once talents + party chat are verified, move both into Ragefire
                # Chasm with Telda knowing Resurrection (2006) so BrainPolicyListener
                # holds auto-release, kill Loada, and verify Release spirit -> GHOST.
                if (
                    talent_stage == 3
                    and heard_party_event is not None
                    and not ghost_verified
                ):
                    state = self_info.get("state", "")
                    if not kill_issued:
                        server.console("llmtest learn Telda 2006", inst=TEST)
                        server.console("llmtest tele Loada 389 2.0 -8.0 -15.0 0", inst=TEST)
                        server.console("llmtest tele Telda 389 2.0 -8.0 -15.0 0", inst=TEST)
                        time.sleep(1.5)
                        server.console("llmtest kill Loada", inst=TEST)
                        kill_issued = True
                    elif state == "DEAD_BODY" and not release_cmd_sent:
                        rel_idx = next(
                            (
                                i
                                for i, o in enumerate(options, 1)
                                if o.get("guid") == "rez:release"
                            ),
                            None,
                        )
                        if rel_idx is not None:
                            cmd_id += 1
                            _send_cmd(
                                sock,
                                {
                                    "bot": "Loada",
                                    "command_id": cmd_id,
                                    "choose": {"option": rel_idx, "seq": seq},
                                },
                            )
                            release_cmd_sent = True
                    elif release_cmd_sent and state == "GHOST":
                        ghost_verified = True

                if ghost_verified and not left_group:
                    if "Telda" not in party_members:
                        left_group = True
                    elif not leave_sent:
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
                            leave_sent = True
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
    expect(
        talent_stage == 3,
        f"incremental talent spending stalled at stage {talent_stage} (expected 3)",
    )
    expect(
        release_cmd_sent and ghost_verified,
        f"dead Release spirit failed (release_cmd_sent={release_cmd_sent}, ghost_verified={ghost_verified})",
    )
    return "PARTY at_me=False; L15->L16->L17 incremental talents spent; DEAD_BODY->Release spirit->GHOST ok"
