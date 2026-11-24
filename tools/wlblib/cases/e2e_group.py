"""Grouping with another player: quest sharing, roles, wait/follow (RH-H3, M1, M2).

  1. Level Loada and Telda to 20, group them (Loada leads), give Loada a quest.
  2. Telda's party panel shows Loada's role (spec-derived, not only in LFG).
  3. Loada chooses `Share quest ...`; Telda gets a QUEST_SHARE prompt event and
     an `Accept shared quest ...` option; choosing it takes the quest, so
     Loada's share option goes away.
  4. Telda chooses `Wait here`; then `Follow the party leader again` is offered.
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

NAME = "group"
DESCRIPTION = "Quest sharing, spec roles and wait/follow in a hand-formed party"
TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
BOT = "Loada"
ONLINE = 2
# The Coastal Menace: sharable, any race, level 15+, and not auto-accept (a
# shared auto-accept quest lands in the log at once, with nothing to answer).
QUEST_ID = 104


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
    server.console(f"llmtest questadd Loada {QUEST_ID}", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    sent = {}  # step -> command_id
    role_seen = None
    share_prompt = False
    share_gone = False
    loada_overlap = False
    telda_overlap = False
    wait_done = False
    follow_offered = False
    errors = []

    def choose(bot, idx, seq, step):
        nonlocal cmd_id
        cmd_id += 1
        sent[step] = cmd_id
        _send_cmd(sock, {"bot": bot, "command_id": cmd_id,
                         "choose": {"option": idx, "seq": seq}})

    deadline = time.time() + 45.0
    try:
        while time.time() < deadline and not (
            share_gone and follow_offered and loada_overlap and telda_overlap
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
                    bot = msg["event"].get("bot")
                    ev = msg["event"].get("event", {})
                    err = ev.get("error")
                    if err and err.get("command_id") in sent.values():
                        errors.append(f"{bot}: {err.get('text')}")
                    prompt = ev.get("prompt")
                    if (bot == "Telda" and prompt
                            and prompt.get("kind") in ("QUEST_SHARE", 9)):
                        share_prompt = True
                    continue
                snap = msg.get("snapshot")
                if not snap:
                    continue
                bot, seq = snap.get("bot", ""), snap.get("seq", 0)
                options = snap.get("options", [])
                members = snap.get("ext", {}).get("party", {}).get("members", [])
                tracker = snap.get("ext", {}).get("quest_tracker", [])
                if bot == "Telda":
                    for m in members:
                        if m.get("name") == "Loada":
                            role_seen = m.get("group", {}).get("role") or role_seen
                    for obj in tracker:
                        q = obj.get("quest") or {}
                        if q.get("quest_id") == QUEST_ID and "Loada" in (q.get("party_members") or []):
                            telda_overlap = True
                    if share_prompt and "accept" not in sent:
                        idx = _find(options, "Accept shared quest")
                        if idx:
                            choose("Telda", idx, seq, "accept")
                    elif "accept" in sent and "wait" not in sent:
                        idx = _find(options, "Wait here")
                        if idx:
                            choose("Telda", idx, seq, "wait")
                    elif "wait" in sent and not follow_offered:
                        wait_done = True
                        follow_offered = _find(
                            options, "Follow the party leader again") is not None
                if bot == "Loada":
                    for obj in tracker:
                        q = obj.get("quest") or {}
                        if q.get("quest_id") == QUEST_ID and "Telda" in (q.get("party_members") or []):
                            loada_overlap = True
                    idx = _find(options, "Share quest")
                    if "share" not in sent and idx and members:
                        choose("Loada", idx, seq, "share")
                    elif "accept" in sent and idx is None:
                        share_gone = True
    finally:
        sock.close()
        server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

    expect(role_seen not in (None, "", "ROLE_UNSPECIFIED", 0),
           f"Telda's party panel showed no role for Loada (got {role_seen!r})")
    expect("share" in sent, "Loada was never offered `Share quest` while grouped")
    expect(share_prompt, "Telda got no QUEST_SHARE prompt event after Loada shared")
    expect("accept" in sent, "Telda was never offered `Accept shared quest`")
    expect(share_gone, "Loada's share option stayed after Telda took the quest")
    expect(loada_overlap and telda_overlap,
           f"quest overlap missing after share (loada={loada_overlap}, telda={telda_overlap})")
    expect(wait_done and follow_offered,
           "`Wait here` did not turn into `Follow the party leader again`")
    expect(not errors, f"group choices produced errors: {errors}")
    return (f"Loada's role shown as {role_seen}; quest shared, accepted, and overlap shown; "
            "wait/follow toggled")
