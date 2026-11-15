"""Minimal stand-in for the harness, for testing mod-llm-brain's socket.

Connects to the brain socket, prints what the server sends for a few seconds,
optionally sends messages, then disconnects. Messages are length-prefixed frames
(4-byte big-endian length, then the payload), as in common/net/frame_codec.h.
Module messages (wlb.snapshot.ModuleMessage) are printed as JSON when the
generated Python code is available, otherwise as a byte count and hex preview.

Dev tool only (production is C++). For JSON, generate the code once with
tools/validate-snapshots.sh and run with the tools venv:
  out/tools-venv/bin/python -I tools/fake_harness.py [seconds=5] [message ...]
"""

import pathlib
import socket
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOCK = str(ROOT / "server" / "run" / "brain.sock")
HEADER = struct.Struct(">I")

try:
    sys.path.insert(0, str(ROOT / "out" / "proto"))
    from google.protobuf import json_format
    from proto import snapshot_pb2
except ImportError:
    snapshot_pb2 = None


def describe(payload):
    if snapshot_pb2 is not None:
        message = snapshot_pb2.ModuleMessage()
        try:
            message.ParseFromString(payload)
            return json_format.MessageToJson(message, indent=1, preserving_proto_field_name=True)
        except Exception:  # Not a ModuleMessage: fall through to hex.
            pass
    return f"[{len(payload)} bytes] {payload[:32].hex()}"


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    to_send = sys.argv[2:]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.2)
    for message in to_send:
        payload = message.encode()
        s.sendall(HEADER.pack(len(payload)) + payload)
        print(f">> {message}")
    buf = b""
    received = 0
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(65536)
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
            payload = buf[HEADER.size:HEADER.size + size]
            buf = buf[HEADER.size + size:]
            received += 1
            print(f"<< {describe(payload)}")
    s.close()
    print(f"received {received} messages in {seconds:.0f}s")


if __name__ == "__main__":
    main()
