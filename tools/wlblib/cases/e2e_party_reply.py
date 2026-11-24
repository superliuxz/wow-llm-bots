"""One bot answers a human's party chat (RH-H4), through the real harness.

Runs `out/common/harness run` against a mock LLM that answers every party line it is woken by
(`party: <its name> here`), so the harness, not the model, decides who speaks. Loadd stands in for
the user (`llmtest testhuman Loadd 1`) in a party with Loada, Loadb and Loadc.

  1. Loadd types "anyone up for RFC?": all three bots wake, but only Loada (first by name) says
     anything in party.
  2. Loadd types "loadc can you heal?": only Loadc answers.
  3. Loadd types "lol": no bot is woken by it.
"""

import http.server
import json
import pathlib
import re
import subprocess
import tempfile
import threading
import time

from wlblib import build, server
from wlblib.cases.e2e_boss import _ProxyRecorder
from wlblib.common import OUT, ROOT, TEST
from wlblib.testing import expect

NAME = "party_reply"
DESCRIPTION = "Real harness: one bot answers a human's party line; named bot answers; 'lol' wakes none"
TEST_CONFIG = "party"
BOT = "Loada"
ONLINE = 4

BOTS = ("Loada", "Loadb", "Loadc")
KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)
NAME_RE = re.compile(r"^You are (\w+)\.", re.M)


class _PartyMockBrain:
    """Keeps doing what it does; answers any party line it was woken by."""

    def __init__(self):
        self.lock = threading.Lock()
        self.wakes = []  # (time, bot, woken line)
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
                text = "\n".join(m.get("content", "") for m in body.get("messages", []))
                keep = KEEP_RE.search(text)
                name = NAME_RE.search(text)
                woken = next((ln for ln in text.splitlines() if ln.startswith("Woken: ")), "")
                with parent.lock:
                    parent.wakes.append((time.monotonic(), name.group(1) if name else "?", woken))
                lines = [f"do: {keep.group(1) if keep else 1}", "expect: none",
                         "watch: none", "look: 300"]
                if "party from Loadd" in woken:
                    lines.append(f"party: {name.group(1) if name else 'bot'} here")
                reply = json.dumps({
                    "choices": [{"message": {"content": "\n".join(lines)}}],
                    "usage": {"prompt_tokens": 100, "completion_tokens": 20},
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


def _party_says(proxy, since):
    """bot -> number of party lines it sent after `since`."""
    out = {}
    for t, cmd in proxy.commands:
        say = cmd.get("say")
        if t >= since and say and say.get("channel") == "PARTY":
            out[cmd.get("bot")] = out.get(cmd.get("bot"), 0) + 1
    return out


def run(stack):
    build.common("default", tests=False)
    for bot in ("Loadb", "Loadc", "Loadd"):
        server.console(f"llmtest group Loada {bot}", wait_s=0.5, inst=TEST)
    server.console("llmtest testhuman Loadd 1", wait_s=0.5, inst=TEST)

    with tempfile.TemporaryDirectory(prefix="wlb-e2e-party-") as tmp:
        tmp_dir = pathlib.Path(tmp)
        proxy_sock = tmp_dir / "proxy.sock"
        proxy = _ProxyRecorder(stack.socket, proxy_sock)
        brain = _PartyMockBrain()
        proxy.start()
        brain.start()
        harness = subprocess.Popen(
            [str(OUT / "common" / "harness"), "run", f"--socket={proxy_sock}",
             f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={tmp_dir / 'bots'}",
             "--host=127.0.0.1", f"--port={brain.port}", "--slots=4", "--temperature=0"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            # Every bot has had its first look (so the harness knows all of them).
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                with brain.lock:
                    seen = {b for _, b, _ in brain.wakes}
                if set(BOTS) <= seen:
                    break
                time.sleep(0.2)
            expect(set(BOTS) <= seen, f"setup: not every bot woke once (saw {sorted(seen)})")
            time.sleep(1.0)

            def say_and_wait(text):
                since = time.monotonic()
                server.console(f"llmtest chat Loadd party {text}", wait_s=0.5, inst=TEST)
                time.sleep(6.0)
                with proxy.lock:
                    says = _party_says(proxy, since)
                with brain.lock:
                    woken = sorted({b for t, b, w in brain.wakes
                                    if t >= since and f"party from Loadd: {text}" in w})
                return says, woken

            says1, woken1 = say_and_wait("anyone up for RFC?")
            says2, woken2 = say_and_wait("loadc can you heal?")
            says3, woken3 = say_and_wait("lol")
        finally:
            harness.terminate()
            try:
                harness.wait(timeout=5)
            except subprocess.TimeoutExpired:
                harness.kill()
            proxy.stop()
            brain.stop()
            server.console("llmtest testhuman Loadd 0", wait_s=0.2, inst=TEST)
            for bot in ("Loadb", "Loadc", "Loadd"):
                server.console(f"llmtest ungroup {bot}", wait_s=0.2, inst=TEST)

    expect(woken1 == list(BOTS), f"'anyone up for RFC?' woke {woken1}, expected all of {list(BOTS)}")
    expect(says1 == {"Loada": 1}, f"'anyone up for RFC?' got party replies {says1}, expected only Loada")
    expect(says2 == {"Loadc": 1}, f"'loadc can you heal?' got party replies {says2}, expected only Loadc")
    expect(not woken3 and not says3, f"'lol' woke {woken3} / got replies {says3}")
    return (f"unnamed line: {woken1} woke, only Loada answered; named: only Loadc answered; "
            "'lol' woke nobody")
