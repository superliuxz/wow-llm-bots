"""Real harness reconnects cleanly after the module socket drops (F4).

Runs `out/common/harness run` against a mock LLM through a droppable Unix-socket proxy to
`worldserver`'s `brain.sock`:
  1. Harness connects, wakes Loada, and sends a valid `Choose` command.
  2. Proxy forcibly drops both the harness-side and module-side sockets, closes
     and unlinks its listening socket, and pauses for 1.5 s (strictly longer
     than `UnixSocketClient`'s 1 s fixed `retry_delay`, exercising both
     `ModuleConnection` fixed-interval reconnect retry + `BotLoop` disconnect
     reset and `BrainBridge` peer disconnect + accept).
  3. Proxy resumes accepting; harness reconnects, wakes Loada fresh on the new connection, and
     sends another valid `Choose` command with zero module errors.
"""

import http.server
import json
import pathlib
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time

from wlblib import build
from wlblib.common import OUT, ROOT
from wlblib.testing import expect

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
import wire_py  # noqa: E402
from frame_reader import FrameReader  # noqa: E402

NAME = "reconnect"
DESCRIPTION = "Real harness reconnects after socket drop and resumes sending valid choices"
TEST_CONFIG = "choose"
BOT = "Loada"

KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)


class _DroppableProxy:
    """Forwards framed bytes between `listen_path` and `upstream_path`, recording commands/errors
    and supporting on-demand connection drops."""

    def __init__(self, upstream_path, listen_path):
        self.upstream_path = pathlib.Path(upstream_path)
        self.listen_path = pathlib.Path(listen_path)
        self.lock = threading.Lock()
        self.generations = 0
        self.commands = []  # (generation, cmd)
        self.errors = []  # (generation, text)
        self._stop = threading.Event()
        self._drop = threading.Event()
        self._closed = threading.Event()
        self._allow_accept = threading.Event()
        self._allow_accept.set()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()
        deadline = time.monotonic() + 5.0
        while not self.listen_path.exists() and time.monotonic() < deadline:
            time.sleep(0.02)

    def drop_and_pause(self, pause_s=1.5):
        self._closed.clear()
        self._allow_accept.clear()
        self._drop.set()
        self._closed.wait(timeout=5.0)
        time.sleep(pause_s)
        self._allow_accept.set()

    def stop(self):
        self._stop.set()
        self._drop.set()
        self._allow_accept.set()
        self._thread.join(timeout=5)
        try:
            self.listen_path.unlink()
        except OSError:
            pass

    def _run(self):
        srv = None

        def open_listener():
            try:
                self.listen_path.unlink()
            except OSError:
                pass
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.bind(str(self.listen_path))
            s.listen(1)
            s.settimeout(0.1)
            return s

        def close_listener(s):
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass
            try:
                self.listen_path.unlink()
            except OSError:
                pass

        try:
            while not self._stop.is_set():
                if not self._allow_accept.is_set():
                    if srv is not None:
                        close_listener(srv)
                        srv = None
                    self._closed.set()
                    time.sleep(0.05)
                    continue
                if srv is None:
                    srv = open_listener()
                try:
                    cli, _ = srv.accept()
                except socket.timeout:
                    continue
                self._drop.clear()
                self._closed.clear()
                up = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    up.connect(str(self.upstream_path))
                except OSError:
                    cli.close()
                    continue
                with self.lock:
                    self.generations += 1
                    gen = self.generations
                self._pump(cli, up, gen)
                if not self._allow_accept.is_set():
                    if srv is not None:
                        close_listener(srv)
                        srv = None
                    self._closed.set()
        finally:
            close_listener(srv)

    def _pump(self, cli, up, gen):
        c2u = FrameReader()
        u2c = FrameReader()
        try:
            while not self._stop.is_set() and not self._drop.is_set():
                r, _, _ = select.select([cli, up], [], [], 0.1)
                if cli in r:
                    data = cli.recv(1 << 16)
                    if not data:
                        break
                    up.sendall(data)
                    for payload in c2u.feed(data):
                        cmd = wire_py.decode_command(payload)
                        if cmd:
                            with self.lock:
                                self.commands.append((gen, cmd))
                if up in r:
                    data = up.recv(1 << 16)
                    if not data:
                        break
                    cli.sendall(data)
                    for payload in u2c.feed(data):
                        msg = wire_py.decode_module_message(payload)
                        if msg and "event" in msg:
                            err = msg["event"].get("event", {}).get("error")
                            if err:
                                with self.lock:
                                    self.errors.append((gen, err.get("text", "")))
        except OSError:
            pass
        finally:
            for s in (cli, up):
                try:
                    s.close()
                except OSError:
                    pass


