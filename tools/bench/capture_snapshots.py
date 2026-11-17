"""Saves live snapshots from mod-llm-brain's socket, for offline brain benchmarks.

Connects like the harness would, and for `seconds` only receives: frames are kept as raw bytes
with their arrival time, so the receive loop never falls behind the module. When the time is up
they are written as JSON (proto field names, defaults left out), one file per frame:
  <out_dir>/<bot>/<seq>.json

Dev tool only. Needs the generated Python code (tools/wlb check snapshots once):
  out/tools-venv/bin/python -I tools/bench/capture_snapshots.py <out_dir> [seconds=15]
"""

import json
import pathlib
import socket
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SOCK = str(ROOT / "out" / "server" / "run" / "brain.sock")

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
sys.path.insert(0, str(ROOT / "out" / "proto"))
from frame_reader import FrameReader  # noqa: E402
from google.protobuf import json_format  # noqa: E402
from proto import snapshot_pb2  # noqa: E402


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
    saved = bots = 0
    seen = set()
    for payload in frames:
        message = snapshot_pb2.ModuleMessage()
        message.ParseFromString(payload)
        if not message.HasField("snapshot"):
            continue
        snap = message.snapshot
        path = out_dir / snap.bot / f"{snap.seq}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(json_format.MessageToDict(
            snap, preserving_proto_field_name=True)))
        saved += 1
        seen.add(snap.bot)
    print(f"saved {saved} snapshots from {len(seen)} bots in {out_dir}")


if __name__ == "__main__":
    main()
