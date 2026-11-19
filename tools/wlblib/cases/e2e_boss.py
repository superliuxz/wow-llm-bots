"""Live boss plumbing e2e test on the isolated test server (Halls of Lightning — Loken).

Runs real `out/common/harness run` against two geared, grouped level-80 priests
(`Loada` and `Telda`) fighting **Loken** (map 602) with playerbots' hardcoded
`wotlk-hol` boss script stripped (`config/tests/boss.llm-brain.overrides`).

Uses a deterministic local mock brain (so it tests boss-encounter plumbing
without depending on llama-server):
  1. Verifies Loken's `Lightning Nova` (`CURRENT_GENERIC_SPELL`) is visible in
     snapshots even while Loken channels `Pulsing Shockwave`
     (`CURRENT_CHANNELED_SPELL`).
  2. Verifies `order: Lightning Nova cast -> away Loken 25` fires on Loken's
     first Lightning Nova cast (< 500 ms harness reaction, 0 extra brain wakes)
     and moves Loada clear of the 20 yd blast radius with 0 deaths and 0
     visibility/command errors.
"""

import http.server
import json
import math
import pathlib
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time

from wlblib import build, server
from wlblib.common import OUT, ROOT, TEST
from wlblib.testing import expect

sys.path.insert(0, str(ROOT / "tools" / "snapshot"))
from frame_reader import FrameReader  # noqa: E402
import wire_py  # noqa: E402

NAME = "boss"
DESCRIPTION = "live boss plumbing (Loken, HoL): Lightning Nova cast -> away Loken 25"
TEST_CONFIG = "boss"
BOT = "Loada"
ONLINE = 2

LOKEN_X = 1186.47
LOKEN_Y = 33.8305
ATTACK_LOKEN_RE = re.compile(r"^\s+(\d+)\. Attack Loken\b", re.M)
KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)


def _pos2d(snap):
    p = snap.get("base", {}).get("where", {}).get("position", {})
    return (p.get("x", 0.0), p.get("y", 0.0))


def _find_loken(snap):
    for u in snap.get("base", {}).get("nearby", {}).get("units", []):
        if u.get("name") == "Loken":
            return u
    t = snap.get("ext", {}).get("target", {}).get("target", {})
    if t.get("name") == "Loken":
        return t
    return None


def _loken_cast_name(loken):
    if not loken:
        return ""
    act_obj = loken.get("activity", {})
    cast_obj = act_obj.get("cast", {}) if isinstance(act_obj, dict) else {}
    sp_obj = cast_obj.get("spell", {}) if isinstance(cast_obj, dict) else {}
    return sp_obj.get("name", "") if isinstance(sp_obj, dict) else ""


def _loken_dist(snap, loken):
    if loken and isinstance(loken.get("rel"), dict) and "dist_yd" in loken["rel"]:
        return float(loken["rel"]["dist_yd"])
    x, y = _pos2d(snap)
    return math.hypot(x - LOKEN_X, y - LOKEN_Y)


