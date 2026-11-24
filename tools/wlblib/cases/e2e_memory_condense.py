"""Memory condensing (F5), end-to-end through the real worldserver and harness.

Loada starts with 30 `remember:` notes in its memory.md (over the 24-note threshold). Runs
`out/common/harness run --only-bots=Loada` against an in-process mock LLM so Loada always picks
`Keep doing what I'm doing` (leaving shared `whisper`-stack world state untouched and keeping
`wlb test e2e` CPU-only with no GPU/`LlamaServer` dependency). After the out-of-combat reply, the
harness issues a P4 memory-condense request containing the 20 oldest notes (excluding the 10
newest), and atomically rewrites memory.md with `## Earlier summary` followed by `## Recent`.
"""

import http.server
import json
import pathlib
import re
import subprocess
import tempfile
import threading
import time

from wlblib import build
from wlblib.common import OUT, ROOT
from wlblib.testing import expect

NAME = "memory_condense"
DESCRIPTION = "Real harness condenses 30 memory notes into an Earlier summary and keeps 10 recent"
TEST_CONFIG = "whisper"  # Two level-1 priests standing still.
BOT = "Loada"
ONLINE = 2

KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)

NOTES = (
    ["- 2026-11-01 Hele tanked Ragefire Chasm for us, good tank, patient with pulls",
     "- 2026-11-01 Taragaman the Hungerer hits hard, Hele kited him away from the healer",
     "- 2026-11-02 Kelra is a mage who keeps pulling extra packs in Wailing Caverns",
     "- 2026-11-02 learned to drink between pulls, healer mana ran out twice",
     "- 2026-11-03 want to finish Deadmines with Hele, we wiped twice on Mr. Smite",
     "- 2026-11-03 Hele said he plays most evenings, invite him for Deadmines"]
    + [f"- 2026-11-{4 + i // 4:02d} killed boars and scorpids near Razor Hill, nothing new ({i})"
       for i in range(24)]
)
KEY_WORDS = ("Hele", "Deadmines", "Kelra", "Ragefire", "Smite", "Wailing")
SUMMARY_LINES = (
    "- Hele is a patient tank who ran Ragefire Chasm with us and kited Taragaman away from the healer.",
    "- Kelra is a mage who pulled extra packs in Wailing Caverns.",
    "- Lesson: drink between pulls so healer mana does not run out.",
    "- Unfinished goal: invite Hele in the evening to finish Deadmines after wiping on Mr. Smite.",
)


class _Brain:
    """Answers plan wakes with Keep (no world mutation) and condenses memory on P4 requests."""

    def __init__(self):
        self.lock = threading.Lock()
        self.plan_prompts = []
        self.condense_prompts = []
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
                messages = body.get("messages", [])
                system = "\n".join(m.get("content", "") for m in messages if m.get("role") == "system")
                user = "\n".join(m.get("content", "") for m in messages if m.get("role") == "user")
                if "You condense a World of Warcraft player's older session notes" in system:
                    with parent.lock:
                        parent.condense_prompts.append(user)
                    content = "\n".join(SUMMARY_LINES)
                else:
                    with parent.lock:
                        parent.plan_prompts.append(user)
                    keep = KEEP_RE.search(user)
                    content = "\n".join([
                        f"do: {keep.group(1) if keep else 1}",
                        "expect: none",
                        "watch: none",
                        "look: 300",
                    ])
                reply = json.dumps({
                    "choices": [{"message": {"content": content}}],
                    "usage": {"prompt_tokens": 100, "completion_tokens": 30},
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


def run(stack):
    build.common("default", tests=False)
    with tempfile.TemporaryDirectory(prefix="wlb-e2e-memory-") as tmp:
        bots_dir = pathlib.Path(tmp) / "bots"
        memory = bots_dir / BOT / "memory.md"
        memory.parent.mkdir(parents=True)
        memory.write_text("\n".join(NOTES) + "\n")

        brain = None
        harness = None
        out = ""
        text = ""
        exited_early = None
        try:
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
                text = memory.read_text()
                if "## Earlier summary" in text:
                    break
                if harness.poll() is not None:
                    exited_early = harness.returncode
                    break
                time.sleep(0.2)
        finally:
            if harness is not None:
                harness.terminate()
                try:
                    out, _ = harness.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    harness.kill()
                    out, _ = harness.communicate()
            if brain is not None:
                brain.stop()

    expect(exited_early is None,
           f"harness exited early (rc={exited_early}); harness log tail:\n{out[-1500:]}")
    with brain.lock:
        plan_prompts = list(brain.plan_prompts)
        condense_prompts = list(brain.condense_prompts)

    expect(plan_prompts, f"Loada never woke for a plan; harness log tail:\n{out[-1500:]}")
    expect(all(f"You are {BOT}." in p for p in plan_prompts),
           "--only-bots=Loada leaked another bot into the mock LLM")
    expect(len(condense_prompts) == 1,
           f"expected 1 memory-condense request, got {len(condense_prompts)}; "
           f"harness log tail:\n{out[-1500:]}")
    condense_req = condense_prompts[0]
    expect("Hele tanked Ragefire Chasm" in condense_req and "(13)" in condense_req,
           f"condense request missing older notes:\n{condense_req}")
    expect("(14)" not in condense_req and "(23)" not in condense_req,
           f"condense request included notes that should stay under ## Recent:\n{condense_req}")

    expect("## Earlier summary" in text,
           f"memory.md was not condensed within 30 s; harness log tail:\n{out[-1500:]}")
    summary_part, sep, recent_part = text.partition("## Recent")
    expect(bool(sep), f"condensed memory.md missing ## Recent heading:\n{text}")
    summary = [ln for ln in summary_part.splitlines() if ln.startswith("- ")]
    recent = [ln for ln in recent_part.splitlines() if ln.startswith("- ")]
    kept = [w for w in KEY_WORDS if w in summary_part]
    expect(1 <= len(summary) <= 6 and len(summary) == len(SUMMARY_LINES),
           f"expected {len(SUMMARY_LINES)} summary bullets (<= 6), got {len(summary)}:\n{summary_part}")
    expect(len(kept) == len(KEY_WORDS),
           f"summary kept {kept} of {KEY_WORDS}:\n{summary_part}")
    expect(len(recent) == 10, f"expected 10 recent notes kept, got {len(recent)}:\n{recent_part}")
    expect("(14)" in recent_part and "(23)" in recent_part and "(13)" not in recent_part,
           f"unexpected recent notes window:\n{recent_part}")
    return (f"{len(NOTES)} notes -> {len(summary)}-bullet summary keeping {len(kept)}/{len(KEY_WORDS)} "
            f"keywords + {len(recent)} recent")
