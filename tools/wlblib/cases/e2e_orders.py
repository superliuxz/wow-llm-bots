"""Live standing-orders e2e test on the isolated test server.

Runs the real C++ `out/common/harness run` (`BotLoop` + `CollectOrderNames` +
`PlanGrammar` + `ParsePlan` + `OrderEvaluator` + `OrderActToCommand`) against
two grouped level-80 priests (`Loada` and `Telda`) next to Mottled Boar spawns
on the isolated test worldserver (`act_executor.cc`), exercising one scenario
per verb family:
  1. moving + `then` chain + `group` with nearby mobs + edge-latching:
     `Shadow Word: Pain on me -> away group 10, then to behind Telda`
  2. `target` + `mark` (`then` chain):
     `Renew on me -> target nearest Mottled Boar, then mark my target skull`
  3. `use` (client spell handler):
     `Shadow Word: Pain on me -> use Power Word: Shield on me`
  4. `kite` (continuous forced tangential steering without 5 s timeout, plus
     `stop moving` when the condition clears):
     `Renew on me -> kite nearest Mottled Boar`
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

NAME = "orders"
DESCRIPTION = "standing orders live: away group + to behind chain, target + mark, use, kite"
TEST_CONFIG = "orders"
BOT = "Loada"
ONLINE = 2

KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)


def _pos2d(snap):
    p = snap.get("base", {}).get("where", {}).get("position", {})
    return (p.get("x", 0.0), p.get("y", 0.0))


def _auras_on_self(snap):
    return {a.get("name", "") for a in snap.get("base", {}).get("self", {}).get("unit", {}).get("auras", [])}


class _ProxyRecorder:
    """Forwards BEVE frames between `harness run` and `mod-llm-brain` while
    recording decoded snapshots, immediate events, visibility errors, and
    commands."""

    def __init__(self, real_sock: pathlib.Path, proxy_sock: pathlib.Path):
        self.real_sock = real_sock
        self.proxy_sock = proxy_sock
        self.lock = threading.Lock()
        self.snapshots = []  # (monotonic_s, snap_dict)
        self.events = []     # (monotonic_s, imm_dict)
        self.commands = []   # (monotonic_s, cmd_dict)
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
                        verr = wire_py.visibility_errors(payload) or []
                        msg = wire_py.decode_module_message(payload)
                        with self.lock:
                            if verr and len(self.visibility_errors) < 10:
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

    def wait_until(self, pred, timeout_s=15.0, desc="condition"):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            with self.lock:
                val = pred(self)
                if val:
                    return val
            time.sleep(0.05)
        raise AssertionError(f"timed out ({timeout_s}s) waiting for {desc}")


class _MockBrainServer:
    """Local HTTP `/v1/chat/completions` stand-in returning hand-written plans."""

    def __init__(self):
        self.lock = threading.Lock()
        self.orders_for_loada = []
        self.loada_replies = 0
        self.loada_grammars = []
        self.loada_wake_reasons = []
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
                grammar = body.get("grammar", "")
                m = KEEP_RE.search(user_text)
                keep_opt = int(m.group(1)) if m else 1
                lines = [
                    f"do: {keep_opt}",
                    "expect: none",
                    "watch: none",
                    "look: 300",
                ]
                is_loada = user_text.startswith("You are Loada.")
                with parent.lock:
                    if is_loada:
                        for o in parent.orders_for_loada:
                            lines.append(f"order: {o}")
                        parent.loada_replies += 1
                        parent.loada_grammars.append(grammar)
                        for line in user_text.splitlines():
                            if line.startswith("Woken: "):
                                parent.loada_wake_reasons.append(line)
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

    def set_orders(self, orders):
        with self.lock:
            self.orders_for_loada = list(orders)
            return self.loada_replies


def run(stack):
    build.common("default", tests=False)

    # Place Telda at (-669, -4282, 41) facing East (0 rad) and Loada 1 yd East
    # of Telda, grouped with Loada as party leader, right next to Mottled Boars.
    server.console("llmtest move Telda -669.0 -4282.0 41.0 0.0", wait_s=0.3, inst=TEST)
    server.console("llmtest move Loada -668.0 -4282.0 41.0 0.0", wait_s=0.3, inst=TEST)
    server.console("llmtest group Loada Telda", wait_s=0.5, inst=TEST)

    with tempfile.TemporaryDirectory(prefix="wlb-e2e-orders-") as tmp:
        tmp_dir = pathlib.Path(tmp)
        bots_dir = tmp_dir / "bots"
        (bots_dir / "Loada").mkdir(parents=True)
        (bots_dir / "Loada" / "notes.md").write_text(
            "Mottled Boar, Shadow Word: Pain, Renew: use Power Word: Shield on me.\n"
        )
        proxy_sock = tmp_dir / "proxy.sock"
        proxy = _ProxyRecorder(stack.socket, proxy_sock)
        brain = _MockBrainServer()

        # Scenario 1 orders installed on Loada's initial "first look" wake.
        brain.set_orders([
            "Renew on me -> away group 10, then to behind Telda",
        ])
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
            def wait_plan(prev_count):
                deadline = time.monotonic() + 10.0
                while time.monotonic() < deadline:
                    with brain.lock:
                        if brain.loada_replies > prev_count:
                            return brain.loada_replies
                    time.sleep(0.05)
                expect(False, "Loada never completed a brain call")

            def act_cmds(since_idx=0):
                return [
                    (i, t, c) for i, (t, c) in enumerate(proxy.commands)
                    if i >= since_idx and c.get("bot") == "Loada" and "act" in c
                ]

            def check_no_errors():
                with proxy.lock:
                    expect(not proxy.visibility_errors,
                           f"visibility errors: {proxy.visibility_errors}")
                    errs = [
                        ev["event"]["error"] for _, ev in proxy.events
                        if "error" in ev.get("event", {})
                    ]
                    expect(not errs, f"module reported command errors: {errs}")

            # Wait for both bots' initial snapshots + Loada's first plan.
            replies = wait_plan(0)
            proxy.wait_until(
                lambda p: any(
                    s.get("bot") == "Loada"
                    and any(u.get("name") == "Mottled Boar"
                            for u in s.get("base", {}).get("nearby", {}).get("units", []))
                    for _, s in p.snapshots
                ),
                timeout_s=10.0,
                desc="Loada snapshot with nearby Mottled Boar",
            )
            with brain.lock:
                g0 = brain.loada_grammars[0]
            for expected_name in ("Shadow Word: Pain", "Power Word: Shield", "Telda", "Mottled Boar"):
                expect(f'"{expected_name}"' in g0,
                       f"{expected_name!r} missing from live PlanGrammar")

            # ---- 1. Moving (`away group 10, then to behind Telda`) + latching ----
            cmd_mark = len(proxy.commands)
            server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)

            away_entry = proxy.wait_until(
                lambda _: next(
                    ((i, t, c) for i, t, c in act_cmds(cmd_mark)
                     if c["act"].get("verb") == "AWAY" and c["act"].get("thing") == "GROUP"),
                    None,
                ),
                timeout_s=8.0,
                desc="AWAY GROUP act command",
            )
            away_id = away_entry[2]["command_id"]
            away_done = proxy.wait_until(
                lambda p: next(
                    (s for _, s in p.snapshots
                     if s.get("bot") == "Loada" and s.get("done_act_command_id") == away_id),
                    None,
                ),
                timeout_s=15.0,
                desc="away group 10 completion",
            )
            telda_pos = _pos2d(next(
                s for _, s in reversed(proxy.snapshots) if s.get("bot") == "Telda"
            ))
            away_dist = math.dist(_pos2d(away_done), telda_pos)
            expect(away_dist >= 7.0,
                   f"away group 10 finished only {away_dist:.1f} yd from Telda")

            behind_entry = proxy.wait_until(
                lambda _: next(
                    ((i, t, c) for i, t, c in act_cmds(away_entry[0] + 1)
                     if c["act"].get("verb") == "TO" and c["act"].get("place") == "BEHIND"),
                    None,
                ),
                timeout_s=8.0,
                desc="then-chain TO BEHIND Telda act command",
            )
            behind_id = behind_entry[2]["command_id"]
            behind_done = proxy.wait_until(
                lambda p: next(
                    (s for _, s in p.snapshots
                     if s.get("bot") == "Loada" and s.get("done_act_command_id") == behind_id),
                    None,
                ),
                timeout_s=15.0,
                desc="to behind Telda completion",
            )
            behind_pos = _pos2d(behind_done)
            behind_dist = math.dist(behind_pos, telda_pos)
            expect(behind_pos[0] < telda_pos[0] - 0.5 and behind_dist <= 6.0,
                   f"to behind Telda ended at {behind_pos} vs Telda {telda_pos} ({behind_dist:.1f} yd)")

            # Edge-latching: Renew is still on Loada; the finished
            # chain must NOT fire a third act across subsequent snapshots.
            time.sleep(1.5)
            extra_acts = act_cmds(behind_entry[0] + 1)
            expect(not extra_acts,
                   f"order re-fired while condition stayed true: {extra_acts}")
            server.console("llmtest unaura Loada 139", wait_s=0.3, inst=TEST)
            check_no_errors()

            # ---- 2. `target` + `mark` (`then` chain) -----------------------------
            prev_replies = brain.set_orders([
                "Renew on me -> target nearest Mottled Boar, then mark my target skull",
            ])
            server.console("llmbrain whisper Telda Loada phase2", wait_s=0.1, inst=TEST)
            wait_plan(prev_replies)
            cmd_mark = len(proxy.commands)
            server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)

            mark_entry = proxy.wait_until(
                lambda _: next(
                    ((i, t, c) for i, t, c in act_cmds(cmd_mark)
                     if c["act"].get("verb") == "MARK" and c["act"].get("marker") == "SKULL"),
                    None,
                ),
                timeout_s=10.0,
                desc="MARK SKULL act command",
            )
            mark_id = mark_entry[2]["command_id"]
            marked_snap = proxy.wait_until(
                lambda p: next(
                    (s for _, s in p.snapshots
                     if s.get("bot") == "Loada"
                     and (s.get("ext", {}).get("target", {}).get("target") or {}).get("name") == "Mottled Boar"
                     and (
                         (s.get("ext", {}).get("target", {}).get("target") or {}).get("raid_marker") == "SKULL"
                         or any(u.get("raid_marker") == "SKULL"
                                for u in s.get("base", {}).get("nearby", {}).get("units", []))
                     )),
                    None,
                ),
                timeout_s=10.0,
                desc="snapshot showing Mottled Boar targeted and marked SKULL",
            )
            expect(marked_snap is not None, "Mottled Boar was never marked SKULL")
            server.console("llmtest unaura Loada 139", wait_s=0.3, inst=TEST)
            check_no_errors()

            # ---- 3. `use` (Power Word: Shield through client handler) ------------
            prev_replies = brain.set_orders([
                "Renew on me & not Power Word: Shield on me -> use Power Word: Shield on me",
            ])
            server.console("llmbrain whisper Telda Loada phase3", wait_s=0.1, inst=TEST)
            wait_plan(prev_replies)
            cmd_mark = len(proxy.commands)
            server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)

            use_entry = proxy.wait_until(
                lambda _: next(
                    ((i, t, c) for i, t, c in act_cmds(cmd_mark)
                     if c["act"].get("verb") == "USE"
                     and c["act"].get("name") == "Power Word: Shield"),
                    None,
                ),
                timeout_s=8.0,
                desc="USE Power Word: Shield act command",
            )
            shield_snap = proxy.wait_until(
                lambda p: next(
                    (s for _, s in p.snapshots
                     if s.get("bot") == "Loada" and "Power Word: Shield" in _auras_on_self(s)),
                    None,
                ),
                timeout_s=8.0,
                desc="Power Word: Shield aura on Loada",
            )
            expect(shield_snap is not None, "Power Word: Shield never appeared on Loada")
            server.console("llmtest unaura Loada 139", wait_s=0.3, inst=TEST)
            check_no_errors()

            # ---- 4. `kite` (> 5 s without timeout + `stop moving` on clear) ------
            prev_replies = brain.set_orders([
                "Renew on me -> kite nearest Mottled Boar",
            ])
            server.console("llmbrain whisper Telda Loada phase4", wait_s=0.1, inst=TEST)
            replies_before_kite = wait_plan(prev_replies)
            start_pos = _pos2d(next(
                s for _, s in reversed(proxy.snapshots) if s.get("bot") == "Loada"
            ))
            cmd_mark = len(proxy.commands)
            server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)

            kite_entry = proxy.wait_until(
                lambda _: next(
                    ((i, t, c) for i, t, c in act_cmds(cmd_mark)
                     if c["act"].get("verb") == "KITE"),
                    None,
                ),
                timeout_s=8.0,
                desc="KITE act command",
            )
            # Let Loada kite for 6.0 s (> the 5 s default step timeout) to prove
            # kite neither stalls nor triggers a step-timeout wake.
            time.sleep(6.0)
            with proxy.lock:
                kite_positions = [
                    _pos2d(s) for t, s in proxy.snapshots
                    if s.get("bot") == "Loada" and t >= kite_entry[1]
                ]
            max_kite_disp = max((math.dist(start_pos, p) for p in kite_positions), default=0.0)
            expect(max_kite_disp >= 5.0,
                   f"kite moved Loada at most {max_kite_disp:.1f} yd from {start_pos}")
            with brain.lock:
                timeout_wakes = [w for w in brain.loada_wake_reasons if "timed out" in w]
                expect(not timeout_wakes and brain.loada_replies == replies_before_kite,
                       f"unexpected wake while kiting: {brain.loada_wake_reasons}")

            # Clearing Renew ends the kite and sends `stop moving`.
            stop_mark = len(proxy.commands)
            server.console("llmtest unaura Loada 139", wait_s=0.1, inst=TEST)
            proxy.wait_until(
                lambda _: next(
                    ((i, t, c) for i, t, c in act_cmds(stop_mark)
                     if c["act"].get("verb") == "STOP" and c["act"].get("stop") == "MOVING"),
                    None,
                ),
                timeout_s=8.0,
                desc="STOP MOVING act command when kite condition clears",
            )
            check_no_errors()

            return (f"away {away_dist:.1f} yd -> behind Telda ({behind_dist:.1f} yd), "
                    f"marked boar SKULL, shielded self, kited {max_kite_disp:.1f} yd + stopped")
        finally:
            harness_proc.terminate()
            try:
                harness_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                harness_proc.kill()
            brain.stop()
            proxy.stop()