class _KeepMockBrain:
    """Always replies with `Keep doing what I'm doing` and counts wakes."""

    def __init__(self):
        self.lock = threading.Lock()
        self.wakes = 0
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
                text = "\n".join(m.get("content", "") for m in body.get("messages", []))
                keep = KEEP_RE.search(text)
                with parent.lock:
                    parent.wakes += 1
                reply = json.dumps({
                    "choices": [{"message": {"content": (
                        f"do: {keep.group(1) if keep else 1}\n"
                        "expect: none\nwatch: none\nlook: 300"
                    )}}],
                    "usage": {"prompt_tokens": 100, "completion_tokens": 15},
                }).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(reply)))
                self.end_headers()
                self.wfile.write(reply)

        self._httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._httpd.server_address[1]
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._httpd.shutdown()
        self._httpd.server_close()
        self._thread.join(timeout=5)


def run(stack):
    build.common("default", tests=False)
    with tempfile.TemporaryDirectory(prefix="wlb-e2e-reconnect-") as tmp:
        tmp_dir = pathlib.Path(tmp)
        proxy_sock = tmp_dir / "proxy.sock"
        proxy = _DroppableProxy(stack.socket, proxy_sock)
        brain = _KeepMockBrain()
        proxy.start()
        brain.start()
        harness = subprocess.Popen(
            [str(OUT / "common" / "harness"), "run", f"--socket={proxy_sock}",
             f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={tmp_dir / 'bots'}",
             "--host=127.0.0.1", f"--port={brain.port}", "--slots=2", "--temperature=0"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            # 1. Wait for generation 1 connect + wake + Choose command.
            deadline = time.monotonic() + 20.0
            while time.monotonic() < deadline:
                with proxy.lock:
                    gen1_choose = any(g == 1 and "choose" in c for g, c in proxy.commands)
                if gen1_choose:
                    break
                time.sleep(0.1)
            expect(gen1_choose, "harness never sent a Choose command on initial connection")
            with brain.lock:
                wakes_before = brain.wakes

            # 2. Drop both sides of the socket, unlink the listener, and hold it
            #    closed for 1.5 s (> UnixSocketClient's 1 s retry_delay).
            proxy.drop_and_pause(pause_s=1.5)

            # 3. Verify harness reconnects (generation >= 2), wakes again, and sends Choose.
            deadline = time.monotonic() + 25.0
            gen2_choose = False
            while time.monotonic() < deadline:
                expect(harness.poll() is None, "harness exited instead of reconnecting")
                with proxy.lock:
                    gen2_choose = any(g >= 2 and "choose" in c for g, c in proxy.commands)
                if gen2_choose:
                    break
                time.sleep(0.1)
            with brain.lock:
                wakes_after = brain.wakes
            with proxy.lock:
                gens = proxy.generations
                errors = list(proxy.errors)
        finally:
            harness.terminate()
            try:
                out, _ = harness.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                harness.kill()
                out, _ = harness.communicate(timeout=5)
            proxy.stop()
            brain.stop()

    expect(gens >= 2, f"expected >=2 connection generations, got {gens}")
    expect(gen2_choose, f"harness did not send Choose after reconnect (log tail: {out[-300:]!r})")
    expect(wakes_after > wakes_before,
           f"harness did not wake Loada after reconnect (wakes {wakes_before} -> {wakes_after})")
    expect("disconnected from mod-llm-brain" in out,
           f"harness did not log disconnect (log tail: {out[-300:]!r})")
    expect(out.count("connected to mod-llm-brain") >= 2,
           f"harness did not log second connect (log tail: {out[-300:]!r})")
    expect(not errors, f"module reported errors across reconnect: {errors}")
    return f"reconnected across drop ({gens} generations, wakes {wakes_before}->{wakes_after})"