class _ProxyRecorder:
    def __init__(self, real_sock: pathlib.Path, proxy_sock: pathlib.Path):
        self.real_sock = real_sock
        self.proxy_sock = proxy_sock
        self.lock = threading.Lock()
        self.snapshots = []
        self.events = []
        self.commands = []
        self.visibility_errors = []
        self._stop = threading.Event()
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        proxy_sock.unlink(missing_ok=True)
        self._listener.bind(str(proxy_sock))
        self._listener.listen(2)
        self._listener.settimeout(0.2)
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        try:
            self._listener.close()
        except OSError:
            pass
        self._thread.join(timeout=5)
        self.proxy_sock.unlink(missing_ok=True)

    def _serve(self):
        client = None
        upstream = None
        try:
            while not self._stop.is_set():
                try:
                    client, _ = self._listener.accept()
                    break
                except socket.timeout:
                    continue
            if client is None:
                return
            upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            upstream.connect(str(self.real_sock))
            in_mod = FrameReader()
            in_cmd = FrameReader()
            while not self._stop.is_set():
                r, _, _ = select.select([client, upstream], [], [], 0.1)
                now = time.monotonic()
                if upstream in r:
                    data = upstream.recv(1 << 20)
                    if not data:
                        break
                    client.sendall(data)
                    for payload in in_mod.feed(data):
                        verr = wire_py.visibility_errors(payload)
                        msg = wire_py.decode_module_message(payload)
                        with self.lock:
                            if verr is None:
                                if len(self.visibility_errors) < 10:
                                    self.visibility_errors.append(
                                        "failed to decode module frame for visibility check"
                                    )
                            elif verr and len(self.visibility_errors) < 10:
                                self.visibility_errors.extend(verr[:5])
                            if msg and "snapshot" in msg:
                                self.snapshots.append((now, msg["snapshot"]))
                            elif msg and "event" in msg:
                                self.events.append((now, msg["event"]))
                if client in r:
                    data = client.recv(1 << 20)
                    if not data:
                        break
                    upstream.sendall(data)
                    for payload in in_cmd.feed(data):
                        cmd = wire_py.decode_command(payload)
                        if cmd:
                            with self.lock:
                                self.commands.append((now, cmd))
        finally:
            for s in (client, upstream):
                if s is not None:
                    try:
                        s.close()
                    except OSError:
                        pass

    def wait_until(self, pred, timeout_s=30.0, desc="condition"):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            with self.lock:
                val = pred(self)
                if val:
                    return val
            time.sleep(0.05)
        raise AssertionError(f"timed out ({timeout_s}s) waiting for {desc}")


