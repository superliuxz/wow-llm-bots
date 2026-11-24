"""Grouped e2e cases: whisper, party_reply, lfg_channel."""

import types
from wlblib import wire
from wlblib.testing import expect, find_option, send_cmd

def _case_whisper():
    """One bot whispers another and gets a whisper back.

    The chat path of phase 2's first "done when" item (docs/design.md), without the model: the
    module carries out a Say command as a whisper, its whisper hook hears it on the other bot, and the
    harness gets it as a chat event. The brain is the scripted one of tools/snapshot/fake_harness.py.
    """

    from wlblib.testing import expect

    NAME = "whisper"
    DESCRIPTION = "Loada whispers Telda, Telda hears it and whispers back, Loada hears that"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2
    # From the Say command to the chat event on the other bot: one world tick or two.
    HEARD_WITHIN_S = 2.0

    ASK = "hey, want to group up?"
    ANSWER = "sure, send me an invite"


    def run(stack):
        t = stack.whisper("Loada", "Telda", ASK, ANSWER)
        expect(not t["closed"], "the module closed the connection")
        expect(not t["errors"], f"a whisper failed: {t['errors']}")
        expect(len(t["commands"]) == 2, f"expected 2 whispers sent, got {t['commands']}")
        ask, answer = t["commands"]

        def heard(bot, sender, text):
            return next((h for h in t["heard"] if h["bot"] == bot and h.get("from") == sender
                         and h.get("text") == text and h.get("channel") == "WHISPER"), None)

        telda = heard("Telda", "Loada", ASK)
        expect(telda, f"Telda never heard Loada's whisper (heard: {t['heard']})")
        expect(telda["t"] - ask["t"] <= HEARD_WITHIN_S,
               f"Telda heard it {telda['t'] - ask['t']:.2f} s after it was sent")
        loada = heard("Loada", "Telda", ANSWER)
        expect(loada, f"Loada never heard Telda's answer (heard: {t['heard']})")
        expect(loada["t"] - answer["t"] <= HEARD_WITHIN_S,
               f"Loada heard it {loada['t'] - answer['t']:.2f} s after it was sent")
        # Only this case's two whispers: the log also holds earlier cases' lines
        # when several run on one server.
        ours = (f"Loada says (WHISPER to Telda, len={len(ASK)})",
                f"Telda says (WHISPER to Loada, len={len(ANSWER)})")
        said = [line for line in stack.server_log().splitlines()
                if any(o in line for o in ours)][-2:]
        expect(len(said) == 2 and all(line.endswith(": ok") for line in said),
               f"the module's record of the whispers: {said}")
        return (f"heard in {telda['t'] - ask['t']:.2f} s and {loada['t'] - answer['t']:.2f} s")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_party_reply():
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
    from wlblib.testing import ProxyRecorder as _ProxyRecorder
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
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_lfg_channel():
    """Bots hear the LookingForGroup channel (RH-M7).

    `party` test config (Channel.RestrictedLfg = 0, so bots outside the queue can be in the channel).

      1. Loada and Loadb join LookingForGroup.
      2. Loada types "LFM RFC need heals" there (through the client's chat handler): Loadb hears it
         as LFG channel chat, without being woken (not named).
      3. Loada types "loadb want to come to RFC?": Loadb hears it, woken (named).
      4. The harness makes Loada post "LFM Deadmines" (a brain `lfg:` line, F1): Loadb hears it.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "lfg_channel"
    DESCRIPTION = "LookingForGroup channel chat reaches bots; only a named bot is woken"
    TEST_CONFIG = "party"
    BOT = "Loada"
    ONLINE = 4

    PLAIN = "LFM RFC need heals"
    NAMED = "loadb want to come to RFC?"
    POSTED = "LFM Deadmines"


    def run(stack):
        for bot in ("Loada", "Loadb"):
            out = server.console(f"llmtest joinchannel {bot} LookingForGroup", inst=TEST)
            expect("joined" in out, f"setup: {bot} could not join LookingForGroup ({out.strip()[-160:]})")

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        heard = {}  # text -> chat event as Loadb heard it
        errors = []  # refusals of Loada's posted line
        try:
            time.sleep(1.0)  # Let the harness connection settle.
            server.console(f"llmtest chat Loada channel:LookingForGroup {PLAIN}", inst=TEST)
            server.console(f"llmtest chat Loada channel:LookingForGroup {NAMED}", inst=TEST)
            payload = wire.encode_command(
                {"bot": "Loada", "command_id": 1, "say": {"channel": "LFG", "text": POSTED}})
            sock.sendall(len(payload).to_bytes(4, "big") + payload)
            deadline = time.time() + 10.0
            while time.time() < deadline and len(heard) < 3:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg or "event" not in msg:
                        continue
                    err = msg["event"].get("event", {}).get("error")
                    if msg["event"].get("bot") == "Loada" and err and err.get("command_id") == 1:
                        errors.append(err.get("text"))
                    if msg["event"].get("bot") != "Loadb":
                        continue
                    chat = msg["event"].get("event", {}).get("chat")
                    if chat and chat.get("from") == "Loada" and chat.get("text") in (PLAIN, NAMED, POSTED):
                        heard[chat["text"]] = chat
        finally:
            sock.close()

        plain, named = heard.get(PLAIN), heard.get(NAMED)
        expect(plain, f"Loadb never heard {PLAIN!r} in LookingForGroup (heard {list(heard)})")
        expect(named, f"Loadb never heard {NAMED!r} in LookingForGroup (heard {list(heard)})")
        expect(plain.get("channel") == "LFG" and named.get("channel") == "LFG",
               f"not reported as LFG channel chat: {plain.get('channel')}, {named.get('channel')}")
        expect(not plain.get("at_me"), "an unnamed LFG line woke Loadb")
        expect(named.get("at_me"), "a line naming Loadb did not wake it")
        expect(not errors, f"Loada's LFG post was refused: {errors}")
        posted = heard.get(POSTED)
        expect(posted and posted.get("channel") == "LFG",
               f"Loadb never heard Loada's posted {POSTED!r} as LFG chat (heard {list(heard)})")
        return ("Loadb heard both typed LookingForGroup lines (only the named one woke it) "
                "and Loada's posted LFM")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

CASES = [_case_whisper(), _case_party_reply(), _case_lfg_channel()]
