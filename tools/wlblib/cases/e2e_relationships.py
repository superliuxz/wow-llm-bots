"""Per-player relationships file (F5), through the real harness.

Loadd stands in for the user (`llmtest testhuman Loadd 1`) in a party with Loada. A mock LLM
answers Loadd's party line with `remember: Loadd tanked RFC well` (and a note naming nobody).

  1. Loada's relationships.md gets a section for Loadd with that note, and no section for
     anyone else.
  2. Loadd's next party line wakes Loada again: the prompt shows `People you know:` with the
     note about Loadd.
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
from wlblib.common import OUT, ROOT, TEST
from wlblib.testing import expect

NAME = "relationships"
DESCRIPTION = "Real harness: a remember: note naming a party member lands in relationships.md and the prompt"
TEST_CONFIG = "party"
BOT = "Loada"
ONLINE = 4

KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)
NOTE = "Loadd tanked RFC well"


class _Brain:
    def __init__(self):
        self.lock = threading.Lock()
        self.prompts = []  # (woken line, full prompt text) for Loada
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
                text = "\n".join(m.get("content", "") for m in body.get("messages", []))
                keep = KEEP_RE.search(text)
                woken = next((ln for ln in text.splitlines() if ln.startswith("Woken: ")), "")
                is_loada = "You are Loada." in text
                if is_loada:
                    with parent.lock:
                        parent.prompts.append((woken, text))
                lines = [f"do: {keep.group(1) if keep else 1}", "expect: none",
                         "watch: none", "look: 300"]
                if is_loada and "party from Loadd: first" in woken:
                    lines.append(f"remember: {NOTE}")
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


def run(stack):
    build.common("default", tests=False)
    server.console("llmtest group Loada Loadd", wait_s=0.5, inst=TEST)
    server.console("llmtest testhuman Loadd 1", wait_s=0.5, inst=TEST)

    with tempfile.TemporaryDirectory(prefix="wlb-e2e-rel-") as tmp:
        bots_dir = pathlib.Path(tmp) / "bots"
        rel_path = bots_dir / "Loada" / "relationships.md"
        brain = _Brain()
        brain.start()
        harness = subprocess.Popen(
            [str(OUT / "common" / "harness"), "run", f"--socket={stack.socket}",
             f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={bots_dir}",
             "--host=127.0.0.1", f"--port={brain.port}", "--slots=4", "--temperature=0"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        rel_text = ""
        second = None
        try:
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                with brain.lock:
                    if brain.prompts:
                        break
                time.sleep(0.2)
            time.sleep(1.0)
            server.console("llmtest chat Loadd party first pull is the scorpids",
                           wait_s=0.5, inst=TEST)
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline and not rel_path.exists():
                time.sleep(0.2)
            rel_text = rel_path.read_text() if rel_path.exists() else ""
            server.console("llmtest chat Loadd party second pull now",
                           wait_s=0.5, inst=TEST)
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline and second is None:
                with brain.lock:
                    second = next((p for w, p in brain.prompts
                                   if "party from Loadd: second" in w), None)
                time.sleep(0.2)
        finally:
            harness.terminate()
            try:
                harness.wait(timeout=5)
            except subprocess.TimeoutExpired:
                harness.kill()
            brain.stop()
            server.console("llmtest testhuman Loadd 0", wait_s=0.2, inst=TEST)
            server.console("llmtest ungroup Loadd", wait_s=0.2, inst=TEST)

    expect(rel_text, "relationships.md was never written after the remember: note")
    expect("Loadd" in rel_text and NOTE in rel_text,
           f"relationships.md lacks Loadd's note:\n{rel_text}")
    expect(not any(name in rel_text for name in ("Loadb", "Loadc")),
           f"relationships.md names players the note didn't mention:\n{rel_text}")
    expect(second is not None, "Loadd's second party line never woke Loada")
    expect("People you know:" in second and NOTE in second,
           "the next prompt didn't show `People you know:` with the note about Loadd")
    return "note about Loadd recorded in relationships.md and shown again while Loadd is present"
