"""Saves live snapshots from mod-llm-brain's socket, for offline brain benchmarks.

Connects like the harness would, and for `seconds` only receives: frames are kept as raw BEVE bytes
with their arrival time, so the receive loop never falls behind the module. When the time is up
they are written as JSON (defaults left out), one file per frame:
  <out_dir>/<bot>/<seq>.json

Dev tool only. Needs `tools/wlb build common --no-tests` once for wire_py:
  python3 -I tools/bench/capture_snapshots.py <out_dir> [seconds=15]
"""

import json
import pathlib
import socket
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SOCK = str(ROOT / "out" / "server" / "run" / "brain.sock")

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
from frame_reader import FrameReader  # noqa: E402
import wire_py  # noqa: E402


def main():
    out_dir = pathlib.Path(sys.argv[1])
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.2)
    reader = FrameReader()
    frames = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            print("server closed the connection")
            break
        frames += reader.feed(chunk)
    s.close()
    saved = 0
    seen = set()
    for payload in frames:
        message = wire_py.decode_module_message(payload)
        if not message or "snapshot" not in message:
            continue
        snap = message["snapshot"]
        bot = snap.get("bot", "unknown")
        seq = snap.get("seq", 0)
        path = out_dir / bot / f"{seq}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(snap))
        saved += 1
        seen.add(bot)
    print(f"saved {saved} snapshots from {len(seen)} bots in {out_dir}")


if __name__ == "__main__":
    main()
