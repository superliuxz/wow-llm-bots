"""A Dungeon Finder proposal wakes the brain while it is open (RH-H1).

Uses AzerothCore's LFG testing mode (`Debug.LFG = 1` in the `party` test config), so a proposal
forms for a group of two instead of a full tank/healer/3-dps five.

  1. Level Loada and Loadb to 20 and group them (Loada leads).
  2. Loada queues (`Queue Random Dungeon (LFG: Healer)`); Loadb confirms a non-healer role.
  3. Loadb gets an LFG_PROPOSAL prompt event and an `Accept Dungeon Finder proposal` option;
     both accept without an error, and the group is put into the dungeon (both bots' snapshots
     say `in an instance`).
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

NAME = "lfg_proposal"
DESCRIPTION = "LFG proposal wakes the member's brain; both accept and enter the dungeon"
TEST_CONFIG = "party"  # config/tests/party.*.overrides: 4 level-1 priests, Debug.LFG on.
BOT = "Loada"
ONLINE = 4


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def _find(options, prefix, avoid=None):
    return next((i for i, o in enumerate(options, 1)
                 if o.get("text", "").startswith(prefix)
                 and not (avoid and avoid in o.get("text", ""))), None)


def run(stack):
    for bot in ("Loada", "Loadb"):
        server.console(f"character level {bot} 20", inst=TEST)
    server.console("llmtest group Loada Loadb", inst=TEST)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()
    cmd_id = 0
    sent = {}
    errors = []
    prompts = {}  # bot -> set of prompt kinds
    role_at = None
    proposal_at = None
    in_instance = set()

    def choose(bot, idx, seq, step):
        nonlocal cmd_id
        cmd_id += 1
        sent[step] = cmd_id
        _send_cmd(sock, {"bot": bot, "command_id": cmd_id,
                         "choose": {"option": idx, "seq": seq}})

    deadline = time.time() + 60.0
    try:
        while time.time() < deadline and len(in_instance) < 2:
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                continue
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
                    if ev.get("prompt"):
                        kind = str(ev["prompt"].get("kind"))
                        prompts.setdefault(bot, set()).add(kind)
                        if bot == "Loadb" and kind == "LFG_PROPOSAL" and proposal_at is None:
                            proposal_at = time.time()
                    continue
                snap = msg.get("snapshot")
                if not snap:
                    continue
                bot, seq = snap.get("bot", ""), snap.get("seq", 0)
                options = snap.get("options", [])
                if snap.get("base", {}).get("where", {}).get("in_instance"):
                    in_instance.add(bot)
                if bot == "Loada" and "queue" not in sent:
                    idx = _find(options, "Queue Random Dungeon (LFG: Healer)")
                    if idx:
                        choose("Loada", idx, seq, "queue")
                elif bot == "Loadb" and "queue" in sent and "role" not in sent:
                    idx = _find(options, "Confirm Dungeon Finder role", avoid="Healer")
                    if idx:
                        choose("Loadb", idx, seq, "role")
                        role_at = time.time()
                elif bot in ("Loada", "Loadb") and f"accept_{bot}" not in sent:
                    idx = _find(options, "Accept Dungeon Finder proposal")
                    if idx:
                        choose(bot, idx, seq, f"accept_{bot}")
    finally:
        sock.close()
        server.console("lfg clean", inst=TEST)
        server.console("llmtest ungroup Loadb", inst=TEST)

    expect("queue" in sent, "Loada was never offered `Queue Random Dungeon (LFG: Healer)`")
    expect("role" in sent, "Loadb was never offered a non-healer `Confirm Dungeon Finder role`")
    expect(proposal_at is not None,
           f"Loadb got no LFG_PROPOSAL prompt event (prompts: {prompts})")
    delay = proposal_at - role_at
    expect(delay <= 10.0, f"LFG_PROPOSAL took {delay:.1f} s after the role check (> 10 s)")
    expect("accept_Loadb" in sent and "accept_Loada" in sent,
           "both bots were not offered `Accept Dungeon Finder proposal`")
    expect(not errors, f"LFG choices produced errors: {errors}")
    expect(len(in_instance) == 2, f"not both bots entered the dungeon (in: {sorted(in_instance)})")
    return f"LFG_PROPOSAL woke Loadb {delay:.2f} s after the role check; both accepted and entered"
