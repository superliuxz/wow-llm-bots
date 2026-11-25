"""Grouped e2e cases: reconnect, memory_condense, relationships."""

import types
from wlblib import wire
from wlblib.testing import expect, find_option, send_cmd

def _case_reconnect():
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
            c2u = wire.FrameReader()
            u2c = wire.FrameReader()
            try:
                while not self._stop.is_set() and not self._drop.is_set():
                    r, _, _ = select.select([cli, up], [], [], 0.1)
                    if cli in r:
                        data = cli.recv(1 << 16)
                        if not data:
                            break
                        up.sendall(data)
                        for payload in c2u.feed(data):
                            cmd = wire.decode_command(payload)
                            if cmd:
                                with self.lock:
                                    self.commands.append((gen, cmd))
                    if up in r:
                        data = up.recv(1 << 16)
                        if not data:
                            break
                        cli.sendall(data)
                        for payload in u2c.feed(data):
                            msg = wire.decode_module_message(payload)
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
        log_start = len(stack.server_log())  # Cases batched before this one log too.
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
        with proxy.lock:
            firsts = {}
            for g, c in proxy.commands:
                firsts.setdefault(g, c)
        for g, c in sorted(firsts.items()):
            expect(c.get("hello", {}).get("role") == "harness",
                   f"connection {g}: the harness's first frame was not its hello: {c}")
        log = stack.server_log()[log_start:]
        hello_lines = log.count("mod-llm-brain: harness hello: wire version")
        expect(hello_lines >= gens,
               f"module logged {hello_lines} harness hellos for {gens} connections")
        expect("refusing the connection" not in log, "the module refused a harness of this build")
        return f"reconnected across drop ({gens} generations, wakes {wakes_before}->{wakes_after})"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_memory_condense():
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
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_relationships():
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
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_handshake():
    """Hellos on connect (docs/specs/snapshot.md, "Handshake").

    1. The module's first frame on a raw connection is its hello, naming this build's wire
       version and schema hash; a passive reader that says nothing gets snapshots after it.
    2. A client whose hello names another schema is refused: one clear line in the server log,
       no snapshots while it stays, and its commands are not applied.
    3. The next client, of this build, is served again (its hello logged, snapshots back).
    4. Tick owner: the bot is playerbots-led while the refused client stays and brain-led
       again under this build's client; every gated playerbots action name was found at startup.
    """

    import json
    import re
    import socket
    import time

    from wlblib import wire
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "handshake"
    DESCRIPTION = "Module hello first; a client of another build is refused, the next one served"
    TEST_CONFIG = "choose"
    BOT = "Loada"

    def connect():
        """A connection the module accepted (the last one may still be closing)."""
        deadline = time.monotonic() + 10.0
        while True:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(0.5)
            s.connect(str(stack_socket[0]))
            reader = wire.FrameReader()
            try:
                chunk = s.recv(1 << 20)
            except socket.timeout:
                chunk = None
            if chunk:
                return s, reader, reader.feed(chunk)
            s.close()
            expect(time.monotonic() < deadline, "the module kept refusing new connections")
            time.sleep(0.2)

    def read(s, reader, seconds):
        """Decoded (raw json, decoded-or-None) module frames for `seconds`."""
        frames = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                chunk = s.recv(1 << 20)
            except socket.timeout:
                continue
            if not chunk:
                break
            frames.extend(reader.feed(chunk))
        return frames

    stack_socket = [None]

    def run(stack):
        stack_socket[0] = stack.socket
        mine = wire.hello_command("")["hello"]

        # 1. Hello first, then snapshots for a passive reader.
        s, reader, first = connect()
        frames = first + read(s, reader, 3.0)
        s.close()
        expect(frames, "no frames at all")
        hello = json.loads(wire.module_message_to_json(frames[0]))
        expect(set(hello) == {"version", "hello"}, f"first frame is not a bare hello: {hello}")
        expect(hello["hello"] == {"wire_version": mine["wire_version"],
                                  "schema_hash": mine["schema_hash"], "role": "module"},
               f"module hello {hello['hello']} vs this build {mine}")
        decoded = [wire.decode_module_message(f) for f in frames]
        expect(decoded[0] is None, "decode_module_message passed the hello through")
        hellos = sum(1 for f in frames[1:] if "hello" in json.loads(wire.module_message_to_json(f)))
        expect(hellos == 0, f"{hellos} more hellos after the first")
        snaps = [m for m in decoded if m and "snapshot" in m]
        expect(snaps, "a passive reader got no snapshots after the hello")

        # 2. A client of another build: refused.
        log_before = len(stack.server_log())
        s, reader, first = connect()
        stale = wire.hello_command("stale e2e")
        stale["hello"]["schema_hash"] = "0000000000000000"
        wire.send_frame(s, wire.encode_command(stale))
        wire.send_frame(s, wire.encode_command(
            {"bot": BOT, "command_id": 9001, "say": {"channel": "SAY", "text": "stale build"}}))
        time.sleep(1.0)  # The refusal lands before later snapshots are built...
        read(s, reader, 0.5)  # ...so drop what was sent before it.
        later = read(s, reader, 3.0)
        s.close()
        log = stack.server_log()[log_before:]
        expect("refusing the connection: stale e2e has wire schema 0000000000000000" in log,
               f"no refusal in the server log: {log[-600:]!r}")
        expect(log.count("refusing the connection") == 1,
               f"refusal logged {log.count('refusing the connection')} times")
        refused_snaps = [m for m in map(wire.decode_module_message, later) if m and "snapshot" in m]
        expect(not refused_snaps, f"{len(refused_snaps)} snapshots sent to a refused client")
        expect(f"{BOT} says (SAY, len=11)" not in log, "the refused client's say was applied")
        # The latest mode line (the refusal may land before any tick saw it connected).
        modes = re.findall(rf"mod-llm-brain: {BOT} is (brain-led|playerbots-led)",
                           stack.server_log())
        expect(not modes or modes[-1] == "playerbots-led",
               f"{BOT} not playerbots-led under a refused client: {modes[-3:]}")

        # 3. This build again: served.
        log_before = len(stack.server_log())
        s, reader, first = connect()
        wire.send_frame(s, wire.encode_command(wire.hello_command("e2e handshake")))
        wire.send_frame(s, wire.encode_command(
            {"bot": BOT, "command_id": 9002, "say": {"channel": "SAY", "text": "fresh build"}}))
        frames = first + read(s, reader, 3.0)
        whole = stack.server_log()  # Before the close turns the bot playerbots-led.
        log = whole[log_before:]
        s.close()
        snaps = [m for m in map(wire.decode_module_message, frames) if m and "snapshot" in m]
        expect(snaps, "no snapshots for a client of this build after a refused one")
        expect("mod-llm-brain: e2e handshake hello: wire version" in log,
               f"the good hello wasn't logged: {log[-600:]!r}")
        expect("refusing" not in log, "a client of this build was refused")
        expect(f"{BOT} says (SAY, len=11)" in log,
               f"the say of a client of this build wasn't applied: {log[-600:]!r}")
        modes = re.findall(rf"mod-llm-brain: {BOT} is (brain-led|playerbots-led)", whole)
        expect(modes and modes[-1] == "brain-led",
               f"{BOT} not brain-led under a client of this build: {modes[-3:]}")

        # 4. Every gated playerbots action name exists (checked once at startup, so before
        # stack.server_log() starts: read the log file from the last server start).
        full = (TEST.logs / "Server.log").read_text(errors="replace")
        full = full[max(0, full.rfind("gated action names found") - 2000):]
        found = re.search(r"mod-llm-brain: (\d+) of (\d+) gated action names found", full)
        expect(found and found.group(1) == found.group(2),
               f"gated action check: {found.group(0) if found else 'not logged'}")
        expect("is not a playerbots action" not in full, "a gated action name is missing")
        return (f"hello {mine['schema_hash']} first; stale schema refused once with no "
                f"snapshots ({BOT} playerbots-led); next client got {len(snaps)} snapshots "
                f"({BOT} brain-led); {found.group(1)} gated action names found")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

CASES = [_case_reconnect(), _case_handshake(), _case_memory_condense(), _case_relationships()]
