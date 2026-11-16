"""Saves live snapshots from mod-llm-brain's socket, for offline brain benchmarks.

Connects like the harness would, and writes every snapshot it receives for
`seconds` as JSON (proto field names, defaults left out), one file per frame:
  <out_dir>/<bot>/<seq>.json

Dev tool only. Needs the generated Python code (tools/validate-snapshots.sh once):
  out/tools-venv/bin/python -I tools/bench/capture_snapshots.py <out_dir> [seconds=15]
"""

import json
import pathlib
import socket
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SOCK = str(ROOT / "out" / "server" / "run" / "brain.sock")
HEADER = struct.Struct(">I")

sys.path.insert(0, str(ROOT / "out" / "proto"))
from google.protobuf import json_format  # noqa: E402
from proto import snapshot_pb2  # noqa: E402


def main():
    out_dir = pathlib.Path(sys.argv[1])
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.2)
    buf = b""
    saved = 0
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            print("server closed the connection")
            break
        buf += chunk
        while len(buf) >= HEADER.size:
            (size,) = HEADER.unpack_from(buf)
            if len(buf) < HEADER.size + size:
                break
            message = snapshot_pb2.ModuleMessage()
            message.ParseFromString(buf[HEADER.size:HEADER.size + size])
            buf = buf[HEADER.size + size:]
            if not message.HasField("snapshot"):
                continue
            snap = message.snapshot
            path = out_dir / snap.bot / f"{snap.seq}.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(json_format.MessageToDict(
                snap, preserving_proto_field_name=True)))
            saved += 1
    s.close()
    bots = len({p.parent.name for p in out_dir.glob("*/*.json")})
    print(f"saved {saved} snapshots from {bots} bots in {out_dir}")


if __name__ == "__main__":
    main()
