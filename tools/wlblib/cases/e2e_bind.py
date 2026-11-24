"""Bind hearthstone option at an innkeeper (F4, docs/specs/options.md).

  1. Record Loada's starting spot, then teleport her to Innkeeper Grosk in
     Razor Hill (Durotar, map 1).
  2. Verify `Bind hearthstone at Innkeeper Grosk (Razor Hill)` is offered.
  3. Choose it: verify `"bound hearthstone at Innkeeper Grosk"` info event,
     no error, and the bind option disappears once bound here.
  4. Open the bags panel and verify `bags.hearthstone.bind == "Razor Hill"`.
  5. Close bags and teleport Loada back to her starting spot so sibling cases
     sharing the `talk` stack are unaffected.
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

NAME = "bind"
DESCRIPTION = "Bind hearthstone option at an innkeeper binds home via the server bind spell"
TEST_CONFIG = "talk"  # Fresh level-1 Troll Priest (starts bound in Valley of Trials).
BOT = "Loada"
# Innkeeper Grosk (entry 6928) in Razor Hill, Durotar (map 1).
RAZOR_HILL_INN = (1, 340.36, -4686.29, 16.54)


def _send_cmd(sock, cmd):
    payload = wire_py.encode_command(cmd)
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def run(stack):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    sock.connect(str(TEST.run / "brain.sock"))
    reader = FrameReader()

    cmd_id = 0
    origin = None
    teleported = False
    bind_cmd_id = None
    bind_option_text = None
    bind_info = None
    bind_gone = False
    bags_opened = False
    hearth_bind = None
    errors = []

    deadline = time.time() + 45.0
    try:
        while time.time() < deadline and not (bind_info and bind_gone and hearth_bind == "Razor Hill"):
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
                    if err and err.get("command_id") == bind_cmd_id:
                        errors.append(err.get("text"))
                    info = ev.get("info")
                    if info and "bound hearthstone at Innkeeper Grosk" in info.get("text", ""):
                        bind_info = info["text"]
                    continue
                snap = msg.get("snapshot")
                if not snap or snap.get("bot") != BOT:
                    continue
                if not teleported:
                    where = snap.get("base", {}).get("where", {})
                    pos = where.get("position") or {}
                    if pos:
                        origin = (pos.get("x", 0.0), pos.get("y", 0.0), pos.get("z", 0.0),
                                  where.get("facing", 0.0))
                    m, x, y, z = RAZOR_HILL_INN
                    server.console(f"llmtest tele {BOT} {m} {x} {y} {z} 0", inst=TEST)
                    teleported = True
                    continue
                seq = snap.get("seq", 0)
                options = snap.get("options", [])
                bind_idx = next(
                    (i for i, o in enumerate(options, 1)
                     if o.get("guid", "").startswith("npc_bind:")),
                    None,
                )
                if bind_cmd_id is None and bind_idx is not None:
                    bind_option_text = options[bind_idx - 1].get("text", "")
                    cmd_id += 1
                    bind_cmd_id = cmd_id
                    _send_cmd(sock, {
                        "bot": BOT,
                        "command_id": cmd_id,
                        "choose": {"option": bind_idx, "seq": seq},
                    })
                elif bind_cmd_id is not None:
                    if bind_idx is None:
                        bind_gone = True
                    if not bags_opened and bind_info:
                        cmd_id += 1
                        _send_cmd(sock, {
                            "bot": BOT,
                            "command_id": cmd_id,
                            "open": {"panel": "BAGS"},
                        })
                        bags_opened = True
                    bags = snap.get("ext", {}).get("bags") or {}
                    hs = bags.get("hearthstone") or {}
                    if hs.get("bind"):
                        hearth_bind = hs["bind"]
    finally:
        if bags_opened:
            try:
                cmd_id += 1
                _send_cmd(sock, {
                    "bot": BOT,
                    "command_id": cmd_id,
                    "close": {"panel": "BAGS"},
                })
            except OSError:
                pass
        sock.close()
        if origin is not None:
            ox, oy, oz, oo = origin
            server.console(f"llmtest tele {BOT} 1 {ox:.2f} {oy:.2f} {oz:.2f} {oo:.2f}",
                           wait_s=0.5, inst=TEST)

    expect(bind_cmd_id is not None, "Loada was never offered `Bind hearthstone` at Innkeeper Grosk")
    expect("Innkeeper Grosk" in (bind_option_text or "") and "Razor Hill" in (bind_option_text or ""),
           f"unexpected bind option text: {bind_option_text!r}")
    expect(not errors, f"Bind hearthstone returned error: {errors}")
    expect(bind_info is not None, "no 'bound hearthstone at Innkeeper Grosk' info event")
    expect(bind_gone, "Bind hearthstone option stayed after binding at Razor Hill")
    expect(hearth_bind == "Razor Hill",
           f"expected hearthstone bind 'Razor Hill' in bags panel, got {hearth_bind!r}")
    return f"offered '{bind_option_text}'; {bind_info}; bags hearthstone bind={hearth_bind}"
