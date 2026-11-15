"""Minimal stand-in for the harness, for testing mod-llm-brain's socket.

Connects to the brain socket, prints what the server sends for a few seconds,
optionally sends lines, then disconnects.

Usage: python3 -I tools/fake_harness.py [seconds=5] [line-to-send ...]
"""

import pathlib
import socket
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOCK = str(ROOT / "server" / "run" / "brain.sock")


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    to_send = sys.argv[2:]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.2)
    for line in to_send:
        s.sendall(line.encode() + b"\n")
        print(f">> {line}")
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
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            received += 1
            print(f"<< {line.decode()[:200]}")
    s.close()
    print(f"received {received} messages in {seconds:.0f}s")


if __name__ == "__main__":
    main()
