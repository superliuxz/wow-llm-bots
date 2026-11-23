"""One brain bot invites another; Playerbots must NOT auto-accept before the brain can.

Verifies High 2 (Review ede3f6c):
  1. Loada whispers Telda and chooses `Invite Telda`.
  2. Telda receives the pending group invite and holds it for >= 2 s (Playerbots'
     `default` strategy must NOT auto-accept it).
  3. Telda's snapshot offers `Accept group invite` and `Decline group invite`.
  4. When the harness chooses `Accept group invite` for Telda, it succeeds with
     no "no pending group invite" error and both bots are in the same party.
"""

import socket
import sys
import time

from wlblib import server
from wlblib.common import ROOT, TEST, running_pid
from wlblib.testing import expect

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
import wire_py  # noqa: E402
from frame_reader import FrameReader  # noqa: E402

NAME = "invite"
DESCRIPTION = "Loada invites Telda; Telda holds the invite until the brain accepts"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def run(stack):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    whisper_sent = False
    invite_sent_at = None
    saw_pending_invite_at = None
    accept_sent = False
    grouped = False
    errors = []
    latest_snaps = {}

    deadline = time.time() + 30.0
    try:
        while time.time() < deadline and not grouped:
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
                    ev = msg["event"].get("event", {})
                    if "error" in ev:
                        errors.append(f"{msg['event'].get('bot')}: {ev['error'].get('text')}")
                    continue
                snap = msg.get("snapshot")
                if not snap:
                    continue
                bot = snap.get("bot", "")
                seq = snap.get("seq", 0)
                latest_snaps[bot] = snap
                options = snap.get("options", [])
                party_members = [
                    m.get("name", "")
                    for m in snap.get("ext", {}).get("party", {}).get("members", [])
                ]

                if bot == "Telda" and not whisper_sent and "Loada" in latest_snaps:
                    cmd_id += 1
                    _send_cmd(sock, {
                        "bot": "Telda",
                        "command_id": cmd_id,
                        "say": {"channel": "WHISPER", "to": "Loada", "text": "group up?"},
                    })
                    whisper_sent = True
                    continue

                if bot == "Loada" and whisper_sent and invite_sent_at is None:
                    inv_idx = next(
                        (i for i, o in enumerate(options, 1)
                         if o.get("kind") == "INVITE" and o.get("name") == "Telda"),
                        None,
                    )
                    if inv_idx is not None:
                        cmd_id += 1
                        _send_cmd(sock, {
                            "bot": "Loada",
                            "command_id": cmd_id,
                            "choose": {"option": inv_idx, "seq": seq},
                        })
                        invite_sent_at = time.time()
                    continue

                if bot == "Telda" and invite_sent_at is not None and not accept_sent:
                    # Must NOT already be in a party before the brain accepts!
                    expect(
                        "Loada" not in party_members,
                        f"Playerbots auto-accepted Loada's invite before the brain could! party={party_members}",
                    )
                    acc_idx = next(
                        (i for i, o in enumerate(options, 1) if o.get("kind") == "ACCEPT_INVITE"),
                        None,
                    )
                    dec_idx = next(
                        (i for i, o in enumerate(options, 1) if o.get("kind") == "DECLINE_INVITE"),
                        None,
                    )
                    if acc_idx is not None and dec_idx is not None:
                        if saw_pending_invite_at is None:
                            saw_pending_invite_at = time.time()
                        # Hold for >= 2.0 s to prove Playerbots does not steal the invite.
                        if time.time() - saw_pending_invite_at >= 2.0:
                            cmd_id += 1
                            _send_cmd(sock, {
                                "bot": "Telda",
                                "command_id": cmd_id,
                                "choose": {"option": acc_idx, "seq": seq},
                            })
                            accept_sent = True
                    continue

                if bot == "Telda" and accept_sent and "Loada" in party_members:
                    grouped = True
                    break
    finally:
        sock.close()

    expect(invite_sent_at is not None, "Loada never saw/sent Invite Telda option")
    expect(saw_pending_invite_at is not None, "Telda never saw pending Accept/Decline group invite options")
    expect(accept_sent, "Test never reached brain-driven Accept group invite")
    expect(not errors, f"Invite/Accept produced errors: {errors}")
    expect(grouped, "Telda did not join Loada's party after brain accepted invite")

    # Verify bot logout destroys the Engine + BlockAutoInviteListener cleanly without
    # crashing worldserver (regression test for static-listener double-free on logout).
    log_path = TEST.logs / "worldserver.out"
    log_before = log_path.read_text(errors="replace") if log_path.exists() else ""
    logins_before = log_before.count("Bot Telda logged in")
    server.console("llmtest logout Telda", inst=TEST)
    # Wait for logout confirmation and BotManager to log Telda back in.
    relogin_deadline = time.time() + 25.0
    saw_logout = False
    saw_relogin = False
    while time.time() < relogin_deadline:
        expect(running_pid("worldserver", TEST) is not None,
               "worldserver crashed after logging out Telda!")
        text = log_path.read_text(errors="replace") if log_path.exists() else ""
        if "llmtest: logged out Telda" in text:
            saw_logout = True
        if saw_logout and text.count("Bot Telda logged in") > logins_before:
            saw_relogin = True
            break
        time.sleep(0.5)
    expect(saw_logout, "llmtest logout Telda did not execute")
    expect(saw_relogin, "Telda did not log back in after logout")
    expect(running_pid("worldserver", TEST) is not None,
           "worldserver crashed after Telda logged back in!")
    return "Telda held invite >= 2.0 s, joined Loada's party, and survived logout/relogin without crash"