class _BossMockBrain:
    """Attacks Loken and installs `Lightning Nova cast -> away Loken 25` on Loada."""

    def __init__(self):
        self.lock = threading.Lock()
        self.loada_wakes = []
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length) if length > 0 else b""
                if not raw:
                    return
                body = json.loads(raw.decode("utf-8"))
                user_text = body["messages"][1]["content"]
                m_atk = ATTACK_LOKEN_RE.search(user_text)
                m_keep = KEEP_RE.search(user_text)
                opt = int(m_atk.group(1)) if m_atk else (int(m_keep.group(1)) if m_keep else 1)
                exp = str(opt) if m_atk else "none"
                lines = [
                    f"do: {opt}",
                    f"expect: {exp}",
                    "watch: none",
                    "look: 300",
                ]
                if user_text.startswith("You are Loada."):
                    lines.append("order: Lightning Nova cast -> away Loken 25")
                    with parent.lock:
                        for line in user_text.splitlines():
                            if line.startswith("Woken: "):
                                parent.loada_wakes.append(line)
                                break
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

    # Gear both priests at level 80, group them, and place them in front of
    # Loken in Halls of Lightning (map 602).
    server.console("llmtest gear Loada 80", wait_s=0.5, inst=TEST)
    server.console("llmtest gear Telda 80", wait_s=0.5, inst=TEST)
    server.console("llmtest group Loada Telda", wait_s=0.5, inst=TEST)
    server.console("llmtest tele Loada 602 1160.0 33.83 60.81 0.0", wait_s=0.5, inst=TEST)
    server.console("llmtest tele Telda 602 1160.0 31.83 60.81 0.0", wait_s=1.0, inst=TEST)

    with tempfile.TemporaryDirectory(prefix="wlb-e2e-boss-") as tmp:
        tmp_dir = pathlib.Path(tmp)
        bots_dir = tmp_dir / "bots"
        (bots_dir / "Loada").mkdir(parents=True)
        (bots_dir / "Loada" / "notes.md").write_text(
            "Loken, Lightning Nova: 4-second cast AoE within 20 yd; run 25 yd away.\n"
        )
        proxy_sock = tmp_dir / "proxy.sock"
        proxy = _ProxyRecorder(stack.socket, proxy_sock)
        brain = _BossMockBrain()
        proxy.start()
        brain.start()
        harness_proc = subprocess.Popen(
            [
                str(OUT / "common" / "harness"), "run",
                f"--socket={proxy_sock}",
                f"--rules={ROOT / 'content' / 'RULES.md'}",
                f"--bots-dir={bots_dir}",
                "--host=127.0.0.1",
                f"--port={brain.port}",
                "--slots=4",
                "--temperature=0",
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            # 1. Wait for Loada to enter combat with Loken.
            proxy.wait_until(
                lambda p: any(
                    s.get("bot") == "Loada"
                    and s.get("base", {}).get("self", {}).get("in_combat", False)
                    and _find_loken(s) is not None
                    for _, s in p.snapshots
                ),
                timeout_s=15.0,
                desc="Loada entering combat with Loken",
            )
            with brain.lock:
                wakes_before_nova = len(brain.loada_wakes)

            # 2. Wait for Loken to cast Lightning Nova (~15 s into the fight).
            nova_start = proxy.wait_until(
                lambda p: next(
                    ((t, s) for t, s in p.snapshots
                     if s.get("bot") == "Loada"
                     and _loken_cast_name(_find_loken(s)) == "Lightning Nova"),
                    None,
                ),
                timeout_s=25.0,
                desc="Loken casting Lightning Nova in snapshot",
            )
            t_nova_start, snap_start = nova_start
            dist_start = _loken_dist(snap_start, _find_loken(snap_start))

            # 3. Verify harness immediately dispatches AWAY Loken 25.
            away_cmd = proxy.wait_until(
                lambda p: next(
                    ((t, c) for t, c in p.commands
                     if c.get("bot") == "Loada"
                     and c.get("act", {}).get("verb") == "AWAY"
                     and t >= t_nova_start - 0.1),
                    None,
                ),
                timeout_s=3.0,
                desc="AWAY Loken 25 act command on Lightning Nova",
            )
            react_ms = max(0.0, (away_cmd[0] - t_nova_start) * 1000.0)
            expect(react_ms <= 500.0,
                   f"Lightning Nova reaction took {react_ms:.0f} ms (> 500 ms)")

            # 4. Wait for AWAY to finish and verify Loada moved substantially
            # away from Loken with 0 deaths and 0 extra wakes.
            away_id = away_cmd[1]["command_id"]
            away_done = proxy.wait_until(
                lambda p: next(
                    (s for t, s in p.snapshots
                     if s.get("bot") == "Loada"
                     and t >= away_cmd[0]
                     and s.get("done_act_command_id") == away_id),
                    None,
                ),
                timeout_s=10.0,
                desc="AWAY Loken 25 completion",
            )
            dist_after = _loken_dist(away_done, _find_loken(away_done))
            expect(dist_after >= dist_start + 8.0 and dist_after >= 20.0,
                   f"Loada only moved {dist_start:.1f} -> {dist_after:.1f} yd from Loken")

            with proxy.lock:
                expect(not proxy.visibility_errors,
                       f"visibility errors: {proxy.visibility_errors}")
                errs = [
                    ev["event"]["error"] for _, ev in proxy.events
                    if "error" in ev.get("event", {})
                ]
                expect(not errs, f"module reported command errors: {errs}")
                died = any(
                    s.get("base", {}).get("self", {}).get("unit", {}).get("dead", False)
                    for _, s in proxy.snapshots
                    if s.get("bot") == "Loada"
                )
                expect(not died, "Loada died during Loken fight")

            with brain.lock:
                extra_wakes = brain.loada_wakes[wakes_before_nova:]
                nova_wakes = [w for w in extra_wakes if "standing order" in w]
                expect(not nova_wakes,
                       f"unexpected standing-order wake during Lightning Nova: {nova_wakes}")

            return (f"Loken Lightning Nova: react {react_ms:.0f} ms, "
                    f"dist {dist_start:.1f} -> {dist_after:.1f} yd, 0 deaths")
        finally:
            harness_proc.terminate()
            try:
                harness_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                harness_proc.kill()
            brain.stop()
            proxy.stop()
