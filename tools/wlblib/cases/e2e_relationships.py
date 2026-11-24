"""Per-player relationships file (F5), through the real harness.

Loadd stands in for the user (`llmtest testhuman Loadd 1`) in a party with Loada. A mock LLM
keeps Loada idle (`Keep doing what I'm doing`, `--only-bots=Loada`) and emits two `remember:` notes:

  1. On Loada's initial wake: a note naming nobody (`killed two boars near Razor Hill`), which
     lands in `memory.md` and does NOT create `relationships.md`.
  2. On Loadd's first party line: `remember: Loadd tanked RFC well`, which writes a single
     `## Loadd` section in `relationships.md` with that note only (no other player sections,
     and not the playerless boar note).
  3. On Loadd's second party line: Loada's next prompt renders `## Loadd` and the note inside
     the `People you know:` section itself (distinct from `What you remember from earlier:`),
     without leaking the playerless boar note.
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
NOBODY_NOTE = "killed two boars near Razor Hill"
NOTE = "Loadd tanked RFC well"


class _Brain:
    def __init__(self):
        self.lock = threading.Lock()
        self.prompts = []  # (woken line, full prompt text) for Loada
        self._sent_nobody = False
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
                is_loada = f"You are {BOT}." in text
                emit_nobody = False
                if is_loada:
                    with parent.lock:
                        parent.prompts.append((woken, text))
                        if not parent._sent_nobody and "party from Loadd:" not in woken:
                            parent._sent_nobody = True
                            emit_nobody = True
                lines = [f"do: {keep.group(1) if keep else 1}", "expect: none",
                         "watch: none", "look: 300"]
                if is_loada and "party from Loadd: first" in woken:
                    lines.append(f"remember: {NOTE}")
                elif emit_nobody:
                    lines.append(f"remember: {NOBODY_NOTE}")
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
        self._started = False

    def start(self):
        self._thread.start()
        self._started = True

    def stop(self):
        try:
            if self._started:
                self._httpd.shutdown()
                self._thread.join(timeout=5)
        finally:
            self._httpd.server_close()


def _extract_people_section(prompt_text):
    marker = "\n\nPeople you know:\n"
    idx = prompt_text.find(marker)
    if idx == -1:
        return None
    after = prompt_text[idx + len(marker):]
    end = len(after)
    for stop in ("\n\nYou remember (not on screen now):", "\n\nYour screen now:\n"):
        pos = after.find(stop)
        if pos != -1 and pos < end:
            end = pos
    return after[:end]


def run(stack):
    build.common("default", tests=False)

    with tempfile.TemporaryDirectory(prefix="wlb-e2e-rel-") as tmp:
        bots_dir = pathlib.Path(tmp) / "bots"
        mem_path = bots_dir / BOT / "memory.md"
        rel_path = bots_dir / BOT / "relationships.md"
        grouped = False
        testhuman = False
        brain = None
        harness = None
        harness_out = ""
        exited_early = None
        initial_woke = False
        got_nobody = False
        mem_text = ""
        rel_before_first = False
        rel_text = ""
        second = None
        try:
            server.console("llmtest group Loada Loadd", wait_s=0.5, inst=TEST)
            grouped = True
            server.console("llmtest testhuman Loadd 1", wait_s=0.5, inst=TEST)
            testhuman = True
            brain = _Brain()
            brain.start()
            harness = subprocess.Popen(
                [str(OUT / "common" / "harness"), "run", f"--socket={stack.socket}",
                 f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={bots_dir}",
                 f"--only-bots={BOT}", "--host=127.0.0.1", f"--port={brain.port}",
                 "--slots=4", "--temperature=0"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                if harness.poll() is not None:
                    exited_early = harness.returncode
                    break
                with brain.lock:
                    initial_woke = bool(brain.prompts)
                if initial_woke and mem_path.exists():
                    mem_text = mem_path.read_text()
                    if NOBODY_NOTE in mem_text:
                        got_nobody = True
                        break
                time.sleep(0.2)

            if exited_early is None and got_nobody:
                time.sleep(0.5)
                rel_before_first = rel_path.exists()
                server.console("llmtest chat Loadd party first pull is the scorpids",
                               wait_s=0.5, inst=TEST)
                deadline = time.monotonic() + 15.0
                while time.monotonic() < deadline and not rel_path.exists():
                    if harness.poll() is not None:
                        exited_early = harness.returncode
                        break
                    time.sleep(0.2)
                rel_text = rel_path.read_text() if rel_path.exists() else ""

            if exited_early is None and rel_text:
                server.console("llmtest chat Loadd party second pull now",
                               wait_s=0.5, inst=TEST)
                deadline = time.monotonic() + 15.0
                while time.monotonic() < deadline and second is None:
                    if harness.poll() is not None:
                        exited_early = harness.returncode
                        break
                    with brain.lock:
                        second = next((p for w, p in brain.prompts
                                       if "party from Loadd: second" in w), None)
                    time.sleep(0.2)
        finally:
            try:
                if harness is not None:
                    harness.terminate()
                    try:
                        harness_out, _ = harness.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        harness.kill()
                        harness_out, _ = harness.communicate()
            finally:
                try:
                    if brain is not None:
                        brain.stop()
                finally:
                    try:
                        if testhuman:
                            server.console("llmtest testhuman Loadd 0", wait_s=0.2, inst=TEST)
                    finally:
                        if grouped:
                            server.console("llmtest ungroup Loadd", wait_s=0.2, inst=TEST)

    expect(exited_early is None,
           f"harness exited early (rc={exited_early}); harness log tail:\n{harness_out[-1500:]}")
    expect(initial_woke,
           f"Loada never received an initial prompt within 30 s; log tail:\n{harness_out[-1500:]}")
    expect(NOBODY_NOTE in mem_text,
           f"memory.md did not record the playerless note:\n{mem_text}")
    expect(not rel_before_first,
           "a remember: note naming nobody should not create relationships.md")
    expect(rel_text,
           f"relationships.md was never written after Loadd's note; log tail:\n{harness_out[-1500:]}")
    headings = [ln for ln in rel_text.splitlines() if ln.startswith("## ")]
    expect(headings == ["## Loadd"],
           f"expected only ['## Loadd'] section in relationships.md, got {headings}:\n{rel_text}")
    expect(NOTE in rel_text, f"relationships.md lacks Loadd's note:\n{rel_text}")
    expect(NOBODY_NOTE not in rel_text,
           f"relationships.md should not include the playerless note:\n{rel_text}")
    expect(not any(name in rel_text for name in ("Loadb", "Loadc")),
           f"relationships.md names players the note didn't mention:\n{rel_text}")
    expect(second is not None,
           f"Loadd's second party line never woke Loada; log tail:\n{harness_out[-1500:]}")
    rel_section = _extract_people_section(second)
    expect(rel_section is not None,
           "the next prompt didn't render a `People you know:` section")
    expect("## Loadd" in rel_section and NOTE in rel_section,
           f"`People you know:` section lacks `## Loadd` or Loadd's note:\n{rel_section}")
    expect(NOBODY_NOTE not in rel_section,
           f"`People you know:` section leaked the playerless note:\n{rel_section}")
    return ("playerless note skipped relationships.md; Loadd note recorded under ## Loadd "
            "and rendered under People you know:")
