"""Open-world death -> Release spirit -> ghost corpse run -> respawn alive (F4).

  1. Kill Loada in Valley of Trials (`llmtest kill Loada`).
  2. Snapshot shows `DEAD_BODY` and offers `Release spirit`; choosing it
     succeeds and moves Loada to `GHOST` at the graveyard.
  3. Playerbots' dead strategy walks the ghost back to her corpse (no teleport
     cheat) and revives her: snapshot returns to `ALIVE` with health > 0.
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

NAME = "death"
DESCRIPTION = "Open-world death, Release spirit, ghost corpse run, and respawn alive"
TEST_CONFIG = "choose"  # Fresh level-1 Troll Priest in Valley of Trials.
BOT = "Loada"


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def run(stack):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    release_cmd_id = None
    phase = "wait_alive"  # wait_alive -> dead -> ghost -> alive
    saw_dead_body = False
    saw_ghost = False
    revived_alive = False
    ghost_at = None
    revived_at = None
    errors = []

    deadline = time.time() + 90.0
    try:
        while time.time() < deadline and phase != "alive":
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
                    err = ev.get("error")
                    if err and err.get("command_id") == release_cmd_id:
                        errors.append(err.get("text"))
                    continue
                snap = msg.get("snapshot")
                if not snap or snap.get("bot") != BOT:
                    continue
                seq = snap.get("seq", 0)
                options = snap.get("options", [])
                self_info = snap.get("base", {}).get("self", {})
                state = self_info.get("state", "")
                health = self_info.get("health", 0)

                if phase == "wait_alive" and state == "ALIVE":
                    server.console(f"llmtest kill {BOT}", inst=TEST)
                    phase = "dead"
                elif phase == "dead" and state == "DEAD_BODY":
                    saw_dead_body = True
                    rel_idx = next(
                        (i for i, o in enumerate(options, 1)
                         if o.get("guid") == "rez:release"),
                        None,
                    )
                    if rel_idx is not None and release_cmd_id is None:
                        cmd_id += 1
                        release_cmd_id = cmd_id
                        _send_cmd(sock, {
                            "bot": BOT,
                            "command_id": cmd_id,
                            "choose": {"option": rel_idx, "seq": seq},
                        })
                elif phase == "dead" and release_cmd_id is not None and state == "GHOST":
                    saw_ghost = True
                    ghost_at = time.time()
                    phase = "ghost"
                elif phase == "ghost" and state == "ALIVE" and health > 0:
                    revived_alive = True
                    revived_at = time.time()
                    phase = "alive"
    finally:
        sock.close()

    expect(saw_dead_body, "Loada never entered DEAD_BODY after llmtest kill")
    expect(release_cmd_id is not None, "dead Loada was never offered `Release spirit`")
    expect(not errors, f"Release spirit returned error: {errors}")
    expect(saw_ghost, "Loada never transitioned to GHOST after Release spirit")
    expect(revived_alive, "ghost Loada did not corpse-run and respawn ALIVE within timeout")
    return f"DEAD_BODY -> Release spirit -> GHOST -> ALIVE in {revived_at - ghost_at:.1f} s"
